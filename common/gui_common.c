/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
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
#include <poll.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xlibint.h>

#include "gui_common.h"
#include "qubes-gui-protocol.h"
#include "xchan.h"


int read_data(struct xchan *xchan, char *buf, int size)
{
	err_t error;

	error = xchan_recvall(xchan, buf, size);
	if (error) {
		print_error(error, "failed to read data");
		exit(EXIT_FAILURE);
	}

//      fprintf(stderr, "read %d bytes\n", size);
	return size;
}

void write_data(struct xchan *xchan, char *buf, int size)
{
	err_t error;

	error = xchan_sendall(xchan, buf, size); // this may block
	if (error) {
		print_error(error, "failed to write data");
		exit(EXIT_FAILURE);
	}
}

int real_write_message(struct xchan *xchan, char *hdr, int size, char *data, int datasize)
{
	write_data(xchan, hdr, size);
	write_data(xchan, data, datasize);
	return 0;
}

int dummy_handler(Display * dpy, XErrorEvent * ev)
{
#define ERROR_BUF_SIZE 256
	char buf[ERROR_BUF_SIZE];
	char request[ERROR_BUF_SIZE];
	_XExtension *ext = NULL;

	XGetErrorText(dpy, ev->error_code, buf, sizeof(buf));
	fprintf(stderr, "ErrorHandler: %s\n", buf);

	/* Find major opcode name */
	if (ev->request_code < 128) {
		snprintf(request, sizeof(request), "%d", ev->request_code);
		XGetErrorDatabaseText(dpy, "XRequest", request, "", buf,
				      sizeof(buf));
	} else {
		for (ext = dpy->ext_procs;
		     ext && ext->codes.major_opcode != ev->request_code;
		     ext = ext->next);
		if (ext)
			strncpy(buf, ext->name, sizeof(buf));
		else
			buf[0] = '\0';
	}
	fprintf(stderr, "                 Major opcode: %d (%s)\n",
		ev->request_code, buf);

	/* Find minor opcode name */
	if (ev->request_code >= 128 && ext) {
		snprintf(request, sizeof(request), "%d", ev->request_code);
		snprintf(request, sizeof(request), "%s.%d",
			 ext->name, ev->minor_code);
		XGetErrorDatabaseText(dpy, "XRequest", request, "", buf,
				      sizeof(buf));
		fprintf(stderr, "                 Minor opcode: %d (%s)\n",
			ev->minor_code, buf);
	}

	/* Provide value information */
	switch (ev->error_code) {
	case BadValue:
		fprintf(stderr, "                 Value:        0x%x\n",
			(int) ev->resourceid);
		break;
	case BadAtom:
		fprintf(stderr, "                 AtomID:       0x%x\n",
			(int) ev->resourceid);
		break;
	default:
		fprintf(stderr, "                 ResourceID:   0x%x\n",
			(int) ev->resourceid);
		break;
	}

	/* Provide serial number information */
	fprintf(stderr, "                 Failed serial number:  %ld\n",
		ev->serial);
	fprintf(stderr, "                 Current serial number: %ld\n",
		dpy->request);
	return 0;
}

// vim: noet:ts=8:
