#define _GNU_SOURCE 1
#include <err.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>

#include "userland.h"

static typeof(accept) *real_accept;
static int hijacked;

/* Trusted guest can't know easily if an error occurs in encapsulated Xorg.
 *
 * Hook accept and redirect stderr to console device when guiclient makes a
 * connection to Xorg. Also chroots to CAPSULE_FS in case new files are opened.
 *
 * It could be cleaner to execute Xorg directly from a chroot, but it seems to
 * be quite difficult...*/
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
	int fd;

	if (real_accept == NULL) {
		if (unsetenv("LD_PRELOAD") == -1)
			warn("unsetenv(\"LD_PRELOAD\")");

		real_accept = dlsym(RTLD_NEXT, "accept");
		if (real_accept == NULL)
			err(1, "dlsym");
	}

	if (!hijacked) {
		fd = open(GUEST_CONSOLE_DEVICE, O_WRONLY, 0);

		/* ensures that Xorg is encapsulated */
		if (fd != -1) {
			/* Xorg closes stdout, redirect stderr only */
			if (dup2(fd, STDERR_FILENO) == -1)
				err(1, "dup2");

			if (close(fd) == -1)
				err(1, "close");

			if (chroot(CAPSULE_FS) == -1)
				err(1, "chroot");

			if (chdir("/") == -1)
				err(1, "chdir");

			hijacked = 1;
		}
	}

	return real_accept(sockfd, addr, addrlen);
}
