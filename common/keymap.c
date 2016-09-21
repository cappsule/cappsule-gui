#include <err.h>
#include <X11/Xlib.h>

#include "gui_common.h"
#include "error.h"
#include "xchan.h"

#define MAX_KEYSYMS_PER_KEYCODE	16

struct keymap_dump {
	int first_keycode;
	int keysyms_per_keycode;
	int num_codes;

	/* The minimum number of KeyCodes returned is never less than 8, and the
	 * maximum number of KeyCodes returned is never greater than 255. */
	KeySym keysyms[256 * MAX_KEYSYMS_PER_KEYCODE];
};


/**
 * Send current keymap through xchan. The keymap is gathered as in the following
 * command: xmodmap -pke [filename].
 */
int send_keymap(struct xchan *xchan, Display *display)
{
	int min_keycode, max_keycode, keysyms_per_keycode, keycode_count;
	struct keymap_dump dump;
	KeySym *keymap;
	size_t size;
	err_t error;

	XDisplayKeycodes(display, &min_keycode, &max_keycode);
	if (min_keycode < 8 || max_keycode > 255) {
		warnx("invalid keycodes (%d-%d)", min_keycode, max_keycode);
		return -1;
	}

	keycode_count = max_keycode - min_keycode + 1;
	keymap = XGetKeyboardMapping(display,
				     min_keycode,
				     keycode_count,
				     &keysyms_per_keycode);
	if (keymap == NULL) {
		warnx("unable to get keyboard mapping table");
		return -1;
	}

	if (keysyms_per_keycode > MAX_KEYSYMS_PER_KEYCODE) {
		warnx("keysyms_per_keycode too large (%d)",
		      keysyms_per_keycode);
		XFree(keymap);
		return -1;
	}

	memset(&dump, 0, sizeof(dump));
	dump.first_keycode = min_keycode;
	dump.keysyms_per_keycode = keysyms_per_keycode;
	dump.num_codes = keycode_count;

	size = sizeof(*keymap) * keycode_count * keysyms_per_keycode;
	memcpy(dump.keysyms, keymap, size);

	XFree(keymap);

	error = xchan_sendall(xchan, &dump, sizeof(dump));
	if (error) {
		print_error(error, "failed to send keymap through xchan");
		return -1;
	}

	return 0;
}

/**
 * Receive host keymap through xchan. The keymap is set as in the following
 * command: xmodmap [filename].
 */
int recv_keymap(struct xchan *xchan, Display *display)
{
	struct keymap_dump dump;
	int max_keycode;
	err_t error;

	error = xchan_recvall(xchan, &dump, sizeof(dump));
	if (error) {
		print_error(error, "failed to recv keymap through xchan");
		return -1;
	}

	max_keycode = dump.first_keycode + dump.num_codes - 1;
	if (dump.first_keycode < 8 || max_keycode > 255) {
		warnx("invalid keycodes (%d-%d)",
		      dump.first_keycode, max_keycode);
		return -1;
	}

	if (dump.keysyms_per_keycode > MAX_KEYSYMS_PER_KEYCODE) {
		warnx("keysyms_per_keycode too large (%d)",
		      dump.keysyms_per_keycode);
		return -1;
	}

	XChangeKeyboardMapping(display,
			       dump.first_keycode,
			       dump.keysyms_per_keycode,
			       dump.keysyms,
			       dump.num_codes);

	return 0;
}
