#ifndef _SERVER_COMMON_H
#define _SERVER_COMMON_H 1

void send_configure(struct xchan *xchan, struct windowdata *vm_window,
		    int x, int y, int w, int h);
void do_shm_update(Ghandles * g, struct windowdata *vm_window,
		int untrusted_x, int untrusted_y, int untrusted_w,
		int untrusted_h);

#endif /* _COMMON_H */

// vim: noet:ts=8:
