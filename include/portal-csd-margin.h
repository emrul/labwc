/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_PORTAL_CSD_MARGIN_H
#define LABWC_PORTAL_CSD_MARGIN_H

#include <stdbool.h>

struct server;

/*
 * Returns true if ANY mapped view's window (visible content geometry)
 * contains the cursor at its current position.
 *
 * Used as a guard before try_csd_margin_resize_context() in
 * get_cursor_context().  Upstream labwc walks views back-to-front and
 * returns the FIRST view whose CSD shadow margin contains the cursor
 * — with no check whether some other view's content covers the same
 * pixel.  When a CSD-decorated remote-desktop window (e.g. chromium
 * with --enable-features=WaylandWindowDecorations) sits behind a
 * front window that visually covers chromium's shadow band, the
 * upstream logic returns chromium's resize-edge context anyway:
 *   - cursor shape is wrong (resize-e when over the front window)
 *   - clicks at that pixel start chromium's resize, not the front
 *     window's input
 *
 * Skipping the CSD margin walk when any view's visible content
 * already claims the pixel is the minimal fix: the scene-tree walk
 * that runs immediately after produces the right surface match.
 */
bool
portal_any_view_window_contains_cursor(struct server *server);

#endif /* LABWC_PORTAL_CSD_MARGIN_H */
