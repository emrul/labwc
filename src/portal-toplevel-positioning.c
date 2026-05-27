// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include "common/macros.h"
#include "common/mem.h"
#include "foreign-toplevel/foreign.h"
#include "labwc.h"
#include "node.h"
#include "output.h"
#include "portal-content-type.h"
#include "portal-toplevel-positioning.h"
#include "portal-toplevel-positioning-v1-protocol.h"
#include "ssd.h"
#include "view.h"
#include "window-rules.h"

/*
 * portal_toplevel_positioning_v1 — exposes toplevel position in compositor-
 * layout coordinates to privileged clients (portal-agent).  Parallel to
 * wlr_foreign_toplevel_management_v1 but with position+stack data.
 */

#define PORTAL_POSITIONING_VERSION 2

/* Internal struct forward declarations */
struct portal_positioning_manager;
struct portal_positioning_manager_resource;
struct portal_positioning_handle;

/* Manager resource — one per bound client */
struct portal_positioning_manager_resource {
	struct wl_resource *resource;
	struct portal_positioning_manager *manager;
	struct wl_list handles;        /* portal_positioning_handle.link */
	struct wl_list popup_handles;  /* portal_positioning_popup_handle.link */
	struct wl_list compositor_menu_handles;
		/* portal_positioning_compositor_menu_handle.link */
	struct wl_list link;     /* portal_positioning_manager.manager_resources */
};

/* Portal positioning handle — one per toplevel per client */
struct portal_positioning_handle {
	struct foreign_toplevel *toplevel;
	struct wl_resource *resource;
	struct portal_positioning_manager_resource *mgr_res;
	struct wl_listener on_new_app_id;
	struct wl_listener on_new_title;
	struct wl_listener on_new_outputs;
	struct wl_listener on_new_geometry;
	struct wl_listener on_maximized;
	struct wl_listener on_minimized;
	struct wl_listener on_fullscreened;
	struct wl_listener on_activated;
	struct wl_listener on_toplevel_destroy;
	struct wl_list link;  /* portal_positioning_manager_resource.handles */
	/* Change-detection cache for geometry / state / stack_position.
	 * Set after each successful send so we only emit (and `done`) when
	 * a value actually changed.  Without this, view_moved fires per
	 * cursor-motion event during drag (60Hz+), producing geometry
	 * events even when nothing changed (labwc emits the signal even
	 * if the box didn't move — only the scene-graph node update is
	 * conditional). */
	int last_sent_x, last_sent_y, last_sent_w, last_sent_h;
	bool last_sent_geometry_valid;
	int last_sent_frame_x, last_sent_frame_y, last_sent_frame_w, last_sent_frame_h;
	bool last_sent_frame_geometry_valid;
	uint32_t last_sent_margin_left, last_sent_margin_top,
		last_sent_margin_right, last_sent_margin_bottom;
	bool last_sent_surface_margin_valid;
	uint32_t last_sent_state;
	bool last_sent_state_valid;
	int last_sent_stack_position;
	bool last_sent_stack_position_valid;
	uint32_t last_sent_content_type_mask;
	uint32_t last_sent_video_surface_count;
	bool last_sent_content_types_valid;
};

/* Per-client handle for one xdg_popup. Separate struct (and separate
 * mgr_res list) from the toplevel handle so the two lifecycles don't
 * get tangled — popups appear and disappear independently of their
 * parent toplevel's identity, and binding them into the same handle
 * list would require a discriminator on every toplevel walk.
 *
 * Per popup, every bound client gets its own popup_handle resource;
 * each has independent listeners on the underlying wlr_popup, so a
 * mid-session client connect can attach to in-flight popups without
 * disturbing the existing ones. */
struct portal_positioning_popup_handle {
	struct wlr_xdg_popup *wlr_popup;
	struct view *parent_view;
	struct wl_resource *resource;
	struct portal_positioning_manager_resource *mgr_res;
	/* The portal_toplevel_positioning_v1 handle resource for the
	 * parent_view BELONGING TO THIS mgr_res. Looked up at popup
	 * creation; null if the parent toplevel has no handle for this
	 * client yet (rare race during initial bring-up). */
	struct wl_resource *parent_handle_resource;
	struct wl_listener on_popup_destroy;
	struct wl_listener on_popup_commit;
	struct wl_listener on_popup_reposition;
	struct wl_list link;  /* in mgr_res->popup_handles */
	/* Geometry dedup — reposition events and commit-driven re-emits
	 * are common during keyboard menu navigation (submenu open/close).
	 * Cache last sent compositor-coord rect and skip identical re-
	 * emits the same way the toplevel handle does. */
	int last_sent_x, last_sent_y, last_sent_w, last_sent_h;
	bool last_sent_geometry_valid;
};

/* Per-client handle for one compositor-internal menu. Unlike popups,
 * compositor menus have no wl_surface or wlr_xdg_popup we can subscribe
 * to — labwc owns their lifecycle entirely. The patch calls
 * portal_positioning_compositor_menu_set_visible() on open and close;
 * we route those calls to create/destroy a handle per visible menu
 * per bound client. `menu_id` (the `struct menu *` from labwc) acts as
 * a stable id across reposition events within a single open cycle. */
struct portal_positioning_compositor_menu_handle {
	void *menu_id;
	struct wl_resource *resource;
	struct portal_positioning_manager_resource *mgr_res;
	struct wl_list link;  /* in mgr_res->compositor_menu_handles */
	int last_sent_x, last_sent_y, last_sent_w, last_sent_h;
	bool last_sent_geometry_valid;
};

/* Portal positioning manager — singleton global */
struct portal_positioning_manager {
	struct server *server;
	struct wl_list manager_resources;  /* portal_positioning_manager_resource.link */
	struct wl_global *global;
	struct wl_listener on_destroy;
	uint32_t damage_seq;
	/* True if a stack_position recompute is already queued for the
	 * end of the current event loop iteration.  Coalesces multiple
	 * activation events (e.g. the deactivate+activate pair that
	 * accompanies any focus switch) into a single broadcast — and,
	 * critically, defers the read of the scene tree past the
	 * synchronous view_move_to_front call that follows the
	 * activation signal in desktop_focus_view().  See
	 * schedule_stack_recompute for the timing rationale. */
	bool stack_recompute_pending;
};

/* State flags mirrored from wlr_foreign_toplevel_handle_v1_state */
enum portal_positioning_state {
	STATE_MAXIMIZED    = (1 << 0),
	STATE_MINIMIZED    = (1 << 1),
	STATE_ACTIVATED    = (1 << 2),
	STATE_FULLSCREEN   = (1 << 3),
};

/* Forward declarations */
static int compute_stack_position(struct view *view, struct server *server);

static struct wlr_box
view_frame_geometry(struct view *view)
{
	struct wlr_box box = ssd_max_extents(view);
	if (box.width <= 0 || box.height <= 0) {
		return (struct wlr_box){
			.x = view->current.x,
			.y = view->current.y,
			.width = view->current.width,
			.height = view->current.height,
		};
	}
	return box;
}

static void
handle_send_frame_geometry(struct portal_positioning_handle *handle,
		struct wlr_box box)
{
	portal_toplevel_positioning_v1_send_frame_geometry(handle->resource,
		box.x, box.y, box.width, box.height);
	handle->last_sent_frame_x = box.x;
	handle->last_sent_frame_y = box.y;
	handle->last_sent_frame_w = box.width;
	handle->last_sent_frame_h = box.height;
	handle->last_sent_frame_geometry_valid = true;
}

/*
 * Compute the margin between the toplevel's declared window rect
 * (xdg_surface.set_window_geometry) and the underlying wlr_surface
 * buffer extent. CSD apps (chromium, electron, GTK4) commit a surface
 * larger than their window rect to render a soft drop shadow into the
 * transparent margin; capture pipelines that size per-window canvases
 * to the window rect lose those shadow pixels to neighboring canvases
 * and produce ghost halos during drags. SSD apps (foot, glfw) and
 * non-xdg views report zero margins by construction.
 */
static void
view_raw_surface_margins(struct view *view, uint32_t *out_left, uint32_t *out_top,
		uint32_t *out_right, uint32_t *out_bottom)
{
	*out_left = *out_top = *out_right = *out_bottom = 0;
	if (!view || !view->surface) {
		return;
	}
	struct wlr_xdg_surface *xdg = xdg_surface_from_view(view);
	if (!xdg) {
		return;
	}
	int geo_x = xdg->current.geometry.x;
	int geo_y = xdg->current.geometry.y;
	int geo_w = xdg->current.geometry.width;
	int geo_h = xdg->current.geometry.height;
	int surf_w = view->surface->current.width;
	int surf_h = view->surface->current.height;
	if (geo_w <= 0 || geo_h <= 0 || surf_w <= 0 || surf_h <= 0) {
		return;
	}
	int left   = geo_x;
	int top    = geo_y;
	int right  = surf_w - geo_x - geo_w;
	int bottom = surf_h - geo_y - geo_h;
	if (left   > 0) *out_left   = (uint32_t)left;
	if (top    > 0) *out_top    = (uint32_t)top;
	if (right  > 0) *out_right  = (uint32_t)right;
	if (bottom > 0) *out_bottom = (uint32_t)bottom;
}

static void
view_surface_margins(struct view *view, uint32_t *out_left, uint32_t *out_top,
		uint32_t *out_right, uint32_t *out_bottom)
{
	(void)view;
	/*
	 * Portal's labwc build clips xdg toplevel rendering to
	 * xdg_surface.window_geometry, so any client-side shadow margin is no
	 * longer visible in screencopy and must not expand viewer canvases or
	 * encoder attribution.
	 */
	*out_left = *out_top = *out_right = *out_bottom = 0;
}

static struct wlr_box
view_surface_origin(struct view *view)
{
	uint32_t left = 0, top = 0, right = 0, bottom = 0;
	view_raw_surface_margins(view, &left, &top, &right, &bottom);
	(void)right;
	(void)bottom;
	return (struct wlr_box){
		.x = view->current.x - (int)left,
		.y = view->current.y - (int)top,
		.width = view->surface ? view->surface->current.width : 0,
		.height = view->surface ? view->surface->current.height : 0,
	};
}

static void
handle_send_surface_margin(struct portal_positioning_handle *handle)
{
	uint32_t left = 0, top = 0, right = 0, bottom = 0;
	view_surface_margins(foreign_toplevel_get_view(handle->toplevel), &left, &top, &right, &bottom);
	bool changed = !handle->last_sent_surface_margin_valid
		|| handle->last_sent_margin_left   != left
		|| handle->last_sent_margin_top    != top
		|| handle->last_sent_margin_right  != right
		|| handle->last_sent_margin_bottom != bottom;
	if (!changed) {
		return;
	}
	portal_toplevel_positioning_v1_send_surface_margin(handle->resource,
		left, top, right, bottom);
	handle->last_sent_margin_left   = left;
	handle->last_sent_margin_top    = top;
	handle->last_sent_margin_right  = right;
	handle->last_sent_margin_bottom = bottom;
	handle->last_sent_surface_margin_valid = true;
}

static void
handle_send_content_types(struct portal_positioning_handle *handle)
{
	uint32_t video_surface_count = 0;
	uint32_t mask = portal_content_type_view_mask(foreign_toplevel_get_view(handle->toplevel),
		&video_surface_count);
	portal_toplevel_positioning_v1_send_content_types(handle->resource,
		mask, video_surface_count);
	handle->last_sent_content_type_mask = mask;
	handle->last_sent_video_surface_count = video_surface_count;
	handle->last_sent_content_types_valid = true;
}

static bool
handle_send_content_types_if_changed(struct portal_positioning_handle *handle)
{
	uint32_t video_surface_count = 0;
	uint32_t mask = portal_content_type_view_mask(foreign_toplevel_get_view(handle->toplevel),
		&video_surface_count);
	if (handle->last_sent_content_types_valid
			&& handle->last_sent_content_type_mask == mask
			&& handle->last_sent_video_surface_count == video_surface_count) {
		return false;
	}
	portal_toplevel_positioning_v1_send_content_types(handle->resource,
		mask, video_surface_count);
	handle->last_sent_content_type_mask = mask;
	handle->last_sent_video_surface_count = video_surface_count;
	handle->last_sent_content_types_valid = true;
	return true;
}

/* Find the wl_resource for an output that belongs to a specific client */
static struct wl_resource *
output_resource_for_client(struct wlr_output *wlr_output, struct wl_client *client)
{
	struct wl_resource *resource;
	wl_resource_for_each(resource, &wlr_output->resources) {
		if (wl_resource_get_client(resource) == client) {
			return resource;
		}
	}
	return NULL;
}

/* Send current state batch for a handle */
static void
handle_send_initial(struct portal_positioning_handle *handle)
{
	struct view *view = foreign_toplevel_get_view(handle->toplevel);
	struct wl_client *client = wl_resource_get_client(handle->resource);

	const char *app_id = view->app_id;
	portal_toplevel_positioning_v1_send_app_id(handle->resource, app_id);

	const char *title = view->title;
	portal_toplevel_positioning_v1_send_title(handle->resource, title);

	if (view->output && view->output->wlr_output) {
		struct wl_resource *output_res =
			output_resource_for_client(view->output->wlr_output, client);
		if (output_res) {
			portal_toplevel_positioning_v1_send_output(handle->resource, output_res);
		}
	}

	portal_toplevel_positioning_v1_send_geometry(handle->resource,
		view->current.x, view->current.y,
		view->current.width, view->current.height);
	handle->last_sent_x = view->current.x;
	handle->last_sent_y = view->current.y;
	handle->last_sent_w = view->current.width;
	handle->last_sent_h = view->current.height;
	handle->last_sent_geometry_valid = true;
	handle_send_frame_geometry(handle, view_frame_geometry(view));
	handle_send_surface_margin(handle);

	/* Encode state flags as individual u32 elements (protocol is an array). */
	uint32_t flags = 0;
	if (view->maximized == VIEW_AXIS_BOTH) flags |= STATE_MAXIMIZED;
	if (view->minimized)                  flags |= STATE_MINIMIZED;
	if (view == view->server->active_view) flags |= STATE_ACTIVATED;
	if (view->fullscreen)                 flags |= STATE_FULLSCREEN;
	struct wl_array states;
	wl_array_init(&states);
	if (flags & STATE_MAXIMIZED) {
		uint32_t *st = wl_array_add(&states, sizeof(*st)); *st = STATE_MAXIMIZED;
	}
	if (flags & STATE_MINIMIZED) {
		uint32_t *st = wl_array_add(&states, sizeof(*st)); *st = STATE_MINIMIZED;
	}
	if (flags & STATE_ACTIVATED) {
		uint32_t *st = wl_array_add(&states, sizeof(*st)); *st = STATE_ACTIVATED;
	}
	if (flags & STATE_FULLSCREEN) {
		uint32_t *st = wl_array_add(&states, sizeof(*st)); *st = STATE_FULLSCREEN;
	}
	portal_toplevel_positioning_v1_send_state(handle->resource, &states);
	wl_array_release(&states);
	handle->last_sent_state = flags;
	handle->last_sent_state_valid = true;

	int pos = compute_stack_position(view, view->server);
	portal_toplevel_positioning_v1_send_stack_position(handle->resource, pos);
	handle->last_sent_stack_position = pos;
	handle->last_sent_stack_position_valid = true;

	handle_send_content_types(handle);

	portal_toplevel_positioning_v1_send_done(handle->resource);
}

/* Recompute and emit state flags for a handle */
/* Returns true iff the state event was actually sent (state changed). */
static bool
handle_send_state(struct portal_positioning_handle *handle)
{
	struct view *view = foreign_toplevel_get_view(handle->toplevel);
	uint32_t flags = 0;
	if (view->maximized == VIEW_AXIS_BOTH) flags |= STATE_MAXIMIZED;
	if (view->minimized)                  flags |= STATE_MINIMIZED;
	if (view == view->server->active_view) flags |= STATE_ACTIVATED;
	if (view->fullscreen)                 flags |= STATE_FULLSCREEN;

	if (handle->last_sent_state_valid && handle->last_sent_state == flags) {
		return false;
	}
	handle->last_sent_state = flags;
	handle->last_sent_state_valid = true;

	struct wl_array states;
	wl_array_init(&states);
	if (flags & STATE_MAXIMIZED) {
		uint32_t *st = wl_array_add(&states, sizeof(*st));
		*st = STATE_MAXIMIZED;
	}
	if (flags & STATE_MINIMIZED) {
		uint32_t *st = wl_array_add(&states, sizeof(*st));
		*st = STATE_MINIMIZED;
	}
	if (flags & STATE_ACTIVATED) {
		uint32_t *st = wl_array_add(&states, sizeof(*st));
		*st = STATE_ACTIVATED;
	}
	if (flags & STATE_FULLSCREEN) {
		uint32_t *st = wl_array_add(&states, sizeof(*st));
		*st = STATE_FULLSCREEN;
	}
	portal_toplevel_positioning_v1_send_state(handle->resource, &states);
	wl_array_release(&states);
	return true;
}

/*
 * Returns the back-to-front stack position of `view` among mapped
 * sibling views in the same scene tree (0 = back-most, higher =
 * closer to the top).
 *
 * Source of truth: the wlroots scene tree, not server->views.
 *
 * wlr_scene_node_raise_to_top appends the node to its parent's
 * children list, so head->next is the BOTTOM-most node and
 * head->prev is the TOP-most. Forward iteration via wl_list_for_each
 * walks bottom-up; counting mapped sibling views encountered before
 * `view` yields its 0-based back-to-front index.
 *
 * Why the scene tree and not server->views: server->views is a
 * labwc bookkeeping list whose ordering is incidental to rendering;
 * the scene tree is what wlroots renders. Sibling-scope (rather
 * than global) walk also gives sensible numbers across the
 * always_on_{top,bottom} trees — each subtree's positions are
 * relative to its peers and the consumer interprets them with the
 * tree role in mind (or, in practice, all our common views share
 * `view->workspace->tree` so the distinction is moot).
 *
 * Clients depend on this semantic when mapping stack_position into
 * their own z-order: wt_worker.js's surfaceVisualZ and viewer.rs's
 * windowVisualZ both ADD stack to a base so higher labwc-stack maps
 * to higher CSS-z. Do not invert this function without flipping
 * both client sites in lockstep.
 *
 * `server` parameter retained for ABI symmetry with the previous
 * implementation; intentionally unused.
 */
int
compute_stack_position(struct view *view, struct server *server)
{
	(void)server;
	if (!view || !view->scene_tree) {
		return -1;
	}
	struct wlr_scene_node *self_node = &view->scene_tree->node;
	if (!self_node->parent) {
		return -1;
	}
	int pos = 0;
	struct wlr_scene_node *node;
	wl_list_for_each(node, &self_node->parent->children, link) {
		if (node == self_node) {
			return pos;
		}
		if (!node->data) {
			continue;
		}
		struct view *iter = node_view_from_node(node);
		if (iter && iter->mapped) {
			pos++;
		}
	}
	return -1; /* view's scene_tree was not under its parent's children list */
}

/*
 * Recompute and broadcast stack_position to every handle.  Runs once
 * per event loop iteration (idle), coalescing any number of activation
 * signals received during the iteration.
 *
 * The recompute MUST run at idle, not directly from the activated
 * signal handler: desktop_focus_view() emits activated BEFORE calling
 * view_move_to_front(), so the synchronous handler sees the pre-raise
 * scene tree.  Deferring until idle lets view_move_to_front complete
 * (and any cascading scene-graph updates) before we read the tree.
 */
static void
stack_recompute_idle(void *data)
{
	struct portal_positioning_manager *manager = data;
	manager->stack_recompute_pending = false;

	struct portal_positioning_manager_resource *mgr_res;
	wl_list_for_each(mgr_res, &manager->manager_resources, link) {
		struct portal_positioning_handle *h;
		wl_list_for_each(h, &mgr_res->handles, link) {
			/* Skip inert handles (closed-but-not-yet-destroyed). Their
			 * toplevel pointer is NULL; the wl_resource is alive but
			 * the view it referenced is gone — dereferencing
			 * foreign_toplevel_get_view(h->toplevel) would crash. */
			if (!h->toplevel) {
				continue;
			}
			int pos = compute_stack_position(
				foreign_toplevel_get_view(h->toplevel), manager->server);
			if (h->last_sent_stack_position_valid
					&& h->last_sent_stack_position == pos) {
				continue;
			}
			h->last_sent_stack_position = pos;
			h->last_sent_stack_position_valid = true;
			portal_toplevel_positioning_v1_send_stack_position(
				h->resource, pos);
			portal_toplevel_positioning_v1_send_done(h->resource);
		}
	}
}

static void
schedule_stack_recompute(struct portal_positioning_manager *manager)
{
	if (!manager || manager->stack_recompute_pending) {
		return;
	}
	manager->stack_recompute_pending = true;
	struct wl_event_loop *loop =
		wl_display_get_event_loop(manager->server->wl_display);
	wl_event_loop_add_idle(loop, stack_recompute_idle, manager);
}

/*
 * Mark a handle inert: detach view-bound signal listeners (re-init each
 * link so subsequent wl_list_remove calls become no-ops), and NULL the
 * toplevel pointer. The wl_resource itself stays alive until the
 * client's `destroy` request fires `handle_resource_destroy` via the
 * destructor set in `wl_resource_set_implementation`.
 *
 * Used in both `send_closed` paths (toplevel_destroy signal + view
 * unmap). Per `portal-toplevel-positioning-v1.xml`, `closed` is NOT
 * `type="destructor"` — the protocol models client-initiated teardown,
 * so the server must keep the resource alive until the client sends
 * `destroy`. The pre-make-inert code path destroyed the resource
 * server-side inside `handle_toplevel_destroy`, which raced the
 * client's destroy request and caused `WL_DISPLAY_ERROR_INVALID_OBJECT`
 * (visible as "invalid object 4278190080" during Chrome window drags,
 * which tore the agent's positioning thread down and produced the
 * client-side wid swap that manifested as "Chrome canvas appears stuck
 * during drag").
 */
static void
handle_make_inert(struct portal_positioning_handle *handle)
{
	wl_list_remove(&handle->on_new_app_id.link);
	wl_list_init(&handle->on_new_app_id.link);
	wl_list_remove(&handle->on_new_title.link);
	wl_list_init(&handle->on_new_title.link);
	wl_list_remove(&handle->on_new_outputs.link);
	wl_list_init(&handle->on_new_outputs.link);
	wl_list_remove(&handle->on_new_geometry.link);
	wl_list_init(&handle->on_new_geometry.link);
	wl_list_remove(&handle->on_maximized.link);
	wl_list_init(&handle->on_maximized.link);
	wl_list_remove(&handle->on_minimized.link);
	wl_list_init(&handle->on_minimized.link);
	wl_list_remove(&handle->on_fullscreened.link);
	wl_list_init(&handle->on_fullscreened.link);
	wl_list_remove(&handle->on_activated.link);
	wl_list_init(&handle->on_activated.link);
	wl_list_remove(&handle->on_toplevel_destroy.link);
	wl_list_init(&handle->on_toplevel_destroy.link);
	handle->toplevel = NULL;
}

/*
 * Implementation of the `destroy` request from the client. The XML
 * marks it `type="destructor"`, but wayland-scanner requires an
 * explicit handler in the implementation struct or it crashes the
 * dispatcher on receipt (NULL function pointer deref). Pre-make-inert
 * the resource was always destroyed server-side before the client
 * could send destroy, so this path was never exercised; now that the
 * `closed` path waits for client-initiated teardown, the destroy
 * request must dispatch cleanly. wl_resource_destroy fires the
 * destructor registered at create-time (handle_resource_destroy).
 */
static void
handle_destroy_request(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct portal_toplevel_positioning_v1_interface handle_impl = {
	.destroy = handle_destroy_request,
};

/*
 * Single source of truth for destroying a handle.  Triggered exclusively by
 * wl_resource_destroy(handle->resource).  All other code paths that want to
 * tear down a handle must call wl_resource_destroy() — never free directly,
 * never call this function directly.
 *
 * Idempotent on already-inert handles: handle_make_inert re-inits each
 * listener's wl_list link, so the wl_list_remove calls below become
 * no-ops on detached links.
 */
static void
handle_resource_destroy(struct wl_resource *resource)
{
	struct portal_positioning_handle *handle =
		wl_resource_get_user_data(resource);
	if (!handle) {
		return;
	}
	wl_list_remove(&handle->on_new_app_id.link);
	wl_list_remove(&handle->on_new_title.link);
	wl_list_remove(&handle->on_new_outputs.link);
	wl_list_remove(&handle->on_new_geometry.link);
	wl_list_remove(&handle->on_maximized.link);
	wl_list_remove(&handle->on_minimized.link);
	wl_list_remove(&handle->on_fullscreened.link);
	wl_list_remove(&handle->on_activated.link);
	wl_list_remove(&handle->on_toplevel_destroy.link);
	wl_list_remove(&handle->link);
	free(handle);
}

/* Manager destructor — called when client destroys the manager resource */
static void
manager_resource_destroy(struct wl_resource *resource)
{
	struct portal_positioning_manager_resource *mgr_res =
		wl_resource_get_user_data(resource);
	if (!mgr_res) {
		return;
	}

	/* Tear down all handles via wl_resource_destroy → handle_resource_destroy.
	 * Each iteration removes one handle from the list. */
	while (!wl_list_empty(&mgr_res->handles)) {
		struct portal_positioning_handle *handle =
			wl_container_of(mgr_res->handles.next, handle, link);
		wl_resource_destroy(handle->resource);
	}
	/* Same shape for popup handles. Their resource destructor pulls
	 * each entry off mgr_res->popup_handles, so the loop terminates. */
	while (!wl_list_empty(&mgr_res->popup_handles)) {
		struct portal_positioning_popup_handle *ph =
			wl_container_of(mgr_res->popup_handles.next, ph, link);
		wl_resource_destroy(ph->resource);
	}
	/* Same shape for compositor-menu handles. The resource destructor
	 * (compositor_menu_handle_resource_destroy) unlinks the entry from
	 * this list and frees it. Without this loop, dangling handles
	 * would survive past the free(mgr_res) below, then later get
	 * destroyed (via client disconnect's auto-destroy) and try to
	 * wl_list_remove from a freed list head — use-after-free. */
	while (!wl_list_empty(&mgr_res->compositor_menu_handles)) {
		struct portal_positioning_compositor_menu_handle *cmh =
			wl_container_of(mgr_res->compositor_menu_handles.next, cmh, link);
		wl_resource_destroy(cmh->resource);
	}

	wl_list_remove(&mgr_res->link);
	free(mgr_res);
}

/* wl_signal handler for toplevel close.
 *
 * Marks the handle inert (detaches view-bound listeners + nulls
 * toplevel) and ships `closed`. The wl_resource stays alive until the
 * client's `destroy` request — destroying server-side here would race
 * the client's destroy request and produce WL_DISPLAY_ERROR_INVALID_OBJECT.
 */
static void
handle_toplevel_destroy(struct wl_listener *listener, void *data)
{
	struct portal_positioning_handle *handle =
		wl_container_of(listener, handle, on_toplevel_destroy);
	assert(handle->resource);
	handle_make_inert(handle);
	portal_toplevel_positioning_v1_send_closed(handle->resource);
}

/* view signal handlers */
static void
handle_new_app_id(struct wl_listener *listener, void *data)
{
	struct portal_positioning_handle *handle =
		wl_container_of(listener, handle, on_new_app_id);
	if (!handle->resource) {
		return;
	}
	const char *app_id = foreign_toplevel_get_view(handle->toplevel)->app_id;
	portal_toplevel_positioning_v1_send_app_id(handle->resource, app_id);
	portal_toplevel_positioning_v1_send_done(handle->resource);
}

static void
handle_new_title(struct wl_listener *listener, void *data)
{
	struct portal_positioning_handle *handle =
		wl_container_of(listener, handle, on_new_title);
	if (!handle->resource) {
		return;
	}
	const char *title = foreign_toplevel_get_view(handle->toplevel)->title;
	portal_toplevel_positioning_v1_send_title(handle->resource, title);
	portal_toplevel_positioning_v1_send_done(handle->resource);
}

static void
handle_new_outputs(struct wl_listener *listener, void *data)
{
	struct portal_positioning_handle *handle =
		wl_container_of(listener, handle, on_new_outputs);
	if (!handle->resource || !foreign_toplevel_get_view(handle->toplevel)->output) {
		return;
	}
	struct wlr_output *wlr_output = foreign_toplevel_get_view(handle->toplevel)->output->wlr_output;
	struct wl_client *client = wl_resource_get_client(handle->resource);
	struct wl_resource *output_res = output_resource_for_client(wlr_output, client);
	if (output_res) {
		portal_toplevel_positioning_v1_send_output(handle->resource, output_res);
		portal_toplevel_positioning_v1_send_done(handle->resource);
	}
}

static void
handle_new_geometry(struct wl_listener *listener, void *data)
{
	struct portal_positioning_handle *handle =
		wl_container_of(listener, handle, on_new_geometry);
	if (!handle->resource) {
		return;
	}
	struct view *view = foreign_toplevel_get_view(handle->toplevel);
	int x = view->current.x, y = view->current.y;
	int w = view->current.width, h = view->current.height;
	struct wlr_box frame = view_frame_geometry(view);
	bool geometry_changed = !handle->last_sent_geometry_valid
		|| handle->last_sent_x != x
		|| handle->last_sent_y != y
		|| handle->last_sent_w != w
		|| handle->last_sent_h != h;
	bool frame_changed = !handle->last_sent_frame_geometry_valid
		|| handle->last_sent_frame_x != frame.x
		|| handle->last_sent_frame_y != frame.y
		|| handle->last_sent_frame_w != frame.width
		|| handle->last_sent_frame_h != frame.height;
	/* Suppress no-op emits.  view_moved() fires for every cursor
	 * motion event during interactive drag (60Hz+), but the scene
	 * graph also receives identical-coord re-emits in some labwc
	 * paths.  Without this guard each one results in a geometry+done
	 * pair to the client AND a wayland round-trip — measurable noise
	 * during drag.  Compare-and-skip is cheap. */
	if (!geometry_changed && !frame_changed) {
		return;
	}
	if (geometry_changed) {
		handle->last_sent_x = x;
		handle->last_sent_y = y;
		handle->last_sent_w = w;
		handle->last_sent_h = h;
		handle->last_sent_geometry_valid = true;
		portal_toplevel_positioning_v1_send_geometry(handle->resource, x, y, w, h);
	}
	if (frame_changed) {
		handle_send_frame_geometry(handle, frame);
	}
	/* Surface margins can drift even when window geometry hasn't —
	 * chromium adds/removes bookmark bar, terminal opens/closes a
	 * find-bar, etc. The internal dedup in handle_send_surface_margin
	 * keeps this cheap when nothing has actually changed. */
	handle_send_surface_margin(handle);
	/* `done` is the per-handle batch barrier; without it the client's
	 * pending entry never flushes to its public snapshot map.  This is
	 * the load-bearing event for tracking interactive moves. */
	portal_toplevel_positioning_v1_send_done(handle->resource);
}

static void
handle_maximized(struct wl_listener *listener, void *data)
{
	struct portal_positioning_handle *handle =
		wl_container_of(listener, handle, on_maximized);
	if (!handle->resource) {
		return;
	}
	if (handle_send_state(handle)) {
		portal_toplevel_positioning_v1_send_done(handle->resource);
	}
}

static void
handle_minimized(struct wl_listener *listener, void *data)
{
	struct portal_positioning_handle *handle =
		wl_container_of(listener, handle, on_minimized);
	if (!handle->resource) {
		return;
	}
	if (handle_send_state(handle)) {
		portal_toplevel_positioning_v1_send_done(handle->resource);
	}
}

static void
handle_fullscreened(struct wl_listener *listener, void *data)
{
	struct portal_positioning_handle *handle =
		wl_container_of(listener, handle, on_fullscreened);
	if (!handle->resource) {
		return;
	}
	if (handle_send_state(handle)) {
		portal_toplevel_positioning_v1_send_done(handle->resource);
	}
}

static void
handle_activated(struct wl_listener *listener, void *data)
{
	struct portal_positioning_handle *handle =
		wl_container_of(listener, handle, on_activated);
	if (!handle->resource) {
		return;
	}
	bool state_changed = handle_send_state(handle);
	if (state_changed) {
		portal_toplevel_positioning_v1_send_done(handle->resource);
	}

	/* Recompute stack_position for this and all sibling handles.
	 * Deferred to idle so the scene tree reflects the
	 * view_move_to_front that follows this activation signal in
	 * desktop_focus_view(); see schedule_stack_recompute. */
	schedule_stack_recompute(handle->mgr_res->manager);
}

/* Create a handle for a toplevel and emit initial state to one manager resource */
static void
create_handle(struct portal_positioning_manager_resource *mgr_res,
		struct foreign_toplevel *toplevel)
{
	struct view *view = foreign_toplevel_get_view(toplevel);

	struct portal_positioning_handle *handle = znew(*handle);
	handle->mgr_res = mgr_res;
	handle->toplevel = toplevel;
	wl_list_insert(&mgr_res->handles, &handle->link);

	/* Create the Wayland resource for this client */
	uint32_t version = wl_resource_get_version(mgr_res->resource);
	if (version > portal_toplevel_positioning_v1_interface.version) {
		version = portal_toplevel_positioning_v1_interface.version;
	}
	handle->resource = wl_resource_create(
		wl_resource_get_client(mgr_res->resource),
		&portal_toplevel_positioning_v1_interface,
		version,
		0);
	if (!handle->resource) {
		/* Listeners not yet wired; just unlink and free. */
		wl_list_remove(&handle->link);
		free(handle);
		return;
	}
	wl_resource_set_implementation(handle->resource, &handle_impl, handle,
		handle_resource_destroy);

	/* Emit manager->toplevel event first so client knows the handle id */
	portal_toplevel_positioning_manager_v1_send_toplevel(mgr_res->resource,
		handle->resource);

	/* Connect view event listeners */
	handle->on_new_app_id.notify = handle_new_app_id;
	wl_signal_add(&view->events.new_app_id, &handle->on_new_app_id);

	handle->on_new_title.notify = handle_new_title;
	wl_signal_add(&view->events.new_title, &handle->on_new_title);

	handle->on_new_outputs.notify = handle_new_outputs;
	wl_signal_add(&view->events.new_outputs, &handle->on_new_outputs);

	handle->on_new_geometry.notify = handle_new_geometry;
	wl_signal_add(&view->events.new_geometry, &handle->on_new_geometry);

	handle->on_maximized.notify = handle_maximized;
	wl_signal_add(&view->events.maximized, &handle->on_maximized);

	handle->on_minimized.notify = handle_minimized;
	wl_signal_add(&view->events.minimized, &handle->on_minimized);

	handle->on_fullscreened.notify = handle_fullscreened;
	wl_signal_add(&view->events.fullscreened, &handle->on_fullscreened);

	handle->on_activated.notify = handle_activated;
	wl_signal_add(&view->events.activated, &handle->on_activated);

	/*
	 * Connect view destroy to emit closed. (0.9.x dropped
	 * foreign_toplevel's own events struct; view->events.destroy is
	 * the equivalent teardown signal.)
	 */
	handle->on_toplevel_destroy.notify = handle_toplevel_destroy;
	wl_signal_add(&view->events.destroy,
		&handle->on_toplevel_destroy);

	/* Send initial state */
	handle_send_initial(handle);
}

/* ===== xdg_popup tracking =================================================
 *
 * Each xdg_popup mapped by labwc gets a popup_handle PER bound client.
 * The handle listens to wlr_popup's destroy/commit/reposition signals and
 * emits geometry events in compositor-logical-pixel coords (already
 * pre-resolved through the parent scene chain — no further math needed
 * client-side).
 *
 * Compositor-coord resolution mirrors xdg_popup.c's popup_unconstrain:
 *   - The popup's `wlr_popup->parent` is either an xdg_toplevel surface
 *     or another xdg_popup surface; its scene tree is at the parent's
 *     compositor position.
 *   - `wlr_popup->scheduled.geometry` is the popup rect relative to its
 *     parent surface origin (NOT the parent's window-geometry origin —
 *     so the math is parent_origin + popup_box).
 *
 * Lifetime: the popup_handle is bound to a single wlr_xdg_popup. When
 * the popup destroys, the destroy listener fires `closed` and drops the
 * handle from mgr_res's list. The handle resource's destructor (client-
 * driven) detaches listeners independently — both paths leave a clean
 * state.
 */

static struct wlr_box
popup_compositor_rect(struct wlr_xdg_popup *wlr_popup)
{
	struct wlr_box box = (struct wlr_box){0};
	if (!wlr_popup || !wlr_popup->parent) {
		return box;
	}
	struct wlr_scene_tree *parent_tree = wlr_popup->parent->data;
	if (!parent_tree) {
		return box;
	}
	int parent_lx = 0, parent_ly = 0;
	wlr_scene_node_coords(&parent_tree->node, &parent_lx, &parent_ly);
	/* Prefer `current` (post-commit applied state), fall back to
	 * `scheduled` (pre-commit requested layout). At popup_mapped
	 * time current is still zero because the client hasn't sent its
	 * initial commit; scheduled carries the geometry the client
	 * declared via xdg_positioner. After the first commit current
	 * picks up scheduled's values. Using max() of the two avoids
	 * emitting a 0x0 placeholder that the agent's wt1 pump would
	 * filter and never upsert. */
	int x = wlr_popup->current.geometry.x;
	int y = wlr_popup->current.geometry.y;
	int w = wlr_popup->current.geometry.width;
	int h = wlr_popup->current.geometry.height;
	if (w <= 0 || h <= 0) {
		x = wlr_popup->scheduled.geometry.x;
		y = wlr_popup->scheduled.geometry.y;
		w = wlr_popup->scheduled.geometry.width;
		h = wlr_popup->scheduled.geometry.height;
	}
	box.x = parent_lx + x;
	box.y = parent_ly + y;
	box.width  = w;
	box.height = h;
	return box;
}

static void
popup_handle_make_inert(struct portal_positioning_popup_handle *handle)
{
	if (handle->on_popup_destroy.notify) {
		wl_list_remove(&handle->on_popup_destroy.link);
		wl_list_init(&handle->on_popup_destroy.link);
		handle->on_popup_destroy.notify = NULL;
	}
	if (handle->on_popup_commit.notify) {
		wl_list_remove(&handle->on_popup_commit.link);
		wl_list_init(&handle->on_popup_commit.link);
		handle->on_popup_commit.notify = NULL;
	}
	if (handle->on_popup_reposition.notify) {
		wl_list_remove(&handle->on_popup_reposition.link);
		wl_list_init(&handle->on_popup_reposition.link);
		handle->on_popup_reposition.notify = NULL;
	}
	handle->wlr_popup = NULL;
}

static void
popup_handle_resource_destroy(struct wl_resource *resource)
{
	struct portal_positioning_popup_handle *handle =
		wl_resource_get_user_data(resource);
	if (!handle) {
		return;
	}
	popup_handle_make_inert(handle);
	wl_list_remove(&handle->link);
	free(handle);
}

static void
popup_handle_destroy_request(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct portal_popup_positioning_v1_interface popup_handle_impl = {
	.destroy = popup_handle_destroy_request,
};

static void
popup_handle_send_geometry(struct portal_positioning_popup_handle *handle)
{
	if (!handle->resource || !handle->wlr_popup) {
		return;
	}
	struct wlr_box box = popup_compositor_rect(handle->wlr_popup);
	bool changed = !handle->last_sent_geometry_valid
		|| handle->last_sent_x != box.x
		|| handle->last_sent_y != box.y
		|| handle->last_sent_w != box.width
		|| handle->last_sent_h != box.height;
	if (!changed) {
		return;
	}
	portal_popup_positioning_v1_send_geometry(handle->resource,
		box.x, box.y, box.width, box.height);
	handle->last_sent_x = box.x;
	handle->last_sent_y = box.y;
	handle->last_sent_w = box.width;
	handle->last_sent_h = box.height;
	handle->last_sent_geometry_valid = true;
	portal_popup_positioning_v1_send_done(handle->resource);
}

static void
popup_on_destroy(struct wl_listener *listener, void *data)
{
	struct portal_positioning_popup_handle *handle =
		wl_container_of(listener, handle, on_popup_destroy);
	assert(handle->resource);
	popup_handle_make_inert(handle);
	portal_popup_positioning_v1_send_closed(handle->resource);
}

static void
popup_on_commit(struct wl_listener *listener, void *data)
{
	struct portal_positioning_popup_handle *handle =
		wl_container_of(listener, handle, on_popup_commit);
	popup_handle_send_geometry(handle);
}

static void
popup_on_reposition(struct wl_listener *listener, void *data)
{
	struct portal_positioning_popup_handle *handle =
		wl_container_of(listener, handle, on_popup_reposition);
	popup_handle_send_geometry(handle);
}

/* Find the toplevel handle resource owned by mgr_res that wraps
 * `parent_view`. Returns NULL if the view has no portal_positioning
 * handle for this client yet (initial bring-up race). */
static struct wl_resource *
find_toplevel_handle_resource(struct portal_positioning_manager_resource *mgr_res,
		struct view *parent_view)
{
	if (!parent_view || !parent_view->foreign_toplevel) {
		return NULL;
	}
	struct portal_positioning_handle *handle;
	wl_list_for_each(handle, &mgr_res->handles, link) {
		if (handle->toplevel == parent_view->foreign_toplevel) {
			return handle->resource;
		}
	}
	return NULL;
}

static void
create_popup_handle(struct portal_positioning_manager_resource *mgr_res,
		struct view *parent_view, struct wlr_xdg_popup *wlr_popup)
{
	struct wl_client *client = wl_resource_get_client(mgr_res->resource);
	uint32_t version = wl_resource_get_version(mgr_res->resource);
	if (version > portal_popup_positioning_v1_interface.version) {
		version = portal_popup_positioning_v1_interface.version;
	}

	struct portal_positioning_popup_handle *handle = znew(*handle);
	handle->wlr_popup = wlr_popup;
	handle->parent_view = parent_view;
	handle->mgr_res = mgr_res;
	handle->parent_handle_resource =
		find_toplevel_handle_resource(mgr_res, parent_view);
	wl_list_init(&handle->link);
	wl_list_init(&handle->on_popup_destroy.link);
	wl_list_init(&handle->on_popup_commit.link);
	wl_list_init(&handle->on_popup_reposition.link);

	handle->resource = wl_resource_create(client,
		&portal_popup_positioning_v1_interface, version, 0);
	if (!handle->resource) {
		free(handle);
		return;
	}
	wl_list_insert(&mgr_res->popup_handles, &handle->link);
	wl_resource_set_implementation(handle->resource, &popup_handle_impl,
		handle, popup_handle_resource_destroy);

	/* Emit manager->popup event first so the client learns the new id */
	portal_toplevel_positioning_manager_v1_send_popup(mgr_res->resource,
		handle->resource);

	/* Install listeners on wlr_popup events */
	handle->on_popup_destroy.notify = popup_on_destroy;
	wl_signal_add(&wlr_popup->events.destroy, &handle->on_popup_destroy);

	handle->on_popup_commit.notify = popup_on_commit;
	wl_signal_add(&wlr_popup->base->surface->events.commit,
		&handle->on_popup_commit);

	handle->on_popup_reposition.notify = popup_on_reposition;
	wl_signal_add(&wlr_popup->events.reposition,
		&handle->on_popup_reposition);

	/* Initial parent event. If the parent toplevel handle hasn't been
	 * advertised to this client yet (rare race), send a NULL parent —
	 * the client should drop the popup until a parent arrives, or in
	 * practice we just skip emitting popups whose parent handle isn't
	 * ready. We do the latter: if no parent handle, send `closed` and
	 * stop. The next popup map after the toplevel handle exists will
	 * succeed. */
	if (!handle->parent_handle_resource) {
		portal_popup_positioning_v1_send_closed(handle->resource);
		return;
	}
	portal_popup_positioning_v1_send_parent(handle->resource,
		handle->parent_handle_resource);
	popup_handle_send_geometry(handle);
}

/* ===== compositor-menu handles ============================================= */

static void
compositor_menu_handle_resource_destroy(struct wl_resource *resource)
{
	struct portal_positioning_compositor_menu_handle *handle =
		wl_resource_get_user_data(resource);
	if (!handle) {
		return;
	}
	wl_list_remove(&handle->link);
	free(handle);
}

static void
compositor_menu_handle_destroy_request(struct wl_client *client,
		struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct portal_compositor_menu_positioning_v1_interface
		compositor_menu_handle_impl = {
	.destroy = compositor_menu_handle_destroy_request,
};

static struct portal_positioning_compositor_menu_handle *
find_compositor_menu_handle(struct portal_positioning_manager_resource *mgr_res,
		void *menu_id)
{
	struct portal_positioning_compositor_menu_handle *h;
	wl_list_for_each(h, &mgr_res->compositor_menu_handles, link) {
		if (h->menu_id == menu_id) {
			return h;
		}
	}
	return NULL;
}

static void
compositor_menu_handle_send_geometry(
		struct portal_positioning_compositor_menu_handle *handle,
		struct wlr_box rect)
{
	if (!handle->resource) {
		return;
	}
	bool changed = !handle->last_sent_geometry_valid
		|| handle->last_sent_x != rect.x
		|| handle->last_sent_y != rect.y
		|| handle->last_sent_w != rect.width
		|| handle->last_sent_h != rect.height;
	if (!changed) {
		return;
	}
	portal_compositor_menu_positioning_v1_send_geometry(handle->resource,
		rect.x, rect.y, rect.width, rect.height);
	handle->last_sent_x = rect.x;
	handle->last_sent_y = rect.y;
	handle->last_sent_w = rect.width;
	handle->last_sent_h = rect.height;
	handle->last_sent_geometry_valid = true;
	portal_compositor_menu_positioning_v1_send_done(handle->resource);
}

static void
create_compositor_menu_handle(
		struct portal_positioning_manager_resource *mgr_res,
		void *menu_id, struct wlr_box rect)
{
	struct wl_client *client = wl_resource_get_client(mgr_res->resource);
	uint32_t version = wl_resource_get_version(mgr_res->resource);
	if (version > portal_compositor_menu_positioning_v1_interface.version) {
		version = portal_compositor_menu_positioning_v1_interface.version;
	}

	struct portal_positioning_compositor_menu_handle *handle = znew(*handle);
	handle->menu_id = menu_id;
	handle->mgr_res = mgr_res;
	wl_list_init(&handle->link);

	handle->resource = wl_resource_create(client,
		&portal_compositor_menu_positioning_v1_interface, version, 0);
	if (!handle->resource) {
		free(handle);
		return;
	}
	wl_list_insert(&mgr_res->compositor_menu_handles, &handle->link);
	wl_resource_set_implementation(handle->resource,
		&compositor_menu_handle_impl, handle,
		compositor_menu_handle_resource_destroy);

	portal_toplevel_positioning_manager_v1_send_compositor_menu(
		mgr_res->resource, handle->resource);
	compositor_menu_handle_send_geometry(handle, rect);
}

void
portal_positioning_compositor_menu_set_visible(struct server *server,
		void *menu_id, struct wlr_box rect, bool visible)
{
	if (!server || !menu_id) {
		return;
	}
	struct portal_positioning_manager *manager =
		server->portal_positioning_manager;
	if (!manager) {
		return;
	}
	struct portal_positioning_manager_resource *mgr_res;
	wl_list_for_each(mgr_res, &manager->manager_resources, link) {
		struct portal_positioning_compositor_menu_handle *handle =
			find_compositor_menu_handle(mgr_res, menu_id);
		if (visible) {
			if (handle) {
				/* Reposition during an open cycle. */
				compositor_menu_handle_send_geometry(handle, rect);
			} else {
				create_compositor_menu_handle(mgr_res, menu_id, rect);
			}
		} else if (handle) {
			/* Closed: terminal per the protocol contract. Send
			 * closed and forget. Client will destroy; if it
			 * doesn't, the next manager_resource_destroy will. */
			portal_compositor_menu_positioning_v1_send_closed(
				handle->resource);
			handle->menu_id = NULL;
			/* Leave the handle in the list so the client can still
			 * call destroy on the resource — destroy_request will
			 * unlink + free. Setting menu_id=NULL ensures
			 * subsequent find_* lookups for this menu return
			 * NULL, so a re-open creates a fresh handle. */
		}
	}
}

void
portal_positioning_popup_mapped(struct view *parent_view,
		struct wlr_xdg_popup *wlr_popup)
{
	if (!parent_view || !parent_view->server || !wlr_popup) {
		return;
	}
	struct portal_positioning_manager *manager =
		parent_view->server->portal_positioning_manager;
	if (!manager) {
		return;
	}
	struct portal_positioning_manager_resource *mgr_res;
	wl_list_for_each(mgr_res, &manager->manager_resources, link) {
		create_popup_handle(mgr_res, parent_view, wlr_popup);
	}
}

/* bind() — a client has connected to the manager global */
static void
manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct portal_positioning_manager *manager = data;
	assert(manager);

	struct portal_positioning_manager_resource *mgr_res = znew(*mgr_res);
	mgr_res->resource = wl_resource_create(client,
		&portal_toplevel_positioning_manager_v1_interface,
		version, id);
	if (!mgr_res->resource) {
		free(mgr_res);
		return;
	}
	mgr_res->manager = manager;
	wl_list_init(&mgr_res->handles);
	wl_list_init(&mgr_res->popup_handles);
	wl_list_init(&mgr_res->compositor_menu_handles);
	wl_list_insert(&manager->manager_resources, &mgr_res->link);
	wl_resource_set_implementation(mgr_res->resource, NULL, mgr_res,
		manager_resource_destroy);

	/* Emit toplevel events for all existing views, back-most first */
	struct view *view;
	wl_list_for_each_reverse(view, &manager->server->views, link) {
		if (!view->mapped || !view->foreign_toplevel) {
			continue;
		}
		create_handle(mgr_res, view->foreign_toplevel);
	}

	/* Signal that initial enumeration is complete */
	portal_toplevel_positioning_manager_v1_send_done(mgr_res->resource);
}

static void
manager_destroy(struct wl_listener *listener, void *data)
{
	struct portal_positioning_manager *manager =
		wl_container_of(listener, manager, on_destroy);
	wl_list_remove(&manager->on_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

void
portal_positioning_manager_create(struct server *server)
{
	struct portal_positioning_manager *manager = znew(*manager);
	manager->server = server;
	wl_list_init(&manager->manager_resources);

	manager->global = wl_global_create(server->wl_display,
		&portal_toplevel_positioning_manager_v1_interface,
	PORTAL_POSITIONING_VERSION,
		manager, manager_bind);
	if (!manager->global) {
		free(manager);
		return;
	}

	manager->on_destroy.notify = manager_destroy;
	wl_display_add_destroy_listener(server->wl_display,
		&manager->on_destroy);

	server->portal_positioning_manager = manager;
}

void
portal_positioning_view_mapped(struct foreign_toplevel *toplevel)
{
	struct portal_positioning_manager *manager =
		foreign_toplevel_get_view(toplevel)->server->portal_positioning_manager;
	if (!manager) {
		return;
	}

	/* For each bound client, create a handle for this toplevel */
	struct portal_positioning_manager_resource *mgr_res;
	wl_list_for_each(mgr_res, &manager->manager_resources, link) {
		create_handle(mgr_res, toplevel);
	}
}

/*
 * Defensive cleanup: in the normal lifecycle the toplevel_destroy signal
 * fires before this is called, and our handle_toplevel_destroy listener
 * has already torn down all matching handles.  This loop is a no-op in
 * that case but keeps the invariant if call ordering ever changes.
 */
void
portal_positioning_view_unmapped(struct foreign_toplevel *toplevel)
{
	struct portal_positioning_manager *manager =
		foreign_toplevel_get_view(toplevel)->server->portal_positioning_manager;
	if (!manager) {
		return;
	}

	struct portal_positioning_manager_resource *mgr_res;
	wl_list_for_each(mgr_res, &manager->manager_resources, link) {
		struct portal_positioning_handle *handle, *tmp;
		wl_list_for_each_safe(handle, tmp, &mgr_res->handles, link) {
			if (handle->toplevel != toplevel) {
				continue;
			}
			handle_make_inert(handle);
			portal_toplevel_positioning_v1_send_closed(handle->resource);
		}
	}
}

void
portal_positioning_view_content_types_changed(struct view *view)
{
	if (!view || !view->server || !view->server->portal_positioning_manager) {
		return;
	}

	struct portal_positioning_manager *manager =
		view->server->portal_positioning_manager;
	struct portal_positioning_manager_resource *mgr_res;
	wl_list_for_each(mgr_res, &manager->manager_resources, link) {
		struct portal_positioning_handle *handle;
		wl_list_for_each(handle, &mgr_res->handles, link) {
			/* Inert handles have NULL toplevel; same guard pattern as
			 * stack_recompute_idle. */
			if (!handle->toplevel
					|| foreign_toplevel_get_view(handle->toplevel) != view
					|| !handle->resource) {
				continue;
			}
			if (handle_send_content_types_if_changed(handle)) {
				portal_toplevel_positioning_v1_send_done(handle->resource);
			}
		}
	}
}

void
portal_positioning_view_damage(struct view *view,
		const pixman_region32_t *damage)
{
	if (!view || !view->server || !view->server->portal_positioning_manager
			|| !damage) {
		return;
	}

	int nrects = 0;
	pixman_box32_t *rects = pixman_region32_rectangles((pixman_region32_t *)damage,
		&nrects);
	if (!rects || nrects <= 0) {
		return;
	}

	struct portal_positioning_manager *manager =
		view->server->portal_positioning_manager;
	uint32_t seq = ++manager->damage_seq;
	struct wlr_box origin = view_surface_origin(view);
	if (origin.width <= 0 || origin.height <= 0) {
		return;
	}

	struct portal_positioning_manager_resource *mgr_res;
	wl_list_for_each(mgr_res, &manager->manager_resources, link) {
		struct portal_positioning_handle *handle;
		wl_list_for_each(handle, &mgr_res->handles, link) {
			if (!handle->toplevel
					|| foreign_toplevel_get_view(handle->toplevel) != view
					|| !handle->resource
					|| wl_resource_get_version(handle->resource) < 2) {
				continue;
			}
			bool sent = false;
			for (int i = 0; i < nrects; i++) {
				int width = rects[i].x2 - rects[i].x1;
				int height = rects[i].y2 - rects[i].y1;
				if (width <= 0 || height <= 0) {
					continue;
				}
				portal_toplevel_positioning_v1_send_damage(handle->resource,
					origin.x + rects[i].x1,
					origin.y + rects[i].y1,
					width,
					height,
					seq);
				sent = true;
			}
			if (sent) {
				portal_toplevel_positioning_v1_send_done(handle->resource);
			}
		}
	}
}
