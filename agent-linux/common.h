#ifndef _GUICLIENT_COMMON_H
#define _GUICLIENT_COMMON_H 1

void send_clipboard_data(Ghandles * g, char *data, int len);
uint32_t flags_from_atom(Ghandles * g, Atom a);
void feed_xdriver(Ghandles *g, int type, int arg1, int arg2);

#endif /* _GUICLIENT_COMMON_H */

// vim: noet:ts=8:
