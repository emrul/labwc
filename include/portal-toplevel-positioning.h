/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_PORTAL_TOPLEVEL_POSITIONING_H
#define LABWC_PORTAL_TOPLEVEL_POSITIONING_H

#include <stdbool.h>
#include <pixman.h>
#include "foreign-toplevel/foreign.h"

struct server;
struct view;
struct wlr_xdg_popup;

/* Create the portal positioning manager global.  Lifetime is tied to
 * wl_display via wl_display_add_destroy_listener — there is no explicit
 * teardown function; the destroy listener cleans up everything. */
void
portal_positioning_manager_create(struct server *server);

/* Called from foreign_toplevel_create/destroy to track mapped toplevels */
void
portal_positioning_view_mapped(struct foreign_toplevel *toplevel);

void
portal_positioning_view_unmapped(struct foreign_toplevel *toplevel);

void
portal_positioning_view_content_types_changed(struct view *view);

void
portal_positioning_view_damage(struct view *view,
		const pixman_region32_t *damage);

/* Called from xdg_popup_create when a new xdg_popup is mapped. The
 * patch tracks the popup's lifetime via its own destroy + commit
 * listeners installed on `wlr_popup`. `parent_view` is the ROOT
 * toplevel view that ultimately owns this popup (resolved via the
 * popup parent chain for nested menus); the patch attaches the
 * portal handle to the toplevel handle of this view so clients can
 * route popup pixels to the right toplevel's canvas tree. */
void
portal_positioning_popup_mapped(struct view *parent_view,
		struct wlr_xdg_popup *wlr_popup);

/* Compositor-internal menu visibility notifications.
 *
 * Called from menu.c on the open path (after wlr_scene_node_set_position
 * sets the scene-tree position, with the resolved bounding box) and on
 * the close path (when set_enabled(false) fires). `menu_id` is the
 * compositor-internal menu pointer used as a stable id across reposition
 * events within a single open cycle; per the protocol contract,
 * `closed` is terminal — a new open creates a new handle.
 *
 * `rect` is the menu's bounding box in logical compositor coordinates.
 * Submenus call this independently with their own rect; the protocol
 * treats each menu node as its own handle. */
void
portal_positioning_compositor_menu_set_visible(struct server *server,
		void *menu_id, struct wlr_box rect, bool visible);

#endif /* LABWC_PORTAL_TOPLEVEL_POSITIONING_H */
