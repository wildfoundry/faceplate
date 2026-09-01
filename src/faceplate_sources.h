/* SPDX-License-Identifier: MIT */
#ifndef FACEPLATE_SOURCES_H
#define FACEPLATE_SOURCES_H

#include <stdint.h>

struct faceplate_sources;

struct faceplate_display_context {
	char device_line[192];
	char system_line[192];
};

int faceplate_sources_new(struct faceplate_sources **out, const char *manifest_dir);
void faceplate_sources_free(struct faceplate_sources *sources);
void faceplate_sources_refresh(struct faceplate_sources *sources, uint64_t now_boottime_ms,
			       struct faceplate_display_context *context);

#endif
