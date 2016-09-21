#ifndef GUI_COMMON_H
#define GUI_COMMON_H

#define NOHV_SHM_FILE_FMT	"/dummy-%u"
#define NOHV_SHM_CAPSULE_FMT	"/run/cappsule-gui" NOHV_SHM_FILE_FMT
#define NOHV_SHM_HOST_FMT	"/run/cappsule/gui/%u" NOHV_SHM_FILE_FMT

struct fake_pixmap {
	size_t size;
	unsigned int memid;
	void *old_ptr;
};

#endif /* GUI_COMMON_H */
