/* SPDX-License-Identifier: MIT */
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "faceplate_events.h"
#include "test_common.h"

static const char valid[] = "FACEPLATE_EVENTS_V1\n"
			    "updated_boottime_ms=1000\n"
			    "console=locked\n"
			    "count=1\n"
			    "e0_boottime_ms=900\n"
			    "e0_code=machine.started\n"
			    "e0_text=Washer 3 started\n";

START_TEST(test_reads_valid_events_snapshot)
{
	char path[64];
	int dirfd;
	struct faceplate_events_snapshot snapshot;

	strcpy(path, "/tmp/faceplate-events-XXXXXX");
	ck_assert_ptr_nonnull(mkdtemp(path));
	dirfd = open(path, O_RDONLY | O_DIRECTORY);
	ck_assert_int_ge(dirfd, 0);
	{
		int fd = openat(dirfd, "faceplate.events", O_WRONLY | O_CREAT | O_TRUNC, 0640);
		ck_assert_int_ge(fd, 0);
		ck_assert_int_eq(write(fd, valid, strlen(valid)), (ssize_t)strlen(valid));
		close(fd);
		ck_assert_int_eq(fchownat(dirfd, "faceplate.events", geteuid(), -1, 0), 0);
	}

	ck_assert_int_eq(faceplate_events_read_at(dirfd, geteuid(), 5000, 60000, &snapshot),
			 FACEPLATE_EVENTS_FRESH);
	ck_assert(snapshot.console_locked);
	ck_assert_uint_eq(snapshot.count, 1);
	ck_assert_str_eq(snapshot.lines[0].code, "machine.started");
	ck_assert_str_eq(snapshot.lines[0].text, "Washer 3 started");

	close(dirfd);
	rmdir(path);
}
END_TEST

Suite *faceplate_events_suite(void)
{
	Suite *suite = suite_create("faceplate_events");
	TCase *tc = tcase_create("core");
	tcase_add_test(tc, test_reads_valid_events_snapshot);
	suite_add_tcase(suite, tc);
	return suite;
}

int main(void)
{
	int failed;
	SRunner *runner = srunner_create(faceplate_events_suite());
	srunner_run_all(runner, CK_ENV);
	failed = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failed ? 1 : 0;
}
