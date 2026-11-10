// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdlib.h>
#include <wlr/util/log.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_touch.h>
#include <wlr/types/wlr_touch.h>
#include "common/mem.h"
#include "labwc.h"
#include "portal-virtual-touch.h"
#include "portal-virtual-touch-v1-protocol.h"

/*
 * portal_virtual_touch_v1 — a remote-desktop host relays a viewer's
 * touchscreen as a real wlr_touch on the seat.  The device goes through
 * the same new_touch()/seat_add_device() path a hardware touchscreen
 * takes (seat_add_virtual_touch), so input/touch.c handles its events:
 * wl_touch for touch-aware clients, pointer emulation for the rest.
 */

#define PORTAL_VIRTUAL_TOUCH_VERSION 1
/* Touch ids currently down, per device, so destroy can cancel them. */
#define MAX_POINTS 16

static const struct wlr_touch_impl virtual_touch_impl = {
	.name = "portal-virtual-touch",
};

struct portal_virtual_touch {
	struct wlr_touch touch;
	struct wl_resource *resource;
	int32_t down[MAX_POINTS];
	int ndown;
};

static const struct portal_virtual_touch_v1_interface touch_impl;

static struct portal_virtual_touch *
from_resource(struct wl_resource *resource)
{
	assert(wl_resource_instance_of(resource,
		&portal_virtual_touch_v1_interface, &touch_impl));
	return wl_resource_get_user_data(resource);
}

static double
fraction(uint32_t v, uint32_t extent)
{
	double f = (double)v / (double)extent;
	return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
}

static void
remember(struct portal_virtual_touch *t, int32_t id)
{
	for (int i = 0; i < t->ndown; i++) {
		if (t->down[i] == id) {
			return;
		}
	}
	if (t->ndown < MAX_POINTS) {
		t->down[t->ndown++] = id;
	}
}

static bool
forget(struct portal_virtual_touch *t, int32_t id)
{
	for (int i = 0; i < t->ndown; i++) {
		if (t->down[i] == id) {
			t->down[i] = t->down[--t->ndown];
			return true;
		}
	}
	return false;
}

static void
handle_down(struct wl_client *client, struct wl_resource *resource,
		uint32_t time, int32_t id, uint32_t x, uint32_t y,
		uint32_t x_extent, uint32_t y_extent)
{
	struct portal_virtual_touch *t = from_resource(resource);
	if (!t || x_extent == 0 || y_extent == 0) {
		return;
	}
	/* A second down for a held id would leave the seat with a stale point. */
	for (int i = 0; i < t->ndown; i++) {
		if (t->down[i] == id) {
			return;
		}
	}
	if (t->ndown >= MAX_POINTS) {
		return;
	}
	remember(t, id);
	struct wlr_touch_down_event event = {
		.touch = &t->touch,
		.time_msec = time,
		.touch_id = id,
		.x = fraction(x, x_extent),
		.y = fraction(y, y_extent),
	};
	wl_signal_emit_mutable(&t->touch.events.down, &event);
}

static void
handle_motion(struct wl_client *client, struct wl_resource *resource,
		uint32_t time, int32_t id, uint32_t x, uint32_t y,
		uint32_t x_extent, uint32_t y_extent)
{
	struct portal_virtual_touch *t = from_resource(resource);
	if (!t || x_extent == 0 || y_extent == 0) {
		return;
	}
	bool held = false;
	for (int i = 0; i < t->ndown; i++) {
		held |= t->down[i] == id;
	}
	if (!held) {
		return;
	}
	struct wlr_touch_motion_event event = {
		.touch = &t->touch,
		.time_msec = time,
		.touch_id = id,
		.x = fraction(x, x_extent),
		.y = fraction(y, y_extent),
	};
	wl_signal_emit_mutable(&t->touch.events.motion, &event);
}

static void
handle_up(struct wl_client *client, struct wl_resource *resource,
		uint32_t time, int32_t id)
{
	struct portal_virtual_touch *t = from_resource(resource);
	if (!t || !forget(t, id)) {
		return;
	}
	struct wlr_touch_up_event event = {
		.touch = &t->touch,
		.time_msec = time,
		.touch_id = id,
	};
	wl_signal_emit_mutable(&t->touch.events.up, &event);
}

static void
cancel_point(struct portal_virtual_touch *t, uint32_t time, int32_t id)
{
	struct wlr_touch_cancel_event event = {
		.touch = &t->touch,
		.time_msec = time,
		.touch_id = id,
	};
	wl_signal_emit_mutable(&t->touch.events.cancel, &event);
}

static void
handle_cancel(struct wl_client *client, struct wl_resource *resource,
		uint32_t time, int32_t id)
{
	struct portal_virtual_touch *t = from_resource(resource);
	if (!t || !forget(t, id)) {
		return;
	}
	cancel_point(t, time, id);
}

static void
handle_frame(struct wl_client *client, struct wl_resource *resource)
{
	struct portal_virtual_touch *t = from_resource(resource);
	if (!t) {
		return;
	}
	wl_signal_emit_mutable(&t->touch.events.frame, NULL);
}

static void
touch_destroy(struct portal_virtual_touch *t)
{
	if (!t) {
		return;
	}
	/* Cancel whatever is still down, so no client keeps a stuck point. */
	while (t->ndown > 0) {
		cancel_point(t, 0, t->down[--t->ndown]);
	}
	wl_signal_emit_mutable(&t->touch.events.frame, NULL);
	wl_resource_set_user_data(t->resource, NULL);
	wlr_touch_finish(&t->touch);
	free(t);
}

static void
handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
handle_resource_destroy(struct wl_resource *resource)
{
	touch_destroy(wl_resource_get_user_data(resource));
}

static const struct portal_virtual_touch_v1_interface touch_impl = {
	.down = handle_down,
	.motion = handle_motion,
	.up = handle_up,
	.cancel = handle_cancel,
	.frame = handle_frame,
	.destroy = handle_destroy,
};

static void
manager_create_touch(struct wl_client *client, struct wl_resource *manager,
		uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&portal_virtual_touch_v1_interface,
		wl_resource_get_version(manager), id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	struct portal_virtual_touch *t = znew(*t);
	t->resource = resource;
	wlr_touch_init(&t->touch, &virtual_touch_impl, "portal-virtual-touch");
	wl_resource_set_implementation(resource, &touch_impl, t,
		handle_resource_destroy);
	seat_add_virtual_touch(&server.seat, &t->touch.base);
}

static void
manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct portal_virtual_touch_manager_v1_interface manager_impl = {
	.create_touch = manager_create_touch,
	.destroy = manager_destroy,
};

static void
manager_bind(struct wl_client *client, void *data, uint32_t version,
		uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&portal_virtual_touch_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

void
portal_virtual_touch_manager_create(struct server *server)
{
	if (!wl_global_create(server->wl_display,
			&portal_virtual_touch_manager_v1_interface,
			PORTAL_VIRTUAL_TOUCH_VERSION, NULL, manager_bind)) {
		wlr_log(WLR_ERROR, "unable to create portal virtual touch manager");
	}
}
