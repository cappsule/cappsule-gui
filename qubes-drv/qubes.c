/*
 * Copyright 2007 Peter Hutterer
 * Copyright 2009 Przemysław Firszt
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of Red Hat
 * not be used in advertising or publicity pertaining to distribution
 * of the software without specific, written prior permission.  Red
 * Hat makes no representations about the suitability of this software
 * for any purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
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


#include "config.h"

#include <linux/input.h>

#include <xf86_OSproc.h>

#include <xf86.h>
#include <xf86Xinput.h>

#include <windowstr.h>

#ifdef HAVE_PROPERTIES
#include <xserver-properties.h>
/* 1.6 has properties, but no labels */
#ifdef AXIS_LABEL_PROP
#define HAVE_LABELS
#else
#undef HAVE_LABELS
#endif

#endif

#include <err.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "qubes-gui-protocol.h"
#include "xdriver-shm-cmd.h"
#include "cuapi/guest/mfn.h"
#include "userland.h"
#include "gui_nohv.h"

#define SOCKET_ADDRESS  "/run/shm/xf86-qubes-socket"
#define PAGE_SIZE	4096UL
#define PAGE_MASK	(~(PAGE_SIZE-1))
#define PAGE_SHIFT	12

typedef struct _QubesDeviceRec
{
    char *device;
    int version;        /* Driver version */
    Atom* labels;
    int num_vals;
    int axes;
} QubesDeviceRec, *QubesDevicePtr;

#ifdef __GNUC__
#  define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
#  define UNUSED(x) UNUSED_ ## x
#endif

#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 12
static int QubesPreInit(InputDriverPtr drv, InputInfoPtr pInfo,
				  int flags);
#else
static InputInfoPtr QubesPreInit(InputDriverPtr drv, IDevPtr dev,
				  int flags);
#endif

static void QubesUnInit(InputDriverPtr drv, InputInfoPtr pInfo,
			 int flags);
static pointer QubesPlug(pointer module, pointer options, int *errmaj,
			  int *errmin);
static void QubesUnplug(pointer p);
static void QubesReadInput(InputInfoPtr pInfo);
static int QubesControl(DeviceIntPtr device, int what);
static int _qubes_init_buttons(DeviceIntPtr device);
static int _qubes_init_axes(DeviceIntPtr device);

static int mfn_fd = -1;
static int nohv;


_X_EXPORT InputDriverRec QUBES = {
	1,
	"qubes",
	NULL,
	QubesPreInit,
	QubesUnInit,
	NULL,
	0,
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 21
	0
#endif
};

static XF86ModuleVersionInfo QubesVersionRec = {
	"qubes",
	MODULEVENDORSTRING,
	MODINFOSTRING1,
	MODINFOSTRING2,
	XORG_VERSION_CURRENT,
	PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR,
	PACKAGE_VERSION_PATCHLEVEL,
	ABI_CLASS_XINPUT,
	ABI_XINPUT_VERSION,
	MOD_CLASS_XINPUT,
	{0, 0, 0, 0}
};

_X_EXPORT XF86ModuleData qubesModuleData = {
	&QubesVersionRec,
	&QubesPlug,
	&QubesUnplug
};

static void QubesUnplug(pointer UNUSED(p))
{
}

static pointer
QubesPlug(pointer module, pointer UNUSED(options), int *UNUSED(errmaj), int *UNUSED(errmin))
{
	xf86AddInputDriver(&QUBES, module, 0);
	return module;
}


#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 12
static int QubesPreInit(InputDriverPtr UNUSED(drv),
			InputInfoPtr pInfo, int UNUSED(flags))
#else
static InputInfoPtr QubesPreInit(InputDriverPtr UNUSED(drv),
				IDevPtr dev, int UNUSED(flags))
#endif
{
	QubesDevicePtr pQubes;
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) < 12
	InputInfoPtr pInfo;

	if (!(pInfo = xf86AllocateInput(drv, 0)))
		return NULL;
#endif

	pQubes = calloc(1, sizeof(QubesDeviceRec));
	if (!pQubes) {
		pInfo->private = NULL;
		xf86DeleteInput(pInfo, 0);
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 12
		return BadAlloc;
#else
		return NULL;
#endif
	}

	nohv = (getenv("CAPPSULE_NOHV") != NULL);

#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) < 12
	pInfo->name = xstrdup(dev->identifier);
	pInfo->flags = 0;
	pInfo->conf_idev = dev;
#endif

	pInfo->private = pQubes;
	pInfo->type_name = XI_MOUSE;	/* see XI.h */
	pInfo->read_input = QubesReadInput;	/* new data avl */
	pInfo->switch_mode = NULL;	/* toggle absolute/relative mode */
	pInfo->device_control = QubesControl;	/* enable/disable dev */
	/* process driver specific options */
	pQubes->device = xf86SetStrOption(
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 12
			pInfo->options,
#else
			dev->commonOptions,
#endif
			"Device", SOCKET_ADDRESS);

	xf86Msg(X_INFO, "%s: Using device %s.\n", pInfo->name,
		pQubes->device);
//	xf86Msg(X_INFO, "%s: dixLookupWindow=%p.\n", pInfo->name,
//		dixLookupWindow);
//	xf86Msg(X_INFO, "%s: dixLookupResourceByClass=%p.\n", pInfo->name,
//		dixLookupResourceByClass);

	/* process generic options */
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 12
	xf86CollectInputOptions(pInfo, NULL);
#else
	xf86CollectInputOptions(pInfo, NULL, NULL);
#endif
	xf86ProcessCommonOptions(pInfo, pInfo->options);
	pInfo->fd = -1;
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 12
	return Success;
#else
	pInfo->flags |= XI86_OPEN_ON_INIT;
	pInfo->flags |= XI86_CONFIGURED;
	return pInfo;
#endif
}

static void QubesUnInit(InputDriverPtr UNUSED(drv), InputInfoPtr pInfo, int UNUSED(flags))
{
	QubesDevicePtr pQubes = pInfo->private;

	if (pQubes->device) {
		free(pQubes->device);
		pQubes->device = NULL;
		/* Common error - pInfo->private must be NULL or valid memoy before
		 * passing into xf86DeleteInput */
		pInfo->private = NULL;
	}
	xf86DeleteInput(pInfo, 0);
}

static int _qubes_init_kbd(DeviceIntPtr device)
{
         InitKeyboardDeviceStruct(device, NULL, NULL, NULL);
	 return Success;
}

static int _qubes_init_buttons(DeviceIntPtr device)
{
	InputInfoPtr pInfo = device->public.devicePrivate;
	QubesDevicePtr pQubes = pInfo->private;
	CARD8 *map;
	int i;
	int ret = Success;
	const int num_buttons = 6;

	map = calloc(num_buttons, sizeof(CARD8));

	xf86Msg(X_INFO, "%s: num_buttons=%d\n", pInfo->name, num_buttons);

	for (i = 0; i < num_buttons; i++)
		map[i] = i;

	pQubes->labels = malloc(sizeof(Atom));

	if (!InitButtonClassDeviceStruct
	    (device, num_buttons, pQubes->labels, map)) {
		xf86Msg(X_ERROR, "%s: Failed to register buttons.\n",
			pInfo->name);
		ret = BadAlloc;
	}

	free(map);
	return ret;
}

static void QubesInitAxesLabels(QubesDevicePtr UNUSED(pQubes), int UNUSED(natoms),
				Atom * UNUSED(atoms))
{
#ifdef HAVE_LABELS
	Atom atom;
	int axis;
	char **labels;
	int labels_len = 0;
	char *misc_label;

	labels = rel_labels;
	labels_len = ArrayLength(rel_labels);
	misc_label = AXIS_LABEL_PROP_REL_MISC;

	memset(atoms, 0, natoms * sizeof(Atom));

	/* Now fill the ones we know */
	for (axis = 0; axis < labels_len; axis++) {
		if (pQubes->axis_map[axis] == -1)
			continue;

		atom = XIGetKnownProperty(labels[axis]);
		if (!atom)	/* Should not happen */
			continue;

		atoms[pQubes->axis_map[axis]] = atom;
	}
#endif
}


static int _qubes_init_axes(DeviceIntPtr device)
{
	InputInfoPtr pInfo = device->public.devicePrivate;
	QubesDevicePtr pQubes = pInfo->private;
	int i;
	const int num_axes = 2;
	Atom *atoms;

	pQubes->num_vals = num_axes;
	atoms = malloc(pQubes->num_vals * sizeof(Atom));

	QubesInitAxesLabels(pQubes, pQubes->num_vals, atoms);
	if (!InitValuatorClassDeviceStruct(device, num_axes,
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 7
					   atoms,
#endif
					   GetMotionHistorySize(), 0))
		return BadAlloc;

#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) < 12
	pInfo->dev->valuator->mode = Relative;
#endif
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) < 13
	if (!InitAbsoluteClassDeviceStruct(device))
		return BadAlloc;
#endif

	for (i = 0; i < pQubes->axes; i++) {
		xf86InitValuatorAxisStruct(device, i, *pQubes->labels, -1,
					   -1, 1, 1, 1
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 12
					   , Relative
#endif
					   );
		xf86InitValuatorDefaults(device, i);
	}
	free(atoms);
	return Success;
}

static int connect_unix_socket(QubesDevicePtr pQubes)
{
	struct sockaddr_un remote;
	int s, len;

	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		perror("socket");
		return -1;
	}

	remote.sun_family = AF_UNIX;
	remote.sun_path[0] = '\x00';
	strncpy(remote.sun_path + 1, pQubes->device, sizeof(remote.sun_path) - 1);
	len = strlen(pQubes->device) + 1 + sizeof(remote.sun_family);

	if (connect(s, (struct sockaddr *) &remote, len) == -1) {
		warn("connect");
		close(s);
		return -1;
	}

	return s;
}

static void QubesPtrCtrlProc(DeviceIntPtr UNUSED(device), PtrCtrl *UNUSED(ctrl))
{
	/* This function intentionally left blank */
}

static int QubesControl(DeviceIntPtr device, int what)
{
	InputInfoPtr pInfo = device->public.devicePrivate;
	QubesDevicePtr pQubes = pInfo->private;

	switch (what) {
	case DEVICE_INIT:
		xf86Msg(X_INFO, "%s: Init.\n", pInfo->name);
		device->public.on = FALSE;
		_qubes_init_buttons(device);
		_qubes_init_axes(device);
		_qubes_init_kbd(device);
		InitPtrFeedbackClassDeviceStruct(device, QubesPtrCtrlProc);
		break;

		/* Switch device on.  Establish socket, start event delivery.  */
	case DEVICE_ON:
		xf86Msg(X_INFO, "%s: On.\n", pInfo->name);
		if (device->public.on)
			break;
		do {
			pInfo->fd = connect_unix_socket(pQubes);
			if (pInfo->fd < 0) {
				xf86Msg(X_ERROR,
					"%s: cannot open device; sleeping...\n",
					pInfo->name);
				usleep(1000000);
			}
		} while (pInfo->fd < 0);

		xf86FlushInput(pInfo->fd);
		xf86AddEnabledDevice(pInfo);
		device->public.on = TRUE;
		break;

	case DEVICE_OFF:
		xf86Msg(X_INFO, "%s: Off.\n", pInfo->name);
		if (!device->public.on)
			break;
		xf86RemoveEnabledDevice(pInfo);
		close(pInfo->fd);
		pInfo->fd = -1;
		device->public.on = FALSE;
		break;

	case DEVICE_CLOSE:
		xf86Msg(X_INFO, "%s: Close.\n", pInfo->name);
		/* free what we have to free */
		break;
	}

	return Success;
}

/* The following helper is copied from Xen sources */
static int write_exact(int fd, const void *data, size_t size)
{
    size_t offset = 0;
    ssize_t len;

    while (offset < size) {
        len = write(fd, (const char *)data + offset, size - offset);
        if ((len == -1) && (errno == EINTR))
            continue;
        else if (len <= 0)
            return -1;
        offset += len;
    }

    return 0;
}

static __always_inline void trigger_ept_violation(unsigned char *addr)
{
	volatile unsigned char __attribute__((unused)) c;

	c = *addr;
}

static unsigned long get_mfn_for_page(unsigned long uaddr)
{
	unsigned long pfn;

	/* /dev/mfn is created inside capsule and thus can't be open before */
	if (mfn_fd == -1) {
		mfn_fd = open("/dev/" GUEST_MFN_DEVICE_NAME, O_RDONLY);
		if (mfn_fd == -1)
			err(1, "open(\"/dev/" GUEST_MFN_DEVICE_NAME "\")");
	}

	/* force page access to trigger ept translation installation */
	trigger_ept_violation((unsigned char *)uaddr);

	pfn = ioctl(mfn_fd, MFN_GET, uaddr);

	return pfn;
}

static void dump_window_mfns(WindowPtr pWin, int UNUSED(id), int fd)
{
	struct fake_pixmap *fake_pixmap;
	ScreenPtr screen;
	PixmapPtr pixmap;
	int i, off, num_mfn, mfn;
	struct shm_cmd shmcmd;
	char *pixels, *pixels_end;

	if (!pWin)
		return;

	screen = pWin->drawable.pScreen;
	pixmap = (*screen->GetWindowPixmap) (pWin);

	pixels = pixmap->devPrivate.ptr;
	pixels_end =
	    pixels +
	    pixmap->drawable.width * pixmap->drawable.height *
	    pixmap->drawable.bitsPerPixel / 8;
	off = ((long) pixels) & (PAGE_SIZE - 1);
	pixels -= off;
	num_mfn = ((long)pixels_end - (long)pixels + 4095) >> PAGE_SHIFT;
	//fprintf(stderr, "%p - %p | %d\n", pixels, pixels_end, num_mfn); fflush(stderr);

	shmcmd.capsule_id = -1;
	shmcmd.nohv = nohv;	/* overwritten by daemon anyway */
	shmcmd.shmid = -1;

	shmcmd.width = pixmap->drawable.width;
	shmcmd.height = pixmap->drawable.height;
	shmcmd.bpp = pixmap->drawable.bitsPerPixel;
	shmcmd.off = off;
	shmcmd.num_mfn = (pixmap->devPrivate.ptr != NULL) ? num_mfn : 0;

	write_exact(fd, &shmcmd, sizeof(shmcmd));
	if (!nohv) {
		/* XXX */
		//mlock(pixels, PAGE_SIZE * num_mfn);
		for (i = 0; i < num_mfn; i++) {
			mfn = get_mfn_for_page((unsigned long)(pixels + PAGE_SIZE * i));
			write_exact(fd, &mfn, 4);
		}
	} else {
		/* This is a dirty hack, but I don't want to have a different
		 * qubes protocol for nohv.
		 *
		 * Since pixels is mmaped, its address is a multiple of
		 * PAGE_SIZE, and the fake_pixmap structure is located at the
		 * beginning of the memory mapping.
		 *
		 * Send memid instead of mfn. */
		fake_pixmap = (struct fake_pixmap *)pixels;

		mfn = fake_pixmap->memid;
		LogMessageVerbSigSafe(X_INFO, 0, "%s: pixels=%p memid=%u\n", __func__, pixels, mfn);
		for (i = 0; i < num_mfn; i++)
			write_exact(fd, &mfn, 4);
	}
}

static WindowPtr id2winptr(unsigned int xid)
{
	WindowPtr result;
	int ret;

	ret = dixLookupResourceByClass((void **)&result, xid, RC_DRAWABLE, 0, 0);
	if (ret != Success)
		result = NULL;

	return result;
}

static void process_request(int fd, InputInfoPtr pInfo)
{
	struct xdriver_cmd cmd;
	WindowPtr w1;
	int ret;

	ret = read(fd, &cmd, sizeof(cmd));
	if (ret == 0) {
		LogMessageVerbSigSafe(X_INFO, 0, "%s: unix closed\n", __func__);
		close(fd);
		exit(1);
	} else if (ret == -1) {
		LogMessageVerbSigSafe(X_INFO, 0, "%s: unix error\n", __func__);
		close(fd);
		exit(1);
	}

	//LogMessageVerbSigSafe(X_INFO, 0, "randdev: received %c 0x%x 0x%x\n", cmd.type, cmd.arg1, cmd.arg2);

	write_exact(fd, "0", 1); // acknowledge the request has been received

	switch (cmd.type) {
	case 'W':
            w1 = id2winptr(cmd.arg1);
            if (!w1) {
                    // This error condition (window not found) can happen when
                    // the window is destroyed before the driver sees the req
                    struct shm_cmd shmcmd;
		    LogMessageVerbSigSafe(X_INFO,
				          0,
				          "randdev: w1=%p, xid1: 0x%x\n",
				          w1,
				          cmd.arg1);
                    shmcmd.num_mfn = 0;
                    write_exact(fd, &shmcmd, sizeof(shmcmd));
                    return;
            }
            dump_window_mfns(w1, cmd.arg1, fd);
            break;

	case 'B':
	    xf86PostButtonEvent(pInfo->dev, 0, cmd.arg1, cmd.arg2, 0,0);
	    break;

        case 'M':
            xf86PostMotionEvent(pInfo->dev, 1, 0, 2, cmd.arg1, cmd.arg2);
            break;

        case 'K':
            xf86PostKeyboardEvent(pInfo->dev, cmd.arg1, cmd.arg2);
            break;

        default:
            xf86Msg(X_INFO, "randdev: unknown command %c\n", cmd.type);
        }
}

static void QubesReadInput(InputInfoPtr pInfo)
{
	while (xf86WaitForInput(pInfo->fd, 0) > 0) {
		process_request(pInfo->fd, pInfo);
#if 0
		xf86PostMotionEvent(pInfo->dev, 0,	/* is_absolute */
				    0,	/* first_valuator */
				    1,	/* num_valuators */
				    data);
#endif
	}
}

// vim: noet:ts=8:
