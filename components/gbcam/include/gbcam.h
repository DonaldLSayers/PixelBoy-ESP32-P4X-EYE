/*
 * gbcam - Game Boy Camera look, in software.
 *
 * Portable C99, no platform dependencies. Used by the ESP32-P4X-EYE firmware
 * and by the host test tool in tools/host.
 *
 * Pipeline (per frame):
 *   camera frame -> centre crop (8:7) + area-average down to 128x112 luma
 *   -> exposure (digital gain, driven by gb-photo's auto-exposure loop)
 *   -> edge enhancement (M64282FP-style)
 *   -> map to MAC-GBD ADC code range
 *   -> 4x4x3 threshold dithering (gb-photo contrast tables and patterns)
 *   -> 128x112 image of shades 0..3 (0 = white, 3 = black)
 */
#ifndef GBCAM_H_INCLUDED
#define GBCAM_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gbcam_palettes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GBCAM_W 128
#define GBCAM_H 112
#define GBCAM_PIXELS (GBCAM_W * GBCAM_H)
#define GBCAM_TILES_SIZE (GBCAM_PIXELS / 4) /* 2bpp tile data: 3584 bytes */

#define GBCAM_CONTRAST_LEVELS 16
/* Contrast 7 reproduces, byte for byte, the dither registers the stock Game Boy
 * Camera ROM writes (Raphael Boichot's register logs, both light tables). */
#define GBCAM_DEFAULT_CONTRAST 7
#define GBCAM_BRIGHTNESS_LEVELS 17 /* 0 = darkest target .. 16 = brightest */
#define GBCAM_DEFAULT_BRIGHTNESS 8
#define GBCAM_EDGE_RATIOS 8        /* 50, 75, 100, 125, 200, 300, 400, 500 % */

typedef enum {
    GBCAM_FMT_GREY8,
    GBCAM_FMT_RGB565_LE,
    GBCAM_FMT_RGB565_BE,
    GBCAM_FMT_RGB888,
} gbcam_pixfmt_t;

/* Dither patterns from gb-photo; DEFAULT is the stock Game Boy Camera Bayer matrix. */
typedef enum {
    GBCAM_DITHER_OFF,
    GBCAM_DITHER_DEFAULT,
    GBCAM_DITHER_2X2,
    GBCAM_DITHER_GRID,
    GBCAM_DITHER_MAZE,
    GBCAM_DITHER_NEST,
    GBCAM_DITHER_FUZZ,
    GBCAM_DITHER_VERTICAL,
    GBCAM_DITHER_HORIZONTAL,
    GBCAM_DITHER_MIX,
    GBCAM_DITHER_COUNT
} gbcam_dither_t;

typedef enum {
    GBCAM_EDGE_AUTO,       /* chosen from exposure tier, like gb-photo's RENDER_REGS_FROM_EXPOSURE */
    GBCAM_EDGE_NONE,
    GBCAM_EDGE_HORIZONTAL,
    GBCAM_EDGE_2D,
} gbcam_edge_mode_t;

/* Which value pixel_luma() reads out of a colour source pixel - the usual
 * weighted-average luma, or one raw channel on its own (GREY8 has none to
 * pick, so it's read as luma regardless). Lets gbcam_downsample() run its
 * whole pipeline (exposure, edge enhancement, threshold dither) on a single
 * colour channel instead of luma - run once per channel on the same frame
 * and recombine the three results for a full-colour "GB Camera" look (see
 * PIXELBOY's RGB palette), the digital equivalent of the real camera's
 * red/green/blue physical-filter trick. */
typedef enum {
    GBCAM_CHANNEL_LUMA,
    GBCAM_CHANNEL_RED,
    GBCAM_CHANNEL_GREEN,
    GBCAM_CHANNEL_BLUE,
} gbcam_channel_t;

typedef struct {
    const void *data;
    int width;
    int height;
    int stride;           /* bytes per row; 0 = tightly packed */
    gbcam_pixfmt_t format;
    bool mirror_x;
    bool mirror_y;
    gbcam_channel_t channel; /* default (0) = GBCAM_CHANNEL_LUMA, existing behaviour */
} gbcam_frame_t;

/* How the image is prepared before the (identical, ROM-accurate) threshold dither. */
typedef enum {
    GBCAM_STYLE_PIXELCAM,        /* PIXEL CAM: auto levels (entropy search), contrast/gamma curve,
                                    no edge enhancement - cleaner, uses all 4 shades */
    GBCAM_STYLE_HARDWARE,        /* M64282FP + MAC-GBD model, pixel-identical to the Pan Docs
                                    reference emulation, with Game Boy Camera auto-exposure */
    GBCAM_STYLE_COUNT
} gbcam_style_t;

typedef enum {
    GBCAM_TABLE_AUTO,            /* from the exposure tier, like the stock camera */
    GBCAM_TABLE_HIGH_LIGHT,
    GBCAM_TABLE_LOW_LIGHT,
} gbcam_table_t;

typedef struct {
    uint8_t brightness;          /* 0..16, auto-exposure target */
    uint8_t contrast;            /* 0..15 */
    gbcam_dither_t dither;
    gbcam_edge_mode_t edge_mode;
    uint8_t edge_ratio;          /* 0..7, index into 50..500 % */
    bool auto_exposure;
    uint16_t manual_gain_q8;     /* used when auto_exposure is false; 256 = 1.0 */
    uint8_t zoom_pct;            /* 100 = full sensor height; larger crops tighter */
    gbcam_table_t light_table;   /* which contrast threshold table to use */
    gbcam_style_t style;
    /* PIXEL CAM style: levels are searched automatically unless auto_levels is false. */
    bool auto_levels;
    float levels_contrast;       /* manual levels: 1.0 = no change */
    float levels_gamma;          /* manual levels: 1.0 = no change */
    /* Temporal noise reduction on the 128x112 image, 0 = off .. 3 = strongest.
     * Motion-adaptive: pixels that change a lot (real movement) update at once. */
    uint8_t denoise;
    /* Downscale quality: up to N x N samples per output pixel (1..8, 0 = 8).
     * Fewer samples = fewer memory reads on large camera frames. */
    uint8_t max_samples;
} gbcam_settings_t;

typedef struct {
    gbcam_settings_t settings;

    /* State */
    uint16_t gain_q8;            /* current digital exposure, 256 = 1.0 */
    uint8_t tier;                /* exposure tier 0 (bright) .. 4 (dark) */
    bool high_light;             /* which gb-photo contrast table is active */
    uint8_t matrix[48];          /* 16 positions x 3 thresholds */
    int16_t last_ae_error;       /* per-tile histogram error, for debugging */
    uint8_t matrix_key[3];       /* dither, contrast, high_light the matrix was built for */
    bool matrix_valid;
    float lv_contrast, lv_gamma; /* PIXEL CAM style: levels in use (smoothed auto result) */
    bool lv_valid;
    int8_t lv_target;            /* PIXEL CAM style: auto-levels candidate being followed, -1 none */
    /* PIXEL CAM style: final shade-mapping curve, cached per-instance since
     * each gbcam_t (e.g. Trichrome's separate R/G/B instances) converges to
     * its own independent (contrast, gamma) - a single shared cache gets
     * invalidated and rebuilt by every other instance's call, defeating the
     * whole point of caching it. */
    uint8_t lv_lut[256];
    float lv_lut_c, lv_lut_g;
    bool acc_valid;
    uint16_t luma_acc[GBCAM_PIXELS]; /* denoise accumulator, luma in Q8 */

    /* Buffers */
    uint8_t luma[GBCAM_PIXELS];  /* downsampled sensor image */
    uint8_t work[GBCAM_PIXELS];  /* exposed image, before edge enhancement */
    uint8_t shades[GBCAM_PIXELS];/* output, 0 = white .. 3 = black */
} gbcam_t;

void gbcam_default_settings(gbcam_settings_t *s);
void gbcam_init(gbcam_t *cam, const gbcam_settings_t *s);

/* Full pipeline: downsample the frame, then gbcam_process_luma(). */
void gbcam_process(gbcam_t *cam, const gbcam_frame_t *frame);

/* Pipeline from an already-downsampled 128x112 luma image in cam->luma. */
void gbcam_process_luma(gbcam_t *cam);

/* The two halves of gbcam_process_luma()'s HARDWARE-style path (PIXEL CAM
 * style has no equivalent split - it's a different pipeline entirely, see
 * gbcam_process_pixelcam()), exposed separately for AEB-style bracketing:
 * gbcam_expose_luma() applies (digital) exposure to cam->luma into cam->work,
 * gbcam_dither_work() runs edge enhancement + threshold dither from cam->work
 * into cam->shades. Neither touches auto-exposure's own feedback step
 * (auto_exposure_step(), still only inside gbcam_process_luma()) - a
 * bracketing caller wants several forced exposures without that adjusting
 * gain_q8 out from under it between steps. */
void gbcam_expose_luma(gbcam_t *cam);
void gbcam_dither_work(gbcam_t *cam);

/* Downsample only (fills cam->luma). */
void gbcam_downsample(gbcam_t *cam, const gbcam_frame_t *frame);

/* Rebuild the threshold matrix after changing contrast or dither settings.
 * gbcam_process() calls this automatically when needed. */
void gbcam_update_matrix(gbcam_t *cam);

/* Convert shades to Game Boy 2bpp tile data (16x14 tiles, 16 bytes each). */
void gbcam_shades_to_tiles(const uint8_t *shades, uint8_t tiles[GBCAM_TILES_SIZE]);

/* Convert Game Boy 2bpp tile data back to shades. */
void gbcam_tiles_to_shades(const uint8_t tiles[GBCAM_TILES_SIZE], uint8_t *shades);

/* Display palettes: the PIXEL CAM project's 4-colour GB palettes
 * (GBCAM_PALETTE_COUNT, GBCAM_PALETTE_DEFAULT in gbcam_palettes.h). */
typedef uint8_t gbcam_palette_t;

/* RGB888 triple for a shade (0 = white .. 3 = black). Indices
 * >= GBCAM_PALETTE_COUNT are served by the extra-palette callback below, if
 * one is registered (falls back to GBCAM_PALETTE_DEFAULT otherwise). */
const uint8_t *gbcam_palette_rgb(gbcam_palette_t p, uint8_t shade);
const char *gbcam_palette_name(gbcam_palette_t p);
/* Built-in count plus however many extra palettes are currently registered. */
int gbcam_palette_count(void);

/* Extra palettes beyond the built-in table (GBCAM_PALETTE_COUNT..count-1) -
 * for a host that loads user palettes at runtime (e.g. from an SD card);
 * this component stays platform-neutral and just calls back into whatever
 * the host registered. index is 0-based within the extra range (already
 * subtracted from GBCAM_PALETTE_COUNT). Passing count=0 clears it. */
typedef const uint8_t *(*gbcam_extra_palette_rgb_fn)(int index, uint8_t shade);
typedef const char *(*gbcam_extra_palette_name_fn)(int index);
void gbcam_set_extra_palettes(int count, gbcam_extra_palette_rgb_fn rgb_fn,
                              gbcam_extra_palette_name_fn name_fn);

/* Names for UI / logs. */
const char *gbcam_style_name(gbcam_style_t s);
const char *gbcam_dither_name(gbcam_dither_t d);
int gbcam_edge_ratio_pct(uint8_t idx);

/* Low-level: build a 48-byte threshold matrix (gb-photo dither_pattern_apply). */
void gbcam_build_matrix(uint8_t out[48], gbcam_dither_t dither, bool high_light, uint8_t contrast);

#ifdef __cplusplus
}
#endif

#endif /* GBCAM_H_INCLUDED */
