/*
 * Dither Cam, ported from the PIXEL CAM project's two implementations:
 * its Python dithercam.py (quantize, _quantize_*) and its C++
 * dithercam.cpp.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dithercam.h"
#include "palettes_data.h"

/* Not pixelboy/dithercam.py's own RESOLUTIONS list - see the DC_SIZE_COUNT
 * comment in dithercam.h for why these are picked to always scale onto the
 * screen by a clean 1x/2x factor instead. Ordered by ascending pixel count,
 * so cycling the menu row is a steady small -> large sweep. Indices moved:
 * keep DC_SIZE_DEFAULT in dithercam.h in sync with wherever 120x90 lands
 * here. */
static const uint16_t dc_sizes[DC_SIZE_COUNT][2] = {
    {120, 60}, {120, 90}, {120, 120}, {240, 120}, {240, 160}, {240, 180}, {240, 240},
};

/* Normal Cam's "digicam" presets - see the NORMAL_SIZE_COUNT comment in
 * dithercam.h for why these are separate from dc_sizes[], 4:3-only, and
 * capped at 640x480. Ascending pixel count. */
static const uint16_t normal_sizes[NORMAL_SIZE_COUNT][2] = {
    {320, 240}, {480, 360}, {640, 480},
};
static const char *const normal_size_labels[NORMAL_SIZE_COUNT] = {
    "320X240", "480X360", "640X480",
};

static const char *const method_names[DC_METHOD_COUNT] = {
    "NONE", "BAYER 4X4", "BAYER 8X8", "FLOYD-STEINBERG", "ATKINSON", "SIERRA LITE",
    /* Floyd-Steinberg's hyphen is part of the two names it's actually called
     * after, unlike a filename's - it stays; the menu row still truncates it
     * (truncate_value(), 11 chars) but the OSD popup shows it in full. */
};

/* Recursive Bayer matrices (M2 = [[0,2],[3,1]]), dithercam.py _generate_bayer_matrix. */
static const uint8_t bayer4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};
static const uint8_t bayer8[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},
    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41},
    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},
    {63, 31, 55, 23, 61, 29, 53, 21},
};

void dc_size(int index, int *w, int *h)
{
    if (index < 0 || index >= DC_SIZE_COUNT) index = DC_SIZE_DEFAULT;
    *w = dc_sizes[index][0];
    *h = dc_sizes[index][1];
}

void normal_size(int index, int *w, int *h)
{
    if (index < 0 || index >= NORMAL_SIZE_COUNT) index = NORMAL_SIZE_DEFAULT;
    *w = normal_sizes[index][0];
    *h = normal_sizes[index][1];
}

const char *normal_size_name(int index)
{
    return (index >= 0 && index < NORMAL_SIZE_COUNT) ? normal_size_labels[index] : "?";
}

const char *dc_method_name(dc_method_t m)
{
    return (m < DC_METHOD_COUNT) ? method_names[m] : "?";
}

static int s_extra_count;
static dc_extra_palette_fn s_extra_colors;
static dc_extra_palette_name_fn s_extra_name;

/* Only the currently-selected extra palette's LUT is ever in RAM - see
 * ensure_active_lut() below and dc_set_extra_lut_io()'s comment in
 * dithercam.h for why. */
static dc_extra_lut_load_fn s_lut_load;
static dc_extra_lut_save_fn s_lut_save;
static int s_active_extra_idx = -1;
static uint8_t *s_active_lut;

void dc_set_extra_palettes(int count, dc_extra_palette_fn colors_fn, dc_extra_palette_name_fn name_fn)
{
    free(s_active_lut);
    s_active_lut = NULL;
    s_active_extra_idx = -1;
    s_extra_count = count > 0 ? count : 0;
    s_extra_colors = colors_fn;
    s_extra_name = name_fn;
}

void dc_set_extra_lut_io(dc_extra_lut_load_fn load_fn, dc_extra_lut_save_fn save_fn)
{
    s_lut_load = load_fn;
    s_lut_save = save_fn;
}

int dc_palette_count(void) { return DC_PALETTE_COUNT + s_extra_count; }

const char *dc_palette_name(int p)
{
    if (p >= 0 && p < DC_PALETTE_COUNT) return dc_palette_names[p];
    if (p >= DC_PALETTE_COUNT && p < DC_PALETTE_COUNT + s_extra_count && s_extra_name)
        return s_extra_name(p - DC_PALETTE_COUNT);
    return "?";
}

int dc_palette_colors(int p, const uint8_t (**colors)[3])
{
    if (p >= DC_PALETTE_COUNT && p < DC_PALETTE_COUNT + s_extra_count && s_extra_colors)
        return s_extra_colors(p - DC_PALETTE_COUNT, colors);
    if (p < 0 || p >= DC_PALETTE_COUNT) p = DC_PALETTE_DEFAULT;
    *colors = &dc_palette_rgb[dc_palette_offset[p]];
    return dc_palette_size[p];
}

/* ------------------------------------------------------------------ sampling */

static void crop_to_aspect(int fw, int fh, int tw, int th, int *x0, int *y0, int *cw, int *ch)
{
    /* dithercam.py _crop_to_aspect: round() the cropped size, centre it. */
    long lhs = (long)fw * th, rhs = (long)fh * tw;
    if (lhs > rhs) {
        *ch = fh;
        *cw = (int)(((long)fh * tw * 2 + th) / (2L * th));
    } else if (lhs < rhs) {
        *cw = fw;
        *ch = (int)(((long)fw * th * 2 + tw) / (2L * tw));
    } else {
        *cw = fw;
        *ch = fh;
    }
    *x0 = (fw - *cw) / 2;
    *y0 = (fh - *ch) / 2;
}

static inline void read_rgb(const gbcam_frame_t *f, const uint8_t *row, int x, uint8_t out[3])
{
    switch (f->format) {
    case GBCAM_FMT_GREY8:
        out[0] = out[1] = out[2] = row[x];
        break;
    case GBCAM_FMT_RGB888:
        memcpy(out, row + x * 3, 3);
        break;
    default: {
        const uint8_t *p = row + x * 2;
        uint16_t v = (f->format == GBCAM_FMT_RGB565_LE) ? (uint16_t)(p[0] | (p[1] << 8))
                                                        : (uint16_t)((p[0] << 8) | p[1]);
        int r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
        out[0] = (uint8_t)((r << 3) | (r >> 2));
        out[1] = (uint8_t)((g << 2) | (g >> 4));
        out[2] = (uint8_t)((b << 3) | (b >> 2));
    }
    }
}

static int frame_stride(const gbcam_frame_t *f)
{
    if (f->stride) return f->stride;
    return f->width * (f->format == GBCAM_FMT_GREY8 ? 1 : f->format == GBCAM_FMT_RGB888 ? 3 : 2);
}

static dc_hw_resample_fn s_hw_resample;
void dc_set_hw_resample(dc_hw_resample_fn fn) { s_hw_resample = fn; }

void dc_sample(const gbcam_frame_t *f, uint8_t *rgb, int w, int h)
{
    int x0, y0, cw, ch;
    crop_to_aspect(f->width, f->height, w, h, &x0, &y0, &cw, &ch);
    if (s_hw_resample && s_hw_resample(f, x0, y0, cw, ch, rgb, w, h)) return;
    const int stride = frame_stride(f);
    for (int y = 0; y < h; y++) {
        /* PIL NEAREST: source pixel at floor((i + 0.5) * src / dst). */
        int sy = y0 + (int)(((long)(2 * y + 1) * ch) / (2L * h));
        int oy = f->mirror_y ? h - 1 - y : y;
        const uint8_t *row = (const uint8_t *)f->data + (size_t)sy * stride;
        for (int x = 0; x < w; x++) {
            int sx = x0 + (int)(((long)(2 * x + 1) * cw) / (2L * w));
            int ox = f->mirror_x ? w - 1 - x : x;
            read_rgb(f, row, sx, rgb + ((size_t)oy * w + ox) * 3);
        }
    }
}

void dc_sample_smooth(const gbcam_frame_t *f, uint8_t *rgb, int w, int h)
{
    int x0, y0, cw, ch;
    crop_to_aspect(f->width, f->height, w, h, &x0, &y0, &cw, &ch);
    if (s_hw_resample && s_hw_resample(f, x0, y0, cw, ch, rgb, w, h)) return;
    const int stride = frame_stride(f);
    for (int y = 0; y < h; y++) {
        int ya = y0 + y * ch / h, yb = y0 + (y + 1) * ch / h;
        if (yb <= ya) yb = ya + 1;
        int ny = yb - ya > 3 ? 3 : yb - ya;   /* up to 3x3 samples: plenty for a preview */
        int oy = f->mirror_y ? h - 1 - y : y;
        for (int x = 0; x < w; x++) {
            int xa = x0 + x * cw / w, xb = x0 + (x + 1) * cw / w;
            if (xb <= xa) xb = xa + 1;
            int nx = xb - xa > 3 ? 3 : xb - xa;
            unsigned sum[3] = {0, 0, 0};
            for (int j = 0; j < ny; j++) {
                const uint8_t *row = (const uint8_t *)f->data +
                                     (size_t)(ya + (j * (yb - ya) + (yb - ya) / 2) / ny) * stride;
                for (int i = 0; i < nx; i++) {
                    uint8_t c[3];
                    read_rgb(f, row, xa + (i * (xb - xa) + (xb - xa) / 2) / nx, c);
                    sum[0] += c[0]; sum[1] += c[1]; sum[2] += c[2];
                }
            }
            int ox = f->mirror_x ? w - 1 - x : x;
            uint8_t *o = rgb + ((size_t)oy * w + ox) * 3;
            unsigned n = (unsigned)(nx * ny);
            o[0] = (uint8_t)(sum[0] / n); o[1] = (uint8_t)(sum[1] / n); o[2] = (uint8_t)(sum[2] / n);
        }
    }
}

/* See dc_temporal_denoise()'s comment in dithercam.h. Single persistent
 * accumulator (one live Dither Cam preview at a time), Q8 fixed point like
 * gbcam's own luma_acc.
 *
 * Blend weight ramps continuously with the delta's size instead of a hard
 * snap-if-bigger-than-threshold cutoff: base (1/(1<<strength)) at delta = 0,
 * rising to a full 1:1 (this frame's value, no smoothing at all) at
 * DC_DENOISE_MOTION_LEVELS. A hard cutoff instead means any real change that
 * lands just under the threshold - exactly what a moving edge's blurred
 * boundary produces, a spread of delta magnitudes frame to frame - gets the
 * *full* noise-strength smoothing as if it were noise, which is what a ghost
 * trail behind fast movement actually is: several frames of "this pixel
 * hasn't caught up yet". The ramp means a bigger real change always gets
 * less smoothing, closing in on instant well before the old hard edge,
 * without giving up noise suppression right at delta = 0 where noise lives. */
#define DC_DENOISE_MOTION_LEVELS 24 /* same threshold gbcam's temporal_denoise() uses */
static uint16_t *s_dc_acc;
static int s_dc_acc_w, s_dc_acc_h;

void dc_temporal_denoise(uint8_t *rgb, int w, int h, int strength)
{
    if (strength < 1) strength = 1;
    if (strength > 3) strength = 3;

    if (w != s_dc_acc_w || h != s_dc_acc_h) {
        free(s_dc_acc);
        s_dc_acc = malloc((size_t)w * h * 3 * sizeof(uint16_t));
        s_dc_acc_w = w;
        s_dc_acc_h = h;
        if (s_dc_acc)
            for (int i = 0; i < w * h * 3; i++) s_dc_acc[i] = (uint16_t)(rgb[i] << 8);
        return; /* no history yet at this size - pass this frame through as-is */
    }
    if (!s_dc_acc) return; /* allocation failed earlier - degrade to no denoise, not a crash */

    const int thresh = DC_DENOISE_MOTION_LEVELS << 8;   /* Q8 */
    const int base = 256 >> strength;                    /* Q8 blend weight at delta = 0 */
    for (int i = 0; i < w * h * 3; i++) {
        int target = rgb[i] << 8;
        int acc = s_dc_acc[i];
        int d = target - acc;
        int ad = d < 0 ? -d : d;
        int weight = ad >= thresh ? 256 : base + (256 - base) * ad / thresh;
        acc += d * weight / 256;
        s_dc_acc[i] = (uint16_t)acc;
        rgb[i] = (uint8_t)((acc + 128) >> 8);
    }
}

/* -------------------------------------------------------------------- levels */

static float level(int v, float contrast, float gamma)
{
    double x = v / 255.0;
    x = (x - 0.5) * contrast + 0.5;
    x = x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
    return (float)(pow(x, gamma) * 255.0);
}

void dc_levels(uint8_t *rgb, int w, int h, float contrast, float gamma)
{
    if (contrast == 1.0f && gamma == 1.0f) return;
    uint8_t lut[256];
    for (int i = 0; i < 256; i++) {
        float v = level(i, contrast, gamma) + 0.5f;
        lut[i] = (uint8_t)(v > 255.0f ? 255.0f : v);
    }
    for (int i = 0; i < w * h * 3; i++) rgb[i] = lut[rgb[i]];
}

/* ------------------------------------------------------------------ quantize */

#include "dc_lut_data.h"

/* Same weighted distance and bucket-centre convention as tools/gen_dc_lut.py's
 * nearest_index()/bucket_center() - see that script's docstring. Fills a
 * caller-owned DC_LUT_SIZE buffer - a few milliseconds, only paid when an
 * extra palette's LUT isn't (yet, or any more) saved on the SD card - see
 * ensure_active_lut(). */
static void build_lut_into(uint8_t *lut, const uint8_t (*colors)[3], int count)
{
    const int span = 256 / (1 << DC_LUT_BITS);
    int idx = 0;
    for (int ri = 0; ri < (1 << DC_LUT_BITS); ri++) {
        int r = ri * span + span / 2;
        for (int gi = 0; gi < (1 << DC_LUT_BITS); gi++) {
            int g = gi * span + span / 2;
            for (int bi = 0; bi < (1 << DC_LUT_BITS); bi++, idx++) {
                int b = bi * span + span / 2;
                int best = 0, best_d = -1;
                for (int i = 0; i < count; i++) {
                    int dr = r - colors[i][0], dg = g - colors[i][1], db = b - colors[i][2];
                    int d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
                    if (best_d < 0 || d < best_d) { best = i; best_d = d; if (d == 0) break; }
                }
                lut[idx] = (uint8_t)best;
            }
        }
    }
}

/* Switches the single active extra-palette LUT slot to index e (a no-op if
 * it's already there). Tries the host's saved copy first (a file read, if
 * dc_set_extra_lut_io() registered one - typically far cheaper than
 * rebuilding); on a miss, builds it from scratch and hands it to the host to
 * save for next time. Either way, at most one extra palette's DC_LUT_SIZE
 * buffer is ever resident - switching to a different one frees this one
 * first, there's no per-palette accumulation over a session. */
static void ensure_active_lut(int e)
{
    if (e == s_active_extra_idx) return;
    free(s_active_lut);
    s_active_lut = NULL;
    s_active_extra_idx = e;

    uint8_t *buf = malloc(DC_LUT_SIZE);
    if (!buf) return;
    if (s_lut_load && s_lut_load(e, buf)) {
        s_active_lut = buf;
        return;
    }

    const uint8_t (*colors)[3];
    int count = s_extra_colors ? s_extra_colors(e, &colors) : 0;
    if (count <= 0) {
        free(buf);
        return;
    }
    build_lut_into(buf, colors, count);
    s_active_lut = buf;
    if (s_lut_save) s_lut_save(e, buf);
}

/* O(1) nearest-palette-colour lookup: dc_palette_lut[] is precomputed offline
 * (tools/gen_dc_lut.py) with the same weighted distance a linear scan would
 * use (2*dr^2 + 4*dg^2 + 3*db^2), at DC_LUT_BITS bits/channel. Replaces a
 * per-pixel O(n) scan over up to 64 palette colours - at 320x240 with a
 * 64-colour palette that scan alone missed the 15fps budget on the ESP32-P4;
 * this doesn't, regardless of palette size. Extra (SD-loaded)
 * palettes get the same treatment - see ensure_active_lut(). */
static inline int nearest(float r, float g, float b, int palette)
{
    int ri = (int)(r < 0 ? 0 : r > 255 ? 255 : r) >> (8 - DC_LUT_BITS);
    int gi = (int)(g < 0 ? 0 : g > 255 ? 255 : g) >> (8 - DC_LUT_BITS);
    int bi = (int)(b < 0 ? 0 : b > 255 ? 255 : b) >> (8 - DC_LUT_BITS);
    int idx = (ri << (DC_LUT_BITS * 2)) | (gi << DC_LUT_BITS) | bi;
    if (palette < DC_PALETTE_COUNT) return dc_palette_lut[palette][idx];

    int e = palette - DC_PALETTE_COUNT;
    if (e < 0 || e >= s_extra_count) return 0;
    ensure_active_lut(e);
    return s_active_lut ? s_active_lut[idx] : 0;
}

static inline void emit(int i, int idx, const uint8_t (*pal)[3], uint8_t *out_rgb, uint8_t *out_index)
{
    memcpy(out_rgb + (size_t)i * 3, pal[idx], 3);
    if (out_index) out_index[i] = (uint8_t)idx;
}

static inline void spread(float *px, float er, float eg, float eb, float k)
{
    px[0] += er * k;
    px[1] += eg * k;
    px[2] += eb * k;
}

void dc_quantize(const uint8_t *rgb, int w, int h, int palette, dc_method_t method, float amount,
                 float contrast, float gamma, uint8_t *out_rgb, uint8_t *out_index, float *work)
{
    const uint8_t (*pal)[3];
    dc_palette_colors(palette, &pal);  /* pal: RGB triples for emit(); nearest() uses the LUT instead of pal/count */

    float lut[256];
    for (int i = 0; i < 256; i++)
        lut[i] = (contrast == 1.0f && gamma == 1.0f) ? (float)i : level(i, contrast, gamma);

    float *buf = work;
    bool own = false;
    if (!buf) {
        buf = malloc((size_t)w * h * 3 * sizeof(float));
        own = true;
        if (!buf) return;
    }
    for (int i = 0; i < w * h * 3; i++) buf[i] = lut[rgb[i]];

    if (method == DC_METHOD_NONE || method == DC_METHOD_BAYER4 || method == DC_METHOD_BAYER8) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int i = y * w + x;
                float *p = buf + (size_t)i * 3;
                float r = p[0], g = p[1], b = p[2];
                if (method != DC_METHOD_NONE) {
                    float t = method == DC_METHOD_BAYER4 ? bayer4[y & 3][x & 3] / 16.0f - 0.5f
                                                         : bayer8[y & 7][x & 7] / 64.0f - 0.5f;
                    float off = t * (amount * 128.0f);
                    r += off; g += off; b += off;
                    r = r < 0 ? 0 : (r > 255 ? 255 : r);
                    g = g < 0 ? 0 : (g > 255 ? 255 : g);
                    b = b < 0 ? 0 : (b > 255 ? 255 : b);
                }
                emit(i, nearest(r, g, b, palette), pal, out_rgb, out_index);
            }
        }
    } else {
        for (int y = 0; y < h; y++) {
            float *row = buf + (size_t)y * w * 3;
            float *row1 = y + 1 < h ? row + (size_t)w * 3 : NULL;
            float *row2 = y + 2 < h ? row + (size_t)w * 6 : NULL;
            for (int x = 0; x < w; x++) {
                float *p = row + x * 3;
                int idx = nearest(p[0], p[1], p[2], palette);
                emit(y * w + x, idx, pal, out_rgb, out_index);
                float er = (p[0] - pal[idx][0]) * amount;
                float eg = (p[1] - pal[idx][1]) * amount;
                float eb = (p[2] - pal[idx][2]) * amount;
                if (method == DC_METHOD_FLOYD_STEINBERG) {
                    if (x + 1 < w) spread(p + 3, er, eg, eb, 7.0f / 16);
                    if (row1) {
                        if (x > 0) spread(row1 + (x - 1) * 3, er, eg, eb, 3.0f / 16);
                        spread(row1 + x * 3, er, eg, eb, 5.0f / 16);
                        if (x + 1 < w) spread(row1 + (x + 1) * 3, er, eg, eb, 1.0f / 16);
                    }
                } else if (method == DC_METHOD_ATKINSON) {
                    er /= 8; eg /= 8; eb /= 8;
                    if (x + 1 < w) spread(p + 3, er, eg, eb, 1.0f);
                    if (x + 2 < w) spread(p + 6, er, eg, eb, 1.0f);
                    if (row1) {
                        if (x > 0) spread(row1 + (x - 1) * 3, er, eg, eb, 1.0f);
                        spread(row1 + x * 3, er, eg, eb, 1.0f);
                        if (x + 1 < w) spread(row1 + (x + 1) * 3, er, eg, eb, 1.0f);
                    }
                    if (row2) spread(row2 + x * 3, er, eg, eb, 1.0f);
                } else { /* Sierra Lite: right 2/4, below-left 1/4, below 1/4 */
                    if (x + 1 < w) spread(p + 3, er, eg, eb, 2.0f / 4);
                    if (row1) {
                        if (x > 0) spread(row1 + (x - 1) * 3, er, eg, eb, 1.0f / 4);
                        spread(row1 + x * 3, er, eg, eb, 1.0f / 4);
                    }
                }
            }
        }
    }
    if (own) free(buf);
}
