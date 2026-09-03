/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "host_session.h"
#include "test_common.h"

static int listen_unix(const char *path)
{
	struct sockaddr_un addr;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	ck_assert_int_ge(fd, 0);
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	unlink(path);
	ck_assert_int_eq(bind(fd, (struct sockaddr *)&addr, sizeof(addr)), 0);
	ck_assert_int_eq(listen(fd, 1), 0);
	return fd;
}

static void pty_client(int slave, const char *sock, char **argv)
{
	if (dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0 ||
	    dup2(slave, STDERR_FILENO) < 0)
		_exit(4);
	if (slave > STDERR_FILENO)
		close(slave);
	setenv("FACEPLATE_HOST_LOGIN_SOCKET", sock, 1);
	unsetenv("FACEPLATE_HOST_LOGIN");
	(void)faceplate_host_session_exec(argv);
	_exit(5);
}

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "r");
	char *buf;
	long n;

	ck_assert_ptr_nonnull(f);
	ck_assert_int_eq(fseek(f, 0, SEEK_END), 0);
	n = ftell(f);
	ck_assert_int_ge(n, 0);
	rewind(f);
	buf = malloc((size_t)n + 1);
	ck_assert_ptr_nonnull(buf);
	ck_assert_uint_eq(fread(buf, 1, (size_t)n, f), (size_t)n);
	buf[n] = '\0';
	fclose(f);
	return buf;
}

static const char *source_root(void)
{
	const char *root = getenv("FACEPLATE_SOURCE_ROOT");

	ck_assert_ptr_nonnull(root);
	ck_assert_int_gt((int)strlen(root), 0);
	return root;
}

START_TEST(test_helper_runs_true_on_pty)
{
	char dir[] = "/tmp/fp-host-XXXXXX";
	char sock[128];
	int lfd, master, slave, status;
	pid_t server, client;
	char *argv[] = {"/bin/false", NULL};

	ck_assert_ptr_nonnull(mkdtemp(dir));
	snprintf(sock, sizeof(sock), "%s/host-login.sock", dir);
	lfd = listen_unix(sock);

	server = fork();
	ck_assert_int_ge(server, 0);
	if (server == 0) {
		int conn = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);

		if (conn < 0)
			_exit(2);
		setenv("FACEPLATE_HOST_LOGIN_SKIP_CGROUP", "1", 1);
		setenv("FACEPLATE_HOST_LOGIN_BIN", "/bin/true", 1);
		_exit(faceplate_host_session_serve_one(conn) == 0 ? 0 : 3);
	}

	ck_assert_int_eq(openpty(&master, &slave, NULL, NULL, NULL), 0);
	client = fork();
	ck_assert_int_ge(client, 0);
	if (client == 0) {
		close(master);
		close(lfd);
		pty_client(slave, sock, argv);
	}

	close(slave);
	ck_assert_int_eq(waitpid(client, &status, 0), client);
	ck_assert(WIFEXITED(status));
	/* Payload argv was /bin/false; helper must still run /bin/true. */
	ck_assert_int_eq(WEXITSTATUS(status), 0);
	ck_assert_int_eq(waitpid(server, &status, 0), server);
	ck_assert(WIFEXITED(status));
	ck_assert_int_eq(WEXITSTATUS(status), 0);
	close(master);
	close(lfd);
	unlink(sock);
	rmdir(dir);
}
END_TEST

START_TEST(test_helper_rejects_non_pty)
{
	char dir[] = "/tmp/fp-host-XXXXXX";
	char sock[128];
	int lfd, sp[2], status;
	pid_t server, client;

	ck_assert_ptr_nonnull(mkdtemp(dir));
	snprintf(sock, sizeof(sock), "%s/host-login.sock", dir);
	lfd = listen_unix(sock);

	server = fork();
	ck_assert_int_ge(server, 0);
	if (server == 0) {
		int conn = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);

		if (conn < 0)
			_exit(2);
		setenv("FACEPLATE_HOST_LOGIN_SKIP_CGROUP", "1", 1);
		setenv("FACEPLATE_HOST_LOGIN_BIN", "/bin/true", 1);
		_exit(faceplate_host_session_serve_one(conn) == 0 ? 0 : 3);
	}

	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, sp), 0);
	client = fork();
	ck_assert_int_ge(client, 0);
	if (client == 0) {
		char *argv[] = {"/bin/true", NULL};

		close(lfd);
		close(sp[1]);
		if (dup2(sp[0], STDIN_FILENO) < 0)
			_exit(4);
		close(sp[0]);
		setenv("FACEPLATE_HOST_LOGIN_SOCKET", sock, 1);
		unsetenv("FACEPLATE_HOST_LOGIN");
		(void)faceplate_host_session_exec(argv);
		_exit(5);
	}

	close(sp[0]);
	close(sp[1]);
	ck_assert_int_eq(waitpid(client, &status, 0), client);
	ck_assert(WIFEXITED(status));
	ck_assert_int_eq(WEXITSTATUS(status), 255);
	ck_assert_int_eq(waitpid(server, &status, 0), server);
	ck_assert(WIFEXITED(status));
	ck_assert_int_eq(WEXITSTATUS(status), 3);
	close(lfd);
	unlink(sock);
	rmdir(dir);
}
END_TEST

START_TEST(test_helper_requires_faceplate_cgroup)
{
	char dir[] = "/tmp/fp-host-XXXXXX";
	char sock[128];
	int lfd, master, slave, status;
	pid_t server, client;
	char *argv[] = {"/bin/true", NULL};

	ck_assert_ptr_nonnull(mkdtemp(dir));
	snprintf(sock, sizeof(sock), "%s/host-login.sock", dir);
	lfd = listen_unix(sock);

	server = fork();
	ck_assert_int_ge(server, 0);
	if (server == 0) {
		int conn = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);

		if (conn < 0)
			_exit(2);
		unsetenv("FACEPLATE_HOST_LOGIN_SKIP_CGROUP");
		unsetenv("FACEPLATE_HOST_LOGIN_BIN");
		_exit(faceplate_host_session_serve_one(conn) == 0 ? 0 : 3);
	}

	ck_assert_int_eq(openpty(&master, &slave, NULL, NULL, NULL), 0);
	client = fork();
	ck_assert_int_ge(client, 0);
	if (client == 0) {
		close(master);
		close(lfd);
		pty_client(slave, sock, argv);
	}

	close(slave);
	ck_assert_int_eq(waitpid(client, &status, 0), client);
	ck_assert(WIFEXITED(status));
	ck_assert_int_eq(WEXITSTATUS(status), 255);
	ck_assert_int_eq(waitpid(server, &status, 0), server);
	ck_assert(WIFEXITED(status));
	ck_assert_int_eq(WEXITSTATUS(status), 3);
	close(master);
	close(lfd);
	unlink(sock);
	rmdir(dir);
}
END_TEST

START_TEST(test_disabled_helper_returns_notsup)
{
	char *argv[] = {"/bin/true", NULL};

	setenv("FACEPLATE_HOST_LOGIN", "0", 1);
	ck_assert_int_eq(faceplate_host_session_exec(argv), -ENOTSUP);
	unsetenv("FACEPLATE_HOST_LOGIN");
}
END_TEST

START_TEST(test_default_socket_path)
{
	unsetenv("FACEPLATE_HOST_LOGIN_SOCKET");
	ck_assert_str_eq(faceplate_host_session_socket_path(), FACEPLATE_HOST_LOGIN_SOCKET_DEFAULT);
	setenv("FACEPLATE_HOST_LOGIN_SOCKET", "/tmp/x.sock", 1);
	ck_assert_str_eq(faceplate_host_session_socket_path(), "/tmp/x.sock");
	unsetenv("FACEPLATE_HOST_LOGIN_SOCKET");
}
END_TEST

START_TEST(test_helper_unit_is_getty_shaped)
{
	char path[512];
	char *unit;

	snprintf(path, sizeof(path), "%s/data/faceplate-host-login.service", source_root());
	unit = read_file(path);
	ck_assert_ptr_nonnull(strstr(unit, "ProtectSystem=strict"));
	ck_assert_ptr_nonnull(strstr(unit, "ProtectHome=yes"));
	ck_assert_ptr_nonnull(strstr(unit, "PrivateTmp=yes"));
	ck_assert(strstr(unit, "RestrictAddressFamilies=") == NULL);
	ck_assert(strstr(unit, "PrivateNetwork=") == NULL);
	ck_assert(strstr(unit, "FACEPLATE_HOST_LOGIN_SKIP_CGROUP") == NULL);
	ck_assert(strstr(unit, "FACEPLATE_HOST_LOGIN_BIN") == NULL);
	free(unit);

	snprintf(path, sizeof(path), "%s/data/faceplate-host-login.socket", source_root());
	unit = read_file(path);
	ck_assert_ptr_nonnull(strstr(unit, "SocketMode=0600"));
	ck_assert_ptr_nonnull(strstr(unit, "SocketUser=root"));
	free(unit);
}
END_TEST

START_TEST(test_compositor_fails_closed_without_helper)
{
	char path[512];
	char *src;

	snprintf(path, sizeof(path), "%s/src/misc/pty.c", source_root());
	src = read_file(path);
	ck_assert_ptr_nonnull(strstr(src, "faceplate_host_session_exec"));
	ck_assert(strstr(src, "faceplate_host_session_enter_init_namespaces") == NULL);
	ck_assert_ptr_nonnull(strstr(src, "Fail closed"));
	free(src);
}
END_TEST

TEST_DEFINE_CASE(host_session)
TEST(test_helper_runs_true_on_pty)
TEST(test_helper_rejects_non_pty)
TEST(test_helper_requires_faceplate_cgroup)
TEST(test_disabled_helper_returns_notsup)
TEST(test_default_socket_path)
TEST(test_helper_unit_is_getty_shaped)
TEST(test_compositor_fails_closed_without_helper)
TEST_END_CASE

TEST_DEFINE(TEST_SUITE(faceplate_host_session, TEST_CASE(host_session), TEST_END))
