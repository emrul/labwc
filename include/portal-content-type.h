/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_PORTAL_CONTENT_TYPE_H
#define LABWC_PORTAL_CONTENT_TYPE_H

#include <stdint.h>

struct server;
struct view;

enum portal_content_type_mask {
	PORTAL_CONTENT_TYPE_PHOTO = 1 << 0,
	PORTAL_CONTENT_TYPE_VIDEO = 1 << 1,
	PORTAL_CONTENT_TYPE_GAME = 1 << 2,
};

void
portal_content_type_manager_create(struct server *server);

uint32_t
portal_content_type_view_mask(struct view *view, uint32_t *video_surface_count);

#endif /* LABWC_PORTAL_CONTENT_TYPE_H */
