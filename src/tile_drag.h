/* ========================================================================= */
/**
 * @file tile_drag.h
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
#ifndef __WLMAKER_TILE_DRAG_H__
#define __WLMAKER_TILE_DRAG_H__

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include <libbase/libbase.h>
#include <libbase/plist.h>

/** Forward declaration: A tile being dragged. */
typedef struct _wlmaker_tile_drag_t wlmaker_tile_drag_t;

#include "clip.h"
#include "server.h"
#include "toolkit/toolkit.h"

struct wlr_buffer;

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * The column of a tile that comes from none: A new launcher, made from an
 * application's icon (see @ref wlmaker_tile_drag_source_t::launcher_ptr).
 */
#define WLMAKER_COLUMN_NONE -1

/** What is dragged. */
typedef enum {
    /**
     * A launcher of the Dock or the Clip (shown by `wlmdock`), or of a
     * drawer. Or a drawer's tile on the Dock.
     */
    WLMAKER_TILE_DRAG_LAUNCHER,
    /** A dock-app, attached to the Dock, the Clip or a drawer. */
    WLMAKER_TILE_DRAG_DOCKAPP,
} wlmaker_tile_drag_kind_t;

/** Callback: The dragged tile is lifted off its slot (or put back). */
typedef void (*wlmaker_tile_drag_lift_t)(void *ud_ptr, bool lifted);

/** Describes the dragged tile. */
typedef struct {
    /** What it is. */
    wlmaker_tile_drag_kind_t  kind;
    /** The column it comes from, see @ref WLMAKER_COLUMN_CLIP. */
    int                       column;
    /**
     * Whether it is a drawer's tile: It stays on the Dock, and is removed
     * only when empty (else it stays where it was, as if locked).
     */
    bool                      drawer;
    /** Its index among the launchers, or among the dock-apps there. */
    size_t                    index;
    /** The dock-app's tile. */
    wlmtk_tile_t              *tile_ptr;
    /** The dock-app's process, terminated when dropped off the docks. */
    pid_t                     pid;
    /** The dock-app's name. May be NULL. */
    const char                *name_ptr;
    /** The tile's image. Referenced while dragging. */
    struct wlr_buffer         *image_ptr;
    /** Where the pointer is on the tile. */
    int                       grab_x;
    /** Where the pointer is on the tile. */
    int                       grab_y;
    /**
     * Whether the icon stays on the docks when dropped elsewhere, as Window
     * Maker does for locked icons, and those of running applications (unless
     * the move modifier is held).
     */
    bool                      running;
    /** Whether it is locked: It always stays on the docks. */
    bool                      locked;
    /**
     * A new launcher, from an application's icon (Window Maker: an appicon
     * dragged to the Dock or the Clip): Inserted where it is dropped, with
     * @ref WLMAKER_COLUMN_NONE as `column`. Dropped elsewhere, nothing
     * happens: The application keeps its icon. Referenced while dragging.
     */
    bspl_dict_t               *launcher_ptr;
    /** Called when lifted off its slot, and put back. May be NULL. */
    wlmaker_tile_drag_lift_t  lift;
    /** Argument to @ref wlmaker_tile_drag_source_t::lift. */
    void                      *lift_ud_ptr;
} wlmaker_tile_drag_source_t;

/** Where a dragged tile will be attached. */
typedef struct {
    /** The column, see @ref WLMAKER_COLUMN_CLIP. */
    int                       column;
    /** Index among the launchers or the dock-apps there. */
    size_t                    index;
    /** Slot in the column: 0 is the Dock's main tile, or the Clip. */
    size_t                    slot;
    /**
     * Position among the launchers and dock-apps of the column, as it will
     * be: `slot - 1`.
     */
    size_t                    position;
    /** Launchers before it, of those staying in the column. */
    size_t                    launchers_before;
    /** Dock-apps before it, of those staying in the column. */
    size_t                    dockapps_before;
    /**
     * Set when it does not attach because the column is full: Another tile
     * would go past the edge of the output. Window Maker's Dock has no more
     * slots then, either.
     */
    bool                      full;
    /** Position of the slot, in layout coordinates. */
    int                       x;
    /** Position of the slot, in layout coordinates. */
    int                       y;
    /**
     * Whether it is a cell of the Clip's grid (`cell`): Not a slot of a
     * column's order.
     */
    bool                      grid;
    /** The cell, see @ref wlmaker_tile_drag_target_t::grid. */
    wlmaker_clip_cell_t       cell;
} wlmaker_tile_drag_target_t;

/**
 * Starts dragging a tile, like Window Maker's wHandleAppIconMove: The tile
 * follows the pointer, and the ghost shows where it will be attached.
 *
 * @param server_ptr
 * @param source_ptr          Copied.
 * @param x                   Pointer position, in layout coordinates.
 * @param y
 *
 * @return The drag, or NULL on error. Ends with @ref wlmaker_tile_drag_drop
 *     or @ref wlmaker_tile_drag_cancel. Also held in the server.
 */
wlmaker_tile_drag_t *wlmaker_tile_drag_start(
    wlmaker_server_t *server_ptr,
    const wlmaker_tile_drag_source_t *source_ptr,
    double x,
    double y);

/** The pointer moved, to `x`, `y` in layout coordinates. */
void wlmaker_tile_drag_motion(
    wlmaker_tile_drag_t *tile_drag_ptr,
    double x,
    double y);

/**
 * The button was released at `x`, `y`: Attaches the tile where the ghost
 * is (it slides there), or removes it with Window Maker's "kaboom". Ends
 * the drag.
 */
void wlmaker_tile_drag_drop(
    wlmaker_tile_drag_t *tile_drag_ptr,
    double x,
    double y);

/**
 * Cancels the drag: The tile stays where it was. Esc does it, as for Window
 * Maker's icons, unless already dropped.
 */
void wlmaker_tile_drag_cancel(wlmaker_tile_drag_t *tile_drag_ptr);

/** The dock-app's tile is going away: Forgets it, if dragged. */
void wlmaker_tile_drag_forget_tile(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr);

/**
 * Computes where a tile with its top-left at `x`, `y` attaches to the
 * column, as Window Maker's wDockSnapIcon: When right over the column; or,
 * for the column it comes from, until it is 3 tiles away from it.
 *
 * In the Dock and the Clip (@ref wlmaker_dock_column_t::mixed), launchers
 * and dock-apps share one order, as on Window Maker's Dock: The tile goes
 * to the slot it is over, before or after any other, up to right after the
 * last one. In a drawer, launchers come first, then dock-apps.
 *
 * @param column_ptr
 * @param kind
 * @param own_column          Whether the tile comes from this column.
 * @param own_position        Its position there (see
 *                            @ref wlmaker_tile_drag_target_t::position), if
 *                            from this column.
 * @param x
 * @param y
 * @param target_ptr          All but `column` are set.
 *
 * @return Whether it attaches.
 */
bool wlmaker_tile_drag_snap(
    const wlmaker_dock_column_t *column_ptr,
    wlmaker_tile_drag_kind_t kind,
    bool own_column,
    size_t own_position,
    int x,
    int y,
    wlmaker_tile_drag_target_t *target_ptr);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_tile_drag_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_TILE_DRAG_H__ */
/* == End of tile_drag.h =================================================== */
