/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_PORTAL_CURSOR_SHAPE_H
#define LABWC_PORTAL_CURSOR_SHAPE_H

#include <stdint.h>
#include "input/cursor.h"

struct server;

/* Create the portal cursor-shape manager global.  Lifetime is tied
 * to wl_display via wl_display_add_destroy_listener — there is no
 * explicit teardown. */
void
portal_cursor_shape_manager_create(struct server *server);

/* Broadcast a shape change to all bound clients.  Callers pass the
 * raw wp_cursor_shape_device_v1.shape enum value (1..34) or 0 for
 * "hidden".  Duplicate values are dropped server-side. */
void
portal_cursor_shape_broadcast(struct server *server, uint32_t shape);

/* Map a labwc-internal cursor enum to a wp_cursor_shape_device_v1.shape
 * value.  Used by callers in cursor.c that don't already have a shape
 * enum value (e.g. cursor_set, called when the cursor enters labwc-
 * native areas like the desktop background or SSD edges).  Returns 1
 * (default) for unmapped values. */
uint32_t
portal_cursor_shape_from_lab(enum lab_cursors cursor);

struct wlr_surface;

/* Force-broadcast "default" when the pointer enters a NEW client
 * surface.  labwc itself never calls cursor_set() on enter — it just
 * sets seat->server_cursor = LAB_CURSOR_CLIENT and waits for the
 * client to request its own cursor via wp_cursor_shape_v1 (or the
 * legacy wl_pointer.set_cursor).  If the client has no opinion for
 * the entered pixel (vscode/Electron over empty editor gutter, gaps
 * between panels, etc.) it sends nothing — and our last broadcasted
 * shape, often a resize-edge cursor from the previous surface,
 * persists.  This helper is called from cursor_update_common() on
 * every motion; the manager tracks the last entered surface and
 * only broadcasts when it actually changes, so the client's
 * wp_cursor_shape_v1 request (if any) fires right after and
 * overwrites without flicker. */
void
portal_cursor_shape_notify_surface_enter(struct server *server,
		struct wlr_surface *surface);

#endif /* LABWC_PORTAL_CURSOR_SHAPE_H */
