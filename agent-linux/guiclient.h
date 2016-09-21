#ifndef _GUICLIENT_H
#define _GUICLIENT_H 1

#include <stdbool.h>

#include <X11/Xlibint.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

struct xchan;

struct _global_handles {
	Display *display;
	int screen;		/* shortcut to the default screen */
	Window root_win;	/* root attributes */
	GC context;
	Atom wmDeleteMessage;
	Atom wmProtocols;
	Atom tray_selection;	/* Atom: _NET_SYSTEM_TRAY_SELECTION_S<creen number> */
	Atom tray_opcode;	/* Atom: _NET_SYSTEM_TRAY_MESSAGE_OPCODE */
	Atom xembed_info;	/* Atom: _XEMBED_INFO */
	Atom utf8_string_atom; /* Atom: UTF8_STRING */
	Atom wm_state;         /* Atom: _NET_WM_STATE */
	Atom wm_state_fullscreen; /* Atom: _NET_WM_STATE_FULLSCREEN */
	Atom wm_state_demands_attention; /* Atom: _NET_WM_STATE_DEMANDS_ATTENTION */
	Atom wm_take_focus;	/* Atom: WM_TAKE_FOCUS */
	int xserver_fd;
	Window stub_win;    /* window for clipboard operations and to simulate LeaveNotify events */
	unsigned char *clipboard_data;
	unsigned int clipboard_data_len;
	int log_level;
	int sync_all_modifiers;

	struct xchan *xchan;
	bool debug;
	char *userspec;
	int pipe_device_ready_w;
};

struct window_data {
	int is_docked; /* is it docked icon window */
	XID embeder;   /* for docked icon points embeder window */
	int input_hint; /* the window should get input focus - False=Never */
	int support_take_focus;
};

struct embeder_data {
	XID icon_window;
};

struct genlist *windows_list;
struct genlist *embeder_list;
typedef struct _global_handles Ghandles;

extern int damage_event, damage_error;

bool handle_message(Ghandles *g);
void process_xevent(Ghandles * g);

#endif /* _GUICLIENT_H */

// vim: noet:ts=8:
