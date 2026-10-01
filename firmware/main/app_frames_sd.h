#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_frames.h"

/* Just a cap on the s_meta/s_names bookkeeping arrays (app_frames_sd.c) -
 * each slot is ~50 bytes; the actual frame pixel data is heap/PSRAM
 * allocated separately per frame regardless, so this costs almost nothing
 * to keep generous. Raised from 40 after it turned out too easy to hit with
 * more than one or two ROMs/packs on the card at once (a single Standard
 * cartridge alone uses 18-25 slots before a single /FRAMES .json is even
 * added). Keep in sync with app_frames_sd.c. */
#define MAX_SD_FRAMES 200

/* Loads user frames from the SD card's /FRAMES and /ROMS folders, once at
 * boot - not rescanned while running, so switching frames or taking a photo
 * never touches the SD card or re-decodes anything (see app_frames.c's
 * frames_total()/frames_get()). Call once, after storage_init().
 *
 * Returns immediately: the scan runs on its own core-1 task (~1.6s on a card
 * with a few dozen frames, previously spent blocking boot), and the list only
 * becomes visible through frames_sd_count()/frames_sd_get() once it finishes -
 * call frames_sd_ready() before using a frame that might have come from the
 * card, or frame_available() (app_frames.h), which folds that in.
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
void frames_sd_start(void);

/* True once the background scan has finished and the list is usable - see
 * frames_sd_start(). */
bool frames_sd_ready(void);

/* The scan task's latest status line ("CONVERTING"/"CONVERTED", while it's
 * extracting frames from a ROM it hasn't seen before), if it changed since the
 * last call - false when there's nothing new. l2 is "" when there is no second
 * line. Read from the main task only: the scan task itself must not draw, and
 * never calls the display. */
bool frames_sd_status(char *l1, size_t n1, char *l2, size_t n2);

int frames_sd_count(void);
const frame_meta_t *frames_sd_get(int index);
const char *frames_sd_name(int index);
