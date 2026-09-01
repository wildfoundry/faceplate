/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/magic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/timex.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define OUTPUT_DIR "/run/faceplate-system"

static bool token_ok(const char *s, size_t max)
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

static uint64_t boottime_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_BOOTTIME, &ts))
		return 0;
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void os_image(char *out, size_t cap)
{
	FILE *f = fopen("/etc/os-release", "re");
	char line[256];
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *value, *end;
		if (strncmp(line, "VERSION_ID=", 11))
			continue;
		value = line + 11;
		value[strcspn(value, "\r\n")] = '\0';
		if (*value == '"') {
			++value;
			end = strrchr(value, '"');
			if (end)
				*end = '\0';
		}
		if (token_ok(value, 63) && strlen(value) < cap)
			memcpy(out, value, strlen(value) + 1);
		break;
	}
	fclose(f);
}

static unsigned long long uptime_seconds(void)
{
	FILE *f = fopen("/proc/uptime", "re");
	double value = 0;
	if (f) {
		if (fscanf(f, "%lf", &value) != 1)
			value = 0;
		fclose(f);
	}
	return value > 0 ? (unsigned long long)value : 0;
}

static void primary_ip(char *out, size_t cap)
{
	struct ifaddrs *all, *it;
	if (getifaddrs(&all))
		return;
	for (it = all; it; it = it->ifa_next) {
		struct sockaddr_in *in;
		if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET ||
		    !strcmp(it->ifa_name, "lo"))
			continue;
		in = (struct sockaddr_in *)it->ifa_addr;
		if (inet_ntop(AF_INET, &in->sin_addr, out, (socklen_t)cap))
			break;
	}
	freeifaddrs(all);
}

static bool json_token(const char *json, const char *key, char *out, size_t cap)
{
	char needle[64];
	const char *start, *end;
	snprintf(needle, sizeof(needle), "\"%s\":\"", key);
	start = strstr(json, needle);
	if (!start)
		return false;
	start += strlen(needle);
	end = strchr(start, '"');
	if (!end || (size_t)(end - start) >= cap)
		return false;
	memcpy(out, start, (size_t)(end - start));
	out[end - start] = '\0';
	return token_ok(out, cap - 1);
}

static void rauc_status(char *slot, size_t slot_cap, char *image, size_t image_cap, char *boot,
			size_t boot_cap)
{
	char json[8192];
	ssize_t total = 0, n;
	int pipes[2], status;
	pid_t pid;
	if (access("/usr/bin/rauc", X_OK) || pipe2(pipes, O_CLOEXEC))
		return;
	pid = fork();
	if (pid == 0) {
		int nullfd = open("/dev/null", O_WRONLY | O_CLOEXEC);
		dup2(pipes[1], STDOUT_FILENO);
		if (nullfd >= 0)
			dup2(nullfd, STDERR_FILENO);
		close(pipes[0]);
		close(pipes[1]);
		clearenv();
		execl("/usr/bin/rauc", "rauc", "status", "--output-format=json", (char *)NULL);
		_exit(127);
	}
	close(pipes[1]);
	if (pid < 0) {
		close(pipes[0]);
		return;
	}
	while (total < (ssize_t)sizeof(json) - 1 &&
	       (n = read(pipes[0], json + total, sizeof(json) - 1 - (size_t)total)) > 0)
		total += n;
	close(pipes[0]);
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) || !total)
		return;
	json[total] = '\0';
	if (!json_token(json, "booted", slot, slot_cap))
		slot[0] = '\0';
	if (!json_token(json, "version", image, image_cap))
		image[0] = '\0';
	if (!json_token(json, "boot_status", boot, boot_cap))
		boot[0] = '\0';
}

static int write_snapshot(const char *data, size_t len)
{
	struct statfs fs;
	struct stat st;
	int dirfd = -1, fd = -1, ret = -1;
	dirfd = open(OUTPUT_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (dirfd < 0 || fstatfs(dirfd, &fs) || (unsigned long)fs.f_type != TMPFS_MAGIC ||
	    fstat(dirfd, &st) || st.st_uid != geteuid() || (st.st_mode & 0022))
		goto out;
	unlinkat(dirfd, ".status.tmp", 0);
	fd = openat(dirfd, ".status.tmp", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
		    0640);
	if (fd < 0)
		goto out;
	if (write(fd, data, len) != (ssize_t)len || fchmod(fd, 0640) || close(fd))
		goto out;
	fd = -1;
	if (!renameat(dirfd, ".status.tmp", dirfd, "status"))
		ret = 0;
out:
	if (fd >= 0)
		close(fd);
	if (dirfd >= 0) {
		if (ret)
			unlinkat(dirfd, ".status.tmp", 0);
		close(dirfd);
	}
	return ret;
}

int main(void)
{
	char image[64] = "unknown", ip[48] = "unknown", data[1024];
	char rauc_slot[32] = "", rauc_image[64] = "", rauc_boot[24] = "";
	struct timex tx = {0};
	const char *synced;
	int n;
	os_image(image, sizeof(image));
	primary_ip(ip, sizeof(ip));
	rauc_status(rauc_slot, sizeof(rauc_slot), rauc_image, sizeof(rauc_image), rauc_boot,
		    sizeof(rauc_boot));
	synced = adjtimex(&tx) >= 0 && !(tx.status & STA_UNSYNC) ? "yes" : "unknown";
	n = snprintf(data, sizeof(data),
		     "FACEPLATE_SYSTEM_V1\n"
		     "os_image=%s\n"
		     "uptime_seconds=%llu\n"
		     "clock_synced=%s\n"
		     "primary_ip=%s\n",
		     image, uptime_seconds(), synced, ip);
	if (n > 0 && (size_t)n < sizeof(data) && rauc_slot[0])
		n += snprintf(data + n, sizeof(data) - (size_t)n,
			      "rauc_slot=%s\nrauc_image=%s\nrauc_boot=%s\n", rauc_slot,
			      rauc_image[0] ? rauc_image : "unknown",
			      rauc_boot[0] ? rauc_boot : "unknown");
	if (n > 0 && (size_t)n < sizeof(data))
		n += snprintf(data + n, sizeof(data) - (size_t)n, "updated_boottime_ms=%llu\n",
			      (unsigned long long)boottime_ms());
	if (n <= 0 || (size_t)n >= sizeof(data))
		return EXIT_FAILURE;
	return write_snapshot(data, (size_t)n) ? EXIT_FAILURE : EXIT_SUCCESS;
}
