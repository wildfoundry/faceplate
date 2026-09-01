/* SPDX-License-Identifier: MIT */
#include "faceplate_status.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/openat2.h>
#endif

#define STATUS_FILENAME "faceplate.status"
#define STATUS_HEADER "FACEPLATE_STATUS_V1"
#define SERIAL_PREFIX "serial="
#define CONNECTION_PREFIX "dataplicity="
#define UPDATED_PREFIX "updated_boottime_ms="

static void hide(struct faceplate_status_snapshot *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->visibility = FACEPLATE_STATUS_HIDDEN;
	snapshot->connection = FACEPLATE_CONNECTION_UNKNOWN;
}

static int open_snapshot(int directory_fd)
{
#if defined(__linux__) && defined(SYS_openat2)
	struct open_how how = {
		.flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW,
		.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS,
	};
	int fd = syscall(SYS_openat2, directory_fd, STATUS_FILENAME, &how, sizeof(how));

	if (fd >= 0 || (errno != ENOSYS && errno != EINVAL && errno != E2BIG))
		return fd;
#endif
	return openat(directory_fd, STATUS_FILENAME, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
}

static bool serial_valid(const char *value, size_t len)
{
	size_t i;

	if (!len || len > FACEPLATE_STATUS_SERIAL_MAX)
		return false;
	for (i = 0; i < len; ++i) {
		unsigned char c = value[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		    c == '.' || c == '_' || c == ':' || c == '/' || c == '-')
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

static bool take_line(const char **cursor, const char *end, const char **line, size_t *len)
{
	const char *newline;

	if (*cursor >= end)
		return false;
	newline = memchr(*cursor, '\n', (size_t)(end - *cursor));
	if (!newline)
		return false;
	*line = *cursor;
	*len = (size_t)(newline - *cursor);
	*cursor = newline + 1;
	return true;
}

static bool prefixed_value(const char *line, size_t len, const char *prefix, const char **value,
			   size_t *value_len)
{
	size_t prefix_len = strlen(prefix);

	if (len < prefix_len || memcmp(line, prefix, prefix_len))
		return false;
	*value = line + prefix_len;
	*value_len = len - prefix_len;
	return true;
}

static bool parse_snapshot(const char *bytes, size_t len,
			   struct faceplate_status_snapshot *snapshot)
{
	const char *cursor = bytes;
	const char *end = bytes + len;
	const char *line;
	const char *value;
	size_t line_len;
	size_t value_len;

	if (!take_line(&cursor, end, &line, &line_len) || line_len != sizeof(STATUS_HEADER) - 1 ||
	    memcmp(line, STATUS_HEADER, sizeof(STATUS_HEADER) - 1))
		return false;
	if (!take_line(&cursor, end, &line, &line_len) ||
	    !prefixed_value(line, line_len, SERIAL_PREFIX, &value, &value_len) ||
	    !serial_valid(value, value_len))
		return false;
	memcpy(snapshot->serial, value, value_len);
	snapshot->serial[value_len] = '\0';

	if (!take_line(&cursor, end, &line, &line_len) ||
	    !prefixed_value(line, line_len, CONNECTION_PREFIX, &value, &value_len))
		return false;
	if (value_len == 9 && !memcmp(value, "connected", 9))
		snapshot->connection = FACEPLATE_CONNECTION_CONNECTED;
	else if (value_len == 12 && !memcmp(value, "disconnected", 12))
		snapshot->connection = FACEPLATE_CONNECTION_DISCONNECTED;
	else if (value_len == 7 && !memcmp(value, "unknown", 7))
		snapshot->connection = FACEPLATE_CONNECTION_UNKNOWN;
	else
		return false;

	if (!take_line(&cursor, end, &line, &line_len) ||
	    !prefixed_value(line, line_len, UPDATED_PREFIX, &value, &value_len) ||
	    !parse_u64(value, value_len, &snapshot->updated_boottime_ms))
		return false;
	return cursor == end;
}

enum faceplate_status_visibility
faceplate_status_read_at(int directory_fd, uid_t expected_uid, uint64_t now_boottime_ms,
			 uint64_t stale_after_ms, struct faceplate_status_snapshot *snapshot)
{
	char bytes[FACEPLATE_STATUS_MAX_BYTES + 1];
	struct stat statbuf;
	ssize_t count;
	ssize_t total = 0;
	int fd;

	if (!snapshot)
		return FACEPLATE_STATUS_HIDDEN;
	hide(snapshot);
	if (directory_fd < 0 || !stale_after_ms)
		return snapshot->visibility;

	fd = open_snapshot(directory_fd);
	if (fd < 0)
		return snapshot->visibility;
	if (fstat(fd, &statbuf) || !S_ISREG(statbuf.st_mode) || statbuf.st_nlink != 1 ||
	    statbuf.st_uid != expected_uid || statbuf.st_size <= 0 ||
	    statbuf.st_size > (off_t)FACEPLATE_STATUS_MAX_BYTES ||
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

	if (now_boottime_ms - snapshot->updated_boottime_ms > stale_after_ms) {
		snapshot->visibility = FACEPLATE_STATUS_STALE;
		snapshot->connection = FACEPLATE_CONNECTION_UNKNOWN;
	} else {
		snapshot->visibility = FACEPLATE_STATUS_FRESH;
	}
out:
	close(fd);
	return snapshot->visibility;
}
