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
		else if (!strncmp(line, "stale_after_ms=", 15)) {
			if (!parse_u64(line + 15, &stale))
				return -EINVAL;
		} else if (*line && *line != '#')
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

static void append_sep(char *out, size_t cap, size_t *used, const char *sep, const char *piece)
{
	if (!piece || !piece[0] || *used >= cap)
		return;
	if (*used)
		*used += (size_t)snprintf(out + *used, cap - *used, "%s%s", sep, piece);
	else
		*used += (size_t)snprintf(out + *used, cap - *used, "%s", piece);
}

static bool rauc_boot_is_healthy(const char *boot)
{
	return boot && (!strcmp(boot, "good") || !strcmp(boot, "ok"));
}

static void refresh_dataplicity(const struct source *source, uint64_t now,
				struct faceplate_display_context *ctx)
{
	struct faceplate_status_snapshot snapshot;
	char path[256], *slash;
	int fd;

	if (ctx->connection_label[0])
		return;

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

	ctx->connection = snapshot.connection;
	if (snapshot.connection == FACEPLATE_CONNECTION_CONNECTED) {
		strncpy(ctx->connection_label, "Online", sizeof(ctx->connection_label) - 1);
		ctx->connection_alert = false;
	} else if (snapshot.connection == FACEPLATE_CONNECTION_DISCONNECTED) {
		strncpy(ctx->connection_label, "Offline", sizeof(ctx->connection_label) - 1);
		ctx->connection_alert = true;
	} else {
		strncpy(ctx->connection_label, "Unknown", sizeof(ctx->connection_label) - 1);
		ctx->connection_alert = true;
	}
	snprintf(ctx->serial_short, sizeof(ctx->serial_short), "%.12s", snapshot.serial);
}

/* System snapshots deliberately permit only locally useful, bounded values. */
static void refresh_system(const struct source *source, uint64_t now,
			   struct faceplate_display_context *ctx)
{
	char path[256], file[64], buf[2048], *slash, *line, *save = NULL;
	char host[64] = "", os[64] = "", ip[48] = "", slot[32] = "", image[64] = "", boot[24] = "";
	uint64_t updated = 0, uptime = 0;
	int fd;

	if (ctx->hostname[0] || ctx->ip[0] || ctx->image[0])
		return;

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
		} else if (!strcmp(line, "hostname"))
			strncpy(host, value, sizeof(host) - 1);
		else if (!strcmp(line, "os_image"))
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
		return;

	snprintf(ctx->hostname, sizeof(ctx->hostname), "%s", host);
	snprintf(ctx->ip, sizeof(ctx->ip), "%s", ip);
	snprintf(ctx->image, sizeof(ctx->image), "%s", os[0] ? os : (image[0] ? image : ""));
	snprintf(ctx->uptime_label, sizeof(ctx->uptime_label), "Uptime %llus",
		 (unsigned long long)uptime);
	snprintf(ctx->rauc_slot, sizeof(ctx->rauc_slot), "%s", slot);
	snprintf(ctx->rauc_boot, sizeof(ctx->rauc_boot), "%s", boot);
	ctx->rauc_unhealthy = slot[0] && boot[0] && !rauc_boot_is_healthy(boot);
}

static void format_display_lines(struct faceplate_display_context *ctx)
{
	size_t used = 0;
	char ip_piece[64] = "", os_piece[80] = "", rauc_piece[80] = "", serial_piece[40] = "";

	/* ASCII separators only — UTF-8 middle dots mojibake on this font path. */
	static const char sep[] = " | ";

	if (ctx->ip[0])
		snprintf(ip_piece, sizeof(ip_piece), "IP %s", ctx->ip);
	if (ctx->image[0])
		snprintf(os_piece, sizeof(os_piece), "OS %s", ctx->image);
	if (ctx->rauc_unhealthy) {
		snprintf(rauc_piece, sizeof(rauc_piece), "RAUC %s %s",
			 ctx->rauc_slot[0] ? ctx->rauc_slot : "?",
			 ctx->rauc_boot[0] ? ctx->rauc_boot : "bad");
	}
	if (ctx->serial_short[0])
		snprintf(serial_piece, sizeof(serial_piece), "Serial %s", ctx->serial_short);

	/* Supporting facts under Online + hostname (no connection token here). */
	used = 0;
	append_sep(ctx->supporting_line, sizeof(ctx->supporting_line), &used, sep, ip_piece);
	append_sep(ctx->supporting_line, sizeof(ctx->supporting_line), &used, sep, os_piece);

	/* Active compact rail: lead with connection, then reachability. */
	used = 0;
	append_sep(ctx->compact_line, sizeof(ctx->compact_line), &used, sep,
		   ctx->connection_label[0] ? ctx->connection_label : NULL);
	append_sep(ctx->compact_line, sizeof(ctx->compact_line), &used, sep, ip_piece);
	if (ctx->rauc_unhealthy)
		append_sep(ctx->compact_line, sizeof(ctx->compact_line), &used, sep, rauc_piece);

	/* Quiet footer — labeled serial; RAUC only when unhealthy. */
	used = 0;
	if (ctx->rauc_unhealthy)
		append_sep(ctx->secondary_line, sizeof(ctx->secondary_line), &used, sep,
			   rauc_piece);
	append_sep(ctx->secondary_line, sizeof(ctx->secondary_line), &used, sep, ctx->uptime_label);
	append_sep(ctx->secondary_line, sizeof(ctx->secondary_line), &used, sep, serial_piece);
}

void faceplate_sources_refresh(struct faceplate_sources *sources, uint64_t now,
			       struct faceplate_display_context *context)
{
	size_t i;
	memset(context, 0, sizeof(*context));
	context->connection = FACEPLATE_CONNECTION_UNKNOWN;
	if (!sources)
		return;
	for (i = 0; i < sources->count; ++i) {
		if (sources->items[i].kind == SOURCE_DATAPLICITY)
			refresh_dataplicity(&sources->items[i], now, context);
		else if (sources->items[i].kind == SOURCE_SYSTEM)
			refresh_system(&sources->items[i], now, context);
	}
	format_display_lines(context);
}
