/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include "labwc.h"
#include "node.h"
#include "view.h"
#include "workspaces.h"
#include "portal-csd-margin.h"

bool
portal_any_view_window_contains_cursor(struct server *server)
{
	if (!server || !server->workspaces.current
			|| !server->workspaces.current->tree) {
		return false;
	}
	struct wlr_cursor *cursor = server->seat.cursor;
	if (!cursor) {
		return false;
	}

	struct wlr_scene_node *node;
	wl_list_for_each(node,
			&server->workspaces.current->tree->children, link) {
		if (!node->data) {
			continue;
		}
		struct node_descriptor *desc = node->data;
		if (desc->type != LAB_NODE_VIEW) {
			continue;
		}
		struct view *view = desc->view;
		if (!view->mapped || !view->surface) {
			continue;
		}
		/*
		 * view->current is the window's visible-content geometry —
		 * the same box csd_margin_resize_part calls `window` to
		 * distinguish "in content" from "in shadow margin".
		 */
		if (wlr_box_contains_point(&view->current,
				cursor->x, cursor->y)) {
			return true;
		}
	}
	return false;
}
