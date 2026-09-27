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
#define DC_MAX_W 640           /* largest of dc_sizes[]/normal_sizes[] - see normal_sizes' comment */
#define DC_MAX_H 480
void dc_size(int index, int *w, int *h);

/* Normal Cam's own size list - "digicam" resolutions (it's meant to look like
 * a real photo, not Dither Cam's tiny pixel art) rather than dc_sizes[]'s
 * presets. 4:3 only (no widescreen presets - a photo with a GB Camera-style
 * border/frame around it, this project's whole reason to exist, only reads
 * right at 4:3) and ascending resolution, with each one's actual dimensions
 * in its own label instead of a bare "360P"/"480P" that doesn't say which of
 * several possible widths that maps to. */
#define NORMAL_SIZE_COUNT 3
#define NORMAL_SIZE_DEFAULT 2  /* 640x480 */
void normal_size(int index, int *w, int *h);
const char *normal_size_name(int index);

const char *dc_method_name(dc_method_t m);
const char *dc_palette_name(int palette);
/* RGB888 colours of a palette; returns the count. Indices >= DC_PALETTE_COUNT
 * are served by the extra-palette callback below, if registered. */
int dc_palette_colors(int palette, const uint8_t (**colors)[3]);
/* Built-in count plus however many extra palettes are currently registered. */
int dc_palette_count(void);

/* Extra palettes beyond the built-in table - see gbcam_set_extra_palettes()
 * in gbcam.h, same idea. colors_fn returns the count and points *colors at
 * a contiguous RGB888 array, same shape as dc_palette_colors(). */
typedef int (*dc_extra_palette_fn)(int index, const uint8_t (**colors)[3]);
typedef const char *(*dc_extra_palette_name_fn)(int index);
void dc_set_extra_palettes(int count, dc_extra_palette_fn colors_fn, dc_extra_palette_name_fn name_fn);

/* Extra palettes' nearest-colour LUTs (see dc_lut_data.h's DC_LUT_BITS
 * comment - same size/shape, DC_LUT_SIZE bytes) aren't built for the whole
 * SD card up front and don't all stay resident: only the currently-selected
 * one is ever in RAM, and the host may persist it (e.g. to the SD card
 * alongside the source palette) so switching back to a palette used earlier
 * this boot - or a previous one - is a file read instead of a rebuild.
 * load_fn fills out_lut (DC_LUT_SIZE bytes) and returns true on a hit; a
 * miss (false, or no load_fn) rebuilds from scratch and calls save_fn
 * (best-effort - a save failure just means rebuilding again next time).
 * Either callback may be NULL. Must match whatever DC_LUT_BITS the built-in
 * table was generated with - see dc_lut_data.h. */
#define DC_LUT_BITS 5
#define DC_LUT_SIZE (1 << (DC_LUT_BITS * 3))
typedef bool (*dc_extra_lut_load_fn)(int index, uint8_t *out_lut);
typedef void (*dc_extra_lut_save_fn)(int index, const uint8_t *lut);
void dc_set_extra_lut_io(dc_extra_lut_load_fn load_fn, dc_extra_lut_save_fn save_fn);

/* Crop + nearest-neighbour downscale any camera frame to w x h RGB888. */
void dc_sample(const gbcam_frame_t *frame, uint8_t *rgb, int w, int h);

/* Crop + box-average downscale any camera frame to w x h RGB888 (smooth;
 * used for the Normal Cam preview). */
void dc_sample_smooth(const gbcam_frame_t *frame, uint8_t *rgb, int w, int h);

/* Optional hardware-accelerated resample - e.g. the ESP32-P4's PPA (Pixel
 * Processing Accelerator) peripheral, which can crop+scale+convert a whole
 * frame in one hardware pass instead of the CPU touching every source pixel
 * itself. If registered, both dc_sample() and dc_sample_smooth() try it
 * first (same crop rect they'd compute themselves - cx0/cy0/cw/ch, already
 * clipped to the source frame), falling back to their own CPU loop if it
 * returns false (not registered, or this particular frame/size combination
 * isn't something the hardware path handles). Component stays platform-
 * neutral either way - it just calls back into whatever the host
 * registered, same idea as the extra-palette hooks above. */
typedef bool (*dc_hw_resample_fn)(const gbcam_frame_t *frame, int cx0, int cy0, int cw, int ch,
                                   uint8_t *out_rgb, int out_w, int out_h);
void dc_set_hw_resample(dc_hw_resample_fn fn);

/* Temporal noise reduction for Dither Cam's live preview, in place: per
 * pixel/channel, moves toward the new value by a blend weight that ramps
 * with the size of the change - 1/(1<<strength) (strength 1 = 1/2 .. 3 =
 * 1/8) at zero delta, rising smoothly to a full 1:1 (no smoothing) once the
 * delta reaches the noise floor - rather than gbcam's own temporal_denoise()
 * hard snap-if-bigger-than-threshold, which (tried here first) ghosts fast
 * movement: a moving edge's blurred boundary produces a spread of per-frame
 * deltas, and any of them landing just under a hard threshold gets the full
 * noise-strength smoothing, i.e. a visible trail of "hasn't caught up yet"
 * frames. Call every frame with dc_sample()'s output, before dc_quantize();
 * without it, ordinary per-frame sensor noise can flip a boundary pixel
 * between two far-apart palette colours every frame, which reads as flicker
 * - worse the fewer/more spread-out the palette's colours are, and much
 * worse again (a visible shimmer, not just a flickering pixel) under an
 * error-diffusion method (Floyd-Steinberg/Atkinson/Sierra Lite): one flipped
 * pixel's error propagates to every pixel after it in scan order, so it's
 * worth a stronger strength there than Bayer/none need. Keeps one w*h*3
 * accumulator, reset (reseeded from this call's frame, no denoise yet)
 * whenever w or h changes from the last call. */
void dc_temporal_denoise(uint8_t *rgb, int w, int h, int strength);

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
