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

#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <dirent.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/file.h>

#include "guiserver.h"
#include "qubes-gui-protocol.h"
#include "qubes-xorg-tray-defs.h"
#include "list.h"
#include "gui_common.h"
#include "server_common.h"
#include "xchan.h"

#define min(x, y)	((x) < (y) ? (x) : (y))

/* macro used to verify data from VM */
#define VERIFY(x) if (!(x)) {				       \
		if (ask_whether_verify_failed(g, __STRING(x))) \
			return;				       \
	}

/* ask user when VM sent invalid message */
static int ask_whether_verify_failed(Ghandles * g, const char *cond)
{
	g = g;
	/* XXX */
	fprintf(stderr, "%s %s\n", __func__, cond);
	return 1;
}

/* ask user when VM creates too many windows */
static void ask_whether_flooding(Ghandles * g)
{
	g = g;
	/* XXX */
	fprintf(stderr, "%s\n", __func__);
}

/* create local window - on VM request.
 * parameters are sanitized already
 */
static Window mkwindow(Ghandles * g, struct windowdata *vm_window)
{
	char *gargv[1] = { NULL };
	Window child_win;
	Window parent;
	XSizeHints my_size_hints;	/* hints for the window manager */
	Atom atom_label;

	my_size_hints.flags = PSize;
	my_size_hints.width = vm_window->width;
	my_size_hints.height = vm_window->height;

	if (vm_window->parent)
		parent = vm_window->parent->local_winid;
	else
		parent = g->root_win;
	// we will set override_redirect later, if needed
	child_win = XCreateSimpleWindow(g->display, parent,
					vm_window->x, vm_window->y,
					vm_window->width,
					vm_window->height, 0,
					BlackPixel(g->display, g->screen),
					WhitePixel(g->display, g->screen));
	/* pass my size hints to the window manager, along with window
	   and icon names */
	(void) XSetStandardProperties(g->display, child_win,
				      "VMapp command", "Pixmap", None,
				      gargv, 0, &my_size_hints);
	(void) XSelectInput(g->display, child_win,
			    ExposureMask | KeyPressMask | KeyReleaseMask |
			    ButtonPressMask | ButtonReleaseMask |
			    PointerMotionMask | EnterWindowMask | LeaveWindowMask |
			    FocusChangeMask | StructureNotifyMask | PropertyChangeMask);
	XSetWMProtocols(g->display, child_win, &g->wmDeleteMessage, 1);
	// Set '_QUBES_LABEL' property so that Window Manager can read it and draw proper decoration
	atom_label = XInternAtom(g->display, "_QUBES_LABEL", 0);
	XChangeProperty(g->display, child_win, atom_label, XA_INTEGER,
			32, PropModeReplace,
			(unsigned char *) &g->label_index, 1);

	// Set '_QUBES_VMNAME' property so that Window Manager can read it and nicely display it
	atom_label = XInternAtom(g->display, "_QUBES_VMNAME", 0);
	XChangeProperty(g->display, child_win, atom_label, XA_STRING,
			8 /* 8 bit is enough */ , PropModeReplace,
			(const unsigned char *) g->vmname,
			strlen(g->vmname));

	if (vm_window->remote_winid == FULLSCREEN_WINDOW_ID) {
		/* whole screen window */
		g->screen_window = vm_window;
	}

	return child_win;
}

/* undo the calculations that fix_docked_xy did, then perform move&resize */
static void moveresize_vm_window(Ghandles * g, struct windowdata *vm_window)
{
	int x = 0, y = 0;
	Window win;
	Atom act_type;
	long *frame_extents; // left, right, top, bottom
	unsigned long nitems, bytesleft;
	int ret, act_fmt;

	if (!vm_window->is_docked) {
		/* we have window content coordinates, but XMoveResizeWindow requires
		 * left top *border* pixel coordinates (if any border is present). */
		ret = XGetWindowProperty(g->display, vm_window->local_winid, g->frame_extents, 0, 4,
				False, XA_CARDINAL, &act_type, &act_fmt, &nitems, &bytesleft, (unsigned char**)&frame_extents);
		if (ret == Success && nitems == 4) {
			x = vm_window->x - frame_extents[0];
			y = vm_window->y - frame_extents[2];
			XFree(frame_extents);
		} else {
			/* assume no border */
			x = vm_window->x;
			y = vm_window->y;
		}
	} else
		if (!XTranslateCoordinates(g->display, g->root_win,
				      vm_window->local_winid, vm_window->x,
				      vm_window->y, &x, &y, &win))
			return;
	if (g->log_level > 1)
		fprintf(stderr,
			"XMoveResizeWindow local 0x%x remote 0x%x, xy %d %d (vm_window is %d %d) wh %d %d\n",
			(int) vm_window->local_winid,
			(int) vm_window->remote_winid, x, y, vm_window->x,
			vm_window->y, vm_window->width, vm_window->height);
	XMoveResizeWindow(g->display, vm_window->local_winid, x, y,
			  vm_window->width, vm_window->height);
}

/* force window to not hide its frame
 * checks if at least border_width is from every screen edge (and fix if not)
 * Exception: allow window to be entirely off the screen */
static int force_on_screen(Ghandles * g, struct windowdata *vm_window,
		    int border_width, const char *caller)
{
	int do_move = 0, reason = -1;
	int x = vm_window->x, y = vm_window->y, w = vm_window->width, h =
	    vm_window->height;

	if (vm_window->x < border_width
	    && vm_window->x + vm_window->width > 0) {
		vm_window->x = border_width;
		do_move = 1;
		reason = 1;
	}
	if (vm_window->y < border_width
	    && vm_window->y + vm_window->height > 0) {
		vm_window->y = border_width;
		do_move = 1;
		reason = 2;
	}
	if (vm_window->x < g->root_width &&
	    vm_window->x + (int)vm_window->width >
	    g->root_width - border_width) {
		vm_window->width =
		    g->root_width - vm_window->x - border_width;
		do_move = 1;
		reason = 3;
	}
	if (vm_window->y < g->root_height &&
	    vm_window->y + (int)vm_window->height >
	    g->root_height - border_width) {
		vm_window->height =
		    g->root_height - vm_window->y - border_width;
		do_move = 1;
		reason = 4;
	}
	if (do_move)
		if (g->log_level > 0)
			fprintf(stderr,
				"force_on_screen(from %s) returns 1 (reason %d): window 0x%x, xy %d %d, wh %d %d, root %d %d borderwidth %d\n",
				caller, reason,
				(int) vm_window->local_winid, x, y, w, h,
				g->root_width, g->root_height,
				border_width);
	return do_move;
}

/* handle VM message: MSG_CREATE
 * checks given attributes and create appropriate window in local Xserver
 * (using mkwindow) */
static void handle_create(Ghandles * g, XID window)
{
	struct windowdata *vm_window;
	struct genlist *l;
	struct msg_create untrusted_crt;
	XID parent;

	if (g->windows_count++ > g->windows_count_limit)
		ask_whether_flooding(g);
	vm_window =
	    (struct windowdata *) calloc(1, sizeof(struct windowdata));
	if (!vm_window) {
		perror("malloc(vm_window in handle_create)");
		exit(1);
	}
	/*
	   because of calloc vm_window->image = 0;
	   vm_window->is_mapped = 0;
	   vm_window->local_winid = 0;
	   vm_window->dest = vm_window->src = vm_window->pix = 0;
	 */
	read_struct(g->xchan, untrusted_crt);
	/* sanitize start */
	VERIFY((int) untrusted_crt.width >= 0
	       && (int) untrusted_crt.height >= 0);
	vm_window->width =
	    min((int) untrusted_crt.width, MAX_WINDOW_WIDTH);
	vm_window->height =
	    min((int) untrusted_crt.height, MAX_WINDOW_HEIGHT);
	/* there is no really good limits for x/y, so pass them to Xorg and hope
	 * that everything will be ok... */
	vm_window->x = untrusted_crt.x;
	vm_window->y = untrusted_crt.y;
	if (untrusted_crt.override_redirect)
		vm_window->override_redirect = 1;
	else
		vm_window->override_redirect = 0;
	parent = untrusted_crt.parent;
	/* sanitize end */
	vm_window->remote_winid = window;
	if (!list_insert(g->remote2local, window, vm_window)) {
		fprintf(stderr, "list_insert(g->remote2local failed\n");
		exit(1);
	}
	l = list_lookup(g->remote2local, parent);
	if (l)
		vm_window->parent = l->data;
	else
		vm_window->parent = NULL;
	vm_window->transient_for = NULL;
	vm_window->local_winid = mkwindow(g, vm_window);
	if (g->log_level > 0)
		fprintf(stderr,
			"Created 0x%x(0x%x) parent 0x%x(0x%x) ovr=%d x/y %d/%d w/h %d/%d\n",
			(int) vm_window->local_winid, (int) window,
			(int) (vm_window->parent ? vm_window->parent->
			       local_winid : 0), (unsigned) parent,
			vm_window->override_redirect,
			vm_window->x, vm_window->y,
			vm_window->width, vm_window->height);
	if (!list_insert
	    (g->wid2windowdata, vm_window->local_winid, vm_window)) {
		fprintf(stderr, "list_insert(g->wid2windowdata failed\n");
		exit(1);
	}

	/* do not allow to hide color frame off the screen */
	if (vm_window->override_redirect
	    && force_on_screen(g, vm_window, 0, "handle_create"))
		moveresize_vm_window(g, vm_window);
}

/* Obtain/release inter-vm lock
 * Used for handling shared Xserver memory and clipboard file */
static void inter_appviewer_lock(Ghandles *g, int mode)
{
	int cmd;

	cmd = mode ? LOCK_EX : LOCK_UN;
	if (flock(g->inter_appviewer_lock_fd, cmd) == -1)
		err(1, "lock");
}

/* release shared memory connected with given window */
static void release_mapped_mfns(Ghandles * g, struct windowdata *vm_window)
{
	inter_appviewer_lock(g, 1);
	g->shmcmd->shmid = vm_window->shminfo.shmid;
	XShmDetach(g->display, &vm_window->shminfo);
	XDestroyImage(vm_window->image);
	XSync(g->display, False);
	inter_appviewer_lock(g, 0);
	vm_window->image = NULL;
	if (shmctl(vm_window->shminfo.shmid, IPC_RMID, 0) == -1)
		warn("%s: shmctl", __func__);
}

/* handle VM message: MSG_DESTROY
 * destroy window locally, as requested */
static void handle_destroy(Ghandles * g, struct genlist *l)
{
	struct genlist *l2;
	struct windowdata *vm_window = l->data;
	g->windows_count--;
	if (vm_window == g->last_input_window)
		g->last_input_window = NULL;
	XDestroyWindow(g->display, vm_window->local_winid);
	if (g->log_level > 0)
		fprintf(stderr, " XDestroyWindow 0x%x\n",
			(int) vm_window->local_winid);
	if (vm_window->image)
		release_mapped_mfns(g, vm_window);
	l2 = list_lookup(g->wid2windowdata, vm_window->local_winid);
	list_remove(l);
	list_remove(l2);
	if (vm_window == g->screen_window)
		g->screen_window = NULL;
	free(vm_window);
}

/* handle VM message: MSG_MAP
 * Map a window with given parameters */
static void handle_map(Ghandles * g, struct windowdata *vm_window)
{
	struct genlist *trans;
	struct msg_map_info untrusted_txt;
	XSetWindowAttributes attr;

	read_struct(g->xchan, untrusted_txt);
	vm_window->is_mapped = 1;
	if (untrusted_txt.transient_for
	    && (trans =
		list_lookup(g->remote2local,
			    untrusted_txt.transient_for))) {
		struct windowdata *transdata = trans->data;
		vm_window->transient_for = transdata;
		XSetTransientForHint(g->display, vm_window->local_winid,
				     transdata->local_winid);
	} else
		vm_window->transient_for = NULL;

	vm_window->override_redirect = !!(untrusted_txt.override_redirect);
	attr.override_redirect = vm_window->override_redirect;
	XChangeWindowAttributes(g->display, vm_window->local_winid,
	                        CWOverrideRedirect, &attr);
	if (vm_window->override_redirect
	    && force_on_screen(g, vm_window, 0, "handle_map"))
		moveresize_vm_window(g, vm_window);

	(void) XMapWindow(g->display, vm_window->local_winid);
}

/* handle VM message: MSG_CONFIGURE
 * check if we like new dimensions/position and move relevant window */
static void handle_configure_from_vm(Ghandles * g, struct windowdata *vm_window)
{
	struct msg_configure untrusted_conf;
	int x, y;
	unsigned width, height, override_redirect;
	int conf_changed;

	read_struct(g->xchan, untrusted_conf);
	if (g->log_level > 1)
		fprintf(stderr,
			"handle_configure_from_vm, local 0x%x remote 0x%x, %d/%d, was"
			" %d/%d, ovr=%d, xy %d/%d, was %d/%d\n",
			(int) vm_window->local_winid,
			(int) vm_window->remote_winid,
			untrusted_conf.width, untrusted_conf.height,
			vm_window->width, vm_window->height,
			untrusted_conf.override_redirect, untrusted_conf.x,
			untrusted_conf.y, vm_window->x, vm_window->y);
	/* sanitize start */
	if (untrusted_conf.width > MAX_WINDOW_WIDTH)
		untrusted_conf.width = MAX_WINDOW_WIDTH;
	if (untrusted_conf.height > MAX_WINDOW_HEIGHT)
		untrusted_conf.height = MAX_WINDOW_HEIGHT;
	width = untrusted_conf.width;
	height = untrusted_conf.height;
	VERIFY(width > 0 && height > 0);
	if (untrusted_conf.override_redirect > 0)
		override_redirect = 1;
	else
		override_redirect = 0;
	/* there is no really good limits for x/y, so pass them to Xorg and hope
	 * that everything will be ok... */
	x = untrusted_conf.x;
	y = untrusted_conf.y;
	/* sanitize end */
	if (vm_window->width != width || vm_window->height != height ||
	    vm_window->x != x || vm_window->y != y)
		conf_changed = 1;
	else
		conf_changed = 0;
	vm_window->override_redirect = override_redirect;

	/* We do not allow a docked window to change its size, period. */
	if (vm_window->is_docked) {
		if (conf_changed)
			send_configure(g->xchan, vm_window, vm_window->x,
				       vm_window->y, vm_window->width,
				       vm_window->height);
		vm_window->have_queued_configure = 0;
		return;
	}


	if (vm_window->have_queued_configure) {
		if (conf_changed) {
			send_configure(g->xchan, vm_window, vm_window->x,
				       vm_window->y, vm_window->width,
				       vm_window->height);
			return;
		} else {
			// same dimensions; this is an ack for our previously sent configure req
			vm_window->have_queued_configure = 0;
		}
	}
	if (!conf_changed)
		return;
	vm_window->width = width;
	vm_window->height = height;
	vm_window->x = x;
	vm_window->y = y;
	if (vm_window->override_redirect)
		// do not let menu window hide its color frame by moving outside of the screen
		// if it is located offscreen, then allow negative x/y
		force_on_screen(g, vm_window, 0,
				"handle_configure_from_vm");
	moveresize_vm_window(g, vm_window);
}

/* validate single UTF-8 character
 * return bytes count of this character, or 0 if the character is invalid */
static int validate_utf8_char(unsigned char *untrusted_c) {
	int tails_count = 0;
	int total_size = 0;
	/* it is safe to access byte pointed by the parameter and the next one
	 * (which can be terminating NULL), but every next byte can access only if
	 * neither of previous bytes was NULL
	 */

	/* According to http://www.ietf.org/rfc/rfc3629.txt:
	 *   UTF8-char   = UTF8-1 / UTF8-2 / UTF8-3 / UTF8-4
	 *   UTF8-1      = %x00-7F
	 *   UTF8-2      = %xC2-DF UTF8-tail
	 *   UTF8-3      = %xE0 %xA0-BF UTF8-tail / %xE1-EC 2( UTF8-tail ) /
	 *                 %xED %x80-9F UTF8-tail / %xEE-EF 2( UTF8-tail )
	 *   UTF8-4      = %xF0 %x90-BF 2( UTF8-tail ) / %xF1-F3 3( UTF8-tail ) /
	 *                 %xF4 %x80-8F 2( UTF8-tail )
	 *   UTF8-tail   = %x80-BF
	 */

	if (*untrusted_c <= 0x7F) {
		return 1;
	} else if (*untrusted_c >= 0xC2 && *untrusted_c <= 0xDF) {
		total_size = 2;
		tails_count = 1;
	} else switch (*untrusted_c) {
		case 0xE0:
			untrusted_c++;
			total_size = 3;
			if (*untrusted_c >= 0xA0 && *untrusted_c <= 0xBF)
				tails_count = 1;
			else
				return 0;
			break;
		case 0xE1: case 0xE2: case 0xE3: case 0xE4:
		case 0xE5: case 0xE6: case 0xE7: case 0xE8:
		case 0xE9: case 0xEA: case 0xEB: case 0xEC:
			/* 0xED */
		case 0xEE:
		case 0xEF:
			total_size = 3;
			tails_count = 2;
			break;
		case 0xED:
			untrusted_c++;
			total_size = 3;
			if (*untrusted_c >= 0x80 && *untrusted_c <= 0x9F)
				tails_count = 1;
			else
				return 0;
			break;
		case 0xF0:
			untrusted_c++;
			total_size = 4;
			if (*untrusted_c >= 0x90 && *untrusted_c <= 0xBF)
				tails_count = 2;
			else
				return 0;
			break;
		case 0xF1:
		case 0xF2:
		case 0xF3:
			total_size = 4;
			tails_count = 3;
			break;
		case 0xF4:
			untrusted_c++;
			if (*untrusted_c >= 0x80 && *untrusted_c <= 0x8F)
				tails_count = 2;
			else
				return 0;
			break;
		default:
			return 0;
	}

	while (tails_count-- > 0) {
		untrusted_c++;
		if (!(*untrusted_c >= 0x80 && *untrusted_c <= 0xBF))
			return 0;
	}
	return total_size;
}

/* replace non-printable characters with '_'
 * given string must be NULL terminated already */
static void sanitize_string_from_vm(unsigned char *untrusted_s, int allow_utf8)
{
	int utf8_ret;
	for (; *untrusted_s; untrusted_s++) {
		// allow only non-control ASCII chars
		if (*untrusted_s >= 0x20 && *untrusted_s <= 0x7E)
			continue;
		if (allow_utf8 && *untrusted_s >= 0x80) {
			utf8_ret = validate_utf8_char(untrusted_s);
			if (utf8_ret > 0) {
				/* loop will do one additional increment */
				untrusted_s += utf8_ret - 1;
				continue;
			}
		}
		*untrusted_s = '_';
	}
}

/* handle VM message: MSG_VMNAME
 * remove non-printable characters and pass to X server */
static void handle_wmname(Ghandles * g, struct windowdata *vm_window)
{
	XTextProperty text_prop;
	struct msg_wmname untrusted_msg;
	char buf[sizeof(untrusted_msg.data)];
	char *list[1] = { buf };

	read_struct(g->xchan, untrusted_msg);
	/* sanitize start */
	untrusted_msg.data[sizeof(untrusted_msg.data) - 1] = 0;
	sanitize_string_from_vm((unsigned char *) (untrusted_msg.data),
				g->allow_utf8_titles);
	snprintf(buf, sizeof(buf), "%s", untrusted_msg.data);
	/* sanitize end */
	if (g->log_level > 1)
		fprintf(stderr, "set title for window 0x%x\n",
			(int) vm_window->local_winid);
	Xutf8TextListToTextProperty(g->display, list, 1, XUTF8StringStyle,
				    &text_prop);
	XSetWMName(g->display, vm_window->local_winid, &text_prop);
	XSetWMIconName(g->display, vm_window->local_winid, &text_prop);
	XFree(text_prop.value);
}

/* handle VM message: MSG_WMHINTS
 * Pass hints for window manager to local X server */
static void handle_wmhints(Ghandles * g, struct windowdata *vm_window)
{
	struct msg_window_hints untrusted_msg;
	XSizeHints size_hints;

	memset(&size_hints, 0, sizeof(size_hints));

	read_struct(g->xchan, untrusted_msg);

	/* sanitize start */
	size_hints.flags = 0;
	/* check every value and pass it only when sane */
	if ((untrusted_msg.flags & PMinSize)
	    && untrusted_msg.min_width <= MAX_WINDOW_WIDTH
	    && untrusted_msg.min_height <= MAX_WINDOW_HEIGHT) {
		size_hints.flags |= PMinSize;
		size_hints.min_width = untrusted_msg.min_width;
		size_hints.min_height = untrusted_msg.min_height;
	} else
		fprintf(stderr, "invalid PMinSize for 0x%x (%d/%d)\n",
			(int) vm_window->local_winid,
			untrusted_msg.min_width, untrusted_msg.min_height);
	if ((untrusted_msg.flags & PMaxSize) && untrusted_msg.max_width > 0
	    && untrusted_msg.max_width <= MAX_WINDOW_WIDTH
	    && untrusted_msg.max_height > 0
	    && untrusted_msg.max_height <= MAX_WINDOW_HEIGHT) {
		size_hints.flags |= PMaxSize;
		size_hints.max_width = untrusted_msg.max_width;
		size_hints.max_height = untrusted_msg.max_height;
	} else
		fprintf(stderr, "invalid PMaxSize for 0x%x (%d/%d)\n",
			(int) vm_window->local_winid,
			untrusted_msg.max_width, untrusted_msg.max_height);
	if ((untrusted_msg.flags & PResizeInc) && size_hints.width_inc >= 0
	    && size_hints.width_inc < MAX_WINDOW_WIDTH
	    && size_hints.height_inc >= 0
	    && size_hints.height_inc < MAX_WINDOW_HEIGHT) {
		size_hints.flags |= PResizeInc;
		size_hints.width_inc = untrusted_msg.width_inc;
		size_hints.height_inc = untrusted_msg.height_inc;
	} else
		fprintf(stderr, "invalid PResizeInc for 0x%x (%d/%d)\n",
			(int) vm_window->local_winid,
			untrusted_msg.width_inc, untrusted_msg.height_inc);
	if ((untrusted_msg.flags & PBaseSize) && size_hints.base_width >= 0
	    && size_hints.base_width <= MAX_WINDOW_WIDTH
	    && size_hints.base_height >= 0
	    && size_hints.base_height <= MAX_WINDOW_HEIGHT) {
		size_hints.flags |= PBaseSize;
		size_hints.base_width = untrusted_msg.base_width;
		size_hints.base_height = untrusted_msg.base_height;
	} else
		fprintf(stderr, "invalid PBaseSize for 0x%x (%d/%d)\n",
			(int) vm_window->local_winid,
			untrusted_msg.base_width,
			untrusted_msg.base_height);
	/* sanitize end */

	if (g->log_level > 1)
		fprintf(stderr,
			"set WM_NORMAL_HINTS for window 0x%x to min=%d/%d, max=%d/%d, base=%d/%d, inc=%d/%d (flags 0x%x)\n",
			(int) vm_window->local_winid, size_hints.min_width,
			size_hints.min_height, size_hints.max_width,
			size_hints.max_height, size_hints.base_width,
			size_hints.base_height, size_hints.width_inc,
			size_hints.height_inc, (int) size_hints.flags);
	XSetWMNormalHints(g->display, vm_window->local_winid, &size_hints);
}

/* handle VM message: MSG_WINDOW_FLAGS
 * Pass window state flags for window manager to local X server */
static void handle_wmflags(Ghandles * g, struct windowdata *vm_window)
{
	struct msg_window_flags untrusted_msg;
	struct msg_window_flags msg;

	read_struct(g->xchan, untrusted_msg);

	/* sanitize start */
	VERIFY((untrusted_msg.flags_set & untrusted_msg.flags_unset) == 0);
	msg.flags_set = untrusted_msg.flags_set & (WINDOW_FLAG_FULLSCREEN | WINDOW_FLAG_DEMANDS_ATTENTION);
	msg.flags_unset = untrusted_msg.flags_unset & (WINDOW_FLAG_FULLSCREEN | WINDOW_FLAG_DEMANDS_ATTENTION);
	/* sanitize end */

	if (!vm_window->is_mapped) {
		/* for unmapped windows, set property directly; only "set" list is
		 * processed (will override property) */
		Atom state_list[10];
		int i = 0;

		vm_window->flags_set = 0;
		if (msg.flags_set & WINDOW_FLAG_FULLSCREEN) {
			if (g->allow_fullscreen) {
				vm_window->flags_set |= WINDOW_FLAG_FULLSCREEN;
				state_list[i++] = g->wm_state_fullscreen;
			} else {
				/* if fullscreen not allowed, substitute request with maximize */
				state_list[i++] = XInternAtom(g->display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
				state_list[i++] = XInternAtom(g->display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
			}
		}
		if (msg.flags_set & WINDOW_FLAG_DEMANDS_ATTENTION) {
			vm_window->flags_set |= WINDOW_FLAG_DEMANDS_ATTENTION;
			state_list[i++] = g->wm_state_demands_attention;
		}
		if (i > 0) {
			/* FIXME: error checking? */
			XChangeProperty(g->display, vm_window->local_winid, g->wm_state,
					XA_ATOM, 32, PropModeReplace, (unsigned char*)state_list,
					i);
		} else
			/* just in case */
			XDeleteProperty(g->display, vm_window->local_winid, g->wm_state);
	} else {
		/* for mapped windows, send message to window manager (via root window) */
		XClientMessageEvent ev;
		uint32_t flags_all = msg.flags_set | msg.flags_unset;

		if (!flags_all)
			/* no change requested */
			return;

		memset(&ev, 0, sizeof(ev));
		ev.type = ClientMessage;
		ev.display = g->display;
		ev.window = vm_window->local_winid;
		ev.message_type = g->wm_state;
		ev.format = 32;
		ev.data.l[3] = 1; /* source indication: normal application */

		/* ev.data.l[0]: 1 - add/set property, 0 - remove/unset property */
		if (flags_all & WINDOW_FLAG_FULLSCREEN) {
			ev.data.l[0] = (msg.flags_set & WINDOW_FLAG_FULLSCREEN) ? 1 : 0;
			if (g->allow_fullscreen) {
				ev.data.l[1] = g->wm_state_fullscreen;
				ev.data.l[2] = 0;
			} else {
				ev.data.l[1] = XInternAtom(g->display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
				ev.data.l[2] = XInternAtom(g->display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
			}
			XSendEvent(g->display, g->root_win, False,
					(SubstructureNotifyMask|SubstructureRedirectMask),
					(XEvent*) &ev);
		}
		if (msg.flags_set & WINDOW_FLAG_DEMANDS_ATTENTION) {
			ev.data.l[0] = (msg.flags_set & WINDOW_FLAG_DEMANDS_ATTENTION) ? 1 : 0;
			ev.data.l[1] = g->wm_state_demands_attention;
			ev.data.l[2] = 0;
			XSendEvent(g->display, g->root_win, False,
					(SubstructureNotifyMask|SubstructureRedirectMask),
					(XEvent*) &ev);
		}
	}
}

/* handle VM message: MSG_DOCK
 * Try to dock window in the tray
 * Rest of XEMBED protocol is catched in VM */
static void handle_dock(Ghandles * g, struct windowdata *vm_window)
{
	Window tray;
	if (g->log_level > 0)
		fprintf(stderr, "docking window 0x%x\n",
			(int) vm_window->local_winid);
	tray = XGetSelectionOwner(g->display, g->tray_selection);
	if (tray != None) {
		long data[2];
		XClientMessageEvent msg;

		data[0] = 0;
		data[1] = 1;
		XChangeProperty(g->display, vm_window->local_winid,
				g->xembed_info, g->xembed_info, 32,
				PropModeReplace, (unsigned char *) data,
				2);

		memset(&msg, 0, sizeof(msg));
		msg.type = ClientMessage;
		msg.window = tray;
		msg.message_type = g->tray_opcode;
		msg.format = 32;
		msg.data.l[0] = CurrentTime;
		msg.data.l[1] = SYSTEM_TRAY_REQUEST_DOCK;
		msg.data.l[2] = vm_window->local_winid;
		msg.display = g->display;
		XSendEvent(msg.display, msg.window, False, NoEventMask,
			(XEvent *) & msg);
	}
	vm_window->is_docked = 1;
}

/* handle VM message: MSG_SHMIMAGE
 * pass message data to do_shm_update - there input validation will be done */
static void handle_shmimage(Ghandles * g, struct windowdata *vm_window)
{
	struct msg_shmimage untrusted_mx;

	read_struct(g->xchan, untrusted_mx);
	if (!vm_window->is_mapped)
		return;

	DBG1("shmimage for 0x%x(remote 0x%x), x: %d, y: %d, w: %d, h: %d\n",
		(int)vm_window->local_winid, (int) vm_window->remote_winid,
		untrusted_mx.x, untrusted_mx.y, untrusted_mx.width,
		untrusted_mx.height);

	/* WARNING: passing raw values, input validation is done inside of
	 * do_shm_update */
	do_shm_update(g, vm_window, untrusted_mx.x, untrusted_mx.y,
		untrusted_mx.width, untrusted_mx.height);
}

/* handle VM message: MSG_MFNDUMP
 * Retrieve memory addresses connected with composition buffer of remote window
 */
static void handle_mfndump(Ghandles * g, struct windowdata *vm_window)
{
	char untrusted_shmcmd_data_from_remote[4096 * SHM_CMD_NUM_PAGES];
	struct shm_cmd *untrusted_shmcmd;
	size_t cmd_size, mfns_size, size;
	static char dummybuf[100];
	unsigned num_mfn, off;

	untrusted_shmcmd = (struct shm_cmd *)untrusted_shmcmd_data_from_remote;

	if (vm_window->image)
		release_mapped_mfns(g, vm_window);

	read_data(g->xchan, untrusted_shmcmd_data_from_remote,
		sizeof(struct shm_cmd));

	DBG1("MSG_MFNDUMP for 0x%x(0x%x): %dx%d, num_mfn 0x%x off 0x%x\n",
		(int)vm_window->local_winid, (int) vm_window->remote_winid,
		untrusted_shmcmd->width, untrusted_shmcmd->height,
		untrusted_shmcmd->num_mfn, untrusted_shmcmd->off);

	/* sanitize start */
	VERIFY(untrusted_shmcmd->num_mfn <= MAX_MFN_COUNT);
	num_mfn = untrusted_shmcmd->num_mfn;
	VERIFY((int)untrusted_shmcmd->width >= 0
		&& (int)untrusted_shmcmd->height >= 0);
	VERIFY((int)untrusted_shmcmd->width <= MAX_WINDOW_WIDTH
		&& (int)untrusted_shmcmd->height <= MAX_WINDOW_HEIGHT);
	VERIFY(untrusted_shmcmd->off < 4096);
	off = untrusted_shmcmd->off;
	/* unused for now: VERIFY(untrusted_shmcmd->bpp == 24); */
	/* sanitize end */

	vm_window->image_width = untrusted_shmcmd->width;
	vm_window->image_height = untrusted_shmcmd->height;/* sanitized above */

	mfns_size = SIZEOF_SHARED_MFN * num_mfn;
	read_data(g->xchan, (char *)untrusted_shmcmd->mfns, mfns_size);
	vm_window->image = XShmCreateImage(g->display,
					DefaultVisual(g->display, g->screen), 24,
					ZPixmap, NULL, &vm_window->shminfo,
					vm_window->image_width,
					vm_window->image_height);
	if (!vm_window->image) {
		perror("XShmCreateImage");
		exit(1);
	}

	/* the below sanity check must be AFTER XShmCreateImage, it uses
	 * vm_window->image */
	size = vm_window->image->bytes_per_line * vm_window->image->height + off;
	if (num_mfn * 4096 < size) {
		fprintf(stderr,
			"handle_mfndump for window 0x%x(remote 0x%x)"
			" got too small num_mfn= 0x%x\n",
			(int) vm_window->local_winid,
			(int) vm_window->remote_winid, num_mfn);
		exit(1);
	}

	// temporary shmid; see shmoverride/README
	vm_window->shminfo.shmid = shmget(IPC_PRIVATE, 1, IPC_CREAT | 0700);
	if (vm_window->shminfo.shmid < 0)
		err(1, "shmget");

	/* ensure that _every_ not sanitized field is overrided by some trusted
	 * value */
	untrusted_shmcmd->shmid = vm_window->shminfo.shmid;
	untrusted_shmcmd->capsule_id = g->capsule_id;

	inter_appviewer_lock(g, 1);
	cmd_size = sizeof(struct shm_cmd) + mfns_size;
	memcpy(g->shmcmd, untrusted_shmcmd_data_from_remote, cmd_size);
	size = 4096 * SHM_CMD_NUM_PAGES;
	if (cmd_size < size) {
		size -= mfns_size + sizeof(struct shm_cmd);
		memset((char *)g->shmcmd->mfns + mfns_size, 0, size);
	}

	vm_window->shminfo.shmaddr = dummybuf;
	vm_window->image->data = dummybuf;
	vm_window->shminfo.readOnly = True;
	XSync(g->display, False);
	if (!XShmAttach(g->display, &vm_window->shminfo)) {
		fprintf(stderr,
			"XShmAttach failed for window 0x%x(remote 0x%x)\n",
			(int) vm_window->local_winid,
			(int) vm_window->remote_winid);
	}
	XSync(g->display, False);
	g->shmcmd->shmid = g->cmd_shmid;
	inter_appviewer_lock(g, 0);
}

/* VM message dispatcher
 * return false if no message is available, true otherwise */
bool handle_message(Ghandles * g)
{
	struct msg_hdr untrusted_hdr;
	uint32_t type;
	XID window = 0;
	struct genlist *l;
	struct windowdata *vm_window = NULL;
	unsigned char *p;
	size_t n, size;
	err_t error;

	error = xchan_recv_nopoll(g->xchan, &untrusted_hdr,
				  sizeof(untrusted_hdr), &size);
	if (error) {
		print_error(error, "failed to receive message header");
		exit(EXIT_FAILURE);
	}

	if (size == 0) {
		/* no data available */
		return false;
	}

	while (size != sizeof(untrusted_hdr)) {
		p = (unsigned char *)&untrusted_hdr + size;
		error = xchan_recv(g->xchan, p, sizeof(untrusted_hdr)-size, &n);
		if (error) {
			print_error(error, "failed to receive message header");
			exit(EXIT_FAILURE);
		}
		size += n;
	}

	if (!(untrusted_hdr.type > MSG_MIN && untrusted_hdr.type < MSG_MAX)) {
		if (ask_whether_verify_failed(g, "untrusted_hdr.type > MSG_MIN && untrusted_hdr.type < MSG_MAX"))
			return true;
	}

	/* sanitized msg type */
	type = untrusted_hdr.type;
	if (type == MSG_CLIPBOARD_DATA) {
		/* window field has special meaning here */
		/* XXX */
		//handle_clipboard_data(g, untrusted_hdr.window);
		return true;
	}

	l = list_lookup(g->remote2local, untrusted_hdr.window);
	if (type == MSG_CREATE) {
		if (l) {
			fprintf(stderr,
				"CREATE for already existing window id 0x%x?\n",
				untrusted_hdr.window);
			exit(1);
		}
		window = untrusted_hdr.window;
	} else {
		if (!l) {
			fprintf(stderr,
				"msg 0x%x without CREATE for 0x%x\n",
				type, untrusted_hdr.window);
			exit(1);
		}
		vm_window = l->data;
		/* not needed as it is in vm_window struct
		   window = untrusted_hdr.window;
		 */
	}

	switch (type) {
	case MSG_CREATE:
		handle_create(g, window);
		break;
	case MSG_DESTROY:
		handle_destroy(g, l);
		break;
	case MSG_MAP:
		handle_map(g, vm_window);
		break;
	case MSG_UNMAP:
		vm_window->is_mapped = 0;
		(void) XUnmapWindow(g->display, vm_window->local_winid);
		break;
	case MSG_CONFIGURE:
		handle_configure_from_vm(g, vm_window);
		break;
	case MSG_MFNDUMP:
		handle_mfndump(g, vm_window);
		break;
	case MSG_SHMIMAGE:
		handle_shmimage(g, vm_window);
		break;
	case MSG_WMNAME:
		handle_wmname(g, vm_window);
		break;
	case MSG_DOCK:
		handle_dock(g, vm_window);
		break;
	case MSG_WINDOW_HINTS:
		handle_wmhints(g, vm_window);
		break;
	case MSG_WINDOW_FLAGS:
		handle_wmflags(g, vm_window);
		break;
	default:
		fprintf(stderr, "got unknown msg type %d\n", type);
		exit(1);
	}

	return true;
}

// vim: noet:ts=8:
