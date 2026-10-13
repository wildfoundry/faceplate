/* SPDX-License-Identifier: MIT */
#ifndef FACEPLATE_EVENTS_H
#define FACEPLATE_EVENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define FACEPLATE_EVENTS_MAX_BYTES 8192U
#define FACEPLATE_EVENTS_MAX_LINES 12U
#define FACEPLATE_EVENTS_TEXT_MAX 96U
#define FACEPLATE_EVENTS_CODE_MAX 64U

enum faceplate_events_visibility {
	FACEPLATE_EVENTS_HIDDEN,
	FACEPLATE_EVENTS_FRESH,
	FACEPLATE_EVENTS_STALE,
};

struct faceplate_event_line {
	char code[FACEPLATE_EVENTS_CODE_MAX + 1];
	char text[FACEPLATE_EVENTS_TEXT_MAX + 1];
	uint64_t boottime_ms;
};

struct faceplate_events_snapshot {
	enum faceplate_events_visibility visibility;
	bool console_locked;
	uint64_t updated_boottime_ms;
	size_t count;
	struct faceplate_event_line lines[FACEPLATE_EVENTS_MAX_LINES];
};

enum faceplate_events_visibility
faceplate_events_read_at(int directory_fd, uid_t expected_uid, uint64_t now_boottime_ms,
			 uint64_t stale_after_ms, struct faceplate_events_snapshot *snapshot);

#endif
