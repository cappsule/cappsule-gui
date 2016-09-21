#ifndef _GUISERVER_H
#define _GUISERVER_H 1

#include <stdint.h>
#include <stdbool.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/XShm.h>

/* per-window data */
struct windowdata {
	unsigned width;
	unsigned height;
	int x;
	int y;
	int is_mapped;
	int is_docked;		/* is it docked tray icon */
	XID remote_winid;	/* window id on VM side */
	Window local_winid;	/* window id on X side */
	struct windowdata *parent;	/* parent window */
	struct windowdata *transient_for;	/* transient_for hint for WM, see http://tronche.com/gui/x/icccm/sec-4.html#WM_TRANSIENT_FOR */
	int override_redirect;	/* see http://tronche.com/gui/x/xlib/window/attributes/override-redirect.html */
	XShmSegmentInfo shminfo;	/* temporary shmid; see shmoverride/README */
	XImage *image;		/* image with window content */
	int image_height;	/* size of window content, not always the same as window in dom0! */
	int image_width;
	int have_queued_configure;	/* have configure request been sent to VM - waiting for confirmation */
	uint32_t flags_set;	/* window flags acked to gui-agent */
};

struct _global_handles {
	/* local X server handles and attributes */
	Display *display;
	int screen;		/* shortcut to the default screen */
	Window root_win;	/* root attributes */
	int root_width;		/* size of root window */
	int root_height;
	GC context;		/* context for pixmap operations */
	GC frame_gc;		/* graphic context to paint window frame */
#ifdef FILL_TRAY_BG
	GC tray_gc;		/* graphic context to paint tray background */
#endif
	/* atoms for comunitating with xserver */
	Atom wmDeleteMessage;	/* Atom: WM_DELETE_WINDOW */
	Atom tray_selection;	/* Atom: _NET_SYSTEM_TRAY_SELECTION_S<creen number> */
	Atom tray_opcode;	/* Atom: _NET_SYSTEM_TRAY_MESSAGE_OPCODE */
	Atom xembed_message;	/* Atom: _XEMBED */
	Atom xembed_info;	/* Atom: _XEMBED_INFO */
	Atom wm_state;         /* Atom: _NET_WM_STATE */
	Atom wm_state_fullscreen; /* Atom: _NET_WM_STATE_FULLSCREEN */
	Atom wm_state_demands_attention; /* Atom: _NET_WM_STATE_DEMANDS_ATTENTION */
	Atom frame_extents; /* Atom: _NET_FRAME_EXTENTS */
	/* shared memory handling */
	struct shm_cmd *shmcmd;	/* shared memory with Xorg */
	uint32_t cmd_shmid;		/* shared memory id - received from shmoverride.so through shm.id file */
	int inter_appviewer_lock_fd; /* FD of lock file used to synchronize shared memory access */
	/* Client VM parameters */
	char vmname[32];	/* name of VM */
	char *cmdline_color;	/* color of frame */
	int label_index;	/* label (frame color) hint for WM */
	struct windowdata *screen_window; /* window of whole VM screen */
	/* lists of windows: */
	/*   indexed by remote window id */
	struct genlist *remote2local;
	/*   indexed by local window id */
	struct genlist *wid2windowdata;
	/* counters and other state */
	int clipboard_requested;	/* if clippoard content was requested by dom0 */
	int windows_count;	/* created window count */
	int windows_count_limit;	/* current window limit; ask user what to do when exceeded */
	int windows_count_limit_param; /* initial limit of created windows - after exceed, warning the user */
	struct windowdata *last_input_window;
	/* configuration */
	int log_level;		/* log level */
	int nohv;
	int nofork;			   /* do not fork into background - used during guid restart */
	int allow_utf8_titles;	/* allow UTF-8 chars in window title */
	int allow_fullscreen;   /* allow fullscreen windows without decoration */
	int copy_seq_mask;	/* modifiers mask for secure-copy key sequence */
	KeySym copy_seq_key;	/* key for secure-copy key sequence */
	int paste_seq_mask;	/* modifiers mask for secure-paste key sequence */
	KeySym paste_seq_key;	/* key for secure-paste key sequence */
	int qrexec_clipboard;	/* 0: use GUI protocol to fetch/put clipboard, 1: use qrexec */
	int use_kdialog;	/* use kdialog for prompts (default on KDE) or zenity (default on non-KDE) */
	unsigned int capsule_id;

	int debug;
	struct xchan *xchan;
};

typedef struct _global_handles Ghandles;

// Special window ID meaning "whole screen"
#define FULLSCREEN_WINDOW_ID 0

void process_xevent(Ghandles * g);
bool handle_message(Ghandles * g);

#endif /* _GUISERVER_H */

// vim: noet:ts=8:
