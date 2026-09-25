/*
 * Dither Cam - arbitrary-palette pixel art, ported from the PIXEL CAM
 * project (pixelboy/dithercam.py quantize() and xiao-pixelcam/src/dithercam.cpp).
 *
 *   camera frame -> centre crop to the target aspect -> nearest-neighbour
 *   downscale (crisp, like dithercam.py's Image.NEAREST) -> contrast pivot +
 *   gamma -> nearest palette colour (weighted 2*dr^2 + 4*dg^2 + 3*db^2) with
 *   an optional dither.
 *
 * Portable C99, no platform dependencies.
 */
#ifndef DITHERCAM_H_INCLUDED
#define DITHERCAM_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include "gbcam.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DC_METHOD_NONE,
    DC_METHOD_BAYER4,
    DC_METHOD_BAYER8,
    DC_METHOD_FLOYD_STEINBERG,
    DC_METHOD_ATKINSON,
    DC_METHOD_SIERRA_LITE,
    DC_METHOD_COUNT
} dc_method_t;

#define DC_METHOD_DEFAULT DC_METHOD_SIERRA_LITE   /* dithercam.py DEFAULT_DITHER_METHOD */
#define DC_AMOUNT_DEFAULT 0.5f                    /* dithercam.py DEFAULT_DITHER_AMOUNT */

/* Output sizes: not PIXEL CAM's own RESOLUTIONS list - chosen instead so
 * every preset scales onto the 240x240 LCD by a clean integer factor (1x or
 * 2x) with no crop and no fractional resample, in 4:3 (120x90, 240x180),
 * 2:1 (120x60, 240x120), 3:2/"4x6 print" (240x160 at 1x only), and 1:1
 * (120x120 at 2x, 240x240 at 1x) - the preview is always pixel-identical to
 * the saved photo, just some presets show it bigger. Ordered by ascending
 * pixel count. */
#define DC_SIZE_COUNT 7
#define DC_SIZE_DEFAULT 1      /* 120x90 */
#define DC_MAX_W 1280          /* largest of dc_sizes[]/normal_sizes[] - see normal_sizes' comment */
#define DC_MAX_H 720
void dc_size(int index, int *w, int *h);

/* Normal Cam's own size list - "digicam" resolutions (it's meant to look like
 * a real photo, not Dither Cam's tiny pixel art) rather than dc_sizes[]'s
 * presets. Capped at 1280x720: the OV2710 sensor defaults to 1920x1080
 * natively (see ov2710's Kconfig CAMERA_OV2710_MIPI_DEFAULT_FMT), so there's
 * headroom for a 1080p preset too, but that would double DC_MAX_W/H's RGB888
 * working buffers again - held off until real hardware confirms the PSRAM
 * budget, since the simulator can't check that. */
#define NORMAL_SIZE_COUNT 5
#define NORMAL_SIZE_DEFAULT 2  /* 640x480 */
void normal_size(int index, int *w, int *h);
const char *normal_size_name(int index);

const char *dc_method_name(dc_method_t m);
const char *dc_palette_name(int palette);
/* RGB888 colours of a palette; returns the count. */
int dc_palette_colors(int palette, const uint8_t (**colors)[3]);

/* Crop + nearest-neighbour downscale any camera frame to w x h RGB888. */
void dc_sample(const gbcam_frame_t *frame, uint8_t *rgb, int w, int h);

/* Crop + box-average downscale any camera frame to w x h RGB888 (smooth;
 * used for the Normal Cam preview). */
void dc_sample_smooth(const gbcam_frame_t *frame, uint8_t *rgb, int w, int h);

/* In place, rounded back to 8 bits (for the Normal Cam preview):
 * v = clip((v/255 - 0.5) * contrast + 0.5) ^ gamma * 255, per channel. */
void dc_levels(uint8_t *rgb, int w, int h, float contrast, float gamma);

/* Quantize w x h RGB888 to the palette. Contrast/gamma are applied first, in
 * floating point like dithercam.py (1.0/1.0 = unchanged). out_rgb may alias
 * rgb. out_index (w*h, optional) receives palette indices. work: w*h*3 floats
 * (NULL = allocate internally). */
void dc_quantize(const uint8_t *rgb, int w, int h, int palette, dc_method_t method, float amount,
                 float contrast, float gamma, uint8_t *out_rgb, uint8_t *out_index, float *work);

#ifdef __cplusplus
}
#endif

#endif /* DITHERCAM_H_INCLUDED */
