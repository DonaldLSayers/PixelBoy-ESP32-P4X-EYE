/*
 * gbcam - Game Boy Camera look, in software. See gbcam.h for the pipeline.
 *
 * Sensor + MAC-GBD model follows the Pan Docs reference emulation by Antonio
 * Nino Diaz (GiiBiiAdvance), cross-checked against Raphael Boichot's hardware
 * measurements (Mitsubishi-M64282FP-dashcam):
 *   - exposure scales the pixel, then the output sits on the MAC-GBD's
 *     0..3.3 V ADC scale with the black level at 1.5 V = code 112
 *     (Pan Docs: value = 128 + (value - 128) / 8)
 *   - edge enhancement runs on that voltage scale, before the thresholds
 *   - 4x4x3 threshold matrix: shade 3 below t0, 2 below t1, 1 below t2, else 0
 *
 * The auto-exposure step logic and exposure tiers follow gb-photo
 * (https://github.com/untoxa/gb-photo, MIT, Copyright (c) 2022 Toxa),
 * src/state_camera.c and src/histogram.c.
 */
#include <string.h>

#include "gbcam.h"

/* gbcam_pixelcam.c */
void gbcam_process_pixelcam(gbcam_t *cam);

static const char *const style_names[GBCAM_STYLE_COUNT] = {"PIXEL CAM", "HARDWARE"};

const char *gbcam_style_name(gbcam_style_t s)
{
    return (s < GBCAM_STYLE_COUNT) ? style_names[s] : "?";
}

/* MAC-GBD ADC code of a black pixel (1.5 V on a 0..3.3 V, 8-bit scale). */
#define BLACK_LEVEL 112
/* Pixel values are compressed by 8 onto the voltage scale (Pan Docs "adapt to 3.1/5.0 V"). */
#define VOLTAGE_SHIFT 3

/* Digital exposure limits (Q8: 256 = 1.0). A normally lit, normally exposed
 * webcam/ISP frame settles around 3.5x. */
#define GAIN_MIN 64     /* 0.25x */
#define GAIN_MAX 8192   /* 32x   */
#define GAIN_START 896  /* 3.5x  */

/* gb-photo auto-exposure sensitivities (per-tile histogram error). */
#define AUTOEXP_SENSIVITY0 5
#define AUTOEXP_SENSIVITY1 10
#define AUTOEXP_SENSIVITY2 20
#define AUTOEXP_SENSIVITY3 95

/* Histogram: 12 sample tiles, each tile sums 64 shades of 0..3. */
#define HISTOGRAM_POINTS 12
#define HISTOGRAM_MAX (HISTOGRAM_POINTS * 64 * 3) /* 2304 */

static const uint8_t histogram_center_tiles[HISTOGRAM_POINTS][2] = {
    {6, 4}, {9, 4},
    {5, 5}, {10, 5},
    {7, 6}, {8, 6},
    {7, 7}, {8, 7},
    {5, 8}, {10, 8},
    {6, 9}, {9, 9},
};

static const uint16_t edge_ratio_pct[GBCAM_EDGE_RATIOS] = {50, 75, 100, 125, 200, 300, 400, 500};

/* Exposure tiers, gain thresholds in Q8. Tier 0 = brightest scene (lowest gain).
 * The stock camera switches register sets at exposure C = 0x0030, 0x0D80, 0x3500
 * and 0x8500. We only know our digital gain (the ISP auto-exposure runs first), so
 * the tiers are centred on the ~3.5x a normal frame settles at: tier 2. */
static const uint16_t tier_gain_limits[4] = {384, 768, 1536, 3072};

typedef struct {
    gbcam_edge_mode_t edge;
    bool high_light;
} tier_params_t;

/* Stock Game Boy Camera register sets, bright to dark (Raphael Boichot's logs,
 * dashcam config.h camReg1..5 and dithering_strategy): edge ratio is always 50 %,
 * the "high light" threshold table is used everywhere except the darkest set. */
static const tier_params_t tier_params[5] = {
    {GBCAM_EDGE_HORIZONTAL, true},  /* reg1 0x20: horizontal edge, gain 14 dB  */
    {GBCAM_EDGE_2D, true},          /* reg1 0xE0: 2-D edge, 14 dB              */
    {GBCAM_EDGE_2D, true},          /* reg1 0xE4: 2-D edge, 20 dB              */
    {GBCAM_EDGE_2D, true},          /* reg1 0xE8: 2-D edge, 26 dB              */
    {GBCAM_EDGE_NONE, false},       /* reg1 0x0A: no edge, 32 dB, low-light table */
};

void gbcam_default_settings(gbcam_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->brightness = GBCAM_DEFAULT_BRIGHTNESS;
    s->contrast = GBCAM_DEFAULT_CONTRAST;
    s->dither = GBCAM_DITHER_DEFAULT;
    s->edge_mode = GBCAM_EDGE_AUTO;
    s->edge_ratio = 0;
    s->auto_exposure = true;
    s->manual_gain_q8 = 256;
    s->zoom_pct = 100;
    s->style = GBCAM_STYLE_PIXELCAM;
    s->auto_levels = true;
    s->levels_contrast = 1.15f;
    s->levels_gamma = 1.0f;
    s->denoise = 2;
}

void gbcam_init(gbcam_t *cam, const gbcam_settings_t *s)
{
    memset(cam, 0, sizeof(*cam));
    if (s)
        cam->settings = *s;
    else
        gbcam_default_settings(&cam->settings);
    cam->gain_q8 = GAIN_START;
    cam->tier = 2;
    cam->lv_target = -1;
    cam->high_light = true;
    gbcam_update_matrix(cam);
}

int gbcam_edge_ratio_pct(uint8_t idx)
{
    return edge_ratio_pct[idx < GBCAM_EDGE_RATIOS ? idx : GBCAM_EDGE_RATIOS - 1];
}

void gbcam_update_matrix(gbcam_t *cam)
{
    const gbcam_settings_t *s = &cam->settings;
    uint8_t key[3] = {(uint8_t)s->dither, s->contrast, (uint8_t)cam->high_light};

    if (cam->matrix_valid && memcmp(key, cam->matrix_key, sizeof(key)) == 0)
        return;
    gbcam_build_matrix(cam->matrix, s->dither, cam->high_light, s->contrast);
    memcpy(cam->matrix_key, key, sizeof(key));
    cam->matrix_valid = true;
}

/* ---------------------------------------------------------------- downsample */

static inline uint8_t pixel_luma(const uint8_t *row, int x, gbcam_pixfmt_t fmt)
{
    switch (fmt) {
    case GBCAM_FMT_GREY8:
        return row[x];
    case GBCAM_FMT_RGB888: {
        const uint8_t *p = row + x * 3;
        return (uint8_t)((77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8);
    }
    case GBCAM_FMT_RGB565_LE:
    case GBCAM_FMT_RGB565_BE: {
        const uint8_t *p = row + x * 2;
        uint16_t v = (fmt == GBCAM_FMT_RGB565_LE) ? (uint16_t)(p[0] | (p[1] << 8))
                                                  : (uint16_t)((p[0] << 8) | p[1]);
        int r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        return (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
    }
    }
    return 0;
}

static int bytes_per_pixel(gbcam_pixfmt_t fmt)
{
    switch (fmt) {
    case GBCAM_FMT_GREY8: return 1;
    case GBCAM_FMT_RGB888: return 3;
    default: return 2;
    }
}

/* Temporal noise reduction: per pixel, move the running average a fraction of
 * the way toward the new value (1/2, 1/4, 1/8 for strength 1..3). A change bigger
 * than MOTION_LEVELS is treated as real movement and taken at once, so moving
 * things don't smear. Runs once per new frame, from gbcam_downsample(). */
#define MOTION_LEVELS 24

static void temporal_denoise(gbcam_t *cam)
{
    uint8_t strength = cam->settings.denoise > 3 ? 3 : cam->settings.denoise;
    if (strength == 0 || !cam->acc_valid) {
        for (int i = 0; i < GBCAM_PIXELS; i++)
            cam->luma_acc[i] = (uint16_t)(cam->luma[i] << 8);
        cam->acc_valid = strength != 0;
        return;
    }
    for (int i = 0; i < GBCAM_PIXELS; i++) {
        int target = cam->luma[i] << 8;
        int acc = cam->luma_acc[i];
        int d = target - acc;
        if (d > MOTION_LEVELS * 256 || d < -MOTION_LEVELS * 256)
            acc = target;
        else
            acc += d / (1 << strength);
        cam->luma_acc[i] = (uint16_t)acc;
        cam->luma[i] = (uint8_t)((acc + 128) >> 8);
    }
}

/* Up to SAMPLES x SAMPLES evenly spaced samples per output pixel: a full box
 * average for frames up to ~1024 px wide, a close approximation above. */
#define SAMPLES 8

void gbcam_downsample(gbcam_t *cam, const gbcam_frame_t *f)
{
    const int w = f->width, h = f->height;
    const int stride = f->stride ? f->stride : w * bytes_per_pixel(f->format);
    const uint8_t *base = (const uint8_t *)f->data;

    /* Centre crop to 8:7, then zoom. */
    int cw, ch;
    if ((long)w * 7 > (long)h * 8) {
        ch = h;
        cw = (int)((long)h * 8 / 7);
    } else {
        cw = w;
        ch = (int)((long)w * 7 / 8);
    }
    int zoom = cam->settings.zoom_pct < 100 ? 100 : cam->settings.zoom_pct;
    cw = cw * 100 / zoom;
    ch = ch * 100 / zoom;
    if (cw < GBCAM_W) cw = GBCAM_W < w ? GBCAM_W : w;
    if (ch < GBCAM_H) ch = GBCAM_H < h ? GBCAM_H : h;
    const int x0 = (w - cw) / 2, y0 = (h - ch) / 2;

    /* Sample positions per output column / row. */
    const int samples = (cam->settings.max_samples == 0 || cam->settings.max_samples > SAMPLES)
                            ? SAMPLES : cam->settings.max_samples;
    int xs[GBCAM_W][SAMPLES], nx[GBCAM_W];
    for (int ox = 0; ox < GBCAM_W; ox++) {
        int a = x0 + ox * cw / GBCAM_W, b = x0 + (ox + 1) * cw / GBCAM_W;
        int n = b - a;
        if (n < 1) n = 1;
        int cnt = n < samples ? n : samples;
        for (int i = 0; i < cnt; i++)
            xs[ox][i] = a + (i * n + n / 2) / cnt;
        nx[ox] = cnt;
    }

    for (int oy = 0; oy < GBCAM_H; oy++) {
        int a = y0 + oy * ch / GBCAM_H, b = y0 + (oy + 1) * ch / GBCAM_H;
        int n = b - a;
        if (n < 1) n = 1;
        int ny = n < samples ? n : samples;
        const uint8_t *rows[SAMPLES];
        for (int i = 0; i < ny; i++)
            rows[i] = base + (size_t)(a + (i * n + n / 2) / ny) * stride;

        int dy = f->mirror_y ? (GBCAM_H - 1 - oy) : oy;
        uint8_t *out = cam->luma + dy * GBCAM_W;

        for (int ox = 0; ox < GBCAM_W; ox++) {
            unsigned sum = 0;
            for (int j = 0; j < ny; j++)
                for (int i = 0; i < nx[ox]; i++)
                    sum += pixel_luma(rows[j], xs[ox][i], f->format);
            int dx = f->mirror_x ? (GBCAM_W - 1 - ox) : ox;
            out[dx] = (uint8_t)(sum / (unsigned)(ny * nx[ox]));
        }
    }

    temporal_denoise(cam);
}

/* ------------------------------------------------------------------- process */

/* M64282FP edge enhancement on the voltage scale (codes centred on 128), as in the
 * Pan Docs reference: 2-D  P + {4P - (N+S+E+W)} * alpha
 *                     horiz P + {2P - (E+W)} * alpha
 * The result is truncated toward zero (like the reference's float-to-int) and
 * clamped to -128..127 around the centre. The reference clamps the horizontal
 * mode to 0..255 on signed values, which looks like a slip; both modes use the
 * symmetric clamp here. */
static void apply_edge(const uint8_t *in, uint8_t *out, gbcam_edge_mode_t mode, int ratio_pct)
{
    if (mode == GBCAM_EDGE_NONE) {
        memcpy(out, in, GBCAM_PIXELS);
        return;
    }
    for (int y = 0; y < GBCAM_H; y++) {
        const uint8_t *row = in + y * GBCAM_W;
        const uint8_t *up = in + (y > 0 ? y - 1 : y) * GBCAM_W;
        const uint8_t *dn = in + (y < GBCAM_H - 1 ? y + 1 : y) * GBCAM_W;
        for (int x = 0; x < GBCAM_W; x++) {
            int p = row[x] - 128;
            int l = row[x > 0 ? x - 1 : x] - 128;
            int r = row[x < GBCAM_W - 1 ? x + 1 : x] - 128;
            int lap = (mode == GBCAM_EDGE_2D) ? (4 * p - l - r - (up[x] - 128) - (dn[x] - 128))
                                              : (2 * p - l - r);
            int v = (p * 100 + lap * ratio_pct) / 100;
            v = v < -128 ? -128 : (v > 127 ? 127 : v);
            out[y * GBCAM_W + x] = (uint8_t)(v + 128);
        }
    }
}

static int histogram(const uint8_t *shades)
{
    int sum = 0;
    for (int t = 0; t < HISTOGRAM_POINTS; t++) {
        const uint8_t *tile = shades + histogram_center_tiles[t][1] * 8 * GBCAM_W
                                     + histogram_center_tiles[t][0] * 8;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                sum += tile[y * GBCAM_W + x];
    }
    return sum;
}

static void auto_exposure_step(gbcam_t *cam)
{
    const gbcam_settings_t *s = &cam->settings;
    int brightness = s->brightness > 16 ? 16 : s->brightness;
    /* Histogram counts darkness (3 = black), so a brighter setting means a lower target. */
    int target = (16 - brightness) * HISTOGRAM_MAX / 16;
    int error = (histogram(cam->shades) - target) / HISTOGRAM_POINTS;
    int abs_error = error < 0 ? -error : error;
    int32_t g = cam->gain_q8, ng;

    cam->last_ae_error = (int16_t)error;

    /* Positive error = too dark = more exposure. */
    if (abs_error > AUTOEXP_SENSIVITY3) {
        ng = error < 0 ? (g >> 1) : (g << 1);                         /* +-1 EV   */
    } else if (abs_error > AUTOEXP_SENSIVITY2) {
        int d = g >> 3; if (d < 1) d = 1;
        ng = g + (error < 0 ? -d : d);                                /* +-1/8 EV */
    } else if (abs_error > AUTOEXP_SENSIVITY1) {
        int d = g >> 4; if (d < 1) d = 1;
        ng = g + (error < 0 ? -d : d);                                /* +-1/16 EV */
    } else if (abs_error > AUTOEXP_SENSIVITY0) {
        ng = g + (error < 0 ? -1 : 1);
    } else {
        ng = g;
    }
    if (ng < GAIN_MIN) ng = GAIN_MIN;
    if (ng > GAIN_MAX) ng = GAIN_MAX;
    cam->gain_q8 = (uint16_t)ng;
}

static uint8_t tier_from_gain(uint16_t gain_q8)
{
    uint8_t t = 0;
    while (t < 4 && gain_q8 >= tier_gain_limits[t])
        t++;
    return t;
}

void gbcam_process_luma(gbcam_t *cam)
{
    const gbcam_settings_t *s = &cam->settings;

    if (s->style == GBCAM_STYLE_PIXELCAM) {
        gbcam_process_pixelcam(cam);
        return;
    }

    if (!s->auto_exposure)
        cam->gain_q8 = s->manual_gain_q8 ? s->manual_gain_q8 : 256;

    cam->tier = tier_from_gain(cam->gain_q8);
    cam->high_light = s->light_table == GBCAM_TABLE_HIGH_LIGHT ? true
                    : s->light_table == GBCAM_TABLE_LOW_LIGHT  ? false
                    : tier_params[cam->tier].high_light;
    gbcam_edge_mode_t edge = (s->edge_mode == GBCAM_EDGE_AUTO) ? tier_params[cam->tier].edge
                                                               : s->edge_mode;
    gbcam_update_matrix(cam);

    /* Sensor: exposure (unclamped, like the reference), then onto the MAC-GBD
     * voltage scale: black at code 112, pixel values compressed by 8. */
    const uint32_t g = cam->gain_q8;
    for (int i = 0; i < GBCAM_PIXELS; i++) {
        int exposed = (int)((cam->luma[i] * g) >> 8);
        /* Exactly the reference: 128 + (v - 128) / 8, C division (toward zero),
         * so black (v = 0) lands on BLACK_LEVEL. */
        int code = 128 + (exposed - 128) / (1 << VOLTAGE_SHIFT);
        cam->work[i] = (uint8_t)(code < 0 ? 0 : (code > 255 ? 255 : code));
    }

    /* Edge enhancement into shades[] (used as scratch), then the MAC-GBD
     * threshold matrix in place. */
    uint8_t *v = cam->shades;
    apply_edge(cam->work, v, edge, gbcam_edge_ratio_pct(s->edge_ratio));

    const uint8_t *m = cam->matrix;
    for (int y = 0; y < GBCAM_H; y++) {
        for (int x = 0; x < GBCAM_W; x++) {
            int i = y * GBCAM_W + x;
            uint8_t code = v[i];
            const uint8_t *t = m + (((y & 3) << 2) | (x & 3)) * 3;
            v[i] = code < t[0] ? 3 : code < t[1] ? 2 : code < t[2] ? 1 : 0;
        }
    }

    if (s->auto_exposure)
        auto_exposure_step(cam);
}

void gbcam_process(gbcam_t *cam, const gbcam_frame_t *frame)
{
    gbcam_downsample(cam, frame);
    gbcam_process_luma(cam);
}

/* --------------------------------------------------------------------- tiles */

void gbcam_shades_to_tiles(const uint8_t *shades, uint8_t tiles[GBCAM_TILES_SIZE])
{
    uint8_t *out = tiles;
    for (int ty = 0; ty < GBCAM_H / 8; ty++) {
        for (int tx = 0; tx < GBCAM_W / 8; tx++) {
            for (int r = 0; r < 8; r++) {
                const uint8_t *p = shades + (ty * 8 + r) * GBCAM_W + tx * 8;
                uint8_t lo = 0, hi = 0;
                for (int b = 0; b < 8; b++) {
                    lo |= (uint8_t)((p[b] & 1) << (7 - b));
                    hi |= (uint8_t)(((p[b] >> 1) & 1) << (7 - b));
                }
                *out++ = lo;
                *out++ = hi;
            }
        }
    }
}

void gbcam_tiles_to_shades(const uint8_t tiles[GBCAM_TILES_SIZE], uint8_t *shades)
{
    const uint8_t *in = tiles;
    for (int ty = 0; ty < GBCAM_H / 8; ty++) {
        for (int tx = 0; tx < GBCAM_W / 8; tx++) {
            for (int r = 0; r < 8; r++) {
                uint8_t lo = *in++, hi = *in++;
                uint8_t *p = shades + (ty * 8 + r) * GBCAM_W + tx * 8;
                for (int b = 0; b < 8; b++)
                    p[b] = (uint8_t)(((lo >> (7 - b)) & 1) | (((hi >> (7 - b)) & 1) << 1));
            }
        }
    }
}
