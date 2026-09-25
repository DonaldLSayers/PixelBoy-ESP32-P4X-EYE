/*
 * Dither Cam, ported from the PIXEL CAM project: pixelboy/dithercam.py
 * (quantize, _quantize_*) and xiao-pixelcam/src/dithercam.cpp.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dithercam.h"
#include "palettes_data.h"

/* pixelboy/dithercam.py's RESOLUTIONS (the first four 4:3, plus its 256x128
 * 2:1 widescreen extra), plus one more of our own: 128x64, a 2:1 crop at the
 * same width as the 128x96 default. Ordered by ascending pixel count, so
 * cycling the menu row is a steady small -> large sweep instead of jumping
 * around (the old order had 128x64 and 256x128 out of sequence). Indices
 * moved: keep DC_SIZE_DEFAULT in dithercam.h in sync with wherever 128x96
 * lands here. */
static const uint16_t dc_sizes[DC_SIZE_COUNT][2] = {
    {32, 24}, {64, 48}, {128, 64}, {128, 96}, {256, 128}, {256, 192}, {320, 240},
};

/* Normal Cam's "digicam" presets - see the NORMAL_SIZE_COUNT comment in
 * dithercam.h for why these are separate from dc_sizes[] and capped at 720p. */
static const uint16_t normal_sizes[NORMAL_SIZE_COUNT][2] = {
    {320, 240}, {640, 360}, {640, 480}, {854, 480}, {1280, 720},
};
static const char *const normal_size_labels[NORMAL_SIZE_COUNT] = {
    "320X240", "360P", "480P", "480P WIDE", "720P",
};

static const char *const method_names[DC_METHOD_COUNT] = {
    "NONE", "BAYER 4X4", "BAYER 8X8", "FLOYD-ST", "ATKINSON", "SIERRA LITE",
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

const char *dc_palette_name(int p)
{
    return (p >= 0 && p < DC_PALETTE_COUNT) ? dc_palette_names[p] : "?";
}

int dc_palette_colors(int p, const uint8_t (**colors)[3])
{
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

void dc_sample(const gbcam_frame_t *f, uint8_t *rgb, int w, int h)
{
    int x0, y0, cw, ch;
    crop_to_aspect(f->width, f->height, w, h, &x0, &y0, &cw, &ch);
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

/* O(1) nearest-palette-colour lookup: dc_palette_lut[] is precomputed offline
 * (tools/gen_dc_lut.py) with the same weighted distance a linear scan would
 * use (2*dr^2 + 4*dg^2 + 3*db^2), at DC_LUT_BITS bits/channel. Replaces a
 * per-pixel O(n) scan over up to 64 palette colours - at 320x240 with a
 * 64-colour palette that scan alone missed the 15fps budget on the ESP32-P4
 * (see PLAN.md); this doesn't, regardless of palette size. */
static inline int nearest(float r, float g, float b, int palette)
{
    int ri = (int)(r < 0 ? 0 : r > 255 ? 255 : r) >> (8 - DC_LUT_BITS);
    int gi = (int)(g < 0 ? 0 : g > 255 ? 255 : g) >> (8 - DC_LUT_BITS);
    int bi = (int)(b < 0 ? 0 : b > 255 ? 255 : b) >> (8 - DC_LUT_BITS);
    int idx = (ri << (DC_LUT_BITS * 2)) | (gi << DC_LUT_BITS) | bi;
    return dc_palette_lut[palette][idx];
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
