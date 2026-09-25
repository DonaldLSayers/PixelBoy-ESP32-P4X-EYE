#pragma once

#include "app_frames.h"

#define MAX_SD_FRAMES 40 /* keep in sync with app_frames_sd.c */

/* Called (optionally - may be NULL) with a short status line while
 * frames_sd_init() is converting something new, so the caller can show it
 * on screen (this can take a couple of seconds per ROM). line2 may be
 * NULL. */
typedef void (*frames_sd_progress_cb)(const char *line1, const char *line2);

/* Loads user frames from the SD card's /FRAMES and /ROMS folders, once at
 * boot - not rescanned while running, so switching frames or taking a photo
 * never touches the SD card or re-decodes anything (see app_frames.c's
 * frames_total()/frames_get()). Call once, after storage_init().
 *
 *   /FRAMES - .png files the same shape as assets/frames (160x144 or
 *             160x224, see tools/gen_frames.py) - decoded and quantized
 *             the same way; .json files, a frame pack in the original
 *             cartridge ROM's own format (each file may contain many
 *             frames) - decoded the same way as tools/
 *             import_gb_frames.py's decode_frame(), on-device.
 *   /ROMS   - .gb, .gbc and .zip files: frames extracted straight from a
 *             ROM (see app_frames_sd.c's ROM section for which ones). Each
 *             new frame found this way is written back to /FRAMES as a
 *             plain .png, and the source ROM is moved to /ROMS/PROCESSED
 *             once fully handled - so this only happens once per ROM;
 *             every boot after that just loads the cached .png like any
 *             other /FRAMES file, no re-decoding.
 *
 * Files that don't decode, or whose canvas doesn't fit a size/layout this
 * pipeline supports, are skipped with a log warning rather than failing the
 * whole scan. Frames that turn out pixel-identical to one already loaded
 * (from any source, including frames.h's built-ins) are skipped too. */
void frames_sd_init(frames_sd_progress_cb progress);

int frames_sd_count(void);
const frame_meta_t *frames_sd_get(int index);
const char *frames_sd_name(int index);
