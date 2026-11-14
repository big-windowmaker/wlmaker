/* ========================================================================= */
/**
 * @file zones.h
 *
 * Window zones and tiling (wmake-zones) inside the compositor: While a
 * window is dragged, the zones of the layout are shown on its output, and
 * dropping it over a zone snaps it there. Optionally, windows are tiled
 * automatically. Shares the geometry engine and the configuration file
 * (~/.config/wmake-zones/wmake-zones.conf) with wmake-zones on X11, and
 * serves the same control commands on a socket in $XDG_RUNTIME_DIR.
 *
 * Copyright (c) 2026 Big WindowMaker
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __ZONES_H__
#define __ZONES_H__

#include <stdbool.h>
#include <stddef.h>

/** Forward declaration: State of the zones. */
typedef struct _wlmaker_zones_t wlmaker_zones_t;

struct _wlmaker_server_t;

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Creates the zones. Returns NULL if built without wmake-zones, or on error
 * (wlmaker keeps working without zones).
 */
wlmaker_zones_t *wlmaker_zones_create(struct _wlmaker_server_t *server_ptr);

/** Destroys the zones: Hides the overlay, closes the socket. */
void wlmaker_zones_destroy(wlmaker_zones_t *zones_ptr);

/** Reads the configuration file again. */
void wlmaker_zones_reload(wlmaker_zones_t *zones_ptr);

/**
 * Runs a command, as by the `Zones` action or the socket: eg. "zones
 * toggle", "layout next", "move left", "snap 2", "restore", "status".
 *
 * @param zones_ptr
 * @param cmd_ptr
 * @param out_ptr             Reply, may be NULL.
 * @param out_len
 */
void wlmaker_zones_command(wlmaker_zones_t *zones_ptr,
                           const char *cmd_ptr,
                           char *out_ptr,
                           size_t out_len);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __ZONES_H__ */
/* == End of zones.h ======================================================= */
