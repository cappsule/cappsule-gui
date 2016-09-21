/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 * Copyright (C) 2010  Joanna Rutkowska <joanna@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>

#include "guiserver.h"
#include "qubes-gui-protocol.h"
#include "gui_common.h"

#define BORDER_WIDTH	2
#define min(x, y)	((x) < (y) ? (x) : (y))
#define max(x, y)	((x) > (y) ? (x) : (y))


/* send configure request for specified VM window */
void send_configure(struct xchan *xchan, struct windowdata *vm_window,
		    int x, int y, int w, int h)
{
	struct msg_configure msg;
	struct msg_hdr hdr;

	hdr.type = MSG_CONFIGURE;
	hdr.window = vm_window->remote_winid;
	msg.height = h;
	msg.width = w;
	msg.x = x;
	msg.y = y;
	write_message(xchan, hdr, msg);
}

/* update given fragment of window image
 * can be requested by VM (MSG_SHMIMAGE) and Xserver (XExposeEvent)
 * parameters are not sanitized earlier - we must check it carefully
 * also do not let to cover forced colorful frame (for undecoraded windows)
 */
void do_shm_update(Ghandles * g, struct windowdata *vm_window,
		int untrusted_x, int untrusted_y, int untrusted_w,
		int untrusted_h)
{
	int border_width = BORDER_WIDTH;
	int x = 0, y = 0, w = 0, h = 0;

	/* sanitize start */
	if (untrusted_x < 0 || untrusted_y < 0) {
		DBG1("do_shm_update for 0x%x(remote 0x%x), x=%d, y=%d, w=%d, h=%d ?\n",
			(int)vm_window->local_winid,
			(int)vm_window->remote_winid, untrusted_x,
			untrusted_y, untrusted_w, untrusted_h);
		return;
	}
	if (vm_window->image) {
		x = min(untrusted_x, vm_window->image_width);
		y = min(untrusted_y, vm_window->image_height);
		w = min(max(untrusted_w, 0), vm_window->image_width - x);
		h = min(max(untrusted_h, 0), vm_window->image_height - y);
	} else if (g->screen_window) {
		/* update only onscreen window part */
		if (vm_window->x >= g->screen_window->image_width ||
			vm_window->y >= g->screen_window->image_height)
			return;
		if (vm_window->x+untrusted_x < 0)
			untrusted_x = -vm_window->x;
		if (vm_window->y+untrusted_y < 0)
			untrusted_y = -vm_window->y;
		x = min(untrusted_x, g->screen_window->image_width - vm_window->x);
		y = min(untrusted_y, g->screen_window->image_height - vm_window->y);
		w = min(max(untrusted_w, 0), g->screen_window->image_width - vm_window->x - x);
		h = min(max(untrusted_h, 0), g->screen_window->image_height - vm_window->y - y);
	}
	/* else: no image to update, will return after possibly drawing a frame */

	/* sanitize end */

	if (!vm_window->override_redirect) {
		// Window Manager will take care of the frame...
		border_width = 0;
	}

	if (vm_window->is_docked) {
		border_width = 1;
	}

	int do_border = 0;
	int delta, i;
	/* window contains only (forced) frame, so no content to update */
	if ((int)vm_window->width <= border_width * 2
		|| (int)vm_window->height <= border_width * 2) {
		XFillRectangle(g->display, vm_window->local_winid,
			g->frame_gc, 0, 0,
			vm_window->width,
			vm_window->height);
		return;
	}
	if (!vm_window->image && !(g->screen_window && g->screen_window->image))
		return;
	/* force frame to be visible: */
	/*   * left */
	delta = border_width - x;
	if (delta > 0) {
		w -= delta;
		x = border_width;
		do_border = 1;
	}
	/*   * right */
	delta = x + w - (vm_window->width - border_width);
	if (delta > 0) {
		w -= delta;
		do_border = 1;
	}
	/*   * top */
	delta = border_width - y;
	if (delta > 0) {
		h -= delta;
		y = border_width;
		do_border = 1;
	}
	/*   * bottom */
	delta = y + h - (vm_window->height - border_width);
	if (delta > 0) {
		h -= delta;
		do_border = 1;
	}

	/* again check if something left to update */
	if (w <= 0 || h <= 0)
		return;

	DBG1("  do_shm_update for 0x%x(remote 0x%x), after border calc: x=%d, y=%d, w=%d, h=%d\n",
		(int)vm_window->local_winid,
		(int)vm_window->remote_winid,
		x, y, w, h);

#ifdef FILL_TRAY_BG
	if (vm_window->is_docked) {
		char *data, *datap;
		size_t data_sz;
		int xp, yp;

		if (!vm_window->image) {
			/* TODO: implement screen_window handling */
			return;
		}
		/* allocate image_width _bits_ for each image line */
		data_sz =
			(vm_window->image_width / 8 +
				1) * vm_window->image_height;
		data = datap = calloc(1, data_sz);
		if (!data) {
			perror("malloc(%dx%x -> %zu\n",
				vm_window->image_width, vm_window->image_height, data_sz);
			exit(1);
		}

		/* Create local pixmap, put vmside image to it
		 * then get local image of the copy.
		 * This is needed because XGetPixel does not seem to work
		 * with XShmImage data.
		 *
		 * Always use 0,0 w+x,h+y coordinates to generate proper mask. */
		w = w + x;
		h = h + y;
		if (w > vm_window->image_width)
			w = vm_window->image_width;
		if (h > vm_window->image_height)
			h = vm_window->image_height;
		Pixmap pixmap =
			XCreatePixmap(g->display, vm_window->local_winid,
				vm_window->image_width,
				vm_window->image_height,
				24);
		XShmPutImage(g->display, pixmap, g->context,
			vm_window->image, 0, 0, 0, 0,
			vm_window->image_width,
			vm_window->image_height, 0);
		XImage *image = XGetImage(g->display, pixmap, 0, 0, w, h,
					0xFFFFFFFF, ZPixmap);
		/* Use top-left corner pixel color as transparency color */
		unsigned long back = XGetPixel(image, 0, 0);
		/* Generate data for transparency mask Bitmap */
		for (yp = 0; yp < h; yp++) {
			int step = 0;
			for (xp = 0; xp < w; xp++) {
				if (datap - data >= data_sz) {
					fprintf(stderr,
						"Impossible internal error\n");
					exit(1);
				}
				if (XGetPixel(image, xp, yp) != back)
					*datap |= 1 << (step % 8);
				if (step % 8 == 7)
					datap++;
				step++;
			}
			/* ensure that new line will start at new byte */
			if ((step - 1) % 8 != 7)
				datap++;
		}
		Pixmap mask = XCreateBitmapFromData(g->display,
						vm_window->local_winid,
						data, w, h);
		/* set trayicon background to white color */
		XFillRectangle(g->display, vm_window->local_winid,
			g->tray_gc, 0, 0, vm_window->width,
			vm_window->height);
		/* Paint clipped Image */
		XSetClipMask(g->display, g->context, mask);
		XPutImage(g->display, vm_window->local_winid,
			g->context, image, 0, 0, 0, 0, w, h);
		/* Remove clipping */
		XSetClipMask(g->display, g->context, None);

		XFreePixmap(g->display, mask);
		XDestroyImage(image);
		XFreePixmap(g->display, pixmap);
		free(data);
		return;
	} else
#endif
	{
		if (vm_window->image) {
			XShmPutImage(g->display, vm_window->local_winid,
				g->context, vm_window->image, x,
				y, x, y, w, h, 0);
		} else {
			XShmPutImage(g->display, vm_window->local_winid,
				g->context, g->screen_window->image, vm_window->x+x,
				vm_window->y+y, x, y, w, h, 0);
		}
	}

	if (!do_border)
		return;

	for (i = 0; i < border_width; i++) {
		XDrawRectangle(g->display, vm_window->local_winid,
			g->frame_gc, i, i,
			vm_window->width - 1 - 2 * i,
			vm_window->height - 1 - 2 * i);
	}
}

// vim: noet:ts=8:
