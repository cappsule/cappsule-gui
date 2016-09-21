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
#include <stdint.h>
#include <unistd.h>

#include "guiclient.h"
#include "common.h"
#include "qubes-gui-protocol.h"
#include "xdriver-shm-cmd.h"
#include "gui_common.h"

void send_clipboard_data(Ghandles * g, char *data, int len)
{
	struct msg_hdr hdr;
	hdr.type = MSG_CLIPBOARD_DATA;
	if (len > MAX_CLIPBOARD_SIZE)
		hdr.window = MAX_CLIPBOARD_SIZE;
	else
		hdr.window = len;
	hdr.untrusted_len = hdr.window;
	write_struct(g->xchan, hdr);
	write_data(g->xchan, (char *) data, len);
}

uint32_t flags_from_atom(Ghandles * g, Atom a) {
	if (a == g->wm_state_fullscreen)
		return WINDOW_FLAG_FULLSCREEN;
	else if (a == g->wm_state_demands_attention)
		return WINDOW_FLAG_DEMANDS_ATTENTION;
	else {
		/* ignore unsupported states */
	}
	return 0;
}

void feed_xdriver(Ghandles *g, int type, int arg1, int arg2)
{
	struct xdriver_cmd cmd;
	char ans;
	int ret;

	cmd.type = type;
	cmd.arg1 = arg1;
	cmd.arg2 = arg2;
	if (write(g->xserver_fd, &cmd, sizeof(cmd)) != sizeof(cmd))
		err(1, "%s: write", __func__);

	ans = '1';
	do {
		ret = read(g->xserver_fd, &ans, sizeof(ans));
	} while (ret == -1 && (errno == EINTR || errno == EAGAIN));

	if (ret != 1 || ans != '0') {
		fprintf(stderr, "read returned %d, char read=0x%02x\n",
			ret, (unsigned char)ans);
		err(1, "%s: read (%d)", __func__, errno);
	}
}

// vim: noet:ts=8:
