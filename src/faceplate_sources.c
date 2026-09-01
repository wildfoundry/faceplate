/* SPDX-License-Identifier: MIT */
#include "faceplate_sources.h"

#include "faceplate_status.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_SOURCES 16
#define MAX_MANIFEST 1024
#define DEFAULT_STALE_MS 30000

enum source_kind { SOURCE_DATAPLICITY, SOURCE_SYSTEM };

struct source {
	enum source_kind kind;
	char path[256];
	uid_t uid;
	uint64_t stale_ms;
};

struct faceplate_sources {
	struct source items[MAX_SOURCES];
	size_t count;
};

static bool safe_token(const char *s, size_t max)
{
	size_t i, n = strlen(s);
	if (!n || n > max)
		return false;
	for (i = 0; i < n; ++i) {
		unsigned char c = (unsigned char)s[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		    c == '.' || c == '_' || c == '-' || c == ':' || c == '/')
			continue;
		return false;
	}
	return true;
}

static bool parse_u64(const char *s, uint64_t *out)
{
	char *end;
	unsigned long long value;
	errno = 0;
	value = strtoull(s, &end, 10);
	if (errno || !*s || *end)
		return false;
	*out = value;
	return true;
}

static int read_safe_file_at(int dirfd, const char *name, uid_t uid, char *buf, size_t cap)
{
	struct stat st;
	ssize_t n;
	int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != uid ||
	    (st.st_mode & 0022) || st.st_size <= 0 || st.st_size >= (off_t)cap) {
		close(fd);
		return -EPERM;
	}
	n = read(fd, buf, cap - 1);
	close(fd);
	if (n <= 0 || n != st.st_size)
		return -EIO;
	buf[n] = '\0';
	return 0;
}

static int parse_manifest(int dirfd, const char *name, struct source *out)
{
	char buf[MAX_MANIFEST], *line, *save = NULL;
	char kind[32] = "", path[256] = "", user[64] = "";
	uint64_t stale = DEFAULT_STALE_MS;
	struct passwd *pw;

	if (read_safe_file_at(dirfd, name, 0, buf, sizeof(buf)))
		return -EINVAL;
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (!strncmp(line, "kind=", 5))
			strncpy(kind, line + 5, sizeof(kind) - 1);
		else if (!strncmp(line, "path=", 5))
			strncpy(path, line + 5, sizeof(path) - 1);
		else if (!strncmp(line, "user=", 5))
			strncpy(user, line + 5, sizeof(user) - 1);
		else if (!strncmp(line, "stale_after_ms=", 15) && !parse_u64(line + 15, &stale))
			return -EINVAL;
		else if (*line && *line != '#')
			return -EINVAL;
	}
	if (!safe_token(kind, 24) || !safe_token(user, 63) || strncmp(path, "/run/", 5) ||
	    !safe_token(path, 255) || stale < 1000 || stale > 3600000)
		return -EINVAL;
	pw = getpwnam(user);
	if (!pw)
		return -ENOENT;
	if (!strcmp(kind, "dataplicity"))
		out->kind = SOURCE_DATAPLICITY;
	else if (!strcmp(kind, "system"))
		out->kind = SOURCE_SYSTEM;
	else
		return -EINVAL;
	memcpy(out->path, path, strlen(path) + 1);
	out->uid = pw->pw_uid;
	out->stale_ms = stale;
	return 0;
}

int faceplate_sources_new(struct faceplate_sources **out, const char *manifest_dir)
{
	struct faceplate_sources *sources;
	struct dirent *entry;
	DIR *dir;
	int fd;

	if (!out || !manifest_dir)
		return -EINVAL;
	sources = calloc(1, sizeof(*sources));
	if (!sources)
		return -ENOMEM;
	dir = opendir(manifest_dir);
	if (!dir) {
		*out = sources;
		return 0;
	}
	fd = dirfd(dir);
	while ((entry = readdir(dir)) && sources->count < MAX_SOURCES) {
		size_t n = strlen(entry->d_name);
		if (n < 8 || strcmp(entry->d_name + n - 7, ".source"))
			continue;
		if (!parse_manifest(fd, entry->d_name, &sources->items[sources->count]))
			++sources->count;
	}
	closedir(dir);
	*out = sources;
	return 0;
}

void faceplate_sources_free(struct faceplate_sources *sources)
{
	free(sources);
}

static void refresh_dataplicity(const struct source *source, uint64_t now, char *out, size_t cap)
{
	struct faceplate_status_snapshot snapshot;
	char path[256], *slash;
	const char *state;
	int fd;

	strncpy(path, source->path, sizeof(path) - 1);
	path[sizeof(path) - 1] = '\0';
	slash = strrchr(path, '/');
	if (!slash || strcmp(slash + 1, "faceplate.status"))
		return;
	*slash = '\0';
	fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return;
	if (faceplate_status_read_at(fd, source->uid, now, source->stale_ms, &snapshot) ==
	    FACEPLATE_STATUS_HIDDEN) {
		close(fd);
		return;
	}
	close(fd);
	state = snapshot.connection == FACEPLATE_CONNECTION_CONNECTED	   ? "Online"
		: snapshot.connection == FACEPLATE_CONNECTION_DISCONNECTED ? "Offline"
									   : "Unknown";
	snprintf(out, cap, "Device %s   Dataplicity %s", snapshot.serial, state);
}

/* System snapshots deliberately permit only locally useful, bounded values. */
static void refresh_system(const struct source *source, uint64_t now, char *out, size_t cap)
{
	char path[256], file[64], buf[2048], *slash, *line, *save = NULL;
	char os[64] = "", ip[48] = "", slot[32] = "", image[64] = "", boot[24] = "";
	uint64_t updated = 0, uptime = 0;
	int fd;

	strncpy(path, source->path, sizeof(path) - 1);
	path[sizeof(path) - 1] = '\0';
	slash = strrchr(path, '/');
	if (!slash)
		return;
	strncpy(file, slash + 1, sizeof(file) - 1);
	*slash = '\0';
	fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0 || read_safe_file_at(fd, file, source->uid, buf, sizeof(buf))) {
		if (fd >= 0)
			close(fd);
		return;
	}
	close(fd);
	line = strtok_r(buf, "\n", &save);
	if (!line || strcmp(line, "FACEPLATE_SYSTEM_V1"))
		return;
	while ((line = strtok_r(NULL, "\n", &save))) {
		char *value = strchr(line, '=');
		if (!value)
			return;
		*value++ = '\0';
		if (!safe_token(value, 63))
			return;
		if (!strcmp(line, "updated_boottime_ms")) {
			if (!parse_u64(value, &updated))
				return;
		} else if (!strcmp(line, "uptime_seconds")) {
			if (!parse_u64(value, &uptime))
				return;
		} else if (!strcmp(line, "os_image"))
			strncpy(os, value, sizeof(os) - 1);
		else if (!strcmp(line, "primary_ip"))
			strncpy(ip, value, sizeof(ip) - 1);
		else if (!strcmp(line, "rauc_slot"))
			strncpy(slot, value, sizeof(slot) - 1);
		else if (!strcmp(line, "rauc_image"))
			strncpy(image, value, sizeof(image) - 1);
		else if (!strcmp(line, "rauc_boot"))
			strncpy(boot, value, sizeof(boot) - 1);
		else if (strcmp(line, "clock_synced"))
			return;
	}
	if (!updated || updated > now || now - updated > source->stale_ms)
		snprintf(out, cap, "System status Unknown");
	else {
		size_t used = (size_t)snprintf(out, cap, "Image %s   Uptime %llus",
					       os[0] ? os : "Unknown", (unsigned long long)uptime);
		if (ip[0] && used < cap)
			used += (size_t)snprintf(out + used, cap - used, "   IP %s", ip);
		if (slot[0] && used < cap)
			snprintf(out + used, cap - used, "   RAUC %s %s %s", slot,
				 image[0] ? image : "", boot[0] ? boot : "Unknown");
	}
}

void faceplate_sources_refresh(struct faceplate_sources *sources, uint64_t now,
			       struct faceplate_display_context *context)
{
	size_t i;
	memset(context, 0, sizeof(*context));
	if (!sources)
		return;
	for (i = 0; i < sources->count; ++i) {
		if (sources->items[i].kind == SOURCE_DATAPLICITY && !context->device_line[0])
			refresh_dataplicity(&sources->items[i], now, context->device_line,
					    sizeof(context->device_line));
		else if (sources->items[i].kind == SOURCE_SYSTEM && !context->system_line[0])
			refresh_system(&sources->items[i], now, context->system_line,
				       sizeof(context->system_line));
	}
}
