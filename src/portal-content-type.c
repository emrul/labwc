// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "common/mem.h"
#include "labwc.h"
#include "portal-content-type.h"
#include "portal-toplevel-positioning.h"
#include "content-type-v1-protocol.h"
#include "view.h"

#if HAVE_XWAYLAND
#include <wlr/xwayland.h>
#endif

#define PORTAL_CONTENT_TYPE_VERSION 1

struct portal_content_type_manager {
	struct server *server;
	struct wl_global *global;
	struct wl_list entries; /* portal_content_type_entry.link */
	struct wl_listener on_destroy;
};

struct portal_content_type_manager_resource {
	struct wl_resource *resource;
	struct portal_content_type_manager *manager;
};

struct portal_content_type_entry {
	struct portal_content_type_manager *manager;
	struct wl_resource *resource;
	struct wlr_surface *surface;
	uint32_t content_type;
	struct wl_listener on_surface_destroy;
	struct wl_list link;
};

static struct portal_content_type_entry *
entry_for_surface(struct portal_content_type_manager *manager,
		struct wlr_surface *surface)
{
	struct portal_content_type_entry *entry;
	wl_list_for_each(entry, &manager->entries, link) {
		if (entry->surface && entry->surface == surface) {
			return entry;
		}
	}
	return NULL;
}

static uint32_t
mask_from_content_type(uint32_t content_type)
{
	switch (content_type) {
	case WP_CONTENT_TYPE_V1_TYPE_PHOTO:
		return PORTAL_CONTENT_TYPE_PHOTO;
	case WP_CONTENT_TYPE_V1_TYPE_VIDEO:
		return PORTAL_CONTENT_TYPE_VIDEO;
	case WP_CONTENT_TYPE_V1_TYPE_GAME:
		return PORTAL_CONTENT_TYPE_GAME;
	default:
		return 0;
	}
}

static struct wlr_surface *
root_surface_for(struct wlr_surface *surface)
{
	while (surface) {
		struct wlr_subsurface *subsurface =
			wlr_subsurface_try_from_wlr_surface(surface);
		if (!subsurface) {
			return surface;
		}
		surface = subsurface->parent;
	}
	return NULL;
}

static struct view *
view_for_surface(struct wlr_surface *surface)
{
	struct wlr_surface *root = root_surface_for(surface);
	if (!root) {
		return NULL;
	}
	struct wlr_xdg_surface *xdg_surface =
		wlr_xdg_surface_try_from_wlr_surface(root);
	if (xdg_surface) {
		if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
			return xdg_surface->data;
		}
		while (xdg_surface && xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
			xdg_surface = wlr_xdg_surface_try_from_wlr_surface(
				xdg_surface->popup->parent);
		}
		return xdg_surface && xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL
			? xdg_surface->data : NULL;
	}

	struct view *view = view_from_wlr_surface(root);
	if (view) {
		return view;
	}
	return NULL;
}

uint32_t
portal_content_type_view_mask(struct view *view, uint32_t *video_surface_count)
{
	if (video_surface_count) {
		*video_surface_count = 0;
	}
	if (!view || !server.portal_content_type_manager) {
		return 0;
	}

	struct portal_content_type_manager *manager =
		server.portal_content_type_manager;
	uint32_t mask = 0;
	struct portal_content_type_entry *entry;
	wl_list_for_each(entry, &manager->entries, link) {
		if (view_for_surface(entry->surface) != view) {
			continue;
		}
		uint32_t entry_mask = mask_from_content_type(entry->content_type);
		mask |= entry_mask;
		if (video_surface_count && (entry_mask & PORTAL_CONTENT_TYPE_VIDEO)) {
			(*video_surface_count)++;
		}
	}
	return mask;
}

static void
notify_view_changed(struct portal_content_type_entry *entry)
{
	struct view *view = view_for_surface(entry->surface);
	if (view) {
		portal_positioning_view_content_types_changed(view);
	}
}

static void
entry_destroy(struct portal_content_type_entry *entry, bool notify)
{
	if (!entry) {
		return;
	}
	if (notify) {
		notify_view_changed(entry);
	}
	if (entry->surface) {
		wl_list_remove(&entry->on_surface_destroy.link);
	}
	wl_list_remove(&entry->link);
	free(entry);
}

static void
surface_destroy_notify(struct wl_listener *listener, void *data)
{
	struct portal_content_type_entry *entry =
		wl_container_of(listener, entry, on_surface_destroy);
	(void)data;
	notify_view_changed(entry);
	wl_list_remove(&entry->on_surface_destroy.link);
	entry->surface = NULL;
	entry->content_type = WP_CONTENT_TYPE_V1_TYPE_NONE;
}

static void
content_type_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
content_type_set_content_type(struct wl_client *client,
		struct wl_resource *resource, uint32_t content_type)
{
	struct portal_content_type_entry *entry =
		wl_resource_get_user_data(resource);
	if (!entry) {
		return;
	}
	(void)client;
	if (!entry->surface) {
		return;
	}
	if (content_type > WP_CONTENT_TYPE_V1_TYPE_GAME) {
		content_type = WP_CONTENT_TYPE_V1_TYPE_NONE;
	}
	if (entry->content_type == content_type) {
		return;
	}
	entry->content_type = content_type;
	notify_view_changed(entry);
	wlr_log(WLR_DEBUG, "portal-content-type: surface=%p type=%u",
		(void *)entry->surface, content_type);
}

static const struct wp_content_type_v1_interface content_type_impl = {
	.destroy = content_type_destroy,
	.set_content_type = content_type_set_content_type,
};

static void
content_type_resource_destroy(struct wl_resource *resource)
{
	struct portal_content_type_entry *entry =
		wl_resource_get_user_data(resource);
	if (!entry) {
		return;
	}
	entry_destroy(entry, true);
}

static void
manager_destroy_request(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
manager_get_surface_content_type(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface_resource)
{
	struct portal_content_type_manager_resource *mgr_res =
		wl_resource_get_user_data(resource);
	struct portal_content_type_manager *manager = mgr_res->manager;
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (!surface) {
		wl_resource_post_error(resource,
			WP_CONTENT_TYPE_MANAGER_V1_ERROR_ALREADY_CONSTRUCTED,
			"invalid wl_surface");
		return;
	}
	if (entry_for_surface(manager, surface)) {
		wl_resource_post_error(resource,
			WP_CONTENT_TYPE_MANAGER_V1_ERROR_ALREADY_CONSTRUCTED,
			"wl_surface already has a content type object");
		return;
	}

	struct portal_content_type_entry *entry = znew(*entry);
	entry->manager = manager;
	entry->surface = surface;
	entry->content_type = WP_CONTENT_TYPE_V1_TYPE_NONE;
	entry->resource = wl_resource_create(client, &wp_content_type_v1_interface,
		wl_resource_get_version(resource), id);
	if (!entry->resource) {
		free(entry);
		wl_client_post_no_memory(client);
		return;
	}

	entry->on_surface_destroy.notify = surface_destroy_notify;
	wl_signal_add(&surface->events.destroy, &entry->on_surface_destroy);
	wl_list_insert(&manager->entries, &entry->link);
	wl_resource_set_implementation(entry->resource, &content_type_impl, entry,
		content_type_resource_destroy);
}

static const struct wp_content_type_manager_v1_interface manager_impl = {
	.destroy = manager_destroy_request,
	.get_surface_content_type = manager_get_surface_content_type,
};

static void
manager_resource_destroy(struct wl_resource *resource)
{
	struct portal_content_type_manager_resource *mgr_res =
		wl_resource_get_user_data(resource);
	free(mgr_res);
}

static void
manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct portal_content_type_manager *manager = data;
	struct portal_content_type_manager_resource *mgr_res = znew(*mgr_res);
	mgr_res->manager = manager;
	mgr_res->resource = wl_resource_create(client,
		&wp_content_type_manager_v1_interface, version, id);
	if (!mgr_res->resource) {
		free(mgr_res);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(mgr_res->resource, &manager_impl, mgr_res,
		manager_resource_destroy);
}

static void
manager_destroy(struct wl_listener *listener, void *data)
{
	struct portal_content_type_manager *manager =
		wl_container_of(listener, manager, on_destroy);
	while (!wl_list_empty(&manager->entries)) {
		struct portal_content_type_entry *entry =
			wl_container_of(manager->entries.next, entry, link);
		entry_destroy(entry, false);
	}
	wl_list_remove(&manager->on_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

void
portal_content_type_manager_create(struct server *server)
{
	struct portal_content_type_manager *manager = znew(*manager);
	manager->server = server;
	wl_list_init(&manager->entries);
	manager->global = wl_global_create(server->wl_display,
		&wp_content_type_manager_v1_interface,
		PORTAL_CONTENT_TYPE_VERSION, manager, manager_bind);
	if (!manager->global) {
		free(manager);
		wlr_log(WLR_ERROR, "portal-content-type: failed to create global");
		return;
	}

	manager->on_destroy.notify = manager_destroy;
	wl_display_add_destroy_listener(server->wl_display, &manager->on_destroy);
	server->portal_content_type_manager = manager;
	wlr_log(WLR_INFO, "portal-content-type: manager ready");
}
