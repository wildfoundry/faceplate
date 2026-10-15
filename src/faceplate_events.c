/* SPDX-License-Identifier: MIT */
#include "faceplate_events.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/openat2.h>
#endif

#define EVENTS_FILENAME "faceplate.events"
#define EVENTS_HEADER "FACEPLATE_EVENTS_V1"

static void hide(struct faceplate_events_snapshot *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->visibility = FACEPLATE_EVENTS_HIDDEN;
	snapshot->console_locked = true;
}

static int open_snapshot(int directory_fd)
{
#if defined(__linux__) && defined(SYS_openat2)
	struct open_how how = {
		.flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW,
		.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS,
	};
	int fd = syscall(SYS_openat2, directory_fd, EVENTS_FILENAME, &how, sizeof(how));

	if (fd >= 0 || (errno != ENOSYS && errno != EINVAL && errno != E2BIG))
		return fd;
#endif
	return openat(directory_fd, EVENTS_FILENAME, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
}

static bool token_valid(const char *value, size_t len, size_t max)
{
	size_t i;

	if (!len || len > max)
		return false;
	for (i = 0; i < len; ++i) {
		unsigned char c = (unsigned char)value[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		    c == '.' || c == '_' || c == '-' || c == ':' || c == ' ')
			continue;
		return false;
	}
	return true;
}

static bool code_valid(const char *value, size_t len)
{
	size_t i;

	if (!len || len > FACEPLATE_EVENTS_CODE_MAX)
		return false;
	for (i = 0; i < len; ++i) {
		unsigned char c = (unsigned char)value[i];
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
		    c == '-')
			continue;
		return false;
	}
	return true;
}

static bool parse_u64(const char *value, size_t len, uint64_t *out)
{
	uint64_t result = 0;
	size_t i;

	if (!len || len > 20)
		return false;
	for (i = 0; i < len; ++i) {
		unsigned int digit;
		if (value[i] < '0' || value[i] > '9')
			return false;
		digit = (unsigned int)(value[i] - '0');
		if (result > (UINT64_MAX - digit) / 10)
			return false;
		result = result * 10 + digit;
	}
	*out = result;
	return true;
}

static bool split_key(const char *line, size_t len, char *key, size_t key_cap, const char **value,
		      size_t *value_len)
{
	const char *eq = memchr(line, '=', len);

	if (!eq || eq == line || (size_t)(eq - line) + 1 >= key_cap)
		return false;
	memcpy(key, line, (size_t)(eq - line));
	key[eq - line] = '\0';
	*value = eq + 1;
	*value_len = len - (size_t)(eq - line) - 1;
	return true;
}

static bool parse_snapshot(const char *bytes, size_t len,
			   struct faceplate_events_snapshot *snapshot)
{
	char key[32];
	const char *value;
	size_t value_len;
	uint64_t declared = 0;
	bool have_updated = false;
	bool have_console = false;
	bool have_count = false;
	char *cursor;
	char *save = NULL;
	char *line;

	if (len < sizeof(EVENTS_HEADER) || memcmp(bytes, EVENTS_HEADER "\n", sizeof(EVENTS_HEADER)))
		return false;

	cursor = strdup(bytes);
	if (!cursor)
		return false;
	line = strtok_r(cursor, "\n", &save);
	if (!line || strcmp(line, EVENTS_HEADER) != 0) {
		free(cursor);
		return false;
	}

	snapshot->count = 0;
	while ((line = strtok_r(NULL, "\n", &save))) {
		if (!split_key(line, strlen(line), key, sizeof(key), &value, &value_len))
			goto fail;
		if (!strcmp(key, "updated_boottime_ms")) {
			if (!parse_u64(value, value_len, &snapshot->updated_boottime_ms))
				goto fail;
			have_updated = true;
		} else if (!strcmp(key, "console")) {
			if (value_len == 6 && !memcmp(value, "locked", 6))
				snapshot->console_locked = true;
			else if (value_len == 8 && !memcmp(value, "unlocked", 8))
				snapshot->console_locked = false;
			else
				goto fail;
			have_console = true;
		} else if (!strcmp(key, "count")) {
			if (!parse_u64(value, value_len, &declared) ||
			    declared > FACEPLATE_EVENTS_MAX_LINES)
				goto fail;
			have_count = true;
		} else if (!strncmp(key, "e", 1)) {
			unsigned long index;
			char suffix[16];
			const char *underscore = strchr(key, '_');

			if (!underscore || sscanf(key, "e%lu_%15s", &index, suffix) != 2 ||
			    index >= FACEPLATE_EVENTS_MAX_LINES)
				goto fail;
			if (!strcmp(suffix, "boottime_ms")) {
				if (!parse_u64(value, value_len,
					       &snapshot->lines[index].boottime_ms))
					goto fail;
			} else if (!strcmp(suffix, "code")) {
				if (!code_valid(value, value_len))
					goto fail;
				memcpy(snapshot->lines[index].code, value, value_len);
				snapshot->lines[index].code[value_len] = '\0';
			} else if (!strcmp(suffix, "text")) {
				if (!token_valid(value, value_len, FACEPLATE_EVENTS_TEXT_MAX))
					goto fail;
				memcpy(snapshot->lines[index].text, value, value_len);
				snapshot->lines[index].text[value_len] = '\0';
			} else {
				goto fail;
			}
		} else {
			goto fail;
		}
	}

	free(cursor);
	if (!have_updated || !have_console || !have_count)
		return false;
	snapshot->count = (size_t)declared;
	for (size_t i = 0; i < snapshot->count; ++i) {
		if (!snapshot->lines[i].code[0] || !snapshot->lines[i].text[0])
			return false;
	}
	return true;

fail:
	free(cursor);
	return false;
}

enum faceplate_events_visibility
faceplate_events_read_at(int directory_fd, uid_t expected_uid, uint64_t now_boottime_ms,
			 uint64_t stale_after_ms, struct faceplate_events_snapshot *snapshot)
{
	char bytes[FACEPLATE_EVENTS_MAX_BYTES + 1];
	struct stat statbuf;
	ssize_t count;
	ssize_t total = 0;
	int fd;

	if (!snapshot)
		return FACEPLATE_EVENTS_HIDDEN;
	hide(snapshot);
	if (directory_fd < 0 || !stale_after_ms)
		return snapshot->visibility;

	fd = open_snapshot(directory_fd);
	if (fd < 0)
		return snapshot->visibility;
	if (fstat(fd, &statbuf) || !S_ISREG(statbuf.st_mode) || statbuf.st_nlink != 1 ||
	    statbuf.st_uid != expected_uid || statbuf.st_size <= 0 ||
	    statbuf.st_size > (off_t)FACEPLATE_EVENTS_MAX_BYTES ||
	    ((statbuf.st_mode & 0777) & ~0640))
		goto out;

	while (total < statbuf.st_size) {
		count = read(fd, bytes + total, (size_t)(statbuf.st_size - total));
		if (count <= 0)
			goto out;
		total += count;
	}
	count = read(fd, bytes + total, 1);
	if (count != 0)
		goto out;
	bytes[total] = '\0';
	if (!parse_snapshot(bytes, (size_t)total, snapshot) ||
	    snapshot->updated_boottime_ms > now_boottime_ms) {
		hide(snapshot);
		goto out;
	}

	if (now_boottime_ms - snapshot->updated_boottime_ms > stale_after_ms)
		snapshot->visibility = FACEPLATE_EVENTS_STALE;
	else
		snapshot->visibility = FACEPLATE_EVENTS_FRESH;
out:
	close(fd);
	return snapshot->visibility;
}
