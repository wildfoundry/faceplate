/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

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

START_TEST(test_helper_runs_true_on_pty)
{
	char dir[] = "/tmp/fp-host-XXXXXX";
	char sock[128];
	int lfd, master, slave, status;
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
		_exit(faceplate_host_session_serve_one(conn) == 0 ? 0 : 3);
	}

	ck_assert_int_eq(openpty(&master, &slave, NULL, NULL, NULL), 0);
	client = fork();
	ck_assert_int_ge(client, 0);
	if (client == 0) {
		char *argv[] = { "/bin/true", NULL };

		close(master);
		close(lfd);
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

	close(slave);
	ck_assert_int_eq(waitpid(client, &status, 0), client);
	ck_assert(WIFEXITED(status));
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

START_TEST(test_disabled_helper_returns_notsup)
{
	char *argv[] = { "/bin/true", NULL };

	setenv("FACEPLATE_HOST_LOGIN", "0", 1);
	ck_assert_int_eq(faceplate_host_session_exec(argv), -ENOTSUP);
	unsetenv("FACEPLATE_HOST_LOGIN");
}
END_TEST

TEST_DEFINE_CASE(host_session)
TEST(test_helper_runs_true_on_pty)
TEST(test_disabled_helper_returns_notsup)
TEST_END_CASE

TEST_DEFINE(TEST_SUITE(faceplate_host_session, TEST_CASE(host_session), TEST_END))
