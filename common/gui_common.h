#ifndef _GUI_COMMON_H
#define _GUI_COMMON_H 1

#include <stdint.h>

#include <X11/Xlib.h>

#ifdef __GNUC__
#  define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
#  define UNUSED(x) UNUSED_ ## x
#endif

#define DBG0(...) do {				\
		if (g->log_level > 0)		\
		fprintf(stderr, __VA_ARGS__);	\
	} while (0)

#define DBG1(...) do {				\
		if (g->log_level > 1)		\
		fprintf(stderr, __VA_ARGS__);	\
	} while (0)


#define read_struct(xchan, x)	read_data(xchan, (char *)&x, sizeof(x))
#define write_struct(xchan, x)	write_data(xchan, (char *)&x, sizeof(x))

#define write_message(xchan,x,y) do {					\
		x.untrusted_len = sizeof(y);				\
		real_write_message(xchan,				\
				(char *)&x, sizeof(x),			\
				(char *)&y, sizeof(y));			\
	} while(0)

struct xchan;

void write_data(struct xchan *xchan, char *buf, int size);
int real_write_message(struct xchan *xchan, char *hdr, int size, char *data, int datasize);
int read_data(struct xchan *xchan, char *buf, int size);
int dummy_handler(Display * dpy, XErrorEvent * ev);

int send_keymap(struct xchan *xchan, Display *display);
int recv_keymap(struct xchan *xchan, Display *display);

#endif /* _GUI_COMMON_H */

// vim: noet:ts=8:
