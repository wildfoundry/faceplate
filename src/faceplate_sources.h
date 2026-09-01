/* SPDX-License-Identifier: MIT */
#ifndef FACEPLATE_SOURCES_H
#define FACEPLATE_SOURCES_H

#include <stdbool.h>
#include <stdint.h>

#include "faceplate_status.h"

struct faceplate_sources;

/* Structured status for idle identity / active rail / severity paint. */
struct faceplate_display_context {
	char hostname[64];
	char ip[48];
	char image[64];
	char connection_label[24];
	enum faceplate_connection connection;
	bool connection_alert;
	char serial_short[20];
	char uptime_label[32];
	char rauc_slot[32];
	char rauc_boot[24];
	bool rauc_unhealthy;
	/* Preformatted lines for chrome (no pills/cards). */
	char supporting_line[160];
	char secondary_line[160];
	char compact_line[160];
};

int faceplate_sources_new(struct faceplate_sources **out, const char *manifest_dir);
void faceplate_sources_free(struct faceplate_sources *sources);
void faceplate_sources_refresh(struct faceplate_sources *sources, uint64_t now_boottime_ms,
			       struct faceplate_display_context *context);

#endif
