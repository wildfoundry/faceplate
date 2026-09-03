/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "host_session.h"

#define SD_LISTEN_FDS_START 3

static int bind_listen(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	unlink(path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	(void)chmod(path, 0600);
	if (listen(fd, 4) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int main(void)
{
	const char *listen_fds = getenv("LISTEN_FDS");
	int lfd, conn;

	if (listen_fds && strcmp(listen_fds, "1") == 0)
		lfd = SD_LISTEN_FDS_START;
	else {
		lfd = bind_listen(faceplate_host_session_socket_path());
		if (lfd < 0) {
			perror("faceplate-host-login: listen");
			return 1;
		}
	}

	for (;;) {
		conn = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
		if (conn < 0) {
			if (errno == EINTR)
				continue;
			perror("faceplate-host-login: accept");
			return 1;
		}
		(void)faceplate_host_session_serve_one(conn);
		close(conn);
	}
}
