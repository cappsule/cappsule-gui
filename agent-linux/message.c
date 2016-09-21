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

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <X11/XKBlib.h>

#include "guiclient.h"
#include "qubes-gui-protocol.h"
#include "list.h"
#include "gui_common.h"
#include "common.h"
#include "xchan.h"

#define TRUE true


static void handle_configure(Ghandles * g, XID winid)
{
	struct msg_configure r;
	struct genlist *l = list_lookup(windows_list, winid);
	XWindowAttributes attr;
	XGetWindowAttributes(g->display, winid, &attr);
	read_data(g->xchan, (char *) &r, sizeof(r));
	if (l && l->data && ((struct window_data*)l->data)->is_docked) {
		XMoveResizeWindow(g->display, ((struct window_data*)l->data)->embeder, r.x, r.y, r.width, r.height);
		XMoveResizeWindow(g->display, winid, 0, 0, r.width, r.height);
	} else {
		XMoveResizeWindow(g->display, winid, r.x, r.y, r.width, r.height);
	}

	DBG0("configure msg, x/y %d %d (was %d %d), w/h %d %d (was %d %d)\n",
		r.x, r.y, attr.x, attr.y, r.width, r.height, attr.width,
		attr.height);

}

static void handle_map(Ghandles * g, XID winid)
{
	struct msg_map_info inf;
	XSetWindowAttributes attr;
	read_data(g->xchan, (char *) &inf, sizeof(inf));
	attr.override_redirect = inf.override_redirect;
	XChangeWindowAttributes(g->display, winid,
				CWOverrideRedirect, &attr);
	XMapWindow(g->display, winid);

	DBG1("map msg for 0x%x\n", (int)winid);
}

static void handle_close(Ghandles * g, XID winid)
{
	XClientMessageEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = ClientMessage;
	ev.display = g->display;
	ev.window = winid;
	ev.format = 32;
	ev.message_type = g->wmProtocols;
	ev.data.l[0] = g->wmDeleteMessage;
//        XSetInputFocus(g->display, winid, RevertToParent, CurrentTime);
	XSendEvent(ev.display, ev.window, TRUE, 0, (XEvent *) & ev);

	DBG0("wmDeleteMessage sent for 0x%x\n", (int)winid);
}

static void handle_button(Ghandles * g, XID winid)
{
	struct msg_button key;
//      XButtonEvent event;
	XWindowAttributes attr;
	int ret;

	read_data(g->xchan, (char *) &key, sizeof(key));
	ret = XGetWindowAttributes(g->display, winid, &attr);
	if (ret != 1) {
		fprintf(stderr,
			"XGetWindowAttributes for 0x%x failed in "
			"do_button, ret=0x%x\n", (int) winid, ret);
		return;
	}

#if 0
	XSetInputFocus(g->display, winid, RevertToParent, CurrentTime);
//      XRaiseWindow(g->display, winid);
	event.display = g->display;
	event.window = winid;
	event.root = g->root_win;
	event.subwindow = None;
	event.time = CurrentTime;
	event.x = key.x;
	event.y = key.y;
	event.x_root = attr.x + key.x;
	event.y_root = attr.y + key.y;
	event.same_screen = TRUE;
	event.type = key.type;
	event.button = key.button;
	event.state = key.state;
	XSendEvent(event.display, event.window, TRUE,
//                 event.type==KeyPress?KeyPressMask:KeyReleaseMask, 
		   ButtonPressMask, (XEvent *) & event);
//      XSync(g->display, 0);
#endif

	DBG1("send buttonevent, win 0x%x type=%d button=%d\n",
		(int)winid, key.type, key.button);

	feed_xdriver(g, 'B', key.button, key.type == ButtonPress ? 1 : 0);
}

static void handle_motion(Ghandles * g, XID winid)
{
	struct msg_motion key;
//      XMotionEvent event;
	XWindowAttributes attr;
	int ret;
	struct genlist *l = list_lookup(windows_list, winid);

	read_data(g->xchan, (char *) &key, sizeof(key));
	if (l && l->data && ((struct window_data*)l->data)->is_docked) {
		/* get position of embeder, not icon itself*/
		winid = ((struct window_data*)l->data)->embeder;
	}
	ret = XGetWindowAttributes(g->display, winid, &attr);
	if (ret != 1) {
		fprintf(stderr,
			"XGetWindowAttributes for 0x%x failed in "
			"do_button, ret=0x%x\n", (int) winid, ret);
		return;
	}

#if 0
	event.display = g->display;
	event.window = winid;
	event.root = g->root_win;
	event.subwindow = None;
	event.time = CurrentTime;
	event.x = key.x;
	event.y = key.y;
	event.x_root = attr.x + key.x;
	event.y_root = attr.y + key.y;
	event.same_screen = TRUE;
	event.is_hint = key.is_hint;
	event.state = key.state;
	event.type = MotionNotify;
//      fprintf(stderr, "motion notify for 0x%x\n", (int)winid);
	XSendEvent(event.display, event.window, TRUE,
//                 event.type==KeyPress?KeyPressMask:KeyReleaseMask, 
		   0, (XEvent *) & event);
//      XSync(g->display, 0);
#endif
	feed_xdriver(g, 'M', attr.x + key.x, attr.y + key.y);
}

// ensure that LeaveNotify is delivered to the window - if pointer is still
// above this window, place stub window between pointer and the window
static void handle_crossing(Ghandles * g, XID winid)
{
	struct msg_crossing key;
	XWindowAttributes attr;
	int ret;
	struct genlist *l = list_lookup(windows_list, winid);

	/* we want to always get root window child (as this we get from
	 * XQueryPointer and can compare to window_under_pointer), so for embeded
	 * window get the embeder */
	if (l && l->data && ((struct window_data*)l->data)->is_docked) {
		winid = ((struct window_data*)l->data)->embeder;
	}

	read_data(g->xchan, (char *) &key, sizeof(key));

	ret = XGetWindowAttributes(g->display, winid, &attr);
	if (ret != 1) {
		fprintf(stderr,
			"XGetWindowAttributes for 0x%x failed in "
			"handle_crossing, ret=0x%x\n", (int) winid, ret);
		return;
	}

	if (key.type == EnterNotify) {
		// hide stub window
		XUnmapWindow(g->display, g->stub_win);
	} else if (key.type == LeaveNotify) {
		XID window_under_pointer, root_returned;
		int root_x, root_y, win_x, win_y;
		unsigned int mask_return;
		ret =
		    XQueryPointer(g->display, g->root_win, &root_returned,
				  &window_under_pointer, &root_x, &root_y,
				  &win_x, &win_y, &mask_return);
		if (ret != 1) {
			fprintf(stderr,
				"XQueryPointer for 0x%x failed in "
				"handle_crossing, ret=0x%x\n", (int) winid,
				ret);
			return;
		}
		// if pointer is still on the same window - place some stub window
		// just under it
		if (window_under_pointer == winid) {
			XMoveResizeWindow(g->display, g->stub_win,
					  root_x, root_y, 1, 1);
			XMapWindow(g->display, g->stub_win);
			XRaiseWindow(g->display, g->stub_win);
		}
	} else {
		fprintf(stderr, "Invalid crossing event: %d\n", key.type);
	}

}

static void handle_keypress(Ghandles * g, XID UNUSED(winid))
{
	struct msg_keypress key;
	XkbStateRec state;
//      XKeyEvent event;
//        char buf[256];
	read_data(g->xchan, (char *) &key, sizeof(key));
#if 0
//XGetInputFocus(g->display, &focus_return, &revert_to_return);
//      fprintf(stderr, "vmside: type=%d keycode=%d currfoc=0x%x\n", key.type,
//              key.keycode, (int)focus_return);

//      XSetInputFocus(g->display, winid, RevertToParent, CurrentTime);
	event.display = g->display;
	event.window = winid;
	event.root = g->root_win;
	event.subwindow = None;
	event.time = CurrentTime;
	event.x = key.x;
	event.y = key.y;
	event.x_root = 1;
	event.y_root = 1;
	event.same_screen = TRUE;
	event.type = key.type;
	event.keycode = key.keycode;
	event.state = key.state;
	XSendEvent(event.display, event.window, TRUE,
//                 event.type==KeyPress?KeyPressMask:KeyReleaseMask, 
		   KeyPressMask, (XEvent *) & event);
#else
	// sync modifiers state
	if (XkbGetState(g->display, XkbUseCoreKbd, &state) != Success) {
		DBG0("failed to get modifier state\n");
		state.mods = key.state;
	}
	if (!g->sync_all_modifiers) {
		// ignore all but CapsLock
		state.mods &= LockMask;
		key.state &= LockMask;
	}
	if (state.mods != key.state) {
		XModifierKeymap *modmap;
		int mod_index;
		int mod_mask;

		modmap = XGetModifierMapping(g->display);
		if (!modmap) {
			DBG0("failed to get modifier mapping\n");
		} else {
			// from X.h:
			// #define ShiftMapIndex           0
			// #define LockMapIndex            1
			// #define ControlMapIndex         2
			// #define Mod1MapIndex            3
			// #define Mod2MapIndex            4
			// #define Mod3MapIndex            5
			// #define Mod4MapIndex            6
			// #define Mod5MapIndex            7
			for (mod_index = 0; mod_index < 8; mod_index++) {
				if (modmap->modifiermap[mod_index*modmap->max_keypermod] == 0x00) {
					DBG1("ignoring disabled modifier %d\n",
						mod_index);
					// no key set for this modifier, ignore
					continue;
				}
				mod_mask = (1<<mod_index);
				// special case for caps lock switch by press+release
				if (mod_index == LockMapIndex) {
					if ((state.mods & mod_mask) ^ (key.state & mod_mask)) {
						feed_xdriver(g, 'K', modmap->modifiermap[mod_index*modmap->max_keypermod], 1);
						feed_xdriver(g, 'K', modmap->modifiermap[mod_index*modmap->max_keypermod], 0);
					}
				} else {
					if ((state.mods & mod_mask) && !(key.state & mod_mask))
						feed_xdriver(g, 'K', modmap->modifiermap[mod_index*modmap->max_keypermod], 0);
					else if (!(state.mods & mod_mask) && (key.state & mod_mask))
						feed_xdriver(g, 'K', modmap->modifiermap[mod_index*modmap->max_keypermod], 1);
				}
			}
			XFreeModifiermap(modmap);
		}
	}

	feed_xdriver(g, 'K', key.keycode, key.type == KeyPress ? 1 : 0);
#endif
//      fprintf(stderr, "win 0x%x type %d keycode %d\n",
//              (int) winid, key.type, key.keycode);
//      XSync(g->display, 0);
}

void take_focus(Ghandles * g, XID winid)
{
	// Send
	XClientMessageEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = ClientMessage;
	ev.display = g->display;
	ev.window = winid;
	ev.format = 32;
	ev.message_type = g->wmProtocols;
	ev.data.l[0] = g->wm_take_focus;
	ev.data.l[1] = CurrentTime;
	XSendEvent(ev.display, ev.window, TRUE, 0, (XEvent *) & ev);

	DBG0("WM_TAKE_FOCUS sent for 0x%x\n", (int)winid);

}

static void handle_focus(Ghandles * g, XID winid)
{
	struct msg_focus key;
	struct genlist *l;
	int input_hint;
	int use_take_focus;
//      XFocusChangeEvent event;

	read_data(g->xchan, (char *) &key, sizeof(key));
#if 0
	event.display = g->display;
	event.window = winid;
	event.type = key.type;
	event.mode = key.mode;
	event.detail = key.detail;

	fprintf(stderr, "send focuschange for 0x%x type %d\n",
		(int) winid, key.type);
	XSendEvent(event.display, event.window, TRUE,
		   0, (XEvent *) & event);
#endif
	if (key.type == FocusIn
	    && (key.mode == NotifyNormal || key.mode == NotifyUngrab)) {

		XRaiseWindow(g->display, winid);

		if ( (l=list_lookup(windows_list, winid)) && (l->data) ) {
			input_hint = ((struct window_data*)l->data)->input_hint;
			use_take_focus = ((struct window_data*)l->data)->support_take_focus;
		} else {
			fprintf(stderr, "WARNING handle_focus: Window 0x%x data not initialized", (int)winid);
			input_hint = True;
			use_take_focus = False;
		}

		// Give input focus only to window that set the input hint
		if (input_hint)
			XSetInputFocus(g->display, winid, RevertToParent,
			       CurrentTime);

		// Do not send take focus if the window doesn't support it
		if (use_take_focus)
			take_focus(g, winid);

		DBG1("0x%x raised\n", (int)winid);
	} else if (key.type == FocusOut
		   && (key.mode == NotifyNormal
		       || key.mode == NotifyUngrab)) {

		XSetInputFocus(g->display, None, RevertToParent,
			       CurrentTime);

		DBG1("0x%x lost focus\n", (int)winid);
	}

}

#define CLIPBOARD_4WAY
void handle_clipboard_req(Ghandles * g, XID UNUSED(winid))
{
	Atom Clp;
	Atom QProp = XInternAtom(g->display, "QUBES_SELECTION", False);
	Atom Targets = XInternAtom(g->display, "TARGETS", False);
	Window owner;
#ifdef CLIPBOARD_4WAY
	Clp = XInternAtom(g->display, "CLIPBOARD", False);
#else
	Clp = XA_PRIMARY;
#endif
	owner = XGetSelectionOwner(g->display, Clp);
	DBG0("clipboard req, owner=0x%x\n", (int)owner);
	if (owner == None) {
		send_clipboard_data(g, NULL, 0);
		return;
	}
	XConvertSelection(g->display, Clp, Targets, QProp,
			  g->stub_win, CurrentTime);
}

static void handle_clipboard_data(Ghandles * g, int len)
{
	Atom Clp = XInternAtom(g->display, "CLIPBOARD", False);

	if (g->clipboard_data)
		free(g->clipboard_data);
	// qubes_guid will not bother to send len==-1, really
	g->clipboard_data = malloc(len + 1);
	if (!g->clipboard_data) {
		perror("malloc");
		exit(1);
	}
	g->clipboard_data_len = len;
	read_data(g->xchan, (char *) g->clipboard_data, len);
	g->clipboard_data[len] = 0;
	XSetSelectionOwner(g->display, XA_PRIMARY, g->stub_win,
			   CurrentTime);
	XSetSelectionOwner(g->display, Clp, g->stub_win, CurrentTime);
#ifndef CLIPBOARD_4WAY
	XSync(g->display, False);
	feed_xdriver(g, 'B', 2, 1);
	feed_xdriver(g, 'B', 2, 0);
#endif
}

static void do_execute(char *user, char *cmd)
{
	int i, fd;
	switch (fork()) {
	case -1:
		perror("fork cmd");
		break;
	case 0:
		for (i = 0; i < 256; i++)
			close(i);
		fd = open("/dev/null", O_RDWR);
		for (i = 0; i <= 2; i++)
			dup2(fd, i);
		signal(SIGCHLD, SIG_DFL);
		if (user)
			execl("/bin/su", "su", "-", user, "-c", cmd, NULL);
		else
			execl("/bin/bash", "bash", "-c", cmd, NULL);
		perror("execl cmd");
		exit(1);
	default:;
	}
}

static void handle_execute(Ghandles * g)
{
	char *ptr;
	struct msg_execute exec_data;
	read_data(g->xchan, (char *) &exec_data, sizeof(exec_data));
	exec_data.cmd[sizeof(exec_data.cmd) - 1] = 0;
	ptr = index(exec_data.cmd, ':');
	if (!ptr)
		return;
	*ptr = 0;
	fprintf(stderr, "handle_execute(): cmd = %s:%s\n",
		exec_data.cmd, ptr + 1);
	do_execute(exec_data.cmd, ptr + 1);
}

static int bitset(unsigned char *keys, int num)
{
	return (keys[num / 8] >> (num % 8)) & 1;
}

static void handle_keymap_notify(Ghandles * g)
{
	unsigned char remote_keys[32], local_keys[32];
	int i;

	read_struct(g->xchan, remote_keys);

	XQueryKeymap(g->display, (char *)local_keys);
	for (i = 0; i < 256; i++) {
		if (!bitset(remote_keys, i) && bitset(local_keys, i)) {
			feed_xdriver(g, 'K', i, 0);
			DBG1("handle_keymap_notify: unsetting key %d\n", i);
		}
	}
}

static void handle_window_flags(Ghandles *g, XID winid)
{
	int ret, j, changed;
	unsigned i;
	Atom *state_list;
	Atom new_state_list[12];
	Atom act_type;
	int act_fmt;
	uint32_t tmp_flag;
	unsigned long nitems, bytesleft;
	struct msg_window_flags msg_flags;
	read_data(g->xchan, (char *) &msg_flags, sizeof(msg_flags));

	/* FIXME: only first 10 elements are parsed */
	ret = XGetWindowProperty(g->display, winid, g->wm_state, 0, 10,
			False, XA_ATOM, &act_type, &act_fmt, &nitems, &bytesleft, (unsigned char**)&state_list);
	if (ret != Success)
		return;

	j = 0;
	changed = 0;
	for (i=0; i < nitems; i++) {
		tmp_flag = flags_from_atom(g, state_list[i]);
		if (tmp_flag && tmp_flag & msg_flags.flags_set) {
			/* leave flag set, mark as processed */
			msg_flags.flags_set &= ~tmp_flag;
		} else if (tmp_flag && tmp_flag & msg_flags.flags_unset) {
			/* skip this flag (remove) */
			changed = 1;
			continue;
		}
		/* copy flag to new set */
		new_state_list[j++] = state_list[i];
	}
	XFree(state_list);
	/* set new elements */
	if (msg_flags.flags_set & WINDOW_FLAG_FULLSCREEN)
		new_state_list[j++] = g->wm_state_fullscreen;
	if (msg_flags.flags_set & WINDOW_FLAG_DEMANDS_ATTENTION)
		new_state_list[j++] = g->wm_state_demands_attention;

	if (msg_flags.flags_set)
		changed = 1;

	if (!changed)
		return;

	XChangeProperty(g->display, winid, g->wm_state, XA_ATOM, 32, PropModeReplace, (unsigned char *)new_state_list, j);
}

/* return false if no message is available, true otherwise */
bool handle_message(Ghandles *g)
{
	struct msg_hdr hdr;
	char discard[256];
	unsigned char *p;
	size_t n, size;
	err_t error;

	error = xchan_recv_nopoll(g->xchan, &hdr, sizeof(hdr), &size);
	if (error) {
		print_error(error, "failed to receive message header");
		exit(EXIT_FAILURE);
	}

	if (size == 0) {
		/* no data available */
		return false;
	}

	while (size != sizeof(hdr)) {
		p = (unsigned char *)&hdr + size;
		error = xchan_recv(g->xchan, p, sizeof(hdr) - size, &n);
		if (error) {
			print_error(error, "failed to receive message header");
			exit(EXIT_FAILURE);
		}
		size += n;
	}

	DBG1("received message type %d for 0x%x\n", hdr.type, hdr.window);

	switch (hdr.type) {
	case MSG_KEYPRESS:
		handle_keypress(g, hdr.window);
		break;
	case MSG_CONFIGURE:
		handle_configure(g, hdr.window);
		break;
	case MSG_MAP:
		handle_map(g, hdr.window);
		break;
	case MSG_BUTTON:
		handle_button(g, hdr.window);
		break;
	case MSG_MOTION:
		handle_motion(g, hdr.window);
		break;
	case MSG_CLOSE:
		handle_close(g, hdr.window);
		break;
	case MSG_CROSSING:
		handle_crossing(g, hdr.window);
		break;
	case MSG_FOCUS:
		handle_focus(g, hdr.window);
		break;
	case MSG_CLIPBOARD_REQ:
		handle_clipboard_req(g, hdr.window);
		break;
	case MSG_CLIPBOARD_DATA:
		handle_clipboard_data(g, hdr.window);
		break;
	case MSG_EXECUTE:
		handle_execute(g);
		break;
	case MSG_KEYMAP_NOTIFY:
		handle_keymap_notify(g);
		break;
	case MSG_WINDOW_FLAGS:
		handle_window_flags(g, hdr.window);
		break;
	default:
		fprintf(stderr, "got unknown msg type %d, ignoring\n", hdr.type);
		while (hdr.untrusted_len > 0) {
			hdr.untrusted_len -= read_data(g->xchan, discard, min(hdr.untrusted_len, sizeof(discard)));
		}
	}

	return true;
}

// vim: noet:ts=8:
