/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "faceplate_status.h"
#include "test_common.h"

static const char valid[] = "FACEPLATE_STATUS_V1\n"
			    "serial=DP-1042-7C9A\n"
			    "dataplicity=connected\n"
			    "updated_boottime_ms=1000\n";

static int make_dir(char *path)
{
	strcpy(path, "/tmp/faceplate-status-XXXXXX");
	if (!mkdtemp(path))
		return -errno;
	return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

static int write_status(int dirfd, const char *payload, mode_t mode)
{
	ssize_t len = (ssize_t)strlen(payload);
	int fd = openat(dirfd, "faceplate.status", O_WRONLY | O_CREAT | O_TRUNC, mode);

	if (fd < 0)
		return -errno;
	if (write(fd, payload, (size_t)len) != len) {
		close(fd);
		return -EIO;
	}
	close(fd);
	return 0;
}

static void cleanup(int dirfd, const char *path)
{
	unlinkat(dirfd, "faceplate.status", 0);
	close(dirfd);
	rmdir(path);
}

START_TEST(test_missing_is_hidden)
{
	char path[64];
	struct faceplate_status_snapshot snapshot;
	int dirfd = make_dir(path);

	ck_assert_int_ge(dirfd, 0);
	ck_assert_int_eq(faceplate_status_read_at(dirfd, getuid(), 1000, 30000, &snapshot),
			 FACEPLATE_STATUS_HIDDEN);
	ck_assert_str_eq(snapshot.serial, "");
	cleanup(dirfd, path);
}
END_TEST

START_TEST(test_fresh_is_visible)
{
	char path[64];
	struct faceplate_status_snapshot snapshot;
	int dirfd = make_dir(path);

	ck_assert_int_ge(dirfd, 0);
	ck_assert_int_eq(write_status(dirfd, valid, 0640), 0);
	ck_assert_int_eq(faceplate_status_read_at(dirfd, getuid(), 5000, 30000, &snapshot),
			 FACEPLATE_STATUS_FRESH);
	ck_assert_str_eq(snapshot.serial, "DP-1042-7C9A");
	ck_assert_int_eq(snapshot.connection, FACEPLATE_CONNECTION_CONNECTED);
	cleanup(dirfd, path);
}
END_TEST

START_TEST(test_stale_is_visible_unknown)
{
	char path[64];
	struct faceplate_status_snapshot snapshot;
	int dirfd = make_dir(path);

	ck_assert_int_ge(dirfd, 0);
	ck_assert_int_eq(write_status(dirfd, valid, 0640), 0);
	ck_assert_int_eq(faceplate_status_read_at(dirfd, getuid(), 31001, 30000, &snapshot),
			 FACEPLATE_STATUS_STALE);
	ck_assert_str_eq(snapshot.serial, "DP-1042-7C9A");
	ck_assert_int_eq(snapshot.connection, FACEPLATE_CONNECTION_UNKNOWN);
	cleanup(dirfd, path);
}
END_TEST

START_TEST(test_invalid_and_unsafe_are_hidden)
{
	char path[64];
	struct faceplate_status_snapshot snapshot;
	int dirfd = make_dir(path);

	ck_assert_int_ge(dirfd, 0);
	ck_assert_int_eq(write_status(dirfd, "not-status\n", 0640), 0);
	ck_assert_int_eq(faceplate_status_read_at(dirfd, getuid(), 5000, 30000, &snapshot),
			 FACEPLATE_STATUS_HIDDEN);
	ck_assert_int_eq(fchmodat(dirfd, "faceplate.status", 0666, 0), 0);
	ck_assert_int_eq(faceplate_status_read_at(dirfd, getuid(), 5000, 30000, &snapshot),
			 FACEPLATE_STATUS_HIDDEN);
	cleanup(dirfd, path);
}
END_TEST

START_TEST(test_future_is_invalid_not_stale)
{
	char path[64];
	struct faceplate_status_snapshot snapshot;
	int dirfd = make_dir(path);

	ck_assert_int_ge(dirfd, 0);
	ck_assert_int_eq(write_status(dirfd, valid, 0640), 0);
	ck_assert_int_eq(faceplate_status_read_at(dirfd, getuid(), 999, 30000, &snapshot),
			 FACEPLATE_STATUS_HIDDEN);
	cleanup(dirfd, path);
}
END_TEST

START_TEST(test_symlink_and_hardlink_are_hidden)
{
	char path[64];
	struct faceplate_status_snapshot snapshot;
	int dirfd = make_dir(path);

	ck_assert_int_ge(dirfd, 0);
	ck_assert_int_eq(symlinkat("/etc/passwd", dirfd, "faceplate.status"), 0);
	ck_assert_int_eq(faceplate_status_read_at(dirfd, getuid(), 5000, 30000, &snapshot),
			 FACEPLATE_STATUS_HIDDEN);
	ck_assert_int_eq(unlinkat(dirfd, "faceplate.status", 0), 0);
	ck_assert_int_eq(write_status(dirfd, valid, 0640), 0);
	ck_assert_int_eq(linkat(dirfd, "faceplate.status", dirfd, "second-link", 0), 0);
	ck_assert_int_eq(faceplate_status_read_at(dirfd, getuid(), 5000, 30000, &snapshot),
			 FACEPLATE_STATUS_HIDDEN);
	unlinkat(dirfd, "second-link", 0);
	cleanup(dirfd, path);
}
END_TEST

START_TEST(test_duplicate_overflow_and_oversize_are_hidden)
{
	char path[64];
	char oversized[FACEPLATE_STATUS_MAX_BYTES + 2];
	struct faceplate_status_snapshot snapshot;
	int dirfd = make_dir(path);
	const char duplicate[] = "FACEPLATE_STATUS_V1\nserial=A\nserial=B\n"
				 "dataplicity=connected\nupdated_boottime_ms=1\n";
	const char overflow[] = "FACEPLATE_STATUS_V1\nserial=A\ndataplicity=connected\n"
				"updated_boottime_ms=18446744073709551616\n";

	ck_assert_int_ge(dirfd, 0);
	ck_assert_int_eq(write_status(dirfd, duplicate, 0640), 0);
	ck_assert_int_eq(faceplate_status_read_at(dirfd, getuid(), 5000, 30000, &snapshot),
			 FACEPLATE_STATUS_HIDDEN);
	ck_assert_int_eq(write_status(dirfd, overflow, 0640), 0);
	ck_assert_int_eq(faceplate_status_read_at(dirfd, getuid(), 5000, 30000, &snapshot),
			 FACEPLATE_STATUS_HIDDEN);
	memset(oversized, 'A', sizeof(oversized) - 1);
	oversized[sizeof(oversized) - 1] = '\0';
	ck_assert_int_eq(write_status(dirfd, oversized, 0640), 0);
	ck_assert_int_eq(faceplate_status_read_at(dirfd, getuid(), 5000, 30000, &snapshot),
			 FACEPLATE_STATUS_HIDDEN);
	cleanup(dirfd, path);
}
END_TEST

TEST_DEFINE_CASE(status)
TEST(test_missing_is_hidden)
TEST(test_fresh_is_visible)
TEST(test_stale_is_visible_unknown)
TEST(test_invalid_and_unsafe_are_hidden)
TEST(test_future_is_invalid_not_stale)
TEST(test_symlink_and_hardlink_are_hidden)
TEST(test_duplicate_overflow_and_oversize_are_hidden)
TEST_END_CASE

TEST_DEFINE(TEST_SUITE(faceplate_status, TEST_CASE(status), TEST_END))
