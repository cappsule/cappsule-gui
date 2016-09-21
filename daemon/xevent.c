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
#include <stdlib.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <execinfo.h>

#include "qubes-xorg-tray-defs.h"
#include "qubes-gui-protocol.h"
#include "gui_common.h"
#include "guiserver.h"
#include "server_common.h"
#include "list.h"

/* short macro for beginning of each xevent handling function
 * checks if this window is managed by guid and declares windowdata struct
 * pointer */
#define CHECK_NONMANAGED_WINDOW(g, id) struct windowdata *vm_window; \
	if (!(vm_window=check_nonmanaged_window(g, id))) return

static uint32_t flags_from_atom(Ghandles * g, Atom a) {
	if (a == g->wm_state_fullscreen)
		return WINDOW_FLAG_FULLSCREEN;
	else if (a == g->wm_state_demands_attention)
		return WINDOW_FLAG_DEMANDS_ATTENTION;
	else {
		/* ignore unsupported states */
	}
	return 0;
}

static int is_special_keypress(Ghandles * g, const XKeyEvent * ev, XID remote_winid)
{
	g = g;
	ev = ev;
	remote_winid = remote_winid;

	return 0;
}

/* find if window (given by id) is managed by this guid */
static struct windowdata *check_nonmanaged_window(Ghandles * g, XID id)
{
	struct genlist *item = list_lookup(g->wid2windowdata, id);
	if (!item) {
		if (g->log_level > 0)
			fprintf(stderr, "cannot lookup 0x%x in wid2windowdata\n",
					(int) id);
		return NULL;
	}
	return item->data;
}

/* fix position of docked tray icon;
 * icon position is relative to embedder 0,0 so we must translate it to
 * absolute position */
static int fix_docked_xy(Ghandles * g, struct windowdata *vm_window, const char *caller)
{

	/* docked window is reparented to root_win on vmside */
	Window win;
	int x, y, ret = 0;
	if (XTranslateCoordinates
	    (g->display, vm_window->local_winid, g->root_win,
	     0, 0, &x, &y, &win) == True) {
		/* ignore offscreen coordinates */
		if (x < 0 || y < 0)
			x = y = 0;
		if (vm_window->x != x || vm_window->y != y)
			ret = 1;
		if (g->log_level > 1)
			fprintf(stderr,
				"fix_docked_xy(from %s), calculated xy %d/%d, was "
				"%d/%d\n", caller, x, y, vm_window->x,
				vm_window->y);
		vm_window->x = x;
		vm_window->y = y;
	}
	return ret;
}

/* handle local Xserver event: XKeyEvent
 * send it to relevant window in VM
 */
static void process_xevent_keypress(Ghandles * g, const XKeyEvent * ev)
{
	struct msg_hdr hdr;
	struct msg_keypress k;
	CHECK_NONMANAGED_WINDOW(g, ev->window);
	g->last_input_window = vm_window;
	if (is_special_keypress(g, ev, vm_window->remote_winid))
		return;
	k.type = ev->type;
	k.x = ev->x;
	k.y = ev->y;
	k.state = ev->state;
	k.keycode = ev->keycode;
	hdr.type = MSG_KEYPRESS;
	hdr.window = vm_window->remote_winid;
	write_message(g->xchan, hdr, k);
//      fprintf(stderr, "win 0x%x(0x%x) type=%d keycode=%d\n",
//              (int) ev->window, hdr.window, k.type, k.keycode);
}

/* handle local Xserver event: XConfigureEvent
 * after some checks/fixes send to relevant window in VM */
static void process_xevent_configure(Ghandles * g, const XConfigureEvent * ev)
{
	CHECK_NONMANAGED_WINDOW(g, ev->window);
	if (g->log_level > 1)
		fprintf(stderr,
			"process_xevent_configure(synth %d) local 0x%x remote 0x%x, %d/%d, was "
			"%d/%d, xy %d/%d was %d/%d\n",
			ev->send_event,
			(int) vm_window->local_winid,
			(int) vm_window->remote_winid, ev->width,
			ev->height, vm_window->width, vm_window->height,
			ev->x, ev->y, vm_window->x, vm_window->y);
	/* non-synthetic events are about window position/size relative to the embeding
	 * frame window, wait for the synthetic one (produced by window manager), which
	 * is about window position relative to original window parent.
	 * See http://tronche.com/gui/x/icccm/sec-4.html#s-4.1.5 for details
	 */
	if (!ev->send_event && !vm_window->is_docked) {
		if ((int)vm_window->width == ev->width
		    && (int)vm_window->height == ev->height)
		return;
	}
	if ((int)vm_window->width == ev->width
	    && (int)vm_window->height == ev->height && vm_window->x == ev->x
	    && vm_window->y == ev->y)
		return;
	vm_window->width = ev->width;
	vm_window->height = ev->height;
	if (!vm_window->is_docked) {
		vm_window->x = ev->x;
		vm_window->y = ev->y;
	} else
		fix_docked_xy(g, vm_window, "process_xevent_configure");

// if AppVM has not unacknowledged previous resize msg, do not send another one
	if (vm_window->have_queued_configure)
		return;
	if (vm_window->remote_winid != FULLSCREEN_WINDOW_ID)
		vm_window->have_queued_configure = 1;
	send_configure(g->xchan, vm_window, vm_window->x, vm_window->y,
		       vm_window->width, vm_window->height);
}

/* handle local Xserver event: XButtonEvent
 * same as XKeyEvent - send to relevant window in VM */
static void process_xevent_button(Ghandles * g, const XButtonEvent * ev)
{
	struct msg_hdr hdr;
	struct msg_button k;
	CHECK_NONMANAGED_WINDOW(g, ev->window);

	g->last_input_window = vm_window;
	k.type = ev->type;

	k.x = ev->x;
	k.y = ev->y;
	k.state = ev->state;
	k.button = ev->button;
	hdr.type = MSG_BUTTON;
	hdr.window = vm_window->remote_winid;
	write_message(g->xchan, hdr, k);
	if (g->log_level > 1)
		fprintf(stderr,
			"xside: win 0x%x(0x%x) type=%d button=%d x=%d, y=%d\n",
			(int) ev->window, hdr.window, k.type, k.button,
			k.x, k.y);
	if (vm_window->is_docked && ev->type == ButtonPress) {
		/* Take focus to that icon, to make possible keyboard nagivation
		 * through the menu */
		XSetInputFocus(g->display, vm_window->local_winid, RevertToParent,
				CurrentTime);
	}
}

/* handle local Xserver event: XMotionEvent
 * send to relevant window in VM */
static void process_xevent_motion(Ghandles * g, const XMotionEvent * ev)
{
	struct msg_hdr hdr;
	struct msg_motion k;
	CHECK_NONMANAGED_WINDOW(g, ev->window);

	k.x = ev->x;
	k.y = ev->y;
	k.state = ev->state;
	k.is_hint = ev->is_hint;
	hdr.type = MSG_MOTION;
	hdr.window = vm_window->remote_winid;
	write_message(g->xchan, hdr, k);
//      fprintf(stderr, "motion in 0x%x", ev->window);
}

/* handle local Xserver event: EnterNotify, LeaveNotify
 * send it to VM, but alwo we use it to fix docked
 * window position */
static void process_xevent_crossing(Ghandles * g, const XCrossingEvent * ev)
{
	struct msg_hdr hdr;
	struct msg_crossing k;
	CHECK_NONMANAGED_WINDOW(g, ev->window);

	if (ev->type == EnterNotify) {
		char keys[32];
		XQueryKeymap(g->display, keys);
		hdr.type = MSG_KEYMAP_NOTIFY;
		hdr.window = 0;
		write_message(g->xchan, hdr, keys);
	}
	/* move tray to correct position in VM */
	if (vm_window->is_docked &&
			fix_docked_xy(g, vm_window, "process_xevent_crossing")) {
		send_configure(g->xchan, vm_window, vm_window->x, vm_window->y,
			       vm_window->width, vm_window->height);
	}

	hdr.type = MSG_CROSSING;
	hdr.window = vm_window->remote_winid;
	k.type = ev->type;
	k.x = ev->x;
	k.y = ev->y;
	k.state = ev->state;
	k.mode = ev->mode;
	k.detail = ev->detail;
	k.focus = ev->focus;
	write_message(g->xchan, hdr, k);
}

/* handle local Xserver event: FocusIn, FocusOut
 * send to relevant window in VM */
static void process_xevent_focus(Ghandles * g, const XFocusChangeEvent * ev)
{
	struct msg_hdr hdr;
	struct msg_focus k;
	CHECK_NONMANAGED_WINDOW(g, ev->window);
	if (ev->type == FocusIn) {
		char keys[32];
		XQueryKeymap(g->display, keys);
		hdr.type = MSG_KEYMAP_NOTIFY;
		hdr.window = 0;
		write_message(g->xchan, hdr, keys);
	}
	hdr.type = MSG_FOCUS;
	hdr.window = vm_window->remote_winid;
	k.type = ev->type;
	k.mode = ev->mode;
	k.detail = ev->detail;
	write_message(g->xchan, hdr, k);
}

/* handle local Xserver event: XExposeEvent
 * update relevant part of window using stored image
 */
static void process_xevent_expose(Ghandles * g, const XExposeEvent * ev)
{
	CHECK_NONMANAGED_WINDOW(g, ev->window);
	do_shm_update(g, vm_window, ev->x, ev->y, ev->width, ev->height);
}

/* handle local Xserver event: XMapEvent
 * after some checks, send to relevant window in VM */
static void process_xevent_mapnotify(Ghandles * g, const XMapEvent * ev)
{
	XWindowAttributes attr;
	CHECK_NONMANAGED_WINDOW(g, ev->window);
	if (vm_window->is_mapped)
		return;
	XGetWindowAttributes(g->display, vm_window->local_winid, &attr);
	if (attr.map_state != IsViewable && !vm_window->is_docked) {
		/* Unmap windows that are not visible on vmside.
		 * WM may try to map non-viewable windows ie. when
		 * switching desktops.
		 */
		(void) XUnmapWindow(g->display, vm_window->local_winid);
		if (g->log_level > 1)
			fprintf(stderr, "WM tried to map 0x%x, revert\n",
				(int) vm_window->local_winid);
	} else {
		/* Tray windows shall be visible always */
		struct msg_hdr hdr;
		struct msg_map_info map_info;
		map_info.override_redirect = attr.override_redirect;
		hdr.type = MSG_MAP;
		hdr.window = vm_window->remote_winid;
		write_message(g->xchan, hdr, map_info);
		if (vm_window->is_docked
		    && fix_docked_xy(g, vm_window,
				     "process_xevent_mapnotify"))
			send_configure(g->xchan, vm_window, vm_window->x,
				       vm_window->y, vm_window->width,
				       vm_window->height);
	}
}

/* handle local Xserver event: XPropertyEvent
 * currently only _NET_WM_STATE is examined */
static void process_xevent_propertynotify(Ghandles *g, const XPropertyEvent * ev)
{
	Atom act_type;
	Atom *state_list;
	unsigned long nitems, bytesleft, i;
	int ret, act_fmt;
	uint32_t flags;
	struct msg_hdr hdr;
	struct msg_window_flags msg;

	CHECK_NONMANAGED_WINDOW(g, ev->window);
	if (ev->atom == g->wm_state) {
		if (!vm_window->is_mapped)
			return;
		if (ev->state == PropertyNewValue) {
			ret = XGetWindowProperty(g->display, vm_window->local_winid, g->wm_state, 0, 10,
					False, XA_ATOM, &act_type, &act_fmt, &nitems, &bytesleft, (unsigned char**)&state_list);
			if (ret == Success && bytesleft > 0) {
			  /* Ensure we read all of the atoms */
			  XFree(state_list);
			  ret = XGetWindowProperty(g->display, vm_window->local_winid, g->wm_state,
			        0, (10 * 4 + bytesleft + 3) / 4, False, XA_ATOM, &act_type, &act_fmt,
			        &nitems, &bytesleft, (unsigned char**)&state_list);
			}
			if (ret != Success) {
				if (g->log_level > 0) {
					fprintf(stderr, "Failed to get 0x%x window state details\n", (int)ev->window);
					return;
				}
			}
			flags = 0;
			for (i = 0; i < nitems; i++) {
				flags |= flags_from_atom(g, state_list[i]);
			}
			XFree(state_list);
		} else { /* PropertyDelete */
			flags = 0;
		}
		if (flags == vm_window->flags_set) {
			/* no change */
			return;
		}
		hdr.type = MSG_WINDOW_FLAGS;
		hdr.window = vm_window->remote_winid;
		msg.flags_set = flags & ~vm_window->flags_set;
		msg.flags_unset = ~flags & vm_window->flags_set;
		write_message(g->xchan, hdr, msg);
		vm_window->flags_set = flags;
	}
}

/* handle local Xserver event: _XEMBED
 * if window isn't mapped already - map it now */
static void process_xevent_xembed(Ghandles * g, const XClientMessageEvent * ev)
{
	CHECK_NONMANAGED_WINDOW(g, ev->window);
	if (g->log_level > 1)
		fprintf(stderr, "_XEMBED message %ld\n", ev->data.l[1]);
	if (ev->data.l[1] == XEMBED_EMBEDDED_NOTIFY) {
		if (vm_window->is_docked < 2) {
			vm_window->is_docked = 2;
			if (!vm_window->is_mapped)
				XMapWindow(g->display, ev->window);
			/* move tray to correct position in VM */
			if (fix_docked_xy
			    (g, vm_window, "process_xevent_xembed")) {
				send_configure(g->xchan, vm_window, vm_window->x,
					       vm_window->y,
					       vm_window->width,
					       vm_window->height);
			}
		}
	}
}

/* handle local Xserver event: XCloseEvent
 * send to relevant window in VM */
static void process_xevent_close(Ghandles * g, XID window)
{
	struct msg_hdr hdr;
	CHECK_NONMANAGED_WINDOW(g, window);
	hdr.type = MSG_CLOSE;
	hdr.window = vm_window->remote_winid;
	hdr.untrusted_len = 0;
	write_struct(g->xchan, hdr);
}

/* dispatch local Xserver event */
void process_xevent(Ghandles * g)
{
	XEvent event_buffer;
	XNextEvent(g->display, &event_buffer);
	switch (event_buffer.type) {
	case KeyPress:
	case KeyRelease:
		process_xevent_keypress(g, (XKeyEvent *) & event_buffer);
		break;
	case ConfigureNotify:
		process_xevent_configure(g, (XConfigureEvent *) &
					 event_buffer);
		break;
	case ButtonPress:
	case ButtonRelease:
		process_xevent_button(g, (XButtonEvent *) & event_buffer);
		break;
	case MotionNotify:
		process_xevent_motion(g, (XMotionEvent *) & event_buffer);
		break;
	case EnterNotify:
	case LeaveNotify:
		process_xevent_crossing(g,
					(XCrossingEvent *) & event_buffer);
		break;
	case FocusIn:
	case FocusOut:
		process_xevent_focus(g,
				     (XFocusChangeEvent *) & event_buffer);
		break;
	case Expose:
		process_xevent_expose(g, (XExposeEvent *) & event_buffer);
		break;
	case MapNotify:
		process_xevent_mapnotify(g, (XMapEvent *) & event_buffer);
		break;
	case PropertyNotify:
		process_xevent_propertynotify(g, (XPropertyEvent *) & event_buffer);
		break;
	case ClientMessage:
//              fprintf(stderr, "xclient, atom=%s\n",
//                      XGetAtomName(g->display,
//                                   event_buffer.xclient.message_type));
		if (event_buffer.xclient.message_type == g->xembed_message) {
			process_xevent_xembed(g, (XClientMessageEvent *) &
					      event_buffer);
		} else if ((Atom)event_buffer.xclient.data.l[0] ==
			   g->wmDeleteMessage) {
			if (g->log_level > 0)
				fprintf(stderr, "close for 0x%x\n",
					(int) event_buffer.xclient.window);
			process_xevent_close(g,
					     event_buffer.xclient.window);
		}
		break;
	default:;
	}
}

// vim: noet:ts=8:
