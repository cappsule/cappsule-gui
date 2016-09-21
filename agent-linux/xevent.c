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
#include <stdio.h>
#include <unistd.h>
#include <X11/extensions/Xdamage.h>

#include "qubes-xorg-tray-defs.h"
#include "qubes-gui-protocol.h"
#include "gui_common.h"
#include "guiclient.h"
#include "common.h"
#include "list.h"

#define SKIP_NONMANAGED_WINDOW if (!list_lookup(windows_list, window)) return


static void read_discarding(int fd, int size)
{
	char buf[1024];
	int n, count, total = 0;
	while (total < size) {
		if (size > (int)sizeof(buf))
			count = sizeof(buf);
		else
			count = size;
		n = read(fd, buf, count);
		if (n < 0) {
			perror("read_discarding");
			exit(1);
		}
		if (n == 0) {
			fprintf(stderr, "EOF in read_discarding\n");
			exit(1);
		}
		total += n;
	}
}

static void readall(int fd, void *buf, size_t count)
{
	unsigned char *p;
	ssize_t n;
	size_t i;

	p = buf;
	i = count;
	while (i > 0) {
		n = read(fd, p, i);
		if (n == 0) {
			errx(1, "%s: connection closed", __func__);
		} else if (n < 0) {
			if (errno == EINTR)
				continue;
			err(1, "%s", __func__);
		}
		i -= n;
		p += n;
	}
}

static void send_pixmap_mfns(Ghandles * g, XID window)
{
	struct shm_cmd shmcmd;
	struct msg_hdr hdr;
	uint32_t *mfnbuf;
	int ret, rcvd = 0, size;

	feed_xdriver(g, 'W', (int) window, 0);
	readall(g->xserver_fd, (char *)&shmcmd, sizeof(shmcmd));

	if (shmcmd.num_mfn == 0 || shmcmd.num_mfn > (unsigned)MAX_MFN_COUNT ||
		shmcmd.width > MAX_WINDOW_WIDTH || shmcmd.height > MAX_WINDOW_HEIGHT) {
		fprintf(stderr, "got num_mfn=0x%x for window 0x%x (%dx%d)\n",
			shmcmd.num_mfn, (int) window, shmcmd.width, shmcmd.height);
		read_discarding(g->xserver_fd,
				shmcmd.num_mfn * sizeof(*mfnbuf));
		return;
	}
	size = shmcmd.num_mfn * sizeof(*mfnbuf);
	mfnbuf = alloca(size);
	while (rcvd < size) {
		ret =
		    read(g->xserver_fd, ((char *) mfnbuf) + rcvd,
			 size - rcvd);
		if (ret == 0) {
			fprintf(stderr, "unix read EOF\n");
			exit(1);
		}
		if (ret < 0) {
			perror("unix read error");
			exit(1);
		}
		rcvd += ret;
	}

	hdr.type = MSG_MFNDUMP;
	hdr.window = window;
	hdr.untrusted_len = sizeof(shmcmd) + size;
	write_struct(g->xchan, hdr);
	write_struct(g->xchan, shmcmd);
	write_data(g->xchan, (char *) mfnbuf, size);
}

static void process_xevent_createnotify(Ghandles * g, XCreateWindowEvent * ev)
{
	struct msg_hdr hdr;
	struct msg_create crt;
	struct window_data *wd;

	XWindowAttributes attr;
	int ret;

	ret = XGetWindowAttributes(g->display, ev->window, &attr);
	if (ret != 1) {
		fprintf(stderr, "XGetWindowAttributes for 0x%x failed in "
			"handle_create, ret=0x%x\n", (int) ev->window,
			ret);
		return;
	}

	DBG0("Create for 0x%x class 0x%x\n", (int)ev->window, attr.class);
	if (list_lookup(windows_list, ev->window)) {
		fprintf(stderr, "CREATE for already existing 0x%x\n",
			(int)ev->window);
		return;
	}
	if (list_lookup(embeder_list, ev->window)) {
		/* ignore CreateNotify for embeder window */
		DBG1("CREATE for embeder 0x%x\n", (int)ev->window);
		return;
	}

	/* Initialize window_data structure */
	wd = (struct window_data*)malloc(sizeof(struct window_data));
	if (!wd) {
		fprintf(stderr, "OUT OF MEMORY\n");
		return;
	}
	/* Default values for window_data. By default, window should receive InputFocus events */
	wd->is_docked = False;
	wd->input_hint = True;
	wd->support_take_focus = True;
	list_insert(windows_list, ev->window, wd);

	if (attr.class != InputOnly)
		XDamageCreate(g->display, ev->window,
			XDamageReportRawRectangles);
	// the following hopefully avoids missed damage events
	XSync(g->display, False);
	XSelectInput(g->display, ev->window, PropertyChangeMask);
	hdr.type = MSG_CREATE;
	hdr.window = ev->window;
	crt.width = ev->width;
	crt.height = ev->height;
	crt.parent = ev->parent;
	crt.x = ev->x;
	crt.y = ev->y;
	crt.override_redirect = ev->override_redirect;
	write_message(g->xchan, hdr, crt);
}

static void process_xevent_destroy(Ghandles * g, XID window)
{
	struct msg_hdr hdr;
	struct genlist *l;
	/* embeders are not manged windows, so must be handled before SKIP_NONMANAGED_WINDOW */
	if ((l = list_lookup(embeder_list, window))) {
		if (l->data) {
			free(l->data);
		}
		list_remove(l);
	}

	SKIP_NONMANAGED_WINDOW;
	if (g->log_level > 0)
		fprintf(stderr, "handle destroy 0x%x\n", (int) window);
	hdr.type = MSG_DESTROY;
	hdr.window = window;
	hdr.untrusted_len = 0;
	write_struct(g->xchan, hdr);
	l = list_lookup(windows_list, window);
	if (l->data) {
		if (((struct window_data*)l->data)->is_docked) {
			XDestroyWindow(g->display, ((struct window_data*)l->data)->embeder);
		}
		free(l->data);
	}
	list_remove(l);
}

static void send_window_state(Ghandles * g, XID window)
{
	int ret;
	unsigned i;
	Atom *state_list;
	Atom act_type;
	int act_fmt;
	unsigned long nitems, bytesleft;
	struct msg_hdr hdr;
	struct msg_window_flags flags;

	/* FIXME: only first 10 elements are parsed */
	ret = XGetWindowProperty(g->display, window, g->wm_state, 0, 10,
			False, XA_ATOM, &act_type, &act_fmt, &nitems, &bytesleft, (unsigned char**)&state_list);
	if (ret != Success)
		return;

	flags.flags_set = 0;
	flags.flags_unset = 0;
	for (i=0; i < nitems; i++) {
		flags.flags_set |= flags_from_atom(g, state_list[i]);
	}
	hdr.window = window;
	hdr.type = MSG_WINDOW_FLAGS;
	write_message(g->xchan, hdr, flags);
	XFree(state_list);
}

static void getwmname_tochar(Ghandles * g, XID window, char *outbuf, int bufsize)
{
	XTextProperty text_prop_return;
	char **list;
	int count;

	outbuf[0] = 0;
	if (!XGetWMName(g->display, window, &text_prop_return) ||
	    !text_prop_return.value || !text_prop_return.nitems)
		return;
	if (Xutf8TextPropertyToTextList(g->display,
					&text_prop_return, &list,
					&count) < 0 || count <= 0
	    || !*list) {
		XFree(text_prop_return.value);
		return;
	}
	strncat(outbuf, list[0], bufsize-1);
	XFree(text_prop_return.value);
	XFreeStringList(list);
	if (g->log_level > 0)
		fprintf(stderr, "got wmname=%s\n", outbuf);
}

static void send_wmname(Ghandles * g, XID window)
{
	struct msg_hdr hdr;
	struct msg_wmname msg;
	getwmname_tochar(g, window, msg.data, sizeof(msg.data));
	hdr.window = window;
	hdr.type = MSG_WMNAME;
	write_message(g->xchan, hdr, msg);
}

static void process_xevent_map(Ghandles * g, XID window)
{
	XWindowAttributes attr;
	struct msg_hdr hdr;
	struct msg_map_info map_info;
	Window transient;
	SKIP_NONMANAGED_WINDOW;

	if (g->log_level > 1)
		fprintf(stderr, "MAP for window 0x%x\n", (int)window);

	send_pixmap_mfns(g, window);
	send_window_state(g, window);
	XGetWindowAttributes(g->display, window, &attr);
	if (XGetTransientForHint(g->display, window, &transient))
		map_info.transient_for = transient;
	else
		map_info.transient_for = 0;
	map_info.override_redirect = attr.override_redirect;
	hdr.type = MSG_MAP;
	hdr.window = window;
	write_message(g->xchan, hdr, map_info);
	send_wmname(g, window);
//      process_xevent_damage(g, window, 0, 0, attr.width, attr.height);
}

static void process_xevent_unmap(Ghandles * g, XID window)
{
	struct msg_hdr hdr;
	SKIP_NONMANAGED_WINDOW;

	if (g->log_level > 1)
		fprintf(stderr, "UNMAP for window 0x%x\n", (int)window);
	hdr.type = MSG_UNMAP;
	hdr.window = window;
	hdr.untrusted_len = 0;
	write_struct(g->xchan, hdr);
	XDeleteProperty(g->display, window, g->wm_state);
}

static void process_xevent_configure(Ghandles * g, XID window,
			      XConfigureEvent * ev)
{
	struct msg_hdr hdr;
	struct msg_configure conf;
	struct genlist *l;
	/* SKIP_NONMANAGED_WINDOW; */
	if (!(l=list_lookup(windows_list, window))) {
		/* if not real managed window, check if this is embeder for another window */
		struct genlist *e;
		if ((e=list_lookup(embeder_list, window))) {
			window = ((struct embeder_data*)e->data)->icon_window;
			if (!list_lookup(windows_list, window))
				/* probably icon window have just destroyed, so ignore message */
				/* "l" not updated intentionally - when configure notify comes
				 * from the embeder, it should be passed to dom0 (in most cases as
				 * ACK for earlier configure request) */
				return;
		} else {
			/* ignore not managed windows */
			return;
		}
	}

	if (g->log_level > 1)
		fprintf(stderr,
			"handle configure event 0x%x w=%d h=%d ovr=%d\n",
			(int) window, ev->width, ev->height,
			(int) ev->override_redirect);
	if (l && l->data && ((struct window_data*)l->data)->is_docked) {
		/* for docked icon, ensure that it fills embeder window; don't send any
		 * message to dom0 - it will be done for embeder itself*/
		XWindowAttributes attr;
		int ret;

		ret = XGetWindowAttributes(g->display, ((struct window_data*)l->data)->embeder, &attr);
		if (ret != 1) {
			fprintf(stderr,
					"XGetWindowAttributes for 0x%x failed in "
					"handle_xevent_configure, ret=0x%x\n", (int) ((struct window_data*)l->data)->embeder, ret);
			return;
		};
		if (ev->x != 0 || ev->y != 0 || ev->width != attr.width || ev->height != attr.height) {
			XMoveResizeWindow(g->display, window, 0, 0, attr.width, attr.height);
		}
		return;
	}
	hdr.type = MSG_CONFIGURE;
	hdr.window = window;
	conf.x = ev->x;
	conf.y = ev->y;
	conf.width = ev->width;
	conf.height = ev->height;
	conf.override_redirect = ev->override_redirect;
	write_message(g->xchan, hdr, conf);
	send_pixmap_mfns(g, window);
}

static void handle_targets_list(Ghandles * g, Atom Qprop, unsigned char *data,
			 int len)
{
	Atom Clp = XInternAtom(g->display, "CLIPBOARD", False);
	Atom *atoms = (Atom *) data;
	int i;
	int have_utf8 = 0;
	if (g->log_level > 1)
		fprintf(stderr, "target list data size %d\n", len);
	for (i = 0; i < len; i++) {
		if (atoms[i] == g->utf8_string_atom)
			have_utf8 = 1;
		if (g->log_level > 1)
			fprintf(stderr, "supported 0x%x %s\n",
				(int) atoms[i], XGetAtomName(g->display,
							     atoms[i]));
	}
	XConvertSelection(g->display, Clp,
			  have_utf8 ? g->utf8_string_atom : XA_STRING, Qprop,
			  g->stub_win, CurrentTime);
}

static void process_xevent_selection(Ghandles * g, XSelectionEvent * ev)
{
	int format, result;
	Atom type;
	unsigned long len, bytes_left, dummy;
	unsigned char *data;
	Atom Clp = XInternAtom(g->display, "CLIPBOARD", False);
	Atom Qprop = XInternAtom(g->display, "QUBES_SELECTION", False);
	Atom Targets = XInternAtom(g->display, "TARGETS", False);
	Atom Utf8_string_atom =
	    XInternAtom(g->display, "UTF8_STRING", False);

	if (g->log_level > 0)
		fprintf(stderr, "selection event, target=%s\n",
			XGetAtomName(g->display, ev->target));
	if (ev->requestor != g->stub_win || ev->property != Qprop)
		return;
	XGetWindowProperty(g->display, ev->requestor, Qprop, 0, 0, 0,
			   AnyPropertyType, &type, &format, &len,
			   &bytes_left, &data);
	if (bytes_left <= 0)
		return;
	result =
	    XGetWindowProperty(g->display, ev->requestor, Qprop, 0,
			       bytes_left, 0,
			       AnyPropertyType, &type,
			       &format, &len, &dummy, &data);
	if (result != Success)
		return;

	if (ev->target == Targets)
		handle_targets_list(g, Qprop, data, len);
	// If we receive TARGETS atom in response for TARGETS query, let's assume
	// that UTF8 is supported.
	// this is workaround for Opera web browser...
	else if (ev->target == XA_ATOM && len >= 4 && len <= 8 &&
		 // compare only first 4 bytes
		 *((unsigned *) data) == Targets)
		XConvertSelection(g->display, Clp,
				  Utf8_string_atom, Qprop,
				  g->stub_win, CurrentTime);
	else
		send_clipboard_data(g, (char *) data, len);
	/* even if the clipboard owner does not support UTF8 and we requested
	   XA_STRING, it is fine - ascii is legal UTF8 */
	XFree(data);

}

static void process_xevent_selection_req(Ghandles * g,
				  XSelectionRequestEvent * req)
{
	XSelectionEvent resp;
	Atom Targets = XInternAtom(g->display, "TARGETS", False);
	Atom Compound_text =
	    XInternAtom(g->display, "COMPOUND_TEXT", False);
	Atom Utf8_string_atom =
	    XInternAtom(g->display, "UTF8_STRING", False);
	int convert_style = XConverterNotFound;

	if (g->log_level > 0)
		fprintf(stderr, "selection req event, target=%s\n",
			XGetAtomName(g->display, req->target));
	resp.property = None;
	if (req->target == Targets) {
		Atom tmp[4] = { XA_STRING, Targets, Utf8_string_atom,
			Compound_text
		};
		XChangeProperty(g->display, req->requestor, req->property,
				XA_ATOM, 32, PropModeReplace,
				(unsigned char *)
				tmp, sizeof(tmp) / sizeof(tmp[0]));
		resp.property = req->property;
	}
	if (req->target == Utf8_string_atom)
		convert_style = XUTF8StringStyle;
	if (req->target == XA_STRING)
		convert_style = XTextStyle;
	if (req->target == Compound_text)
		convert_style = XCompoundTextStyle;
	if (convert_style != XConverterNotFound) {
		XTextProperty ct;
		Xutf8TextListToTextProperty(g->display,
					    (char **) &g->clipboard_data,
					    1, convert_style, &ct);
		XSetTextProperty(g->display, req->requestor, &ct,
				 req->property);
		XFree(ct.value);
		resp.property = req->property;
	}

	if (resp.property == None)
		fprintf(stderr,
			"Not supported selection_req target 0x%x %s\n",
			(int) req->target, XGetAtomName(g->display,
							req->target));
	resp.type = SelectionNotify;
	resp.display = req->display;
	resp.requestor = req->requestor;
	resp.selection = req->selection;
	resp.target = req->target;
	resp.time = req->time;
	XSendEvent(g->display, req->requestor, 0, 0, (XEvent *) & resp);
}

/*	Retrieve the supported WM Protocols
	We don't forward the info to dom0 as we only need specific client protocols
*/
static void retrieve_wmprotocols(Ghandles * g, XID window)
{
	int nitems;
	Atom *supported_protocols;
	int i;
	struct genlist *l;

	if (XGetWMProtocols(g->display, window, &supported_protocols, &nitems) == 1) {
		for (i=0; i < nitems; i++) {
			if (supported_protocols[i] == g->wm_take_focus) {
				// Retrieve window data and set support_take_focus
				if (!((l=list_lookup(windows_list, window)) && (l->data))) {
					fprintf(stderr, "ERROR retrieve_wmprotocols: Window 0x%x data not initialized", (int)window);
					return;
				}
				if (g->log_level > 1)
					fprintf(stderr, "Protocol take_focus supported for Window 0x%x\n", (int)window);

				((struct window_data*)l->data)->support_take_focus = True;
			}
		}
	} else {
		fprintf(stderr, "ERROR reading WM_PROTOCOLS\n");
		return;
	}
	XFree(supported_protocols);
}

/* 	Retrieve the 'real' WMHints.
	We don't forward the info to dom0 as we only need InputHint and dom0 doesn't care about it
*/
static void retrieve_wmhints(Ghandles * g, XID window)
{
	XWMHints *wm_hints;
	struct genlist *l;

	if (!((l=list_lookup(windows_list, window)) && (l->data))) {
		fprintf(stderr, "ERROR retrieve_wmhints: Window 0x%x data not initialized", (int)window);
		return;
	}

	if (!(wm_hints = XGetWMHints(g->display, window))) {
		fprintf(stderr, "ERROR reading WM_HINTS\n");
		return;
	}

	if (wm_hints->flags & InputHint) {
		((struct window_data*)l->data)->input_hint = wm_hints->input;

		if (g->log_level > 1)
			fprintf(stderr, "Received input hint 0x%x for Window 0x%x\n", wm_hints->input, (int)window);
	} else {
		// Default value
		if (g->log_level > 1)
			fprintf(stderr, "Received WMHints without input hint set for Window 0x%x\n", (int)window);
		((struct window_data*)l->data)->input_hint = True;
	}
	XFree(wm_hints);
}

static void send_wmnormalhints(Ghandles * g, XID window)
{
	struct msg_hdr hdr;
	struct msg_window_hints msg;
	XSizeHints size_hints;
	long supplied_hints;

	if (!XGetWMNormalHints
	    (g->display, window, &size_hints, &supplied_hints)) {
		fprintf(stderr, "error reading WM_NORMAL_HINTS\n");
		return;
	}
	/* Nasty workaround for KDE bug affecting gnome-terminal (shrinks to minimal size) */
	/* https://bugzilla.redhat.com/show_bug.cgi?id=707664 */
	if ((size_hints.flags & (PBaseSize|PMinSize|PResizeInc)) ==
			(PBaseSize|PMinSize|PResizeInc)) {
		/* KDE incorrectly uses PMinSize when both are provided */
		if (size_hints.width_inc > 1)
			/* round up to neareset multiply of width_inc */
			size_hints.min_width =
				((size_hints.min_width-size_hints.base_width+1) / size_hints.width_inc)
				* size_hints.width_inc + size_hints.base_width;
		if (size_hints.height_inc > 1)
			/* round up to neareset multiply of height_inc */
			size_hints.min_height =
				((size_hints.min_height-size_hints.base_height+1) / size_hints.height_inc)
				* size_hints.height_inc + size_hints.base_height;
	}

	// pass only some hints
	msg.flags =
	    size_hints.flags & (PMinSize | PMaxSize | PResizeInc |
				PBaseSize);
	msg.min_width = size_hints.min_width;
	msg.min_height = size_hints.min_height;
	msg.max_width = size_hints.max_width;
	msg.max_height = size_hints.max_height;
	msg.width_inc = size_hints.width_inc;
	msg.height_inc = size_hints.height_inc;
	msg.base_width = size_hints.base_width;
	msg.base_height = size_hints.base_height;
	hdr.window = window;
	hdr.type = MSG_WINDOW_HINTS;
	write_message(g->xchan, hdr, msg);
}

static void process_xevent_property(Ghandles * g, XID window, XPropertyEvent * ev)
{
	SKIP_NONMANAGED_WINDOW;
	if (g->log_level > 1)
		fprintf(stderr, "handle property %s for window 0x%x\n",
			XGetAtomName(g->display, ev->atom),
			(int) ev->window);
	if (ev->atom == XInternAtom(g->display, "WM_NAME", False))
		send_wmname(g, window);
	else if (ev->atom ==
		 XInternAtom(g->display, "WM_NORMAL_HINTS", False))
		send_wmnormalhints(g, window);
	else if (ev->atom ==
		 XInternAtom(g->display, "WM_HINTS", False))
		retrieve_wmhints(g,window);
	else if (ev->atom ==
		 XInternAtom(g->display, "WM_PROTOCOLS", False))
		retrieve_wmprotocols(g,window);
	else if (ev->atom == g->xembed_info) {
		struct genlist *l = list_lookup(windows_list, window);
		Atom act_type;
		unsigned long nitems, bytesafter;
		unsigned char *data;
		int ret, act_fmt;

		if (!l->data || !((struct window_data*)l->data)->is_docked)
			/* ignore _XEMBED_INFO change on non-docked windows */
			return;
		ret = XGetWindowProperty(g->display, window, g->xembed_info, 0, 2, False,
				g->xembed_info, &act_type, &act_fmt, &nitems, &bytesafter,
				&data);
		if (ret && act_type == g->xembed_info && nitems == 2) {
			if (((int*)data)[1] & XEMBED_MAPPED)
				XMapWindow(g->display, window);
			else
				XUnmapWindow(g->display, window);
		}
		if (ret == Success && nitems > 0)
			XFree(data);
	}
}

static void process_xevent_message_tray(Ghandles * g, XClientMessageEvent * ev)
{
	XClientMessageEvent resp;
	Window w;
	int ret;
	struct msg_hdr hdr;
	Atom act_type;
	int act_fmt;
	int mapwindow = 0;
	unsigned long nitems, bytesafter;
	unsigned char *data;
	struct genlist *l;
	struct window_data *wd;
	struct embeder_data *ed;

	if (ev->data.l[1] != SYSTEM_TRAY_REQUEST_DOCK) {
		fprintf(stderr, "unhandled tray opcode: %ld\n", ev->data.l[1]);
		return;
	}

	w = ev->data.l[2];

	if (!(l=list_lookup(windows_list, w))) {
		fprintf(stderr, "ERROR process_xevent_message: Window 0x%x not initialized", (int)w);
		return;
	}

	DBG0("tray request dock for window 0x%x\n", (int)w);
	ret = XGetWindowProperty(g->display, w, g->xembed_info, 0, 2,
				False, g->xembed_info, &act_type, &act_fmt, &nitems,
				&bytesafter, &data);
	if (ret != Success) {
		fprintf(stderr, "failed to get window property, probably window doesn't longer exists\n");
		return;
	}
	if (act_type != g->xembed_info) {
		fprintf(stderr, "window 0x%x havn't proper _XEMBED_INFO property, assuming defaults (workaround for buggy applications)\n", (unsigned int)w);
	}
	if (act_type == g->xembed_info && nitems == 2) {
		mapwindow = ((int*)data)[1] & XEMBED_MAPPED;
		/* TODO: handle version */
	}
	if (ret == Success && nitems > 0)
		Xfree(data);

	if (!(l->data)) {
		fprintf(stderr, "ERROR process_xevent_message: Window 0x%x data not initialized", (int)w);
		return;
	}
	wd = (struct window_data*)(l->data);
	/* TODO: error checking */
	wd->embeder = XCreateSimpleWindow(g->display, g->root_win,
					0, 0, 32, 32, /* default icon size, will be changed by dom0 */
					0, BlackPixel(g->display,
						g->screen),
					WhitePixel(g->display,
						g->screen));
	wd->is_docked=True;
	DBG1(" created embeder 0x%x\n", (int)wd->embeder);
	XSelectInput(g->display, wd->embeder, SubstructureNotifyMask);
	ed = (struct embeder_data*)malloc(sizeof(struct embeder_data));
	if (!ed) {
		fprintf(stderr, "OUT OF MEMORY\n");
		return;
	}
	ed->icon_window = w;
	list_insert(embeder_list, wd->embeder, ed);

	ret = XReparentWindow(g->display, w, wd->embeder, 0, 0);
	if (ret != 1) {
		fprintf(stderr,
			"XReparentWindow for 0x%x failed in "
			"handle_dock, ret=0x%x\n", (int) w,
			ret);
		return;
	}

	memset(&resp, 0, sizeof(resp));
	resp.type = ClientMessage;
	resp.window = w;
	resp.message_type =
		XInternAtom(g->display, "_XEMBED", False);
	resp.format = 32;
	resp.data.l[0] = ev->data.l[0];
	resp.data.l[1] = XEMBED_EMBEDDED_NOTIFY;
	resp.data.l[3] = ev->window;
	resp.data.l[4] = 0; /* TODO: handle version; GTK+ uses version 1, but spec says the latest is 0 */
	resp.display = g->display;
	XSendEvent(resp.display, resp.window, False,
		NoEventMask, (XEvent *) & ev);
	XRaiseWindow(g->display, w);
	if (mapwindow)
		XMapRaised(g->display, resp.window);
	XMapWindow(g->display, wd->embeder);
	XLowerWindow(g->display, wd->embeder);
	XMoveWindow(g->display, w, 0, 0);
	/* force refresh of window content */
	XClearWindow(g->display, wd->embeder);
	XClearArea(g->display, w, 0, 0, 32, 32, True); /* XXX defult size once again */
	XSync(g->display, False);

	hdr.type = MSG_DOCK;
	hdr.window = w;
	hdr.untrusted_len = 0;
	write_struct(g->xchan, hdr);
}

static void process_xevent_message_wm_state(Ghandles *g,
					XClientMessageEvent *ev)
{
	struct msg_hdr hdr;
	struct msg_window_flags msg;

	/* SKIP_NONMANAGED_WINDOW */
	if (!list_lookup(windows_list, ev->window))
		return;

	msg.flags_set = 0;
	msg.flags_unset = 0;
	if (ev->data.l[0] == 0) { /* remove/unset property */
		msg.flags_unset |= flags_from_atom(g, ev->data.l[1]);
		msg.flags_unset |= flags_from_atom(g, ev->data.l[2]);
	} else if (ev->data.l[0] == 1) { /* add/set property */
		msg.flags_set |= flags_from_atom(g, ev->data.l[1]);
		msg.flags_set |= flags_from_atom(g, ev->data.l[2]);
	} else if (ev->data.l[0] == 2) { /* toggle property */
		fprintf(stderr, "toggle window 0x%x property %s not supported, "
			"please report it with the application name\n", (int) ev->window,
			XGetAtomName(g->display, ev->data.l[1]));
	} else {
		fprintf(stderr, "invalid window state command (%ld) for window 0x%x"
			"report with application name\n", ev->data.l[0], (int) ev->window);
	}

	hdr.window = ev->window;
	hdr.type = MSG_WINDOW_FLAGS;
	write_message(g->xchan, hdr, msg);
}

static void process_xevent_message(Ghandles * g, XClientMessageEvent * ev)
{
	DBG1("handle message %s to window 0x%x\n",
		XGetAtomName(g->display, ev->message_type),
		(int)ev->window);

	if (ev->message_type == g->tray_opcode)
		process_xevent_message_tray(g, ev);
	else if (ev->message_type == g->wm_state)
		process_xevent_message_wm_state(g, ev);
}

static void process_xevent_damage(Ghandles * g, XID window,
			   int x, int y, int width, int height)
{
	struct msg_shmimage mx;
	struct msg_hdr hdr;
	SKIP_NONMANAGED_WINDOW;

	hdr.type = MSG_SHMIMAGE;
	hdr.window = window;
	mx.x = x;
	mx.y = y;
	mx.width = width;
	mx.height = height;
	write_message(g->xchan, hdr, mx);
}

void process_xevent(Ghandles * g)
{
	XDamageNotifyEvent *dev;
	XEvent event_buffer;

	XNextEvent(g->display, &event_buffer);
	switch (event_buffer.type) {
	case CreateNotify:
		process_xevent_createnotify(g, (XCreateWindowEvent *)
					    & event_buffer);
		break;
	case DestroyNotify:
		process_xevent_destroy(g,
				       event_buffer.xdestroywindow.window);
		break;
	case MapNotify:
		process_xevent_map(g, event_buffer.xmap.window);
		break;
	case UnmapNotify:
		process_xevent_unmap(g, event_buffer.xmap.window);
		break;
	case ConfigureNotify:
		process_xevent_configure(g,
					 event_buffer.xconfigure.window,
					 (XConfigureEvent *) &
					 event_buffer);
		break;
	case SelectionNotify:
		process_xevent_selection(g,
					 (XSelectionEvent *) &
					 event_buffer);
		break;
	case SelectionRequest:
		process_xevent_selection_req(g,
					     (XSelectionRequestEvent *) &
					     event_buffer);
		break;
	case PropertyNotify:
		process_xevent_property(g, event_buffer.xproperty.window,
					(XPropertyEvent *) & event_buffer);
		break;
	case ClientMessage:
		process_xevent_message(g,
				       (XClientMessageEvent *) &
				       event_buffer);
		break;
	default:
		if (event_buffer.type == (damage_event + XDamageNotify)) {
			dev = (XDamageNotifyEvent *) & event_buffer;
//      fprintf(stderr, "x=%hd y=%hd gx=%hd gy=%hd w=%hd h=%hd\n",
//        dev->area.x, dev->area.y, dev->geometry.x, dev->geometry.y, dev->area.width, dev->area.height); 
			process_xevent_damage(g, dev->drawable,
					      dev->area.x,
					      dev->area.y,
					      dev->area.width,
					      dev->area.height);
//                      fprintf(stderr, "@");
		} else {
			DBG1("#");
		}
	}

}

// vim: noet:ts=8:
