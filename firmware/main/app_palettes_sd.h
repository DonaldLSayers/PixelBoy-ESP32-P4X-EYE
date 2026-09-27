#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MAX_SD_PALETTES 32   /* keep in sync with app_palettes_sd.c */
#define MAX_PALETTE_COLORS 64

/* Loads user colour palettes from the SD card's /PALETTES folder, once at
 * boot - not rescanned while running. Call once, after storage_init(), then
 * hand the result to gbcam_set_extra_palettes()/dc_set_extra_palettes() (see
 * app.c) so GB Camera and Dither Cam both pick them up transparently.
 *
 * Each .hex file is a list of RGB colours, one per line as 6 hex digits
 * (optionally prefixed with '#'), blank lines and '#'-led comments ignored -
 * the same format tools/gen_palettes.py reads on the PC. A file with exactly
 * 4 colours is usable as a GB Camera palette (GB palettes are always 4
 * shades); any file with 2..64 colours is usable as a Dither Cam palette -
 * so a 4-colour file shows up in both pickers. GB palettes are stored dark
 * -> light like the PC tool expects, then reversed on read since gbcam
 * shades run 0 = lightest .. 3 = darkest (see palettes_sd_gb_rgb()). */
void palettes_sd_init(void);

int palettes_sd_gb_count(void);
const char *palettes_sd_gb_name(int index);
const uint8_t *palettes_sd_gb_rgb(int index, uint8_t shade);

int palettes_sd_dc_count(void);
const char *palettes_sd_dc_name(int index);
int palettes_sd_dc_colors(int index, const uint8_t (**colors)[3]);

/* Persists a Dither Cam extra palette's nearest-colour LUT to the SD card
 * (<stem>.dclut, next to the source .hex, full filename not the truncated
 * display name) so it doesn't have to be rebuilt -
 * or sit in RAM - every time that palette is picked; see dc_set_extra_lut_io()
 * in dithercam.h, which these are registered with. The saved file starts with
 * the palette's own colours, so a .hex edited since the last save is
 * detected (colour mismatch) and quietly rebuilt rather than serving a stale
 * LUT. Matches dc_extra_lut_load_fn/dc_extra_lut_save_fn's signatures exactly. */
bool palettes_sd_load_lut(int index, uint8_t *out_lut);
void palettes_sd_save_lut(int index, const uint8_t *lut);
