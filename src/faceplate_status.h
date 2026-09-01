/* SPDX-License-Identifier: MIT */
#ifndef FACEPLATE_STATUS_H
#define FACEPLATE_STATUS_H

#include <stdint.h>
#include <sys/types.h>

#define FACEPLATE_STATUS_MAX_BYTES 512U
#define FACEPLATE_STATUS_SERIAL_MAX 64U

enum faceplate_connection {
	FACEPLATE_CONNECTION_CONNECTED,
	FACEPLATE_CONNECTION_DISCONNECTED,
	FACEPLATE_CONNECTION_UNKNOWN,
};

enum faceplate_status_visibility {
	FACEPLATE_STATUS_HIDDEN,
	FACEPLATE_STATUS_FRESH,
	FACEPLATE_STATUS_STALE,
};

struct faceplate_status_snapshot {
	enum faceplate_status_visibility visibility;
	enum faceplate_connection connection;
	char serial[FACEPLATE_STATUS_SERIAL_MAX + 1];
	uint64_t updated_boottime_ms;
};

enum faceplate_status_visibility
faceplate_status_read_at(int directory_fd, uid_t expected_uid, uint64_t now_boottime_ms,
			 uint64_t stale_after_ms, struct faceplate_status_snapshot *snapshot);

#endif
