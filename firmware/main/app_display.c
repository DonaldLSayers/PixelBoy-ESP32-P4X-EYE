/* Frame composition: image, text, on-screen messages. Platform-neutral;
 * the pixels go out through display_hw (LCD on the board, window in the simulator). */
#include <string.h>

#include "app_display.h"
#include "display_hw.h"
#include "font8x8.h"

static uint16_t *s_fb;

/* The panel expects big-endian RGB565 (BSP_LCD_BIGENDIAN). */
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((v >> 8) | (v << 8));
}

esp_err_t display_init(void)
{
    return display_hw_init();
}

void display_begin_blank(uint8_t r, uint8_t g, uint8_t b)
{
    s_fb = display_hw_acquire();
    uint16_t c = rgb565(r, g, b);
    for (int i = 0; i < DISP_W * DISP_H; i++)
        s_fb[i] = c;
}

void display_begin_frame(const uint8_t *shades, gbcam_palette_t palette)
{
    s_fb = display_hw_acquire();
    uint16_t *fb = s_fb;
    uint16_t lut[4];
    for (int i = 0; i < 4; i++) {
        const uint8_t *c = gbcam_palette_rgb(palette, (uint8_t)i);
        lut[i] = rgb565(c[0], c[1], c[2]);
    }

    /* Bars in the darkest palette colour. */
    for (int i = 0; i < DISP_W * DISP_IMG_Y; i++) fb[i] = lut[3];
    for (int i = (DISP_IMG_Y + 224) * DISP_W; i < DISP_W * DISP_H; i++) fb[i] = lut[3];

    /* 2x image, source columns 4..123 (crop 4 source px = 8 screen px each side). */
    for (int sy = 0; sy < GBCAM_H; sy++) {
        const uint8_t *src = shades + sy * GBCAM_W + 4;
        uint16_t *d0 = fb + (DISP_IMG_Y + sy * 2) * DISP_W;
        uint16_t *d1 = d0 + DISP_W;
        for (int sx = 0; sx < DISP_W / 2; sx++) {
            uint16_t c = lut[src[sx] & 3];
            d0[sx * 2] = d0[sx * 2 + 1] = c;
            d1[sx * 2] = d1[sx * 2 + 1] = c;
        }
    }
}

/* ------------------------------------------------ viewfinder scroll bars */

/* 7x7 round end cap and 5x5 ball knob, drawn at 2x. '#' = shade 0 (lightest),
 * 'o' = shade 1, 'x' = shade 3 (darkest, the +/- symbol), '.' = transparent. */
static const char *const cap_plus[7] = {
    "..###..",
    ".#####.",
    "###x###",
    "##xxx##",
    "###x###",
    ".#####.",
    "..###..",
};
static const char *const cap_minus[7] = {
    "..###..",
    ".#####.",
    "#######",
    "##xxx##",
    "#######",
    ".#####.",
    "..###..",
};
static const char *const knob[5] = {
    ".###.",
    "##o##",
    "###o#",
    "#####",
    ".###.",
};

static void fill_rect565(int x, int y, int w, int h, uint16_t c)
{
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < DISP_H; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < DISP_W; xx++)
            s_fb[yy * DISP_W + xx] = c;
}

static void draw_glyph2x(const char *const *rows, int n, int x, int y, const uint16_t lut[4])
{
    for (int r = 0; r < n; r++)
        for (int c = 0; rows[r][c]; c++) {
            char ch = rows[r][c];
            if (ch == '.') continue;
            uint16_t col = ch == '#' ? lut[0] : ch == 'o' ? lut[1] : lut[3];
            fill_rect565(x + c * 2, y + r * 2, 2, 2, col);
        }
}

void display_begin_viewfinder(const uint8_t *shades, gbcam_palette_t palette,
                              int brightness, int brightness_max,
                              int contrast, int contrast_max, int adjust,
                              bool native1x)
{
    bool brightness_active = adjust == 0, contrast_active = adjust == 1;
    s_fb = display_hw_acquire();
    uint16_t lut[4];
    for (int i = 0; i < 4; i++) {
        const uint8_t *c = gbcam_palette_rgb(palette, (uint8_t)i);
        lut[i] = rgb565(c[0], c[1], c[2]);
    }

    /* Dark border (left column + bottom row). */
    fill_rect565(0, 0, VF_IMG_X, DISP_H, lut[3]);
    fill_rect565(VF_IMG_X, VF_IMG_H, DISP_W - VF_IMG_X, DISP_H - VF_IMG_H, lut[3]);

    if (native1x) {
        /* Full 128x112 sensor image at 1x, centred in the same image area. */
        const int ox = VF_IMG_X + (VF_IMG_W - GBCAM_W) / 2, oy = (VF_IMG_H - GBCAM_H) / 2;
        fill_rect565(VF_IMG_X, 0, VF_IMG_W, VF_IMG_H, lut[3]);
        for (int sy = 0; sy < GBCAM_H; sy++) {
            const uint8_t *src = shades + sy * GBCAM_W;
            uint16_t *d = s_fb + (oy + sy) * DISP_W + ox;
            for (int sx = 0; sx < GBCAM_W; sx++) d[sx] = lut[src[sx] & 3];
        }
    } else {
        /* Photo at 2x: source columns 8..119, all 112 rows. */
        const int src_x0 = (GBCAM_W - VF_IMG_W / 2) / 2;
        for (int sy = 0; sy < GBCAM_H; sy++) {
            const uint8_t *src = shades + sy * GBCAM_W + src_x0;
            uint16_t *d0 = s_fb + (sy * 2) * DISP_W + VF_IMG_X;
            uint16_t *d1 = d0 + DISP_W;
            for (int sx = 0; sx < VF_IMG_W / 2; sx++) {
                uint16_t c = lut[src[sx] & 3];
                d0[sx * 2] = d0[sx * 2 + 1] = c;
                d1[sx * 2] = d1[sx * 2 + 1] = c;
            }
        }
    }

    /* Tracks: the active one light, the other one mid-grey. */
    uint16_t on = lut[0], off = lut[2];

    /* Brightness: vertical, + at the top (brighter), - at the bottom. */
    const int bx = 1, by_top = 1, by_bot = VF_IMG_H - 15;
    fill_rect565(7, by_top + 14, 2, by_bot - by_top - 14, brightness_active ? on : off);
    draw_glyph2x(cap_plus, 7, bx, by_top, lut);
    draw_glyph2x(cap_minus, 7, bx, by_bot, lut);
    int b_travel = (by_bot - 10) - (by_top + 14);
    int b = brightness < 0 ? 0 : brightness > brightness_max ? brightness_max : brightness;
    int ky = (by_top + 14) + (brightness_max ? b_travel * (brightness_max - b) / brightness_max : 0);
    draw_glyph2x(knob, 5, 3, ky, lut);

    /* Contrast: horizontal, - on the left, + on the right. */
    const int cy = VF_IMG_H + 1, cx_left = VF_IMG_X + 1, cx_right = DISP_W - 15;
    fill_rect565(cx_left + 14, cy + 6, cx_right - cx_left - 14, 2, contrast_active ? on : off);
    draw_glyph2x(cap_minus, 7, cx_left, cy, lut);
    draw_glyph2x(cap_plus, 7, cx_right, cy, lut);
    int c_travel = (cx_right - 10) - (cx_left + 14);
    int c = contrast < 0 ? 0 : contrast > contrast_max ? contrast_max : contrast;
    int kx = (cx_left + 14) + (contrast_max ? c_travel * c / contrast_max : 0);
    draw_glyph2x(knob, 5, kx, cy + 2, lut);

    /* Corner: which of the three the encoder adjusts. The bottom-left corner
     * (both borders overlap there) is exactly VF_IMG_X x (DISP_H-VF_IMG_H) =
     * 16x16, and the glyph at scale 2 is exactly FONT_W*2 x FONT_H*2 = 16x16
     * too, so (0, VF_IMG_H) fills it with no offset to tune by eye. Nudged
     * 1px up/right from there: 'B'/'C'/'P' all have blank rightmost columns
     * in this font (see font8x8.h), so their actual ink sits within the left
     * ~12px of the cell already - shifting right doesn't approach the image
     * edge, and the left border column runs the full screen height, not just
     * this corner, so shifting up stays on the dark background too. */
    const char *which = brightness_active ? "B" : contrast_active ? "C" : "P";
    const uint8_t *fg = gbcam_palette_rgb(palette, 0);
    display_text(1, VF_IMG_H - 1, 2, which, fg[0], fg[1], fg[2]);
}

void display_begin_camera(const uint8_t *rgb888, int w, int h)
{
    s_fb = display_hw_acquire();
    for (int i = 0; i < DISP_W * DISP_H; i++) s_fb[i] = 0;

    if (w <= DISP_W && h <= DISP_H) {
        /* Fits: crisp nearest-neighbour upscale by the largest integer factor
         * that still fits both ways, so pixel art keeps sharp square edges.
         * Letterboxed on whichever axis (or both) has px left over.
         *
         * Presets 128px wide (128x64, 128x96) can't reach even a 2x integer
         * scale (256 > DISP_W) - they'd sit tiny in the middle of the screen
         * at a bare 1x. A fractional scale would fill the screen instead, but
         * fractional nearest-neighbour resampling has no lowpass at all, and
         * ANY dithered source (not just Bayer's strictly periodic pattern -
         * error diffusion's less regular texture aliases too) breaks up under
         * it. Cropping slightly to let the NEXT integer scale fit instead
         * avoids that entirely - it's still pure pixel-doubling, alias-free
         * regardless of content, just of a centred, slightly smaller crop
         * (128x64 -> centred 120x60 crop, x2 = 240x120; 128x96 -> centred
         * 120x90 crop, x2 = 240x180). Only taken when the crop needed to
         * reach that scale is small (kept under CROP_TOLERANCE); otherwise
         * falls through to the plain small integer scale below. The saved
         * photo is unaffected either way - this only changes the preview. */
        int scale = 1;
        while ((scale + 1) * w <= DISP_W && (scale + 1) * h <= DISP_H) scale++;

        const float CROP_TOLERANCE = 0.15f;  /* don't crop away more than 15% of either axis */
        int scale2 = scale + 1;
        int max_cw = DISP_W / scale2, max_ch = DISP_H / scale2;
        int cw = max_cw, ch = (int)((long)max_cw * h / w);
        if (ch > max_ch) { ch = max_ch; cw = (int)((long)max_ch * w / h); }
        bool crop_ok = cw >= (int)(w * (1.0f - CROP_TOLERANCE) + 0.5f) &&
                      ch >= (int)(h * (1.0f - CROP_TOLERANCE) + 0.5f);

        if (crop_ok) {
            scale = scale2;
            int cx0 = (w - cw) / 2, cy0 = (h - ch) / 2;
            int ow = cw * scale, oh = ch * scale;
            int ox = (DISP_W - ow) / 2, oy = (DISP_H - oh) / 2;
            for (int sy = 0; sy < ch; sy++) {
                const uint8_t *src = rgb888 + (size_t)(cy0 + sy) * w * 3 + (size_t)cx0 * 3;
                uint16_t row[DISP_W];
                int n = 0;
                for (int sx = 0; sx < cw; sx++) {
                    uint16_t c = rgb565(src[sx * 3], src[sx * 3 + 1], src[sx * 3 + 2]);
                    for (int k = 0; k < scale; k++) row[n++] = c;
                }
                for (int k = 0; k < scale; k++)
                    memcpy(s_fb + (size_t)(oy + sy * scale + k) * DISP_W + ox, row, (size_t)n * sizeof(uint16_t));
            }
            return;
        }

        int ow = w * scale, oh = h * scale;
        int ox = (DISP_W - ow) / 2, oy = (DISP_H - oh) / 2;

        for (int sy = 0; sy < h; sy++) {
            const uint8_t *src = rgb888 + (size_t)sy * w * 3;
            uint16_t row[DISP_W]; /* ow <= DISP_W is guaranteed here */
            int n = 0;
            for (int sx = 0; sx < w; sx++) {
                uint16_t c = rgb565(src[sx * 3], src[sx * 3 + 1], src[sx * 3 + 2]);
                for (int k = 0; k < scale; k++) row[n++] = c;
            }
            for (int k = 0; k < scale; k++)
                memcpy(s_fb + (size_t)(oy + sy * scale + k) * DISP_W + ox, row, (size_t)n * sizeof(uint16_t));
        }
    } else {
        /* Too big for the screen (Dither Cam's 256x192, 320x240, 256x128
         * presets): crop to a clean integer decimation instead of a
         * fractional box-average "representation" - the same fix as the
         * upscale crop above, mirrored. A fractional ratio (320x240 -> 240x180
         * is 4:3, not a whole divisor) samples the source at a cell size that
         * shifts phase against a dithered pattern's own period as it scans
         * across the image, beating into moire/banding; an exact NxN average
         * has no shifting phase (every block is identically aligned), so it's
         * alias-free regardless of dither method - no more need to know or
         * guess what produced the pixels. Prefers N=1 (a plain crop, no
         * blending at all) when the source is only a little over the screen
         * size (256x192, 256x128 both crop to 240 wide at full sharpness);
         * only steps up to N=2 when a crop alone would cut too much
         * (320x240, where a straight crop to 240 wide would lose 25% of the
         * frame - instead it divides evenly by 2 with no crop needed at all).
         * The saved photo is unaffected either way - this only changes the
         * preview. */
        const float CROP_TOLERANCE = 0.15f;
        int cw = w, ch = h, N = 1;
        for (; N <= 8; N++) {
            int max_cw = DISP_W * N, max_ch = DISP_H * N;
            cw = w < max_cw ? w : max_cw;
            ch = (int)((long)cw * h / w);
            if (ch > max_ch) { ch = h < max_ch ? h : max_ch; cw = (int)((long)ch * w / h); }
            bool crop_ok = cw >= (int)(w * (1.0f - CROP_TOLERANCE) + 0.5f) &&
                          ch >= (int)(h * (1.0f - CROP_TOLERANCE) + 0.5f);
            if (crop_ok && cw / N <= DISP_W && ch / N <= DISP_H) break;
        }

        int ow = cw / N, oh = ch / N;
        if (ow < 1) ow = 1;
        if (oh < 1) oh = 1;
        int cx0 = (w - cw) / 2, cy0 = (h - ch) / 2;
        int ox = (DISP_W - ow) / 2, oy = (DISP_H - oh) / 2;

        for (int dy = 0; dy < oh; dy++) {
            int ya = cy0 + dy * N, yb = ya + N;
            for (int dx = 0; dx < ow; dx++) {
                int xa = cx0 + dx * N, xb = xa + N;
                unsigned sum[3] = {0, 0, 0};
                for (int sy = ya; sy < yb; sy++) {
                    const uint8_t *src = rgb888 + (size_t)sy * w * 3;
                    for (int sx = xa; sx < xb; sx++) {
                        sum[0] += src[sx * 3];
                        sum[1] += src[sx * 3 + 1];
                        sum[2] += src[sx * 3 + 2];
                    }
                }
                unsigned n = (unsigned)(N * N);
                s_fb[(size_t)(oy + dy) * DISP_W + ox + dx] =
                    rgb565((uint8_t)(sum[0] / n), (uint8_t)(sum[1] / n), (uint8_t)(sum[2] / n));
            }
        }
    }
}

void display_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = rgb565(r, g, b);
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < DISP_H; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < DISP_W; xx++)
            s_fb[yy * DISP_W + xx] = c;
}

#define FONT_W 8
#define FONT_H 8
#define FONT_ADVANCE (FONT_W + 1)  /* 1px gap between glyphs */

int display_text_width(const char *s, int scale)
{
    return (int)strlen(s) * FONT_ADVANCE * scale - scale;
}

void display_text(int x, int y, int scale, const char *s, uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = rgb565(r, g, b);
    for (; *s; s++, x += FONT_ADVANCE * scale) {
        int ch = (unsigned char)*s;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < FONT8X8_FIRST || ch > FONT8X8_LAST) ch = '?';
        const uint8_t *glyph = font8x8[ch - FONT8X8_FIRST];
        for (int col = 0; col < FONT_W; col++) {
            for (int row = 0; row < FONT_H; row++) {
                if (!(glyph[col] & (1 << row))) continue;
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++) {
                        int px = x + col * scale + dx, py = y + row * scale + dy;
                        if (px >= 0 && px < DISP_W && py >= 0 && py < DISP_H)
                            s_fb[py * DISP_W + px] = c;
                    }
            }
        }
    }
}

#define OSD_SCALE 2
#define OSD_W_MIN 200  /* keeps short messages ("SAVED", mode names) at the old box size */
#define OSD_H 64
#define OSD_Y (DISP_IMG_Y + (224 - OSD_H) / 2)

#define MENU_MARGIN 2  /* white border sits this far in from the screen edge */

void display_menu(const char *title, const char *const *labels, const char *const *values,
                  int count, int selected)
{
    /* Full screen, not a floating box over the viewfinder - the camera is
     * paused while a menu is up (see app_camera.h's camera_pause()), so
     * there's nothing live behind it to leave visible anyway. */
    const int x = MENU_MARGIN, y = MENU_MARGIN;
    const int w = DISP_W - 2 * MENU_MARGIN, h = DISP_H - 2 * MENU_MARGIN;
    display_rect(0, 0, DISP_W, DISP_H, 255, 255, 255);
    display_rect(x, y, w, h, 0, 0, 0);
    display_text(x + w / 2 - display_text_width(title, 2) / 2, y + 7, 2, title, 255, 255, 255);
    display_rect(x + 6, y + 26, w - 12, 1, 255, 255, 255);

    int rows_top = y + 30, rows_h = h - 30 - 6;
    int row_h = count > 0 ? rows_h / count : rows_h;
    for (int i = 0; i < count; i++) {
        int ry = rows_top + i * row_h;
        bool sel = i == selected;
        uint8_t fg = sel ? 0 : 255;
        if (sel) display_rect(x + 3, ry, w - 6, row_h - 2, 255, 255, 255);
        /* Scale 1 (native 8px), not 2: at this font's wider advance, a
         * label+value pair at 2x doesn't fit the row without overlapping. */
        int text_y = ry + (row_h - FONT_H) / 2;
        display_text(x + 8, text_y, 1, labels[i], fg, fg, fg);
        if (values && values[i])
            display_text(x + w - 8 - display_text_width(values[i], 1), text_y, 1, values[i], fg, fg, fg);
    }
}

void display_osd(const char *line1, const char *line2)
{
    /* One line: a shorter box, centred where the two-line box would be. */
    bool two = line2 && line2[0];
    int w1 = line1 ? display_text_width(line1, OSD_SCALE) : 0;
    int w2 = two ? display_text_width(line2, OSD_SCALE) : 0;
    /* Box width fits whichever line is wider - fixed-width boxes clipped
     * palette names near the 12-char label() truncation length in gen_
     * palettes.py (e.g. "RESURRECT 64"). Clamped to the screen so a
     * hypothetically longer string still can't run off it. */
    int w = (w1 > w2 ? w1 : w2) + 24;
    if (w < OSD_W_MIN) w = OSD_W_MIN;
    if (w > DISP_W - 8) w = DISP_W - 8;
    int x = (DISP_W - w) / 2;
    int h = two ? OSD_H : 32, y = OSD_Y + (OSD_H - h) / 2;
    display_rect(x - 2, y - 2, w + 4, h + 4, 255, 255, 255);
    display_rect(x, y, w, h, 0, 0, 0);
    if (line1)
        display_text(DISP_W / 2 - w1 / 2, y + 9, OSD_SCALE, line1, 255, 255, 255);
    if (two)
        display_text(DISP_W / 2 - w2 / 2, y + 36, OSD_SCALE, line2, 255, 255, 255);
}

void display_end_frame(void)
{
    display_hw_present(s_fb);
}
