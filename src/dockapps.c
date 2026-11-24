/* ========================================================================= */
/**
 * @file dockapps.c
 *
 * @copyright
 * Copyright (c) 2026 Big WindowMaker contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "dockapps.h"

#include <inttypes.h>
#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-server-core.h>
#define WLR_USE_UNSTABLE
#include <wlr/util/edges.h>
#undef WLR_USE_UNSTABLE

/* == Declarations ========================================================= */

/** A dock-app placed in the dock. */
typedef struct {
    /** Node within @ref wlmaker_dockapps_t::placed. */
    bs_dllist_node_t          dlnode;
    /** The dock-app's tile. */
    wlmtk_tile_t              *tile_ptr;
    /** Name of the dock-app. May be NULL. */
    char                      *name_ptr;
} wlmaker_dockapp_t;

/** State of the dock-app placement. */
struct _wlmaker_dockapps_t {
    /** The dock holding the dock-apps. */
    wlmtk_dock_t              *dock_ptr;
    /** Desired order of names, from the anchor. */
    char                      **names_ptr;
    /** Number of names in @ref wlmaker_dockapps_t::names_ptr. */
    size_t                    names_len;
    /** Placed dock-apps, in order from the anchor. */
    bs_dllist_t               placed;
    /** File for loading and storing the order. May be NULL. */
    char                      *state_fname_ptr;
    /** Key of the order in the state file. */
    char                      *order_key_ptr;
    /**
     * For each of @ref wlmaker_dockapps_t::names_ptr: The number of the
     * dock's launchers before it (dock-apps and launchers share the
     * column), or @ref WLMAKER_DOCKAPPS_AFTER_ALL.
     */
    size_t                    *after_ptr;
    /**
     * Cell of each dock-app on the Clip's grid, by name: Strings "lane,slot".
     * Only for the placement whose cells are stored (the Clip's).
     */
    bspl_dict_t               *cells_dict_ptr;
    /** Where the tiles go, instead of @ref wlmaker_dockapps_t::dock_ptr. */
    wlmaker_dockapps_host_t   host;
    /** Whether @ref wlmaker_dockapps_t::host is set. */
    bool                      has_host;
    /** Called when the placement changed. May be NULL. */
    void                      (*changed)(void *ud_ptr);
    /** Argument to @ref wlmaker_dockapps_t::changed. */
    void                      *changed_ud_ptr;
};

static bool _wlmaker_dockapps_set_names_from_array(
    wlmaker_dockapps_t *dockapps_ptr,
    bspl_array_t *array_ptr);
static void _wlmaker_dockapps_clear_names(wlmaker_dockapps_t *dockapps_ptr);
static size_t _wlmaker_dockapps_rank(
    wlmaker_dockapps_t *dockapps_ptr,
    const char *name_ptr);
static wlmaker_dockapp_t *_wlmaker_dockapps_find(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr,
    size_t *index_ptr);
static void _wlmaker_dockapps_update_names(wlmaker_dockapps_t *dockapps_ptr);
static bool _wlmaker_dockapps_save(wlmaker_dockapps_t *dockapps_ptr);
static bspl_dict_t *_wlmaker_dockapps_load_file(const char *fname_ptr);
static bool _wlmaker_dockapps_store_key(
    const char *fname_ptr,
    const char *key_ptr,
    bspl_object_t *object_ptr);
static void _wlmaker_dockapps_load_after(
    wlmaker_dockapps_t *dockapps_ptr,
    bspl_dict_t *dict_ptr);
static size_t _wlmaker_dockapps_after_of(
    wlmaker_dockapps_t *dockapps_ptr,
    const char *name_ptr);
static void _wlmaker_dockapps_changed(wlmaker_dockapps_t *dockapps_ptr);
static void _wlmaker_dockapps_attach_tile(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr,
    size_t position);
static void _wlmaker_dockapps_detach_tile(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr);
static bool _wlmaker_dockapps_store_cells(wlmaker_dockapps_t *dockapps_ptr);
static void _wlmaker_dockapps_store(wlmaker_dockapps_t *dockapps_ptr);

/** Arguments for @ref _wlmaker_dockapps_cells_copy_entry. */
typedef struct {
    /** Dict to copy into. */
    bspl_dict_t               *dest_dict_ptr;
    /** Cells being set: Their old values are not copied. */
    const wlmaker_dockapps_cell_update_t *updates_ptr;
    /** Number of updates. */
    size_t                    n;
} _wlmaker_dockapps_cells_copy_arg_t;

static bool _wlmaker_dockapps_cells_copy_entry(
    const char *key_ptr,
    bspl_object_t *object_ptr,
    void *userdata_ptr);

/** Key of the array holding the order, in the state file. */
static const char *_wlmaker_dockapps_order_key = "Order";
/**
 * Key of the dict holding, for each placement's key, a dict of the number
 * of launchers before each dock-app (by name).
 */
static const char *_wlmaker_dockapps_after_key = "AfterLaunchers";
/**
 * Key of the dict holding, for each placement's key, a dict of the cell of
 * each dock-app on the Clip's grid (by name).
 */
static const char *_wlmaker_dockapps_cells_key = "Cells";
/** Key of the array of dock-apps removed by the user. */
static const char *_wlmaker_dockapps_removed_key = "Removed";

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_dockapps_t *wlmaker_dockapps_create(
    wlmtk_dock_t *dock_ptr,
    bspl_array_t *order_array_ptr,
    const char *state_fname_ptr)
{
    return wlmaker_dockapps_create_with_key(
        dock_ptr, order_array_ptr, state_fname_ptr,
        _wlmaker_dockapps_order_key);
}

/* ------------------------------------------------------------------------- */
wlmaker_dockapps_t *wlmaker_dockapps_create_with_key(
    wlmtk_dock_t *dock_ptr,
    bspl_array_t *order_array_ptr,
    const char *state_fname_ptr,
    const char *key_ptr)
{
    wlmaker_dockapps_t *dockapps_ptr = logged_calloc(
        1, sizeof(wlmaker_dockapps_t));
    if (NULL == dockapps_ptr) return NULL;
    dockapps_ptr->dock_ptr = dock_ptr;
    dockapps_ptr->order_key_ptr = logged_strdup(key_ptr);
    if (NULL == dockapps_ptr->order_key_ptr) {
        wlmaker_dockapps_destroy(dockapps_ptr);
        return NULL;
    }

    bool loaded = false;
    if (NULL != state_fname_ptr) {
        dockapps_ptr->state_fname_ptr = logged_strdup(state_fname_ptr);
        if (NULL == dockapps_ptr->state_fname_ptr) {
            wlmaker_dockapps_destroy(dockapps_ptr);
            return NULL;
        }

        if (bs_file_realpath_is(state_fname_ptr, S_IFREG)) {
            bspl_object_t *o = bspl_create_object_from_plist_file(
                state_fname_ptr);
            bspl_array_t *a = bspl_dict_get_array(
                bspl_dict_from_object(o), dockapps_ptr->order_key_ptr);
            if (NULL != a) {
                loaded = _wlmaker_dockapps_set_names_from_array(
                    dockapps_ptr, a);
                if (loaded) {
                    _wlmaker_dockapps_load_after(
                        dockapps_ptr, bspl_dict_from_object(o));
                }
                bspl_dict_t *c = bspl_dict_get_dict(
                    bspl_dict_get_dict(bspl_dict_from_object(o),
                                       _wlmaker_dockapps_cells_key),
                    dockapps_ptr->order_key_ptr);
                if (NULL != c) {
                    dockapps_ptr->cells_dict_ptr = bspl_dict_ref(c);
                }
            } else {
                bs_log(BS_INFO, "No '%s' array in dock-apps state \"%s\"",
                       dockapps_ptr->order_key_ptr, state_fname_ptr);
            }
            if (NULL != o) bspl_object_unref(o);
        }
    }

    if (!loaded && NULL != order_array_ptr &&
        !_wlmaker_dockapps_set_names_from_array(
            dockapps_ptr, order_array_ptr)) {
        wlmaker_dockapps_destroy(dockapps_ptr);
        return NULL;
    }
    return dockapps_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_dockapps_destroy(wlmaker_dockapps_t *dockapps_ptr)
{
    bs_dllist_node_t *dlnode_ptr;
    while (NULL != (dlnode_ptr = bs_dllist_pop_front(&dockapps_ptr->placed))) {
        wlmaker_dockapp_t *dockapp_ptr = BS_CONTAINER_OF(
            dlnode_ptr, wlmaker_dockapp_t, dlnode);
        if (NULL != dockapp_ptr->name_ptr) free(dockapp_ptr->name_ptr);
        free(dockapp_ptr);
    }
    _wlmaker_dockapps_clear_names(dockapps_ptr);
    if (NULL != dockapps_ptr->cells_dict_ptr) {
        bspl_dict_unref(dockapps_ptr->cells_dict_ptr);
        dockapps_ptr->cells_dict_ptr = NULL;
    }
    if (NULL != dockapps_ptr->state_fname_ptr) {
        free(dockapps_ptr->state_fname_ptr);
        dockapps_ptr->state_fname_ptr = NULL;
    }
    if (NULL != dockapps_ptr->order_key_ptr) {
        free(dockapps_ptr->order_key_ptr);
        dockapps_ptr->order_key_ptr = NULL;
    }
    free(dockapps_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dockapps_add(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr,
    const char *name_ptr)
{
    wlmaker_dockapp_t *dockapp_ptr = logged_calloc(
        1, sizeof(wlmaker_dockapp_t));
    if (NULL == dockapp_ptr) return false;
    dockapp_ptr->tile_ptr = tile_ptr;
    if (NULL != name_ptr) {
        dockapp_ptr->name_ptr = logged_strdup(name_ptr);
        if (NULL == dockapp_ptr->name_ptr) {
            free(dockapp_ptr);
            return false;
        }
    }

    // Tiles before the dock-apps (eg. the Clip itself) stay where they are.
    size_t base = wlmtk_dock_tiles(dockapps_ptr->dock_ptr) -
        bs_dllist_size(&dockapps_ptr->placed);

    // Insert before the first placed dock-app with a higher rank.
    size_t rank = _wlmaker_dockapps_rank(dockapps_ptr, name_ptr);
    size_t index = 0;
    bs_dllist_node_t *dlnode_ptr = dockapps_ptr->placed.head_ptr;
    for (; NULL != dlnode_ptr; dlnode_ptr = dlnode_ptr->next_ptr, ++index) {
        wlmaker_dockapp_t *d_ptr = BS_CONTAINER_OF(
            dlnode_ptr, wlmaker_dockapp_t, dlnode);
        if (_wlmaker_dockapps_rank(dockapps_ptr, d_ptr->name_ptr) > rank) break;
    }
    if (NULL == dlnode_ptr) {
        bs_dllist_push_back(&dockapps_ptr->placed, &dockapp_ptr->dlnode);
    } else {
        bs_dllist_insert_node_before(
            &dockapps_ptr->placed, dlnode_ptr, &dockapp_ptr->dlnode);
    }

    _wlmaker_dockapps_attach_tile(dockapps_ptr, tile_ptr, base + index);
    _wlmaker_dockapps_changed(dockapps_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
void wlmaker_dockapps_remove(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr)
{
    wlmaker_dockapp_t *dockapp_ptr = _wlmaker_dockapps_find(
        dockapps_ptr, tile_ptr, NULL);
    if (NULL == dockapp_ptr) return;

    _wlmaker_dockapps_detach_tile(dockapps_ptr, tile_ptr);
    bs_dllist_remove(&dockapps_ptr->placed, &dockapp_ptr->dlnode);
    if (NULL != dockapp_ptr->name_ptr) free(dockapp_ptr->name_ptr);
    free(dockapp_ptr);
    _wlmaker_dockapps_changed(dockapps_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_dockapps_move(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr,
    int delta)
{
    size_t index;
    wlmaker_dockapp_t *dockapp_ptr = _wlmaker_dockapps_find(
        dockapps_ptr, tile_ptr, &index);
    if (NULL == dockapp_ptr || 0 == delta) return;

    size_t n = bs_dllist_size(&dockapps_ptr->placed);
    int new_index = BS_MAX(0, BS_MIN((int)n - 1, (int)index + delta));
    if ((size_t)new_index == index) return;

    bs_dllist_remove(&dockapps_ptr->placed, &dockapp_ptr->dlnode);
    bs_dllist_node_t *dlnode_ptr = dockapps_ptr->placed.head_ptr;
    for (int i = 0; i < new_index && NULL != dlnode_ptr; ++i) {
        dlnode_ptr = dlnode_ptr->next_ptr;
    }
    if (NULL == dlnode_ptr) {
        bs_dllist_push_back(&dockapps_ptr->placed, &dockapp_ptr->dlnode);
    } else {
        bs_dllist_insert_node_before(
            &dockapps_ptr->placed, dlnode_ptr, &dockapp_ptr->dlnode);
    }

    if (!dockapps_ptr->has_host) {
        size_t base = wlmtk_dock_tiles(dockapps_ptr->dock_ptr) - n;
        wlmtk_dock_move_tile(dockapps_ptr->dock_ptr, tile_ptr,
                             base + new_index);
    }

    _wlmaker_dockapps_update_names(dockapps_ptr);
    // Without any named dock-app, there is no order worth storing.
    if (NULL != dockapps_ptr->state_fname_ptr &&
        0 < dockapps_ptr->names_len &&
        !_wlmaker_dockapps_save(dockapps_ptr)) {
        bs_log(BS_WARNING, "Failed to store dock-apps order in \"%s\"",
               dockapps_ptr->state_fname_ptr);
    }
    _wlmaker_dockapps_changed(dockapps_ptr);
}

/* ------------------------------------------------------------------------- */
int wlmaker_dockapps_delta_towards(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr,
    uint32_t direction)
{
    size_t index;
    if (NULL == _wlmaker_dockapps_find(dockapps_ptr, tile_ptr, &index)) {
        return 0;
    }
    const wlmtk_dock_positioning_t *pos_ptr = wlmtk_dock_get_positioning(
        dockapps_ptr->dock_ptr);
    bool vertical = (WLR_EDGE_LEFT == pos_ptr->edge ||
                     WLR_EDGE_RIGHT == pos_ptr->edge);

    int delta = 0;
    if (vertical && WLR_EDGE_TOP == direction) delta = -1;
    if (vertical && WLR_EDGE_BOTTOM == direction) delta = 1;
    if (!vertical && WLR_EDGE_LEFT == direction) delta = -1;
    if (!vertical && WLR_EDGE_RIGHT == direction) delta = 1;
    // Positions count away from the anchor.
    if (WLR_EDGE_BOTTOM == pos_ptr->anchor ||
        WLR_EDGE_RIGHT == pos_ptr->anchor) delta = -delta;

    int new_index = (int)index + delta;
    if (0 > new_index ||
        (int)bs_dllist_size(&dockapps_ptr->placed) <= new_index) return 0;
    return delta;
}

/* ------------------------------------------------------------------------- */
int wlmaker_dockapps_drag_delta(
    wlmaker_dockapps_t *dockapps_ptr,
    double dx,
    double dy,
    int tile_pitch)
{
    if (0 >= tile_pitch) return 0;
    const wlmtk_dock_positioning_t *pos_ptr = wlmtk_dock_get_positioning(
        dockapps_ptr->dock_ptr);

    // Tiles are stacked vertically on the left & right edges.
    double d = (WLR_EDGE_LEFT == pos_ptr->edge ||
                WLR_EDGE_RIGHT == pos_ptr->edge) ? dy : dx;
    int slots = (0 <= d) ?
        (int)((d + tile_pitch / 2.0) / tile_pitch) :
        -(int)((-d + tile_pitch / 2.0) / tile_pitch);

    // Positions count away from the anchor.
    if (WLR_EDGE_BOTTOM == pos_ptr->anchor ||
        WLR_EDGE_RIGHT == pos_ptr->anchor) slots = -slots;
    return slots;
}

/* ------------------------------------------------------------------------- */
wlmtk_dock_t *wlmaker_dockapps_dock(wlmaker_dockapps_t *dockapps_ptr)
{
    return dockapps_ptr->dock_ptr;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dockapps_has_name(
    wlmaker_dockapps_t *dockapps_ptr,
    const char *name_ptr)
{
    return NULL != name_ptr &&
        _wlmaker_dockapps_rank(dockapps_ptr, name_ptr) <
        dockapps_ptr->names_len;
}

/* ------------------------------------------------------------------------- */
size_t wlmaker_dockapps_size(wlmaker_dockapps_t *dockapps_ptr)
{
    return bs_dllist_size(&dockapps_ptr->placed);
}

/* ------------------------------------------------------------------------- */
int wlmaker_dockapps_index(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr)
{
    size_t index;
    if (NULL == _wlmaker_dockapps_find(dockapps_ptr, tile_ptr, &index)) {
        return -1;
    }
    return index;
}

/* ------------------------------------------------------------------------- */
wlmtk_tile_t *wlmaker_dockapps_tile_at(
    wlmaker_dockapps_t *dockapps_ptr,
    size_t index)
{
    for (bs_dllist_node_t *dlnode_ptr = dockapps_ptr->placed.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr, --index) {
        if (0 == index) {
            return BS_CONTAINER_OF(dlnode_ptr, wlmaker_dockapp_t, dlnode)->tile_ptr;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
const char *wlmaker_dockapps_name(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr)
{
    wlmaker_dockapp_t *d_ptr = _wlmaker_dockapps_find(
        dockapps_ptr, tile_ptr, NULL);
    return NULL != d_ptr ? d_ptr->name_ptr : NULL;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dockapps_insert(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr,
    const char *name_ptr,
    size_t index)
{
    wlmaker_dockapp_t *dockapp_ptr = logged_calloc(
        1, sizeof(wlmaker_dockapp_t));
    if (NULL == dockapp_ptr) return false;
    dockapp_ptr->tile_ptr = tile_ptr;
    if (NULL != name_ptr) {
        dockapp_ptr->name_ptr = logged_strdup(name_ptr);
        if (NULL == dockapp_ptr->name_ptr) {
            free(dockapp_ptr);
            return false;
        }
    }

    size_t n = bs_dllist_size(&dockapps_ptr->placed);
    size_t base = wlmtk_dock_tiles(dockapps_ptr->dock_ptr) - n;
    if (index > n) index = n;
    bs_dllist_node_t *dlnode_ptr = dockapps_ptr->placed.head_ptr;
    for (size_t i = 0; i < index && NULL != dlnode_ptr; ++i) {
        dlnode_ptr = dlnode_ptr->next_ptr;
    }
    if (NULL == dlnode_ptr) {
        bs_dllist_push_back(&dockapps_ptr->placed, &dockapp_ptr->dlnode);
    } else {
        bs_dllist_insert_node_before(
            &dockapps_ptr->placed, dlnode_ptr, &dockapp_ptr->dlnode);
    }
    _wlmaker_dockapps_attach_tile(dockapps_ptr, tile_ptr, base + index);

    _wlmaker_dockapps_update_names(dockapps_ptr);
    if (NULL != dockapps_ptr->state_fname_ptr && NULL != name_ptr &&
        !_wlmaker_dockapps_save(dockapps_ptr)) {
        bs_log(BS_WARNING, "Failed to store dock-apps order in \"%s\"",
               dockapps_ptr->state_fname_ptr);
    }
    _wlmaker_dockapps_changed(dockapps_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
void wlmaker_dockapps_detach(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr)
{
    wlmaker_dockapp_t *dockapp_ptr = _wlmaker_dockapps_find(
        dockapps_ptr, tile_ptr, NULL);
    if (NULL == dockapp_ptr) return;
    // The order of those staying, as placed.
    _wlmaker_dockapps_update_names(dockapps_ptr);
    char *name_ptr = dockapp_ptr->name_ptr;
    dockapp_ptr->name_ptr = NULL;
    wlmaker_dockapps_remove(dockapps_ptr, tile_ptr);
    if (NULL == name_ptr) return;

    // Forget the name: Keep the others in their order.
    size_t j = 0;
    for (size_t i = 0; i < dockapps_ptr->names_len; ++i) {
        if (0 == strcmp(dockapps_ptr->names_ptr[i], name_ptr)) {
            free(dockapps_ptr->names_ptr[i]);
            continue;
        }
        dockapps_ptr->after_ptr[j] = dockapps_ptr->after_ptr[i];
        dockapps_ptr->names_ptr[j++] = dockapps_ptr->names_ptr[i];
    }
    dockapps_ptr->names_len = j;
    // And its cell: Attached again, it gets a new one.
    if (NULL != dockapps_ptr->cells_dict_ptr &&
        NULL != bspl_dict_get(dockapps_ptr->cells_dict_ptr, name_ptr)) {
        wlmaker_dockapps_cell_update_t u = { .name_ptr = name_ptr };
        wlmaker_dockapps_set_cells(dockapps_ptr, &u, 1);
    }
    free(name_ptr);
    if (NULL != dockapps_ptr->state_fname_ptr &&
        !_wlmaker_dockapps_save(dockapps_ptr)) {
        bs_log(BS_WARNING, "Failed to store dock-apps order in \"%s\"",
               dockapps_ptr->state_fname_ptr);
    }
}

/* ------------------------------------------------------------------------- */
void wlmaker_dockapps_move_to(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr,
    size_t index)
{
    int current = wlmaker_dockapps_index(dockapps_ptr, tile_ptr);
    if (0 > current) return;
    wlmaker_dockapps_move(dockapps_ptr, tile_ptr, (int)index - current);
}

/* ------------------------------------------------------------------------- */
void wlmaker_dockapps_set_host(
    wlmaker_dockapps_t *dockapps_ptr,
    const wlmaker_dockapps_host_t *host_ptr)
{
    dockapps_ptr->has_host = NULL != host_ptr;
    if (NULL != host_ptr) dockapps_ptr->host = *host_ptr;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dockapps_cell(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr,
    wlmaker_clip_cell_t *cell_ptr)
{
    const char *name_ptr = wlmaker_dockapps_name(dockapps_ptr, tile_ptr);
    if (NULL == name_ptr || NULL == dockapps_ptr->cells_dict_ptr) return false;
    return wlmaker_clip_cell_parse(
        bspl_dict_get_string_value(dockapps_ptr->cells_dict_ptr, name_ptr),
        cell_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dockapps_set_cells(
    wlmaker_dockapps_t *dockapps_ptr,
    const wlmaker_dockapps_cell_update_t *updates_ptr,
    size_t n)
{
    bspl_dict_t *dict_ptr = bspl_dict_create();
    if (NULL == dict_ptr) return false;
    bool rv = true, changed = false;
    // Keep the others' cells (also of those not running now).
    if (NULL != dockapps_ptr->cells_dict_ptr) {
        _wlmaker_dockapps_cells_copy_arg_t arg = {
            .dest_dict_ptr = dict_ptr, .updates_ptr = updates_ptr, .n = n };
        rv = bspl_dict_foreach(dockapps_ptr->cells_dict_ptr,
                               _wlmaker_dockapps_cells_copy_entry, &arg);
    }
    for (size_t i = 0; rv && i < n; ++i) {
        const wlmaker_dockapps_cell_update_t *u_ptr = &updates_ptr[i];
        if (NULL == u_ptr->name_ptr) continue;
        const char *old_ptr = NULL == dockapps_ptr->cells_dict_ptr ? NULL :
            bspl_dict_get_string_value(dockapps_ptr->cells_dict_ptr,
                                       u_ptr->name_ptr);
        if (!u_ptr->has_cell) {
            changed |= NULL != old_ptr;
            continue;
        }
        char buf[32];
        wlmaker_clip_cell_format(&u_ptr->cell, buf, sizeof(buf));
        changed |= NULL == old_ptr || 0 != strcmp(old_ptr, buf);
        bspl_string_t *s = bspl_string_create(buf);
        // A name given twice: The first counts.
        rv = NULL != s &&
            (NULL != bspl_dict_get(dict_ptr, u_ptr->name_ptr) ||
             bspl_dict_add(dict_ptr, u_ptr->name_ptr,
                           bspl_object_from_string(s)));
        if (NULL != s) bspl_string_unref(s);
    }
    if (!rv) {
        bspl_dict_unref(dict_ptr);
        return false;
    }
    if (!changed) {
        bspl_dict_unref(dict_ptr);
        return true;
    }
    if (NULL != dockapps_ptr->cells_dict_ptr) {
        bspl_dict_unref(dockapps_ptr->cells_dict_ptr);
    }
    dockapps_ptr->cells_dict_ptr = dict_ptr;
    if (NULL != dockapps_ptr->state_fname_ptr &&
        !_wlmaker_dockapps_store_cells(dockapps_ptr)) {
        bs_log(BS_WARNING, "Failed to store the dock-apps' cells in \"%s\"",
               dockapps_ptr->state_fname_ptr);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
void wlmaker_dockapps_set_changed_callback(
    wlmaker_dockapps_t *dockapps_ptr,
    void (*changed)(void *ud_ptr),
    void *ud_ptr)
{
    dockapps_ptr->changed = changed;
    dockapps_ptr->changed_ud_ptr = ud_ptr;
}

/* ------------------------------------------------------------------------- */
size_t wlmaker_dockapps_after_launchers(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr)
{
    wlmaker_dockapp_t *d_ptr = _wlmaker_dockapps_find(
        dockapps_ptr, tile_ptr, NULL);
    if (NULL == d_ptr) return WLMAKER_DOCKAPPS_AFTER_ALL;
    return _wlmaker_dockapps_after_of(dockapps_ptr, d_ptr->name_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_dockapps_set_after_launchers(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr,
    size_t launchers)
{
    wlmaker_dockapp_t *d_ptr = _wlmaker_dockapps_find(
        dockapps_ptr, tile_ptr, NULL);
    if (NULL == d_ptr || NULL == d_ptr->name_ptr) return;
    // Listed: Its name is in the order.
    _wlmaker_dockapps_update_names(dockapps_ptr);
    size_t rank = _wlmaker_dockapps_rank(dockapps_ptr, d_ptr->name_ptr);
    if (rank >= dockapps_ptr->names_len) return;
    if (dockapps_ptr->after_ptr[rank] == launchers) return;
    dockapps_ptr->after_ptr[rank] = launchers;
    _wlmaker_dockapps_store(dockapps_ptr);
    _wlmaker_dockapps_changed(dockapps_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_dockapps_launcher_inserted(
    wlmaker_dockapps_t *dockapps_ptr,
    size_t index)
{
    bool changed = false;
    for (size_t i = 0; i < dockapps_ptr->names_len; ++i) {
        size_t *a_ptr = &dockapps_ptr->after_ptr[i];
        if (WLMAKER_DOCKAPPS_AFTER_ALL == *a_ptr || *a_ptr <= index) continue;
        ++*a_ptr;
        changed = true;
    }
    if (!changed) return;
    _wlmaker_dockapps_store(dockapps_ptr);
    _wlmaker_dockapps_changed(dockapps_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_dockapps_launcher_removed(
    wlmaker_dockapps_t *dockapps_ptr,
    size_t index)
{
    bool changed = false;
    for (size_t i = 0; i < dockapps_ptr->names_len; ++i) {
        size_t *a_ptr = &dockapps_ptr->after_ptr[i];
        if (WLMAKER_DOCKAPPS_AFTER_ALL == *a_ptr || *a_ptr <= index) continue;
        --*a_ptr;
        changed = true;
    }
    if (!changed) return;
    _wlmaker_dockapps_store(dockapps_ptr);
    _wlmaker_dockapps_changed(dockapps_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_dockapps_freeze_after(
    wlmaker_dockapps_t *dockapps_ptr,
    size_t launchers)
{
    _wlmaker_dockapps_update_names(dockapps_ptr);
    bool changed = false;
    size_t after = 0;
    for (bs_dllist_node_t *dlnode_ptr = dockapps_ptr->placed.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr) {
        wlmaker_dockapp_t *d_ptr = BS_CONTAINER_OF(
            dlnode_ptr, wlmaker_dockapp_t, dlnode);
        // As in wlmaker_dockapps_positions().
        size_t rank = _wlmaker_dockapps_rank(dockapps_ptr, d_ptr->name_ptr);
        size_t a = _wlmaker_dockapps_after_of(dockapps_ptr, d_ptr->name_ptr);
        after = BS_MAX(after, BS_MIN(a, launchers));
        if (rank >= dockapps_ptr->names_len ||
            dockapps_ptr->after_ptr[rank] == after) continue;
        dockapps_ptr->after_ptr[rank] = after;
        changed = true;
    }
    if (changed) _wlmaker_dockapps_store(dockapps_ptr);
}

/* ------------------------------------------------------------------------- */
uint64_t wlmaker_dockapps_positions(
    wlmaker_dockapps_t *dockapps_ptr,
    size_t launchers)
{
    uint64_t positions = 0;
    size_t after = 0, j = 0;
    for (bs_dllist_node_t *dlnode_ptr = dockapps_ptr->placed.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr, ++j) {
        wlmaker_dockapp_t *d_ptr = BS_CONTAINER_OF(
            dlnode_ptr, wlmaker_dockapp_t, dlnode);
        // Not before the launchers of the one before; not past the last.
        size_t a = _wlmaker_dockapps_after_of(dockapps_ptr, d_ptr->name_ptr);
        a = BS_MIN(a, launchers);
        after = BS_MAX(after, a);
        size_t p = after + j;
        if (64 > p) positions |= UINT64_C(1) << p;
    }
    return positions;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dockapps_set_removed(
    const char *fname_ptr,
    const char *name_ptr,
    bool removed)
{
    if (NULL == fname_ptr || NULL == name_ptr) return false;
    bspl_dict_t *dict_ptr = _wlmaker_dockapps_load_file(fname_ptr);
    bspl_array_t *old_ptr = bspl_dict_get_array(
        dict_ptr, _wlmaker_dockapps_removed_key);
    bspl_array_t *array_ptr = bspl_array_create();
    bool rv = NULL != array_ptr;
    bool listed = false;
    for (size_t i = 0;
         rv && NULL != old_ptr && i < bspl_array_size(old_ptr); ++i) {
        const char *s = bspl_array_string_value_at(old_ptr, i);
        if (NULL == s) continue;
        if (0 == strcmp(s, name_ptr)) {
            listed = true;
            if (!removed) continue;
        }
        rv = bspl_array_push_back(array_ptr, bspl_array_at(old_ptr, i));
    }
    if (rv && removed && !listed) {
        bspl_string_t *s = bspl_string_create(name_ptr);
        rv = NULL != s &&
            bspl_array_push_back(array_ptr, bspl_object_from_string(s));
        if (NULL != s) bspl_string_unref(s);
    }
    if (NULL != dict_ptr) bspl_dict_unref(dict_ptr);
    if (rv && listed != removed) {
        rv = _wlmaker_dockapps_store_key(
            fname_ptr, _wlmaker_dockapps_removed_key,
            bspl_object_from_array(array_ptr));
    }
    if (NULL != array_ptr) bspl_array_unref(array_ptr);
    return rv;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dockapps_is_removed(
    const char *fname_ptr,
    const char *name_ptr)
{
    if (NULL == fname_ptr || NULL == name_ptr) return false;
    bspl_dict_t *dict_ptr = _wlmaker_dockapps_load_file(fname_ptr);
    bspl_array_t *array_ptr = bspl_dict_get_array(
        dict_ptr, _wlmaker_dockapps_removed_key);
    bool removed = false;
    for (size_t i = 0;
         NULL != array_ptr && i < bspl_array_size(array_ptr); ++i) {
        const char *s = bspl_array_string_value_at(array_ptr, i);
        if (NULL != s && 0 == strcmp(s, name_ptr)) removed = true;
    }
    if (NULL != dict_ptr) bspl_dict_unref(dict_ptr);
    return removed;
}

/* ------------------------------------------------------------------------- */
char *wlmaker_dockapps_client_name(struct wl_client *wl_client_ptr)
{
    pid_t pid;
    wl_client_get_credentials(wl_client_ptr, &pid, NULL, NULL);

    char path[64];
    snprintf(path, sizeof(path), "/proc/%"PRIdMAX"/comm", (intmax_t)pid);
    FILE *f = fopen(path, "r");
    if (NULL == f) return NULL;
    char name[64] = {};
    char *rv = NULL;
    if (NULL != fgets(name, sizeof(name), f)) {
        name[strcspn(name, "\n")] = '\0';
        if ('\0' != name[0]) rv = logged_strdup(name);
    }
    fclose(f);
    return rv;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * Reads the number of launchers before each listed dock-app from the state
 * file's dict: `AfterLaunchers = { <key> = { <name> = <number>; }; }`.
 */
void _wlmaker_dockapps_load_after(
    wlmaker_dockapps_t *dockapps_ptr,
    bspl_dict_t *dict_ptr)
{
    bspl_dict_t *d = bspl_dict_get_dict(
        bspl_dict_get_dict(dict_ptr, _wlmaker_dockapps_after_key),
        dockapps_ptr->order_key_ptr);
    for (size_t i = 0; NULL != d && i < dockapps_ptr->names_len; ++i) {
        const char *v = bspl_dict_get_string_value(
            d, dockapps_ptr->names_ptr[i]);
        uint64_t value;
        if (NULL != v && bs_strconvert_uint64(v, &value, 10) &&
            value < WLMAKER_DOCKAPPS_AFTER_ALL) {
            dockapps_ptr->after_ptr[i] = value;
        }
    }
}

/* ------------------------------------------------------------------------- */
/** @return The launchers before the listed dock-app `name_ptr`. */
size_t _wlmaker_dockapps_after_of(
    wlmaker_dockapps_t *dockapps_ptr,
    const char *name_ptr)
{
    size_t rank = _wlmaker_dockapps_rank(dockapps_ptr, name_ptr);
    if (rank >= dockapps_ptr->names_len) return WLMAKER_DOCKAPPS_AFTER_ALL;
    return dockapps_ptr->after_ptr[rank];
}

/* ------------------------------------------------------------------------- */
/** Tells the owner of the dock (eg. the Clip) that the placement changed. */
void _wlmaker_dockapps_changed(wlmaker_dockapps_t *dockapps_ptr)
{
    if (NULL != dockapps_ptr->changed) {
        dockapps_ptr->changed(dockapps_ptr->changed_ud_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Stores the order, if there is a state file. Logs failures. */
void _wlmaker_dockapps_store(wlmaker_dockapps_t *dockapps_ptr)
{
    if (NULL == dockapps_ptr->state_fname_ptr ||
        0 == dockapps_ptr->names_len) return;
    if (!_wlmaker_dockapps_save(dockapps_ptr)) {
        bs_log(BS_WARNING, "Failed to store dock-apps order in \"%s\"",
               dockapps_ptr->state_fname_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Sets @ref wlmaker_dockapps_t::names_ptr from an array of strings. */
bool _wlmaker_dockapps_set_names_from_array(
    wlmaker_dockapps_t *dockapps_ptr,
    bspl_array_t *array_ptr)
{
    _wlmaker_dockapps_clear_names(dockapps_ptr);
    size_t n = bspl_array_size(array_ptr);
    if (0 == n) return true;
    dockapps_ptr->names_ptr = logged_calloc(n, sizeof(char*));
    if (NULL == dockapps_ptr->names_ptr) return false;
    dockapps_ptr->after_ptr = logged_calloc(n, sizeof(size_t));
    if (NULL == dockapps_ptr->after_ptr) return false;
    for (size_t i = 0; i < n; ++i) {
        const char *s = bspl_array_string_value_at(array_ptr, i);
        if (NULL == s) {
            bs_log(BS_WARNING, "Dock-apps order: Element %zu not a string", i);
            continue;
        }
        char *d = logged_strdup(s);
        if (NULL == d) return false;
        dockapps_ptr->after_ptr[dockapps_ptr->names_len] =
            WLMAKER_DOCKAPPS_AFTER_ALL;
        dockapps_ptr->names_ptr[dockapps_ptr->names_len++] = d;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/** Clears @ref wlmaker_dockapps_t::names_ptr. */
void _wlmaker_dockapps_clear_names(wlmaker_dockapps_t *dockapps_ptr)
{
    for (size_t i = 0; i < dockapps_ptr->names_len; ++i) {
        free(dockapps_ptr->names_ptr[i]);
    }
    if (NULL != dockapps_ptr->names_ptr) free(dockapps_ptr->names_ptr);
    dockapps_ptr->names_ptr = NULL;
    if (NULL != dockapps_ptr->after_ptr) free(dockapps_ptr->after_ptr);
    dockapps_ptr->after_ptr = NULL;
    dockapps_ptr->names_len = 0;
}

/* ------------------------------------------------------------------------- */
/**
 * @return The rank of `name_ptr` in the order: Its position, or past all
 *     listed names if it is not listed.
 */
size_t _wlmaker_dockapps_rank(
    wlmaker_dockapps_t *dockapps_ptr,
    const char *name_ptr)
{
    if (NULL == name_ptr) return dockapps_ptr->names_len;
    for (size_t i = 0; i < dockapps_ptr->names_len; ++i) {
        if (0 == strcmp(dockapps_ptr->names_ptr[i], name_ptr)) return i;
    }
    return dockapps_ptr->names_len;
}

/* ------------------------------------------------------------------------- */
/** Finds the dock-app holding `tile_ptr`, and its index from the anchor. */
wlmaker_dockapp_t *_wlmaker_dockapps_find(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr,
    size_t *index_ptr)
{
    size_t index = 0;
    for (bs_dllist_node_t *dlnode_ptr = dockapps_ptr->placed.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr, ++index) {
        wlmaker_dockapp_t *d_ptr = BS_CONTAINER_OF(
            dlnode_ptr, wlmaker_dockapp_t, dlnode);
        if (d_ptr->tile_ptr != tile_ptr) continue;
        if (NULL != index_ptr) *index_ptr = index;
        return d_ptr;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/**
 * Updates the order of names from the placed dock-apps: Their names first,
 * then any earlier-listed names of dock-apps that are not running.
 */
void _wlmaker_dockapps_update_names(wlmaker_dockapps_t *dockapps_ptr)
{
    size_t max_len = dockapps_ptr->names_len +
        bs_dllist_size(&dockapps_ptr->placed);
    if (0 == max_len) return;
    char **names_ptr = logged_calloc(max_len, sizeof(char*));
    if (NULL == names_ptr) return;
    size_t *after_ptr = logged_calloc(max_len, sizeof(size_t));
    if (NULL == after_ptr) {
        free(names_ptr);
        return;
    }
    size_t len = 0;

    for (bs_dllist_node_t *dlnode_ptr = dockapps_ptr->placed.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr) {
        wlmaker_dockapp_t *d_ptr = BS_CONTAINER_OF(
            dlnode_ptr, wlmaker_dockapp_t, dlnode);
        if (NULL == d_ptr->name_ptr) continue;
        bool listed = false;
        for (size_t i = 0; i < len; ++i) {
            listed |= (0 == strcmp(names_ptr[i], d_ptr->name_ptr));
        }
        if (listed) continue;
        after_ptr[len] = _wlmaker_dockapps_after_of(
            dockapps_ptr, d_ptr->name_ptr);
        names_ptr[len] = logged_strdup(d_ptr->name_ptr);
        if (NULL != names_ptr[len]) ++len;
    }
    for (size_t j = 0; j < dockapps_ptr->names_len; ++j) {
        bool listed = false;
        for (size_t i = 0; i < len; ++i) {
            listed |= (0 == strcmp(names_ptr[i], dockapps_ptr->names_ptr[j]));
        }
        if (listed) continue;
        after_ptr[len] = dockapps_ptr->after_ptr[j];
        names_ptr[len] = logged_strdup(dockapps_ptr->names_ptr[j]);
        if (NULL != names_ptr[len]) ++len;
    }

    _wlmaker_dockapps_clear_names(dockapps_ptr);
    dockapps_ptr->names_ptr = names_ptr;
    dockapps_ptr->after_ptr = after_ptr;
    dockapps_ptr->names_len = len;
}

/** Arguments for @ref _wlmaker_dockapps_copy_entry. */
typedef struct {
    /** Dict to copy into. */
    bspl_dict_t               *dest_dict_ptr;
    /** The key that gets skipped. */
    const char                *skip_key_ptr;
} _wlmaker_dockapps_copy_arg_t;

/** Copies an entry into a dict, unless it is the skipped key. */
static bool _wlmaker_dockapps_copy_entry(
    const char *key_ptr,
    bspl_object_t *object_ptr,
    void *userdata_ptr)
{
    _wlmaker_dockapps_copy_arg_t *arg_ptr = userdata_ptr;
    if (0 == strcmp(key_ptr, arg_ptr->skip_key_ptr)) return true;
    return bspl_dict_add(arg_ptr->dest_dict_ptr, key_ptr, object_ptr);
}

/* ------------------------------------------------------------------------- */
/** Stores the order into @ref wlmaker_dockapps_t::state_fname_ptr. */
bool _wlmaker_dockapps_save(wlmaker_dockapps_t *dockapps_ptr)
{
    bspl_array_t *array_ptr = bspl_array_create();
    bool rv = NULL != array_ptr;
    for (size_t i = 0; rv && i < dockapps_ptr->names_len; ++i) {
        bspl_string_t *s = bspl_string_create(dockapps_ptr->names_ptr[i]);
        rv = NULL != s &&
            bspl_array_push_back(array_ptr, bspl_object_from_string(s));
        if (NULL != s) bspl_string_unref(s);
    }
    rv = rv && _wlmaker_dockapps_store_key(
        dockapps_ptr->state_fname_ptr, dockapps_ptr->order_key_ptr,
        bspl_object_from_array(array_ptr));
    if (NULL != array_ptr) bspl_array_unref(array_ptr);

    // The launchers before each dock-app: In the dict of all placements,
    // keeping those of the others.
    bspl_dict_t *file_ptr = rv ? _wlmaker_dockapps_load_file(
        dockapps_ptr->state_fname_ptr) : NULL;
    bspl_dict_t *all_ptr = bspl_dict_create();
    bspl_dict_t *mine_ptr = bspl_dict_create();
    rv = rv && NULL != all_ptr && NULL != mine_ptr;
    _wlmaker_dockapps_copy_arg_t arg = {
        .dest_dict_ptr = all_ptr,
        .skip_key_ptr = dockapps_ptr->order_key_ptr };
    bspl_dict_t *old_all_ptr = bspl_dict_get_dict(
        file_ptr, _wlmaker_dockapps_after_key);
    if (rv && NULL != old_all_ptr) {
        rv = bspl_dict_foreach(old_all_ptr, _wlmaker_dockapps_copy_entry, &arg);
    }
    size_t set = 0;
    for (size_t i = 0; rv && i < dockapps_ptr->names_len; ++i) {
        if (WLMAKER_DOCKAPPS_AFTER_ALL == dockapps_ptr->after_ptr[i]) continue;
        char *v_ptr = bs_strdupf("%zu", dockapps_ptr->after_ptr[i]);
        bspl_string_t *v = NULL != v_ptr ? bspl_string_create(v_ptr) : NULL;
        if (NULL != v_ptr) free(v_ptr);
        rv = NULL != v && bspl_dict_add(mine_ptr, dockapps_ptr->names_ptr[i],
                                        bspl_object_from_string(v));
        if (NULL != v) bspl_string_unref(v);
        ++set;
    }
    if (rv && 0 < set) {
        rv = bspl_dict_add(all_ptr, dockapps_ptr->order_key_ptr,
                           bspl_object_from_dict(mine_ptr));
    }
    // Nothing stored before, nothing to store: The file stays as it was.
    if (rv && (0 < set || NULL != old_all_ptr)) {
        rv = _wlmaker_dockapps_store_key(
            dockapps_ptr->state_fname_ptr, _wlmaker_dockapps_after_key,
            bspl_object_from_dict(all_ptr));
    }
    if (NULL != mine_ptr) bspl_dict_unref(mine_ptr);
    if (NULL != all_ptr) bspl_dict_unref(all_ptr);
    if (NULL != file_ptr) bspl_dict_unref(file_ptr);
    return rv;
}

/* ------------------------------------------------------------------------- */
/** @return The dict in the dock-apps state file, or NULL. */
bspl_dict_t *_wlmaker_dockapps_load_file(const char *fname_ptr)
{
    if (!bs_file_realpath_is(fname_ptr, S_IFREG)) return NULL;
    bspl_object_t *o = bspl_create_object_from_plist_file(fname_ptr);
    bspl_dict_t *dict_ptr = bspl_dict_from_object(o);
    if (NULL == dict_ptr && NULL != o) bspl_object_unref(o);
    return dict_ptr;
}

/* ------------------------------------------------------------------------- */
/** Stores `object_ptr` as `key_ptr` of the state file, keeping other keys. */
bool _wlmaker_dockapps_store_key(
    const char *fname_ptr,
    const char *key_ptr,
    bspl_object_t *object_ptr)
{
    bool rv = false;
    bspl_dict_t *old_ptr = _wlmaker_dockapps_load_file(fname_ptr);
    bspl_dict_t *dict_ptr = bspl_dict_create();
    if (NULL == dict_ptr) goto done;
    _wlmaker_dockapps_copy_arg_t arg = {
        .dest_dict_ptr = dict_ptr, .skip_key_ptr = key_ptr };
    if (NULL != old_ptr &&
        !bspl_dict_foreach(old_ptr, _wlmaker_dockapps_copy_entry, &arg)) {
        goto done;
    }
    if (!bspl_dict_add(dict_ptr, key_ptr, object_ptr)) goto done;

    bs_dynbuf_t dynbuf = {};
    if (!bs_dynbuf_init(&dynbuf, 4096, SIZE_MAX)) goto done;
    rv = bspl_object_write(bspl_object_from_dict(dict_ptr), &dynbuf) &&
        bs_dynbuf_write_file(&dynbuf, fname_ptr, S_IRUSR | S_IWUSR);
    bs_dynbuf_fini(&dynbuf);

done:
    if (NULL != old_ptr) bspl_dict_unref(old_ptr);
    if (NULL != dict_ptr) bspl_dict_unref(dict_ptr);
    return rv;
}

/* ------------------------------------------------------------------------- */
/** Copies a cell entry, unless it is one being set. */
bool _wlmaker_dockapps_cells_copy_entry(
    const char *key_ptr,
    bspl_object_t *object_ptr,
    void *userdata_ptr)
{
    _wlmaker_dockapps_cells_copy_arg_t *arg_ptr = userdata_ptr;
    for (size_t i = 0; i < arg_ptr->n; ++i) {
        if (NULL != arg_ptr->updates_ptr[i].name_ptr &&
            0 == strcmp(arg_ptr->updates_ptr[i].name_ptr, key_ptr)) return true;
    }
    return bspl_dict_add(arg_ptr->dest_dict_ptr, key_ptr, object_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Stores the cells as `Cells = { <key> = { <name> = "lane,slot"; }; }` in
 * the state file, keeping the other placements' cells.
 */
bool _wlmaker_dockapps_store_cells(wlmaker_dockapps_t *dockapps_ptr)
{
    bspl_dict_t *file_ptr = _wlmaker_dockapps_load_file(
        dockapps_ptr->state_fname_ptr);
    bspl_dict_t *all_ptr = bspl_dict_create();
    bool rv = NULL != all_ptr;
    _wlmaker_dockapps_copy_arg_t arg = {
        .dest_dict_ptr = all_ptr,
        .skip_key_ptr = dockapps_ptr->order_key_ptr };
    bspl_dict_t *old_all_ptr = bspl_dict_get_dict(
        file_ptr, _wlmaker_dockapps_cells_key);
    if (rv && NULL != old_all_ptr) {
        rv = bspl_dict_foreach(old_all_ptr, _wlmaker_dockapps_copy_entry, &arg);
    }
    rv = rv && (NULL == dockapps_ptr->cells_dict_ptr ||
                bspl_dict_add(all_ptr, dockapps_ptr->order_key_ptr,
                              bspl_object_from_dict(
                                  dockapps_ptr->cells_dict_ptr)));
    rv = rv && _wlmaker_dockapps_store_key(
        dockapps_ptr->state_fname_ptr, _wlmaker_dockapps_cells_key,
        bspl_object_from_dict(all_ptr));
    if (NULL != all_ptr) bspl_dict_unref(all_ptr);
    if (NULL != file_ptr) bspl_dict_unref(file_ptr);
    return rv;
}

/* ------------------------------------------------------------------------- */
/**
 * Puts the tile into the dock at `position`, or hands it to the host. The
 * dock's box sets the position along it only: Across, the tile would keep
 * its position from another dock (eg. a vertical one).
 */
void _wlmaker_dockapps_attach_tile(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr,
    size_t position)
{
    wlmtk_element_set_position(wlmtk_tile_element(tile_ptr), 0, 0);
    if (dockapps_ptr->has_host) {
        dockapps_ptr->host.attach(dockapps_ptr->host.ud_ptr, tile_ptr);
        return;
    }
    wlmtk_dock_add_tile(dockapps_ptr->dock_ptr, tile_ptr);
    wlmtk_dock_move_tile(dockapps_ptr->dock_ptr, tile_ptr, position);
}

/* ------------------------------------------------------------------------- */
/** Takes the tile out of the dock, or has the host do it. */
void _wlmaker_dockapps_detach_tile(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr)
{
    if (dockapps_ptr->has_host) {
        dockapps_ptr->host.detach(dockapps_ptr->host.ud_ptr, tile_ptr);
        return;
    }
    wlmtk_dock_remove_tile(dockapps_ptr->dock_ptr, tile_ptr);
}

/* == Unit tests =========================================================== */

static void test_order(bs_test_t *test_ptr);
static void test_drag_delta(bs_test_t *test_ptr);
static void test_two_docks(bs_test_t *test_ptr);
static void test_after_launchers(bs_test_t *test_ptr);
static void test_cells(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t _wlmaker_dockapps_test_cases[] = {
    { true, "order", test_order },
    { true, "drag_delta", test_drag_delta },
    { true, "two_docks", test_two_docks },
    { true, "after_launchers", test_after_launchers },
    { true, "cells", test_cells },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_dockapps_test_set = BS_TEST_SET(
    true, "dockapps", _wlmaker_dockapps_test_cases);

/** Tile style for the tests. */
static const struct wlmtk_tile_style _test_tile_style = { .size = 64 };

/* ------------------------------------------------------------------------- */
/** Tests placement from a configured order, moving, and the state file. */
void test_order(bs_test_t *test_ptr)
{
    wlmtk_dock_positioning_t pos = {
        .edge = WLR_EDGE_LEFT, .anchor = WLR_EDGE_TOP };
    struct wlmtk_dock_style style = {};
    wlmtk_dock_t *dock_ptr = wlmtk_dock_create(&pos, &style);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dock_ptr);

    // A tile that's not a dock-app, eg. the Clip itself.
    wlmtk_tile_t clip, t[4];
    BS_TEST_VERIFY_TRUE_OR_RETURN(
        test_ptr, wlmtk_tile_init(&clip, &_test_tile_style));
    wlmtk_dock_add_tile(dock_ptr, &clip);
    for (int i = 0; i < 4; ++i) {
        BS_TEST_VERIFY_TRUE_OR_RETURN(
            test_ptr, wlmtk_tile_init(&t[i], &_test_tile_style));
    }

    bspl_object_t *o = bspl_create_object_from_plist_string("(\"b\", \"a\")");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, o);
    const char *fname_ptr = bs_test_temp_path(test_ptr, "Dockapps.plist");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, fname_ptr);
    wlmaker_dockapps_t *d = wlmaker_dockapps_create(
        dock_ptr, bspl_array_from_object(o), fname_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, d);

    // "a" first, then "b" goes before it. Unlisted and nameless after.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_add(d, &t[0], "a"));
    BS_TEST_VERIFY_EQ(test_ptr, 1, wlmtk_dock_tile_index(dock_ptr, &t[0]));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_add(d, &t[1], "b"));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_add(d, &t[2], NULL));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_add(d, &t[3], "c"));
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlmtk_dock_tile_index(dock_ptr, &clip));
    BS_TEST_VERIFY_EQ(test_ptr, 1, wlmtk_dock_tile_index(dock_ptr, &t[1]));
    BS_TEST_VERIFY_EQ(test_ptr, 2, wlmtk_dock_tile_index(dock_ptr, &t[0]));
    BS_TEST_VERIFY_EQ(test_ptr, 3, wlmtk_dock_tile_index(dock_ptr, &t[2]));
    BS_TEST_VERIFY_EQ(test_ptr, 4, wlmtk_dock_tile_index(dock_ptr, &t[3]));

    // The menu's moves: Order is b, a, (none), c from the top.
    BS_TEST_VERIFY_EQ(test_ptr, 0,
                      wlmaker_dockapps_delta_towards(d, &t[1], WLR_EDGE_TOP));
    BS_TEST_VERIFY_EQ(test_ptr, 1, wlmaker_dockapps_delta_towards(
                          d, &t[1], WLR_EDGE_BOTTOM));
    BS_TEST_VERIFY_EQ(test_ptr, -1,
                      wlmaker_dockapps_delta_towards(d, &t[3], WLR_EDGE_TOP));
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlmaker_dockapps_delta_towards(
                          d, &t[3], WLR_EDGE_BOTTOM));
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlmaker_dockapps_delta_towards(
                          d, &t[0], WLR_EDGE_LEFT));
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlmaker_dockapps_delta_towards(
                          d, &clip, WLR_EDGE_BOTTOM));

    // Move "c" towards the anchor, beyond the first dock-app: Stops there.
    wlmaker_dockapps_move(d, &t[3], -10);
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlmtk_dock_tile_index(dock_ptr, &clip));
    BS_TEST_VERIFY_EQ(test_ptr, 1, wlmtk_dock_tile_index(dock_ptr, &t[3]));
    BS_TEST_VERIFY_EQ(test_ptr, 2, wlmtk_dock_tile_index(dock_ptr, &t[1]));

    // Removing works, and a new placement reads the stored order.
    wlmaker_dockapps_remove(d, &t[0]);
    wlmaker_dockapps_remove(d, &t[1]);
    wlmaker_dockapps_remove(d, &t[2]);
    wlmaker_dockapps_remove(d, &t[3]);
    BS_TEST_VERIFY_EQ(test_ptr, 1, wlmtk_dock_tiles(dock_ptr));
    wlmaker_dockapps_destroy(d);

    d = wlmaker_dockapps_create(dock_ptr, bspl_array_from_object(o), fname_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, d);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_add(d, &t[0], "a"));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_add(d, &t[3], "c"));
    BS_TEST_VERIFY_EQ(test_ptr, 1, wlmtk_dock_tile_index(dock_ptr, &t[3]));
    BS_TEST_VERIFY_EQ(test_ptr, 2, wlmtk_dock_tile_index(dock_ptr, &t[0]));
    wlmaker_dockapps_remove(d, &t[0]);
    wlmaker_dockapps_remove(d, &t[3]);
    wlmaker_dockapps_destroy(d);

    unlink(fname_ptr);
    char *old_ptr = bs_strdupf("%s.old", fname_ptr);
    if (NULL != old_ptr) {
        unlink(old_ptr);
        free(old_ptr);
    }
    bspl_object_unref(o);
    for (int i = 0; i < 4; ++i) wlmtk_tile_fini(&t[i]);
    wlmtk_dock_remove_tile(dock_ptr, &clip);
    wlmtk_tile_fini(&clip);
    wlmtk_dock_destroy(dock_ptr);
}

/* ------------------------------------------------------------------------- */
/** Tests conversion of pointer drags into positions. */
void test_drag_delta(bs_test_t *test_ptr)
{
    struct wlmtk_dock_style style = {};
    wlmtk_dock_positioning_t pos = {
        .edge = WLR_EDGE_LEFT, .anchor = WLR_EDGE_TOP };
    wlmtk_dock_t *dock_ptr = wlmtk_dock_create(&pos, &style);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dock_ptr);
    wlmaker_dockapps_t *d = wlmaker_dockapps_create(dock_ptr, NULL, NULL);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, d);
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlmaker_dockapps_drag_delta(d, 0, 20, 64));
    BS_TEST_VERIFY_EQ(test_ptr, 1, wlmaker_dockapps_drag_delta(d, 0, 40, 64));
    BS_TEST_VERIFY_EQ(test_ptr, -2, wlmaker_dockapps_drag_delta(d, 99, -130, 64));
    wlmaker_dockapps_destroy(d);
    wlmtk_dock_destroy(dock_ptr);

    pos = (wlmtk_dock_positioning_t){
        .edge = WLR_EDGE_BOTTOM, .anchor = WLR_EDGE_RIGHT };
    dock_ptr = wlmtk_dock_create(&pos, &style);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dock_ptr);
    d = wlmaker_dockapps_create(dock_ptr, NULL, NULL);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, d);
    // Anchored right: Moving left (negative x) goes away from the anchor.
    BS_TEST_VERIFY_EQ(test_ptr, 1, wlmaker_dockapps_drag_delta(d, -64, 0, 64));
    wlmaker_dockapps_destroy(d);
    wlmtk_dock_destroy(dock_ptr);
}

/* ------------------------------------------------------------------------- */
/** Tests two placements sharing the state file, moving between them. */
void test_two_docks(bs_test_t *test_ptr)
{
    wlmtk_dock_positioning_t pos = {
        .edge = WLR_EDGE_LEFT, .anchor = WLR_EDGE_TOP };
    struct wlmtk_dock_style style = {};
    wlmtk_dock_t *clip_ptr = wlmtk_dock_create(&pos, &style);
    pos.edge = WLR_EDGE_RIGHT;
    wlmtk_dock_t *dock_ptr = wlmtk_dock_create(&pos, &style);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, clip_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dock_ptr);
    wlmtk_tile_t t[3];
    for (int i = 0; i < 3; ++i) {
        BS_TEST_VERIFY_TRUE_OR_RETURN(
            test_ptr, wlmtk_tile_init(&t[i], &_test_tile_style));
    }
    const char *fname_ptr = bs_test_temp_path(test_ptr, "Dockapps2.plist");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, fname_ptr);
    unlink(fname_ptr);

    wlmaker_dockapps_t *c = wlmaker_dockapps_create(clip_ptr, NULL, fname_ptr);
    wlmaker_dockapps_t *d = wlmaker_dockapps_create_with_key(
        dock_ptr, NULL, fname_ptr, "Dock");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, c);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, d);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_add(c, &t[0], "a"));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_add(c, &t[1], "b"));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_add(c, &t[2], "c"));
    BS_TEST_VERIFY_EQ(test_ptr, 3, wlmaker_dockapps_size(c));
    BS_TEST_VERIFY_STREQ(test_ptr, "b", wlmaker_dockapps_name(c, &t[1]));

    // "b" goes over to the Dock; "c" too, before it.
    wlmaker_dockapps_detach(c, &t[1]);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_insert(d, &t[1], "b", 0));
    wlmaker_dockapps_detach(c, &t[2]);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_insert(d, &t[2], "c", 0));
    BS_TEST_VERIFY_EQ(test_ptr, 1, wlmaker_dockapps_size(c));
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlmaker_dockapps_index(d, &t[2]));
    BS_TEST_VERIFY_EQ(test_ptr, 1, wlmaker_dockapps_index(d, &t[1]));
    BS_TEST_VERIFY_EQ(test_ptr, -1, wlmaker_dockapps_index(c, &t[1]));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_has_name(d, "b"));
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_dockapps_has_name(c, "b"));
    wlmaker_dockapps_move_to(d, &t[2], 1);
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlmaker_dockapps_index(d, &t[1]));

    // Both orders are in the file; and the removed ones.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_set_removed(
                            fname_ptr, "x", true));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_is_removed(fname_ptr, "x"));
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_dockapps_is_removed(fname_ptr, "a"));
    wlmaker_dockapps_remove(c, &t[0]);
    wlmaker_dockapps_remove(d, &t[1]);
    wlmaker_dockapps_remove(d, &t[2]);
    wlmaker_dockapps_destroy(c);
    wlmaker_dockapps_destroy(d);

    d = wlmaker_dockapps_create_with_key(dock_ptr, NULL, fname_ptr, "Dock");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, d);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_has_name(d, "b"));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_has_name(d, "c"));
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_dockapps_has_name(d, "a"));
    wlmaker_dockapps_destroy(d);
    c = wlmaker_dockapps_create(clip_ptr, NULL, fname_ptr);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_has_name(c, "a"));
    wlmaker_dockapps_destroy(c);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_set_removed(
                            fname_ptr, "x", false));
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_dockapps_is_removed(fname_ptr, "x"));

    unlink(fname_ptr);
    char *old_ptr = bs_strdupf("%s.old", fname_ptr);
    if (NULL != old_ptr) {
        unlink(old_ptr);
        free(old_ptr);
    }
    for (int i = 0; i < 3; ++i) wlmtk_tile_fini(&t[i]);
    wlmtk_dock_destroy(clip_ptr);
    wlmtk_dock_destroy(dock_ptr);
}

/** Counts calls of the changed callback. */
static void _test_changed(void *ud_ptr) { ++*(int*)ud_ptr; }

/* ------------------------------------------------------------------------- */
/**
 * Tests dock-apps among the launchers: Their positions, kept when launchers
 * are added or removed, and stored with the order.
 */
void test_after_launchers(bs_test_t *test_ptr)
{
    wlmtk_dock_positioning_t pos = {
        .edge = WLR_EDGE_LEFT, .anchor = WLR_EDGE_TOP };
    struct wlmtk_dock_style style = {};
    wlmtk_dock_t *dock_ptr = wlmtk_dock_create(&pos, &style);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dock_ptr);
    wlmtk_tile_t t[3];
    for (int i = 0; i < 3; ++i) {
        BS_TEST_VERIFY_TRUE_OR_RETURN(
            test_ptr, wlmtk_tile_init(&t[i], &_test_tile_style));
    }
    const char *fname_ptr = bs_test_temp_path(test_ptr, "Dockapps3.plist");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, fname_ptr);
    unlink(fname_ptr);

    wlmaker_dockapps_t *d = wlmaker_dockapps_create(dock_ptr, NULL, fname_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, d);
    int changed = 0;
    wlmaker_dockapps_set_changed_callback(d, _test_changed, &changed);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_add(d, &t[0], "eyes"));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_add(d, &t[1], "zones"));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_add(d, &t[2], NULL));
    BS_TEST_VERIFY_EQ(test_ptr, 3, changed);

    // As before: After all 4 launchers. Positions 4, 5, 6.
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_DOCKAPPS_AFTER_ALL,
                      wlmaker_dockapps_after_launchers(d, &t[0]));
    BS_TEST_VERIFY_EQ(test_ptr, UINT64_C(0x70),
                      wlmaker_dockapps_positions(d, 4));

    // "eyes" right after the first launcher; "zones" after the second.
    wlmaker_dockapps_set_after_launchers(d, &t[0], 1);
    wlmaker_dockapps_set_after_launchers(d, &t[1], 2);
    BS_TEST_VERIFY_EQ(test_ptr, 5, changed);
    // L E L Z L L N: Positions 1, 3 and 6 (the nameless one after all).
    BS_TEST_VERIFY_EQ(test_ptr, (1 << 1) | (1 << 3) | (1 << 6),
                      wlmaker_dockapps_positions(d, 4));
    // With only 1 launcher: Both after it, in their order.
    BS_TEST_VERIFY_EQ(test_ptr, (1 << 1) | (1 << 2) | (1 << 3),
                      wlmaker_dockapps_positions(d, 1));

    // A launcher inserted at 1 (before "eyes"? no: "eyes" stays right
    // before it), one removed at 0.
    wlmaker_dockapps_launcher_inserted(d, 1);
    BS_TEST_VERIFY_EQ(test_ptr, 1, wlmaker_dockapps_after_launchers(d, &t[0]));
    BS_TEST_VERIFY_EQ(test_ptr, 3, wlmaker_dockapps_after_launchers(d, &t[1]));
    wlmaker_dockapps_launcher_removed(d, 0);
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlmaker_dockapps_after_launchers(d, &t[0]));
    BS_TEST_VERIFY_EQ(test_ptr, 2, wlmaker_dockapps_after_launchers(d, &t[1]));

    // Moving "zones" before "eyes": It is not before the launchers of the
    // one before it, as placed.
    wlmaker_dockapps_move_to(d, &t[1], 0);
    BS_TEST_VERIFY_EQ(test_ptr, (1 << 2) | (1 << 3) | (1 << 6),
                      wlmaker_dockapps_positions(d, 4));
    // Freezing stores them as placed.
    wlmaker_dockapps_freeze_after(d, 4);
    BS_TEST_VERIFY_EQ(test_ptr, 2, wlmaker_dockapps_after_launchers(d, &t[1]));
    BS_TEST_VERIFY_EQ(test_ptr, 2, wlmaker_dockapps_after_launchers(d, &t[0]));

    // Stored: A new placement reads them back.
    wlmaker_dockapps_set_after_launchers(d, &t[1], 0);
    wlmaker_dockapps_remove(d, &t[0]);
    wlmaker_dockapps_remove(d, &t[1]);
    wlmaker_dockapps_remove(d, &t[2]);
    wlmaker_dockapps_destroy(d);
    d = wlmaker_dockapps_create(dock_ptr, NULL, fname_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, d);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_add(d, &t[0], "eyes"));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_add(d, &t[1], "zones"));
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlmaker_dockapps_index(d, &t[1]));
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlmaker_dockapps_after_launchers(d, &t[1]));
    BS_TEST_VERIFY_EQ(test_ptr, 2, wlmaker_dockapps_after_launchers(d, &t[0]));
    BS_TEST_VERIFY_EQ(test_ptr, (1 << 0) | (1 << 3),
                      wlmaker_dockapps_positions(d, 4));
    wlmaker_dockapps_remove(d, &t[0]);
    wlmaker_dockapps_remove(d, &t[1]);
    wlmaker_dockapps_destroy(d);

    unlink(fname_ptr);
    char *old_ptr = bs_strdupf("%s.old", fname_ptr);
    if (NULL != old_ptr) {
        unlink(old_ptr);
        free(old_ptr);
    }
    for (int i = 0; i < 3; ++i) wlmtk_tile_fini(&t[i]);
    wlmtk_dock_destroy(dock_ptr);
}

/* ------------------------------------------------------------------------- */
/** Host callbacks for the test: Count, and keep the tiles out of docks. */
static void _test_host_attach(void *ud_ptr, __UNUSED__ wlmtk_tile_t *t_ptr)
{
    ++((int*)ud_ptr)[0];
}
/** See @ref _test_host_attach. */
static void _test_host_detach(void *ud_ptr, __UNUSED__ wlmtk_tile_t *t_ptr)
{
    ++((int*)ud_ptr)[1];
}

/* ------------------------------------------------------------------------- */
/**
 * Cells of the Clip's grid: Stored by name, read back, kept for dock-apps
 * not running, forgotten when detached. With a host for the tiles.
 */
void test_cells(bs_test_t *test_ptr)
{
    wlmtk_dock_positioning_t pos = {
        .edge = WLR_EDGE_LEFT, .anchor = WLR_EDGE_TOP };
    struct wlmtk_dock_style style = {};
    wlmtk_dock_t *dock_ptr = wlmtk_dock_create(&pos, &style);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dock_ptr);
    wlmtk_tile_t t[2];
    for (int i = 0; i < 2; ++i) {
        BS_TEST_VERIFY_TRUE_OR_RETURN(
            test_ptr, wlmtk_tile_init(&t[i], &_test_tile_style));
    }
    const char *fname_ptr = bs_test_temp_path(test_ptr, "Dockapps4.plist");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, fname_ptr);
    unlink(fname_ptr);

    wlmaker_dockapps_t *d = wlmaker_dockapps_create(dock_ptr, NULL, fname_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, d);
    int calls[2] = {};
    wlmaker_dockapps_host_t host = {
        _test_host_attach, _test_host_detach, calls };
    wlmaker_dockapps_set_host(d, &host);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_add(d, &t[0], "eyes"));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_add(d, &t[1], "cube"));
    // The host holds the tiles, not the dock.
    BS_TEST_VERIFY_EQ(test_ptr, 2, calls[0]);
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlmtk_dock_tiles(dock_ptr));

    wlmaker_clip_cell_t c;
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_dockapps_cell(d, &t[0], &c));
    wlmaker_dockapps_cell_update_t u[3] = {
        { .name_ptr = "eyes", .has_cell = true, .cell = { 1, 0 } },
        { .name_ptr = "cube", .has_cell = true, .cell = { 2, 0 } },
        { .name_ptr = "gone", .has_cell = true, .cell = { 0, 4 } } };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_set_cells(d, u, 3));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_cell(d, &t[1], &c));
    BS_TEST_VERIFY_EQ(test_ptr, 2, c.lane);
    BS_TEST_VERIFY_EQ(test_ptr, 0, c.slot);

    // Removed (the dock-app exited): Its cell stays. Detached: Forgotten.
    wlmaker_dockapps_remove(d, &t[0]);
    BS_TEST_VERIFY_EQ(test_ptr, 1, calls[1]);
    wlmaker_dockapps_detach(d, &t[1]);
    BS_TEST_VERIFY_EQ(test_ptr, 2, calls[1]);
    wlmaker_dockapps_destroy(d);

    // Read back: "eyes" (not running) and "gone" kept, "cube" forgotten.
    d = wlmaker_dockapps_create(dock_ptr, NULL, fname_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, d);
    wlmaker_dockapps_set_host(d, &host);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_add(d, &t[0], "eyes"));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_add(d, &t[1], "cube"));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapps_cell(d, &t[0], &c));
    BS_TEST_VERIFY_EQ(test_ptr, 1, c.lane);
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_dockapps_cell(d, &t[1], &c));
    bspl_object_t *o = bspl_create_object_from_plist_file(fname_ptr);
    bspl_dict_t *cells_ptr = bspl_dict_get_dict(
        bspl_dict_get_dict(bspl_dict_from_object(o), "Cells"), "Order");
    BS_TEST_VERIFY_STREQ(test_ptr, "0,4",
                         bspl_dict_get_string_value(cells_ptr, "gone"));
    BS_TEST_VERIFY_EQ(test_ptr, NULL,
                      bspl_dict_get_string_value(cells_ptr, "cube"));
    if (NULL != o) bspl_object_unref(o);

    wlmaker_dockapps_remove(d, &t[0]);
    wlmaker_dockapps_remove(d, &t[1]);
    wlmaker_dockapps_destroy(d);
    for (int i = 0; i < 2; ++i) wlmtk_tile_fini(&t[i]);
    wlmtk_dock_destroy(dock_ptr);
    unlink(fname_ptr);
    char *old_ptr = bs_strdupf("%s.old", fname_ptr);
    if (NULL != old_ptr) {
        unlink(old_ptr);
        free(old_ptr);
    }
}

/* == End of dockapps.c ==================================================== */
