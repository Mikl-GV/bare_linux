#ifndef DISPLAY_TIMING_H
#define DISPLAY_TIMING_H

#include <stdint.h>
#include <stdbool.h>

/* Тайминги дисплея (linux include/video/display_timing.h). */

enum display_flags {
	DISPLAY_FLAGS_HSYNC_LOW      = 1 << 0,
	DISPLAY_FLAGS_HSYNC_HIGH     = 1 << 1,
	DISPLAY_FLAGS_VSYNC_LOW      = 1 << 2,
	DISPLAY_FLAGS_VSYNC_HIGH     = 1 << 3,
	DISPLAY_FLAGS_DE_LOW         = 1 << 4,
	DISPLAY_FLAGS_DE_HIGH        = 1 << 5,
	DISPLAY_FLAGS_PIXDATA_POSEDGE = 1 << 6,
	DISPLAY_FLAGS_PIXDATA_NEGEDGE = 1 << 7,
	DISPLAY_FLAGS_INTERLACED     = 1 << 8,
	DISPLAY_FLAGS_DOUBLESCAN     = 1 << 9,
	DISPLAY_FLAGS_DOUBLECLK      = 1 << 10,
};

struct timing_entry {
	uint32_t min;
	uint32_t typ;
	uint32_t max;
};

struct display_timing {
	struct timing_entry pixelclock;
	struct timing_entry hactive;
	struct timing_entry hfront_porch;
	struct timing_entry hback_porch;
	struct timing_entry hsync_len;
	struct timing_entry vactive;
	struct timing_entry vfront_porch;
	struct timing_entry vback_porch;
	struct timing_entry vsync_len;
	enum display_flags flags;
	bool hdmi_monitor;
};

#endif /* DISPLAY_TIMING_H */