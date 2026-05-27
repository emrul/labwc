// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include "common/mem.h"
#include "labwc.h"
#include "portal-cursor-shape.h"
#include "portal-cursor-shape-v1-protocol.h"

/*
 * portal_cursor_shape_v1 — forwards the compositor's current cursor
 * shape to a privileged remote-desktop client (portal-agent).  The
 * viewer renders the matching CSS cursor at zero RTT in position
 * while the server-side cursor sprite stays hidden under
 * PORTAL_HIDE_CURSOR.  Compositor-side hooks live in cursor.c:
 *
 *   - request_set_shape_notify → forwards client-driven shapes
 *     (wp_cursor_shape_v1 from foot/chromium/GTK4/Qt6 apps)
 *   - cursor_set → forwards labwc-internal shapes (default,
 *     resize edges, grab) when the cursor leaves a client surface
 */

#define PORTAL_CURSOR_SHAPE_VERSION 1

/* Default-on-bind value, used when no shape has been observed yet
 * (cold start before any pointer motion). Matches the
 * wp_cursor_shape_device_v1.shape enum value for "default". */
#define DEFAULT_SHAPE 1u

/* Per-bound-client resource. */
struct portal_cursor_shape_resource {
	struct wl_resource *resource;
	struct portal_cursor_shape_manager *manager;
	struct wl_list link;  /* manager.resources */
};

/* Singleton manager. */
struct portal_cursor_shape_manager {
	struct server *server;
	struct wl_global *global;
	struct wl_listener on_destroy;
	struct wl_list resources;  /* portal_cursor_shape_resource.link */
	struct wlr_surface *last_entered_surface;  /* pointer-compare only;
				    * never dereferenced.  See
				    * portal_cursor_shape_notify_surface_enter
				    * below for the rationale. */
	uint32_t current_shape;    /* last value broadcast; sent to new
				    * binders so they don't see "default"
				    * until the next change */
};

static void
resource_destroy(struct wl_resource *resource)
{
	struct portal_cursor_shape_resource *r = wl_resource_get_user_data(resource);
	if (!r) {
		return;
	}
	wl_list_remove(&r->link);
	free(r);
}

static void
manager_destroy_request(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct portal_cursor_shape_manager_v1_interface impl = {
	.destroy = manager_destroy_request,
};

static void
manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct portal_cursor_shape_manager *manager = data;
	assert(manager);

	struct portal_cursor_shape_resource *r = znew(*r);
	r->resource = wl_resource_create(client,
		&portal_cursor_shape_manager_v1_interface, version, id);
	if (!r->resource) {
		free(r);
		return;
	}
	r->manager = manager;
	wl_list_insert(&manager->resources, &r->link);
	wl_resource_set_implementation(r->resource, &impl, r, resource_destroy);

	/* Send the current shape so the new binder doesn't have a stale
	 * mental model of "no cursor yet" — they'll get an immediate
	 * baseline, then deltas as shapes change. */
	portal_cursor_shape_manager_v1_send_shape(r->resource, manager->current_shape);
}

static void
manager_destroy(struct wl_listener *listener, void *data)
{
	struct portal_cursor_shape_manager *manager =
		wl_container_of(listener, manager, on_destroy);
	wl_list_remove(&manager->on_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

void
portal_cursor_shape_manager_create(struct server *server)
{
	struct portal_cursor_shape_manager *manager = znew(*manager);
	manager->server = server;
	manager->current_shape = DEFAULT_SHAPE;
	wl_list_init(&manager->resources);

	manager->global = wl_global_create(server->wl_display,
		&portal_cursor_shape_manager_v1_interface,
		PORTAL_CURSOR_SHAPE_VERSION, manager, manager_bind);
	if (!manager->global) {
		free(manager);
		return;
	}

	manager->on_destroy.notify = manager_destroy;
	wl_display_add_destroy_listener(server->wl_display, &manager->on_destroy);

	server->portal_cursor_shape_manager = manager;
}

void
portal_cursor_shape_broadcast(struct server *server, uint32_t shape)
{
	if (!server || !server->portal_cursor_shape_manager) {
		return;
	}
	struct portal_cursor_shape_manager *manager =
		server->portal_cursor_shape_manager;

	/* Dedup at the source — saves wakeups on every bound client
	 * even when the shape is the same one we just sent. */
	if (manager->current_shape == shape) {
		return;
	}
	manager->current_shape = shape;

	struct portal_cursor_shape_resource *r;
	wl_list_for_each(r, &manager->resources, link) {
		portal_cursor_shape_manager_v1_send_shape(r->resource, shape);
	}
}

uint32_t
portal_cursor_shape_from_lab(enum lab_cursors cursor)
{
	/* Map labwc's internal enum onto wp_cursor_shape_device_v1.shape
	 * values.  Resize edges use the diagonal CSS keywords where
	 * applicable; corner resizes have no protocol equivalent so
	 * they snap to the bidirectional resize values (ew/ns/nesw/nwse). */
	switch (cursor) {
	case LAB_CURSOR_DEFAULT:    return 1;  /* default */
	case LAB_CURSOR_GRAB:       return 16; /* grab */
	case LAB_CURSOR_RESIZE_N:   return 19; /* n-resize */
	case LAB_CURSOR_RESIZE_NE:  return 20; /* ne-resize */
	case LAB_CURSOR_RESIZE_NW:  return 21; /* nw-resize */
	case LAB_CURSOR_RESIZE_E:   return 18; /* e-resize */
	case LAB_CURSOR_RESIZE_SE:  return 23; /* se-resize */
	case LAB_CURSOR_RESIZE_S:   return 22; /* s-resize */
	case LAB_CURSOR_RESIZE_SW:  return 24; /* sw-resize */
	case LAB_CURSOR_RESIZE_W:   return 25; /* w-resize */
	case LAB_CURSOR_CLIENT:     /* fall through — client owns shape */
	default:                    return 1;
	}
}

void
portal_cursor_shape_notify_surface_enter(struct server *server,
		struct wlr_surface *surface)
{
	if (!server || !server->portal_cursor_shape_manager) {
		return;
	}
	struct portal_cursor_shape_manager *manager =
		server->portal_cursor_shape_manager;

	/* Compare by pointer.  We never dereference last_entered_surface,
	 * so a destroyed-and-replaced-at-same-address surface only costs
	 * us one stale-shape frame on the next motion — acceptable; the
	 * full surface-destroy listener wiring would be heavier than the
	 * bug it avoids. */
	if (manager->last_entered_surface == surface) {
		return;
	}
	manager->last_entered_surface = surface;

	/* Force default on transition.  If the client has an opinion for
	 * the entered pixel, its wp_cursor_shape_v1 request fires within
	 * the same motion frame and overwrites via the existing
	 * request_set_shape_notify hook — the user sees no flicker.  If
	 * the client has NO opinion (vscode over empty editor gutter,
	 * Electron over inter-panel gaps), we stay at default, which is
	 * far better than persisting a resize-edge cursor from the
	 * previous surface. */
	portal_cursor_shape_broadcast(server, DEFAULT_SHAPE);
}
