/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "faceplate_sources.h"
#include "test_common.h"

static int make_dir(char *path)
{
	strcpy(path, "/tmp/faceplate-sources-XXXXXX");
	if (!mkdtemp(path))
		return -errno;
	return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

static int write_manifest(int dirfd, const char *name, const char *payload)
{
	ssize_t len = (ssize_t)strlen(payload);
	int fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd < 0)
		return -errno;
	if (write(fd, payload, (size_t)len) != len) {
		close(fd);
		return -EIO;
	}
	close(fd);
	return 0;
}

static void cleanup_dir(int dirfd, const char *path, const char *name)
{
	unlinkat(dirfd, name, 0);
	close(dirfd);
	rmdir(path);
}

START_TEST(test_missing_user_is_kept_pending)
{
	char path[64];
	struct faceplate_sources *sources = NULL;
	struct faceplate_display_context ctx;
	int dirfd = make_dir(path);

	ck_assert_int_ge(dirfd, 0);
	ck_assert_int_eq(write_manifest(dirfd, "dataplicity.source",
					"kind=dataplicity\n"
					"path=/run/dataplicity/faceplate.status\n"
					"user=faceplate-missing-user-xyz\n"
					"stale_after_ms=30000\n"),
			 0);
	ck_assert_int_eq(faceplate_sources_new(&sources, path), 0);
	ck_assert_uint_eq(faceplate_sources_count(sources), 1);
	ck_assert_uint_eq(faceplate_sources_ready_count(sources), 0);

	faceplate_sources_refresh(sources, 1000, &ctx);
	ck_assert_str_eq(ctx.connection_label, "");
	ck_assert_uint_eq(faceplate_sources_ready_count(sources), 0);

	faceplate_sources_free(sources);
	cleanup_dir(dirfd, path, "dataplicity.source");
}
END_TEST

START_TEST(test_refresh_resolves_user_later)
{
	char path[64];
	struct faceplate_sources *sources = NULL;
	struct faceplate_display_context ctx;
	struct passwd *self;
	int dirfd = make_dir(path);

	ck_assert_int_ge(dirfd, 0);
	self = getpwuid(geteuid());
	ck_assert_ptr_nonnull(self);
	ck_assert_ptr_nonnull(self->pw_name);

	ck_assert_int_eq(write_manifest(dirfd, "system.source",
					"kind=system\n"
					"path=/run/faceplate-system/status\n"
					"user=faceplate-missing-user-xyz\n"
					"stale_after_ms=30000\n"),
			 0);
	ck_assert_int_eq(faceplate_sources_new(&sources, path), 0);
	ck_assert_uint_eq(faceplate_sources_ready_count(sources), 0);

	faceplate_sources_refresh(sources, 1000, &ctx);
	ck_assert_uint_eq(faceplate_sources_ready_count(sources), 0);

	/* Simulate the late-created service account appearing in NSS. */
	ck_assert_int_eq(faceplate_sources_rebind_user(sources, 0, self->pw_name), 0);
	ck_assert_uint_eq(faceplate_sources_ready_count(sources), 0);

	faceplate_sources_refresh(sources, 1000, &ctx);
	ck_assert_uint_eq(faceplate_sources_ready_count(sources), 1);

	faceplate_sources_free(sources);
	cleanup_dir(dirfd, path, "system.source");
}
END_TEST

static Suite *suite(void)
{
	Suite *s = suite_create("faceplate_sources");
	TCase *tc = tcase_create("sources");

	tcase_add_test(tc, test_missing_user_is_kept_pending);
	tcase_add_test(tc, test_refresh_resolves_user_later);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *runner = srunner_create(suite());

	srunner_run_all(runner, CK_ENV);
	failed = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failed ? 1 : 0;
}
