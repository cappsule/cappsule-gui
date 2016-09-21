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
#include <fcntl.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/shm.h>
#include <arpa/inet.h>
#include <sys/prctl.h>
#include <sys/types.h>

#include "gui_common.h"
#include "guiserver.h"
#include "qubes-gui-protocol.h"
#include "list.h"
#include "child.h"
#include "policy.h"
#include "userland.h"
#include "xchan.h"

/* calculate virtual width */
#define XORG_DEFAULT_XINC	8
#define _VIRTUALX(x)		( (((x)+XORG_DEFAULT_XINC-1)/XORG_DEFAULT_XINC)*XORG_DEFAULT_XINC )

#define LOCK_FILE		"/var/run/qubes_appviewer.lock"
#define SHMID_PATH_FMT		"/var/run/user/%d/cappsule/gui/shmid.%d.txt"

static struct policies *policies;
static Ghandles ghandles;

char version[] = GIT_VERSION;

struct serve_arg {
	int unused;
};

/* prepare graphic context for painting colorful frame */
static void get_frame_gc(Ghandles *ghandles, int rgb)
{
	unsigned int r, g, b;
	Colormap colormap;
	XGCValues values;
	XColor fcolor;
	int bits;

	r = (rgb >> 8) & 0xf;
	g = (rgb >> 4) & 0xf;
	b = (rgb >> 0) & 0xf;

	r <<= 12;
	g <<= 12;
	b <<= 12;

	for (bits = 4; bits < 16; bits *= 2) {
		r |= (r >> bits);
		g |= (g >> bits);
		b |= (b >> bits);
		bits *= 2;
	}

	fcolor.red = r;
	fcolor.green = g;
	fcolor.blue = b;

	colormap = XDefaultColormap(ghandles->display, ghandles->screen);
	XAllocColor(ghandles->display, colormap, &fcolor);

	values.foreground = fcolor.pixel;
	ghandles->frame_gc = XCreateGC(ghandles->display, ghandles->root_win, GCForeground, &values);
}

static int x11_error_handler(Display * dpy, XErrorEvent * ev)
{
	/* log the error */
	dummy_handler(dpy, ev);
#ifdef MAKE_X11_ERRORS_FATAL
	exit(EXIT_FAILURE);
#endif
	return 0;
}

static int open_lock_file(int create, int *rfd)
{
	int fd, flags;

	flags = O_RDWR;
	if (create) {
		flags |= O_CREAT;
		if (unlink(LOCK_FILE) == -1 && errno != ENOENT)
			warn("unlink(" LOCK_FILE ")");
	}

	fd = open(LOCK_FILE, flags, 0600);
	if (fd == -1) {
		warn("open(" LOCK_FILE ")");
		return -1;
	}

	if (create) {
		close(fd);
		fd = -1;
	}

	if (rfd != NULL)
		*rfd = fd;

	return 0;
}

/* prepare global variables content:
 * most of them are handles to local Xserver structures */
static void mkghandles(Ghandles *g, int lock_fd)
{
	char tray_sel_atom_name[64];
	XWindowAttributes attr;

	g->screen = DefaultScreen(g->display);
	g->root_win = RootWindow(g->display, g->screen);
	XGetWindowAttributes(g->display, g->root_win, &attr);
	g->root_width = _VIRTUALX(attr.width);
	g->root_height = attr.height;
	g->context = XCreateGC(g->display, g->root_win, 0, NULL);
	g->wmDeleteMessage =
	    XInternAtom(g->display, "WM_DELETE_WINDOW", True);
	g->clipboard_requested = 0;
	snprintf(tray_sel_atom_name, sizeof(tray_sel_atom_name),
		 "_NET_SYSTEM_TRAY_S%u", DefaultScreen(g->display));
	g->tray_selection =
	    XInternAtom(g->display, tray_sel_atom_name, False);
	g->tray_opcode =
	    XInternAtom(g->display, "_NET_SYSTEM_TRAY_OPCODE", False);
	g->xembed_message = XInternAtom(g->display, "_XEMBED", False);
	g->xembed_info = XInternAtom(g->display, "_XEMBED_INFO", False);
	g->wm_state = XInternAtom(g->display, "_NET_WM_STATE", False);
	g->wm_state_fullscreen =XInternAtom(g->display, "_NET_WM_STATE_FULLSCREEN", False);
	g->wm_state_demands_attention = XInternAtom(g->display, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
	g->frame_extents = XInternAtom(g->display, "_NET_FRAME_EXTENTS", False);

	/* initialize windows limit */
	g->windows_count_limit = g->windows_count_limit_param;

	/* init window lists */
	g->remote2local = list_new();
	g->wid2windowdata = list_new();
	g->screen_window = NULL;

	/* use qrexec for clipboard operations when stubdom GUI is used */
	g->use_kdialog = (getenv("KDE_SESSION_UID") != NULL);

	g->inter_appviewer_lock_fd = lock_fd;
}

static int shm_init(Ghandles *g, uid_t uid, char *display)
{
	char path[PATH_MAX], *p;
	FILE *f;
	int n;

	/* get display number */
	p = strchr(display, ':');
	p = (p == NULL) ? p : p + 1;
	n = atoi(p);

	snprintf(path, sizeof(path), SHMID_PATH_FMT, uid, n);

	f = fopen(path, "r");
	if (f == NULL) {
		fprintf(stderr, "Missing %s; run X with preloaded shmoverride\n",
			path);
		return -1;
	}

	if (fscanf(f, "%d", &g->cmd_shmid) != 1) {
		warnx("invalid shm id value");
		fclose(f);
		return -1;
	}

	fclose(f);

	g->shmcmd = shmat(g->cmd_shmid, NULL, 0);
	if (g->shmcmd == (void *)(-1UL)) {
		fprintf(stderr,
			"Invalid or stale shm id 0x%x in %s\n",
			g->cmd_shmid, path);
		return -1;
	}

	g->shmcmd->nohv = g->nohv;

	return 0;
}

static struct serve_arg *init(struct child_arg *arg)
{
	struct serve_arg *serve_arg;
	struct policy *policy;
	char path[PATH_MAX];
	int lock_fd;
	err_t error;

	if (prctl(PR_SET_PDEATHSIG, CHILD_DEATH_SIGNAL) == -1) {
		warn("prctl");
		return NULL;
	}

	policy = get_policy_by_uuid(policies, &arg->policy_uuid);
	if (policy == NULL) {
		warnx("bad policy uuid");
		return NULL;
	}

	error = set_logfile(arg->capsule_id, "guiserver.log");
	if (error) {
		print_error(error, "failed to set logfile");
		reset_saved_errno();
	}

	/* lock file is owned by root */
	if (open_lock_file(0, &lock_fd) == -1)
		return NULL;

	snprintf(path, sizeof(path), "/run/user/%d/gdm/Xauthority", arg->uid);
	setenv("XAUTHORITY", path, 1);

	ghandles.display = XOpenDisplay(arg->display);
	if (ghandles.display == NULL) {
		warn("XOpenDisplay");
		fprintf(stderr, "Failed to connect to display \"%s\"?\n",
			arg->display);
		return NULL;
	}

	mkghandles(&ghandles, lock_fd);
	XSetErrorHandler(x11_error_handler);

	ghandles.capsule_id = arg->capsule_id;

	printf("[*] color: #%03x (%s)\n", policy->window_color, policy->name);
	ghandles.label_index = policy->window_color;

	error = xchan_trusted_init(arg->capsule_id, XCHAN_GUI, &ghandles.xchan);
	if (error) {
		print_error(error, "failed to init xchan");
		return NULL;
	}

	if (setgid(arg->gid) != 0) {
		warn("setgid");
		return NULL;
	}

	if (setuid(arg->uid) != 0) {
		warn("setuid");
		return NULL;
	}

	if (shm_init(&ghandles, arg->uid, arg->display) != 0)
		return NULL;

	/* create graphical contexts */
	get_frame_gc(&ghandles, policy->window_color);
#ifdef FILL_TRAY_BG
	get_tray_gc(&ghandles);
#endif

	error = xchan_accept(ghandles.xchan);
	if (error) {
		print_error(error, "failed to accept gui client");
		return NULL;
	}

	if (send_keymap(ghandles.xchan, ghandles.display) != 0) {
		fprintf(stderr, "failed to send keymap to gui client\n");
		return NULL;
	}

	serve_arg = (struct serve_arg *)malloc(sizeof(*serve_arg));
	if (serve_arg == NULL) {
		warn("malloc");
		return NULL;
	}

	return serve_arg;
}

static void serve(struct serve_arg *arg)
{
	struct pollfd pollfds[2];
	err_t error;
	int busy;

	pollfds[0].fd = ghandles.xchan->event_fd;
	pollfds[0].events = POLLIN;

	pollfds[1].fd = ConnectionNumber(ghandles.display);
	pollfds[1].events = POLLIN;

	while (1) {
		if (TEMP_FAILURE_RETRY(poll(pollfds, 2, -1)) == -1)
			break;

		/* discard eventfd notification */
		if (pollfds[0].revents & POLLIN) {
			error = xchan_poll(ghandles.xchan);
			if (error) {
				print_error(error, "xchan poll failed");
				break;
			}
		}

		do {
			busy = 0;
			if (XPending(ghandles.display)) {
				process_xevent(&ghandles);
				busy = 1;
			}

			if (handle_message(&ghandles))
				busy = 1;
		} while (busy);
	}

	free(arg);

	XCloseDisplay(ghandles.display);
	exit(EXIT_SUCCESS);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: qubes-guid [-d] [-i icon name, no suffix, or icon.png path] [-v] [-q] [-a] [-f] [-V]\n");
	fprintf(stderr, "       -d  debug\n");
	fprintf(stderr, "       -v  increase log verbosity\n");
	fprintf(stderr, "       -q  decrease log verbosity\n");
	fprintf(stderr, "       -Q  force usage of Qrexec for clipboard operations\n");
	fprintf(stderr, "       -n  run without hypervisor\n");
	fprintf(stderr, "       -a  low-latency audio mode\n");
	fprintf(stderr, "       -f  do not fork into background\n");
	fprintf(stderr, "       -V  display the version number\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Log levels:\n");
	fprintf(stderr, " 0 - only errors\n");
	fprintf(stderr, " 1 - some basic messages (default)\n");
	fprintf(stderr, " 2 - debug\n");
}

static void parse_cmdline(Ghandles *g, int argc, char **argv)
{
	int opt;

	/* defaults */
	g->log_level = 2;
	g->qrexec_clipboard = 0;
	g->nofork = 0;
	g->allow_utf8_titles = 1;

	/* XXX: servers are launched by daemon whitout arguments. Get this
	 * option from environment. */
	g->nohv = (getenv("CAPPSULE_NOHV") != NULL);

	while ((opt = getopt(argc, argv, "dc:l:i:vqQnafV")) != -1) {
		switch (opt) {
		/*case 'a':
			g->audio_low_latency = 1;
			break;*/
		case 'd':
			g->debug = 1;
			g->log_level = 2;
			break;
		/*case 'i':
			g->cmdline_icon = optarg;
			break;*/
		case 'q':
			if (g->log_level>0)
				g->log_level--;
			break;
		case 'v':
			g->log_level++;
			break;
		case 'Q':
			g->qrexec_clipboard = 1;
			break;
		case 'n':
			g->nohv = 1;
			break;
		case 'f':
			g->nofork = 1;
			break;
		case 'V':
			display_version(argv[0], version, 1);
			break;
		default:
			usage();
			exit(EXIT_FAILURE);
		}
	}
}

int main(int argc, char *argv[])
{
	struct device device;
	err_t error;

	init_children();
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	memset(&ghandles, 0, sizeof(ghandles));
	parse_cmdline(&ghandles, argc, argv);

	display_version(argv[0], version, 0);

	error = parse_configuration_files(POLICIES_PATH, &policies);
	if (error) {
		print_error(error, "failed to parse configuration files in %s",
			    POLICIES_PATH);
		exit(EXIT_FAILURE);
	}

	if (open_lock_file(1, NULL) == -1)
		exit(EXIT_FAILURE);

	if (signal(SIGCHLD, sigchld_handler) == SIG_ERR)
		err(EXIT_FAILURE, "signal");

	device.type = DEVICE_GUI;
	device.policies_path = POLICIES_PATH;
	device.policies = &policies;
	device.init = init;
	device.serve = serve;
	device.prepare_child = NULL;
	device.child_created = NULL;
	device.cleanup_child = NULL;

	if (ghandles.debug) {
		device.notif_fd = -1;
		debug_device(&device);
		exit(EXIT_SUCCESS);
	}

	connect_to_monitor(&device.notif_fd);

	while (1) {
		handle_notif_msg(&device);
	}

	free_policies(policies);
	close(device.notif_fd);

	/* TODO: cleanup() */

	return 0;
}

// vim: noet:ts=8:
