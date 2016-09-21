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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/* get_capsule_gateway */
#include <sys/ioctl.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <linux/netdevice.h>

//#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>

/* XXX: for debug_reopen_pty() */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "device_client.h"
#include "userland.h"

#include "guiclient.h"
#include "gui_common.h"
#include "list.h"

#include "cuapi/guest/xchan.h"
#include "xchan.h"

#define SOCKET_ADDRESS  "/run/shm/xf86-qubes-socket"
#define MAXINTERFACES	20

int damage_event, damage_error;


static int wait_for_unix_socket(void)
{
	struct sockaddr_un peer;
	unsigned int addrlen;
	int fd, s;

	s = bind_abstract_socket(SOCK_STREAM, SOCKET_ADDRESS, 1);
	if (s == -1)
		errx(1, "cannot bind socket to %s", SOCKET_ADDRESS);

	addrlen = sizeof(peer);
	fd = accept(s, (struct sockaddr *)&peer, &addrlen);
	if (fd == -1)
		err(1, "accept");

	close(s);

	fprintf(stderr, "[+] guiclient: connection from qubes' driver\n");

	return fd;
}

/* XXX: debug: remove me */
static void debug_reopen_pty(void)
{
	struct stat st;
	int fd, ret;

	/* Guest initializes xchan before console, so console device may not
	 * exist even if xchan device is already created. */
	while (1) {
		ret = stat(GUEST_CONSOLE_DEVICE, &st);
		if (ret != -1)
			break;
		usleep(1000);
	}

	fd = open(GUEST_CONSOLE_DEVICE, O_WRONLY, 0);
	if (fd == -1)
		err(1, "open(console)");

	if (dup2(fd, STDOUT_FILENO) < 0)
		err(1, "dup2");

	if (dup2(fd, STDERR_FILENO) < 0)
		err(1, "dup2");

	if (close(fd) < 0)
		err(1, "close");
}

static int in_capsule(void)
{
	struct stat st;

	return stat(GUEST_CONSOLE_DEVICE, &st) != -1;
}

static int reconnect_(Ghandles *g)
{
	char *p, *capsule_id;
	char buf[PATH_MAX];
	err_t error;

	/* XXX: dirty. guest kernel should send a SIGCONT to fsclient on first
	 * schedule */
	if (!g->debug) {
		if (!in_capsule())
			return 0;
		debug_reopen_pty();

		p = CAPSULE_FS;
	} else {
		capsule_id = getenv(ENV_CAPSULE_ID);
		if (capsule_id == NULL)
			errx(1, "failed to get capsule id from environment");

		/* XXX: fs/%s shouldn't be hardcoded */
		snprintf(buf, sizeof(buf), CAPPSULE_RUN_DIR "fs/%s", capsule_id);
		p = buf;
	}

	error = xchan_capsule_init(XCHAN_GUI, &g->xchan);
	if (error) {
		print_error(error, "failed to init xchan");
		exit(EXIT_FAILURE);
	}

	if (chroot(p) == -1)
                err(1, "chroot(\"%s\")", p);

        if (chdir("/") == -1)
		err(1, "chdir");

	if (g->userspec != NULL)
		drop_uid_from_str(g->userspec);

	return 1;
}

static void reconnect(Ghandles *g)
{
	while (!reconnect_(g))
		usleep(100000);
}

static void proxy(Ghandles *g)
{
	struct pollfd pollfds[2];
	err_t error;
	int busy;

	pollfds[0].fd = g->xchan->event_fd;
	pollfds[0].events = POLLIN;

	pollfds[1].fd = ConnectionNumber(g->display);
	pollfds[1].events = POLLIN | POLLERR;

	while (1) {
		if (TEMP_FAILURE_RETRY(poll(pollfds, 2, -1)) == -1)
			err(1, "poll");

		/* discard eventfd notification */
		if (pollfds[0].revents & POLLIN) {
			error = xchan_poll(g->xchan);
			if (error) {
				print_error(error, "xchan poll failed");
				break;
			}
		}

		do {
			busy = 0;
			if (XPending(g->display)) {
				process_xevent(g);
				busy = 1;
			}

			if (handle_message(g))
				busy = 1;

		} while (busy);
	}

	exit(EXIT_SUCCESS);
}

static void mkghandles(Ghandles * g)
{
	char tray_sel_atom_name[64];
	Atom net_wm_name, net_supporting_wm_check, net_supported;
	Atom supported[6];

	/* wait for Xorg qubes_drv to connect to us */
	g->xserver_fd = wait_for_unix_socket();

	/* even if /tmp/.X11-unix/X1 doesn't exist in guest filesystem, Xorg
	 * creates an usable abstract socket address (@/tmp/.X11-unix/X1) */
	g->display = XOpenDisplay(NULL);
	if (!g->display)
		err(1, "XOpenDisplay");

	DBG0("Connection to local X server established.\n");

	g->screen = DefaultScreen(g->display);	/* get CRT id number */
	g->root_win = RootWindow(g->display, g->screen);	/* get default attributes */
	g->context = XCreateGC(g->display, g->root_win, 0, NULL);
	g->wmDeleteMessage =
	    XInternAtom(g->display, "WM_DELETE_WINDOW", False);
	g->wmProtocols = XInternAtom(g->display, "WM_PROTOCOLS", False);
	g->utf8_string_atom = XInternAtom(g->display, "UTF8_STRING", False);
	g->stub_win = XCreateSimpleWindow(g->display, g->root_win,
					       0, 0, 1, 1,
					       0, BlackPixel(g->display,
							     g->screen),
					       WhitePixel(g->display,
							  g->screen));
	/* pretend that GUI agent is window manager */
	net_wm_name = XInternAtom(g->display, "_NET_WM_NAME", False);
	net_supporting_wm_check = XInternAtom(g->display, "_NET_SUPPORTING_WM_CHECK", False);
	net_supported = XInternAtom(g->display, "_NET_SUPPORTED", False);
	supported[0] = net_supported;
	supported[1] = net_supporting_wm_check;
	/* _NET_WM_MOVERESIZE required to disable broken GTK+ move/resize fallback */
	supported[2] = XInternAtom(g->display, "_NET_WM_MOVERESIZE", False);
	supported[3] = XInternAtom(g->display, "_NET_WM_STATE", False);
	supported[4] = XInternAtom(g->display, "_NET_WM_STATE_FULLSCREEN", False);
	supported[5] = XInternAtom(g->display, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
	XChangeProperty(g->display, g->stub_win, net_wm_name, g->utf8_string_atom,
			8, PropModeReplace, (unsigned char*)"Qubes", 5);
	XChangeProperty(g->display, g->stub_win, net_supporting_wm_check, XA_WINDOW,
			32, PropModeReplace, (unsigned char*)&g->stub_win, 1);
	XChangeProperty(g->display, g->root_win, net_supporting_wm_check, XA_WINDOW,
			32, PropModeReplace, (unsigned char*)&g->stub_win, 1);
	XChangeProperty(g->display, g->root_win, net_supported, XA_ATOM,
			32, PropModeReplace, (unsigned char*)supported, sizeof(supported)/sizeof(supported[0]));

	g->clipboard_data = NULL;
	g->clipboard_data_len = 0;
	snprintf(tray_sel_atom_name, sizeof(tray_sel_atom_name),
		 "_NET_SYSTEM_TRAY_S%u", DefaultScreen(g->display));
	g->tray_selection =
	    XInternAtom(g->display, tray_sel_atom_name, False);
	g->tray_opcode =
	    XInternAtom(g->display, "_NET_SYSTEM_TRAY_OPCODE", False);
	g->xembed_info = XInternAtom(g->display, "_XEMBED_INFO", False);
	g->wm_state = XInternAtom(g->display, "_NET_WM_STATE", False);
	g->wm_state_fullscreen = XInternAtom(g->display, "_NET_WM_STATE_FULLSCREEN", False);
	g->wm_state_demands_attention = XInternAtom(g->display, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
	g->wm_take_focus = XInternAtom(g->display, "WM_TAKE_FOCUS", False);
}

static void usage(char *argv0)
{
	fprintf(stderr, "Usage: %s [-d] [-v] [-q] [-h] [-u uid:gid] [-p devicereadyfd ] <pipefd>\n", argv0);
	fprintf(stderr, "       -d  no capsule\n");
	fprintf(stderr, "       -v  increase log verbosity\n");
	fprintf(stderr, "       -q  decrease log verbosity\n");
	fprintf(stderr, "       -m  sync all modifiers before key event (default: only Caps Lock)\n");
	fprintf(stderr, "       -u  specify user and group to use\n");
	fprintf(stderr, "       -h  print this message\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Log levels:\n");
	fprintf(stderr, " 0 - only errors\n");
	fprintf(stderr, " 1 - some basic messages (default)\n");
	fprintf(stderr, " 2 - debug\n");
}

static void parse_args(Ghandles * g, int argc, char **argv)
{
	int opt;

	// defaults
	g->log_level = 0;
	g->sync_all_modifiers = 0;
	g->debug = false;
	g->userspec = NULL;
	g->pipe_device_ready_w = -1;

	while ((opt = getopt(argc, argv, "dqvhmp:u:")) != -1) {
		switch (opt) {
		case 'd':
			g->debug = true;
			break;
		case 'q':
			g->log_level--;
			break;
		case 'v':
			g->log_level++;
			break;
		case 'm':
			g->sync_all_modifiers = 1;
			break;
		case 'p':
			g->pipe_device_ready_w = atoi(optarg);
			break;
		case 'u':
			g->userspec = optarg;
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			usage(argv[0]);
			exit(1);
		}
	}
}

int main(int argc, char *argv[])
{
	int i, pipefd;
	Ghandles g;
	char c;

	if (argc < 2) {
		usage(argv[0]);
		exit(0);
	}

	/* DISPLAY is set by daemon */
	if (getenv("DISPLAY") == NULL)
		errx(1, "DISPLAY isn't set");

	parse_args(&g, argc, argv);
	mkghandles(&g);

	for (i = 0; i < ScreenCount(g.display); i++) {
		XCompositeRedirectSubwindows(g.display,
					RootWindow(g.display, i),
					CompositeRedirectManual);
	}

	for (i = 0; i < ScreenCount(g.display); i++) {
		XSelectInput(g.display, RootWindow(g.display, i),
			SubstructureNotifyMask);
	}


	if (!XDamageQueryExtension(g.display, &damage_event,
					&damage_error)) {
		perror("XDamageQueryExtension");
		exit(1);
	}

	XAutoRepeatOff(g.display);

	signal(SIGCHLD, SIG_IGN);
	windows_list = list_new();
	embeder_list = list_new();

	XSetErrorHandler(dummy_handler);
	XSetSelectionOwner(g.display, g.tray_selection, g.stub_win,
			CurrentTime);

	if (XGetSelectionOwner(g.display, g.tray_selection) == g.stub_win) {
		XClientMessageEvent ev;
		memset(&ev, 0, sizeof(ev));
		ev.type = ClientMessage;
		ev.send_event = True;
		ev.message_type = XInternAtom(g.display, "MANAGER", False);
		ev.window = DefaultRootWindow(g.display);
		ev.format = 32;
		ev.data.l[0] = CurrentTime;
		ev.data.l[1] = g.tray_selection;
		ev.data.l[2] = g.stub_win;
		ev.display = g.display;
		XSendEvent(ev.display, ev.window, False, NoEventMask,
			   (XEvent *) & ev);
		if (g.log_level > 0)
			fprintf(stderr,
				"Acquired MANAGER selection for tray\n");
	}

	/* notify daemon that guiclient is connected to Xorg */
	pipefd = atoi(argv[argc-1]);
	c = '1';
	if (write(pipefd, &c, sizeof(c)) != sizeof(c))
		err(1, "write to daemon pipe");

	reconnect(&g);

	if (client_ready(g.pipe_device_ready_w) != 0)
		exit(EXIT_FAILURE);

	if (recv_keymap(g.xchan, g.display) != 0) {
		fprintf(stderr, "failed to recv keymap from gui server\n");
		exit(EXIT_FAILURE);
	}

	proxy(&g);

	return 0;
}

// vim: noet:ts=8:
