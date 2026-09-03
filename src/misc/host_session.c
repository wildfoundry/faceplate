/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include "host_session.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int open_ns(const char *path)
{
	return open(path, O_RDONLY | O_CLOEXEC);
}

static int setns_path(const char *path, int nstype)
{
	int fd, ret;

	fd = open_ns(path);
	if (fd < 0)
		return -errno;
	ret = setns(fd, nstype);
	close(fd);
	return ret < 0 ? -errno : 0;
}

static int migrate_root_cgroup(void)
{
	static const char *paths[] = {
		"/sys/fs/cgroup/cgroup.procs",
		"/sys/fs/cgroup/unified/cgroup.procs",
		NULL,
	};
	char buf[32];
	int n, i, fd, ret;

	n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
	if (n < 0 || n >= (int)sizeof(buf))
		return -EOVERFLOW;

	for (i = 0; paths[i]; ++i) {
		fd = open(paths[i], O_WRONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		ret = write(fd, buf, (size_t)n);
		close(fd);
		if (ret == n)
			return 0;
	}
	return -ENOENT;
}

int faceplate_host_session_enter_init_namespaces(void)
{
	int ret = 0, rc;

	rc = setns_path("/proc/1/ns/mnt", CLONE_NEWNS);
	if (rc)
		ret = rc;
	rc = setns_path("/proc/1/ns/net", CLONE_NEWNET);
	if (rc && !ret)
		ret = rc;
	(void)setns_path("/proc/1/ns/uts", CLONE_NEWUTS);
	(void)setns_path("/proc/1/ns/ipc", CLONE_NEWIPC);
	rc = migrate_root_cgroup();
	if (rc && !ret)
		ret = rc;
	return ret;
}

const char *faceplate_host_session_socket_path(void)
{
	const char *path = getenv("FACEPLATE_HOST_LOGIN_SOCKET");

	if (path && *path)
		return path;
	return FACEPLATE_HOST_LOGIN_SOCKET_DEFAULT;
}

static int send_fd(int sock, int fd, const void *buf, size_t len)
{
	struct iovec iov = { .iov_base = (void *)buf, .iov_len = len };
	union {
		struct cmsghdr align;
		char buf[CMSG_SPACE(sizeof(int))];
	} u;
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = &u,
		.msg_controllen = sizeof(u),
	};
	struct cmsghdr *cmsg;
	ssize_t n;

	memset(&u, 0, sizeof(u));
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

	n = sendmsg(sock, &msg, 0);
	return n == (ssize_t)len ? 0 : -1;
}

static int recv_fd(int sock, int *fd_out, void *buf, size_t len)
{
	struct iovec iov = { .iov_base = buf, .iov_len = len };
	union {
		struct cmsghdr align;
		char buf[CMSG_SPACE(sizeof(int))];
	} u;
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = &u,
		.msg_controllen = sizeof(u),
	};
	struct cmsghdr *cmsg;
	ssize_t n;
	int fd = -1;

	memset(&u, 0, sizeof(u));
	n = recvmsg(sock, &msg, 0);
	if (n <= 0)
		return -1;
	if (msg.msg_flags & MSG_CTRUNC)
		return -1;

	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
		    cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
			memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
			break;
		}
	}
	if (fd < 0)
		return -1;
	*fd_out = fd;
	return (int)n;
}

static int append_cstr(char **buf, size_t *len, size_t *cap, const char *s)
{
	size_t n = strlen(s) + 1;
	char *nbuf;

	if (*len + n > *cap) {
		size_t ncap = (*cap ? *cap : 256);
		while (ncap < *len + n)
			ncap *= 2;
		nbuf = realloc(*buf, ncap);
		if (!nbuf)
			return -ENOMEM;
		*buf = nbuf;
		*cap = ncap;
	}
	memcpy(*buf + *len, s, n);
	*len += n;
	return 0;
}

static int pack_job(char **argv, char **out, size_t *out_len)
{
	uint32_t hdr[3];
	size_t cap = 0, len = 0, body = 0;
	char *buf = NULL;
	uint32_t argc = 0, envc = 0;
	int i, ret;

	if (!argv || !argv[0])
		return -EINVAL;

	for (i = 0; argv[i]; ++i) {
		ret = append_cstr(&buf, &len, &cap, argv[i]);
		if (ret)
			goto err;
		argc++;
	}
	if (environ) {
		for (i = 0; environ[i]; ++i) {
			ret = append_cstr(&buf, &len, &cap, environ[i]);
			if (ret)
				goto err;
			envc++;
		}
	}
	body = len;
	if (12 + body > cap) {
		char *nbuf = realloc(buf, 12 + body);
		if (!nbuf) {
			ret = -ENOMEM;
			goto err;
		}
		buf = nbuf;
	}
	memmove(buf + 12, buf, body);
	hdr[0] = FACEPLATE_HOST_LOGIN_MAGIC;
	hdr[1] = argc;
	hdr[2] = envc;
	memcpy(buf, hdr, 12);
	*out = buf;
	*out_len = 12 + body;
	return 0;
err:
	free(buf);
	return ret;
}

static int unpack_job(const char *buf, size_t len, char ***argv_out, char ***env_out)
{
	uint32_t magic, argc, envc, i;
	const char *p, *end;
	char **argv, **env;

	if (len < 12)
		return -EINVAL;
	memcpy(&magic, buf, 4);
	memcpy(&argc, buf + 4, 4);
	memcpy(&envc, buf + 8, 4);
	if (magic != FACEPLATE_HOST_LOGIN_MAGIC || argc == 0 || argc > 64 || envc > 512)
		return -EINVAL;

	argv = calloc(argc + 1, sizeof(*argv));
	env = calloc(envc + 1, sizeof(*env));
	if (!argv || !env) {
		free(argv);
		free(env);
		return -ENOMEM;
	}

	p = buf + 12;
	end = buf + len;
	for (i = 0; i < argc; ++i) {
		if (p >= end) {
			free(argv);
			free(env);
			return -EINVAL;
		}
		argv[i] = (char *)p;
		p += strlen(p) + 1;
	}
	for (i = 0; i < envc; ++i) {
		if (p >= end) {
			free(argv);
			free(env);
			return -EINVAL;
		}
		env[i] = (char *)p;
		p += strlen(p) + 1;
	}
	*argv_out = argv;
	*env_out = env;
	return 0;
}

int faceplate_host_session_exec(char **argv)
{
	char *payload = NULL;
	size_t plen = 0;
	struct sockaddr_un addr;
	int sock = -1, ret;
	uint32_t status = 255;
	ssize_t n;

	if (getenv("FACEPLATE_HOST_LOGIN") && !strcmp(getenv("FACEPLATE_HOST_LOGIN"), "0"))
		return -ENOTSUP;

	ret = pack_job(argv, &payload, &plen);
	if (ret)
		return ret;

	sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (sock < 0) {
		ret = -errno;
		goto out;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s",
		 faceplate_host_session_socket_path());
	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		ret = -errno;
		goto out;
	}

	if (send_fd(sock, STDIN_FILENO, payload, plen) < 0) {
		ret = -EIO;
		goto out;
	}

	n = read(sock, &status, sizeof(status));
	if (n != (ssize_t)sizeof(status))
		status = 255;
	free(payload);
	close(sock);
	if (WIFEXITED(status))
		_exit(WEXITSTATUS(status));
	if (WIFSIGNALED(status))
		_exit(128 + WTERMSIG(status));
	_exit(255);

out:
	free(payload);
	if (sock >= 0)
		close(sock);
	return ret;
}

static int peer_is_allowed(int conn)
{
	struct ucred cred;
	socklen_t len = sizeof(cred);
	char path[64], line[256];
	FILE *f;
	int ok = 0;

	if (getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0)
		return -errno;
	if (len < sizeof(cred))
		return -EINVAL;
	/* Helper and Faceplate both run as root in production. In tests both
	 * run as the same unprivileged uid. Never accept a different uid.
	 */
	if (cred.uid != geteuid() || cred.pid <= 0)
		return -EPERM;
	if (cred.pid == getpid())
		return -EPERM;

	if (getenv("FACEPLATE_HOST_LOGIN_SKIP_CGROUP"))
		return 0;

	snprintf(path, sizeof(path), "/proc/%d/cgroup", cred.pid);
	f = fopen(path, "r");
	if (!f)
		return -EPERM;
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, "faceplate.service")) {
			ok = 1;
			break;
		}
	}
	fclose(f);
	return ok ? 0 : -EPERM;
}

static int fd_is_pty_slave(int fd)
{
	struct stat st;

#ifdef __linux__
#ifndef UNIX98_PTY_SLAVE_MAJOR
#define UNIX98_PTY_SLAVE_MAJOR 136
#endif
#endif

	if (fd < 0 || !isatty(fd))
		return 0;
	if (fstat(fd, &st) < 0)
		return 0;
	if (!S_ISCHR(st.st_mode))
		return 0;
#ifdef __linux__
	if (major(st.st_rdev) != UNIX98_PTY_SLAVE_MAJOR)
		return 0;
#endif
	return 1;
}

static const char *login_binary(void)
{
	const char *bin;

	/* Tests only. Never taken from the socket payload. Production units
	 * must not set SKIP_CGROUP.
	 */
	bin = getenv("FACEPLATE_HOST_LOGIN_BIN");
	if (bin && *bin && getenv("FACEPLATE_HOST_LOGIN_SKIP_CGROUP"))
		return bin;
	return "/bin/login";
}

static int sanitized_term(char **env, char *out, size_t out_len)
{
	const char *v = "vt220";
	size_t i, n;

	if (env) {
		for (i = 0; env[i]; ++i) {
			if (!strncmp(env[i], "TERM=", 5)) {
				v = env[i] + 5;
				break;
			}
		}
	}
	n = strlen(v);
	if (n < 1 || n > 32)
		v = "vt220";
	for (i = 0; v[i]; ++i) {
		char c = v[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		      (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) {
			v = "vt220";
			break;
		}
	}
	snprintf(out, out_len, "TERM=%s", v);
	return 0;
}

static void run_login(int pty, char **env)
{
	char term[48];
	char *envp[8];
	char *argv[3];
	int i;

	if (setsid() < 0 && errno != EPERM)
		_exit(127);
	if (ioctl(pty, TIOCSCTTY, 0) < 0 && errno != EPERM) {
		/* already controlling */
	}
	if (dup2(pty, STDIN_FILENO) < 0 || dup2(pty, STDOUT_FILENO) < 0 ||
	    dup2(pty, STDERR_FILENO) < 0)
		_exit(127);
	if (pty > STDERR_FILENO)
		close(pty);
	for (i = 3; i < 256; ++i)
		close(i);

	argv[0] = (char *)login_binary();
	argv[1] = "-p";
	argv[2] = NULL;

	sanitized_term(env, term, sizeof(term));
	envp[0] = term;
	envp[1] = "PATH=/usr/sbin:/usr/bin:/sbin:/bin";
	envp[2] = "HOME=/";
	envp[3] = "SHELL=/bin/sh";
	envp[4] = NULL;

	execve(argv[0], argv, envp);
	_exit(127);
}

int faceplate_host_session_serve_one(int conn)
{
	char buf[65536];
	char **argv = NULL, **env = NULL;
	int pty = -1, n, status = 0;
	pid_t pid;
	uint32_t st;

	if (peer_is_allowed(conn) < 0)
		goto fail;

	n = recv_fd(conn, &pty, buf, sizeof(buf));
	if (n < 12)
		goto fail;
	if (unpack_job(buf, (size_t)n, &argv, &env))
		goto fail;
	if (!fd_is_pty_slave(pty))
		goto fail;

	pid = fork();
	if (pid < 0)
		goto fail;
	if (pid == 0)
		run_login(pty, env);

	close(pty);
	pty = -1;
	if (waitpid(pid, &status, 0) < 0)
		status = 255;
	st = (uint32_t)status;
	(void)write(conn, &st, sizeof(st));
	free(argv);
	free(env);
	return 0;

fail:
	st = 255;
	(void)write(conn, &st, sizeof(st));
	if (pty >= 0)
		close(pty);
	free(argv);
	free(env);
	return -1;
}
