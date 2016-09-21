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

#define DEBUG

#define _GNU_SOURCE 1
#include <err.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <alloca.h>
#include <limits.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>


#include "qubes-gui-protocol.h"
#include "cuapi/trusted/mfn.h"
#include "list.h"
#include "gui_nohv.h"
#include "userland.h"
#include "error.h"


#define X11_UNIX_FMT	"/tmp/.X11-unix/X%u"
#define DEVICE_MFN	"/dev/capsule_mfn"
#define SHMID_PATH_FMT	"/var/run/user/%d/cappsule/gui/shmid.%d.txt"
#define SHMAT_ERR	((void *)-1)
#define PAGE_SIZE	4096UL
#define PAGE_MASK	(~(PAGE_SIZE-1))

#ifdef DEBUG
#  define DBG(...)	do {				\
		fprintf(stderr, "shmoverride: ");	\
		fprintf(stderr, __VA_ARGS__);		\
	} while (0)
#else
#  define DBG(...)
#endif

static typeof(shmat) *real_shmat;
static typeof(shmdt) *real_shmdt;
static typeof(shmctl) *real_shmctl;
static typeof(bind) *real_bind;

static int local_shmid = -1;
static struct shm_cmd *cmd_pages;
static struct genlist *addr_list;
static int list_len;
static int mfn_fd = -1;
static int display = -1;


static char *map_mfn(unsigned int id, unsigned long *pfntable,
		unsigned int num_mfn, size_t fakesize)
{
	struct host_mfn host_mfn;
	char *fakeaddr;

	DBG("%s\n", __func__);

	if (mfn_fd == -1) {
		mfn_fd = open(DEVICE_MFN, O_RDWR);
		if (mfn_fd == -1) {
			warn("can't open \"" DEVICE_MFN "\"");
			return MAP_FAILED;
		}
	}

	host_mfn.capsule_id = id;
	host_mfn.num_mfn = num_mfn;
	host_mfn.pfntable = pfntable;

	if (write(mfn_fd, &host_mfn, sizeof(host_mfn)) != sizeof(host_mfn)) {
		warn("can't write to \"" DEVICE_MFN "\"");
		return MAP_FAILED;
	}

	fakeaddr = mmap(NULL, fakesize, PROT_READ, MAP_SHARED, mfn_fd, 0);

	return fakeaddr;
}

static char *map_mfn_nohv(unsigned int capsule_id, unsigned int memid, size_t fakesize)
{
	char path[PATH_MAX];
	char *addr;
	int shm_fd;

	addr = MAP_FAILED;

	snprintf(path, sizeof(path), NOHV_SHM_HOST_FMT, capsule_id, memid);
	shm_fd = open(path, O_RDONLY, 0600);
	if (shm_fd == -1) {
		warn("open(\"%s\")", path);
		return addr;
	}

	//if (unlink(path) == -1)
	//warn("unlink(\"%s\")", path);

	addr = mmap(NULL, fakesize, PROT_READ, MAP_SHARED, shm_fd, 0);
	if (addr == MAP_FAILED)
		warn("mmap");

	/* closing the file descriptor doesn't unmap the region */
	if (close(shm_fd) == -1)
		warn("close");

	return addr;
}

void *shmat(int shmid, const void *shmaddr, int shmflg)
{
 	unsigned long *pfntable;
	unsigned int i, memid;
	ssize_t fakesize;
	char *fakeaddr;

	//fprintf(stderr, "shmat(0x%x) 0x%x\n", shmid, cmd_pages->shmid); fflush(stderr);

	if (!cmd_pages || shmid != (int)cmd_pages->shmid)
		return real_shmat(shmid, shmaddr, shmflg);

	if (cmd_pages->off >= PAGE_SIZE || cmd_pages->num_mfn > MAX_MFN_COUNT
		|| cmd_pages->num_mfn == 0) {
		errno = EINVAL;
		return SHMAT_ERR;
	}

	fakesize = PAGE_SIZE * cmd_pages->num_mfn;
	if (!cmd_pages->nohv) {
		pfntable = alloca(sizeof(*pfntable) * cmd_pages->num_mfn);
		DBG("size=%d table=%p\n", cmd_pages->num_mfn, pfntable);

		for (i = 0; i < cmd_pages->num_mfn; i++) {
			pfntable[i] = cmd_pages->mfns[i];
			//fprintf(stderr, "pfntable[%d] = %ld\n", i, pfntable[i]);
		}

		fakeaddr = map_mfn(cmd_pages->capsule_id, pfntable,
				cmd_pages->num_mfn, fakesize);
	} else {
		memid = cmd_pages->mfns[0];
		fakeaddr = map_mfn_nohv(cmd_pages->capsule_id, memid, fakesize);
	}

	DBG("%s: num=%d, addr=%p, fakesize=%ld len=%d\n", __func__,
		cmd_pages->num_mfn, fakeaddr, fakesize, list_len);
	if (fakeaddr == MAP_FAILED) {
		warnx("failed to map pages");
		return SHMAT_ERR;
	}

	list_insert(addr_list, (long)fakeaddr, (void *)fakesize);
	list_len++;

	return fakeaddr + cmd_pages->off;
}

int shmdt(const void *shmaddr)
{
	struct genlist *item;
	unsigned long addr;

	addr = ((unsigned long)shmaddr) & PAGE_MASK;
	item = list_lookup(addr_list, addr);
	if (item == NULL)
		return real_shmdt(shmaddr);

	DBG("%s: munmap(%p, %ld)\n", __func__, (void *)addr, (size_t)item->data);

	if (munmap((void *)addr, (size_t)item->data) == -1)
		err(1, "munmap");

	list_remove(item);
	list_len--;

	/* close mfn_fd when possible, it allows kernel module to be removed. */
	if (list_len == 0 && mfn_fd != -1) {
		close(mfn_fd);
		mfn_fd = -1;
	}

	return 0;
}

int shmctl(int shmid, int cmd, struct shmid_ds *buf)
{
	//fprintf(stderr, "shmctl(0x%x, %d)\n", shmid, cmd); fflush(stderr);
	if (!cmd_pages || shmid != (int)cmd_pages->shmid || cmd != IPC_STAT)
		return real_shmctl(shmid, cmd, buf);

	memset(&buf->shm_perm, 0, sizeof(buf->shm_perm));
	buf->shm_segsz = cmd_pages->num_mfn * PAGE_SIZE - cmd_pages->off;

	return 0;
}

static int create_shmid_file(int display)
{
	char path[PATH_MAX], idbuf[20];
	int idfd, len;
	err_t error;

	snprintf(path, sizeof(path), SHMID_PATH_FMT, getuid(), display);

	error = make_dirs(path);
	if (error) {
		print_error(error, "failed to make dirs \"%s\"", path);
		return -1;
	}

	idfd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (idfd == -1) {
		warn("shmoverride creating %s", path);
		return -1;
	}

	len = sprintf(idbuf, "%d", local_shmid);
	if (write(idfd, idbuf, len) != len) {
		close(idfd);
		unlink(path);
		warn("shmoverride writing %s", path);
		return -1;
	}

	close(idfd);

	return 0;
}

static int create_shm(int display)
{
	size_t size;

	size = SHM_CMD_NUM_PAGES * PAGE_SIZE;
	local_shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0700);
	if (local_shmid == -1) {
		warn("shmoverride shmget");
		return -1;
	}

	cmd_pages = real_shmat(local_shmid, NULL, 0);
	if (cmd_pages == SHMAT_ERR) {
		cmd_pages = NULL;
		warn("real_shmat");
		return -1;
	}

	if (create_shmid_file(display) != 0)
		return -1;

	cmd_pages->shmid = local_shmid;

	return 0;
}

static int get_display(const struct sockaddr *addr, int *display)
{
	struct sockaddr_un *un;

	if (addr->sa_family != AF_LOCAL)
		return -1;

	un = (struct sockaddr_un *)addr;
	if (sscanf(un->sun_path, X11_UNIX_FMT, display) != 1)
		return -1;

	return 0;
}

/* bind() is hooked to get X11 display number during socket binding */
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	int ret;

	ret = real_bind(sockfd, addr, addrlen);
	if (ret == 0 && local_shmid == -1 && get_display(addr, &display) == 0) {
		if (create_shm(display) != 0)
			errx(EXIT_FAILURE, "failed to create shm");
	}

	return ret;
}

static int __attribute__ ((constructor)) initfunc(void)
{
	if (unsetenv("LD_PRELOAD") == -1)
		warn("unsetenv(\"LD_PRELOAD\")");

	fprintf(stderr, "shmoverride constructor running\n");

	real_shmat = dlsym(RTLD_NEXT, "shmat");
	real_shmctl = dlsym(RTLD_NEXT, "shmctl");
	real_shmdt = dlsym(RTLD_NEXT, "shmdt");
	real_bind = dlsym(RTLD_NEXT, "bind");

	if (real_shmat == NULL || real_shmctl == NULL || real_shmdt == NULL ||
	    real_bind == NULL) {
		err(1, "shmoverride: missing shm API");
	}

	addr_list = list_new();

	return 0;
}

static int __attribute__ ((destructor)) descfunc(void)
{
	char path[PATH_MAX];

	if (cmd_pages != NULL) {
		if (real_shmdt(cmd_pages) == -1)
			warn("%s: shmdt", __func__);
	}

	if (local_shmid != -1) {
		if (real_shmctl(local_shmid, IPC_RMID, 0) == -1)
			warn("%s: shmctl", __func__);
	}

	if (display != -1) {
		snprintf(path, sizeof(path), SHMID_PATH_FMT, getuid(), display);
		if (unlink(path) == -1)
			warn("%s: unlink %s", __func__, path);
	}

	/* XXX: walk addr_list and delete stuff? */

	return 0;
}

// vim: noet:ts=8:
