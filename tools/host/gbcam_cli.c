/*
 * gbcam_cli - run the gbcam pipeline on photos on the PC, for tuning the look.
 *
 * Usage:
 *   gbcam_cli <input image | --test-pattern> [options]
 *
 * Options:
 *   -o <prefix>        output prefix (default: input name without extension)
 *   -b <0..16>         brightness (auto-exposure target), default 8
 *   -c <0..15>         contrast, default 8
 *   -d <name>          dither: off default 2x2 grid maze nest fuzz vertical horizontal mix
 *   -e <mode>          edge: auto none h 2d
 *   -r <0..7>          edge ratio index (50,75,100,125,200,300,400,500 %)
 *   -g <gain>          fixed digital gain (e.g. 1.5), disables auto-exposure
 *   -p <name>          palette, e.g. grayscale gb_green 2bit_demichrome (see PIXEL CAM palettes)
 *   -s <n>             upscale factor for the preview PNG, default 4
 *   -n <n>             auto-exposure iterations to converge, default 40
 *   --sweep contrast   4x4 contact sheet of all 16 contrast levels
 *   --sweep dither     contact sheet of all dither patterns
 *
 * Writes <prefix>_gb.png (128x112), <prefix>_xN.png (upscaled) and <prefix>_luma.png.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "gbcam.h"

static gbcam_t cam; /* large: keep off the stack */

static int parse_dither(const char *s)
{
    for (int i = 0; i < GBCAM_DITHER_COUNT; i++) {
        const char *n = gbcam_dither_name((gbcam_dither_t)i);
        int match = 1;
        for (int k = 0; n[k] || s[k]; k++) {
            char a = n[k], b = s[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) { match = 0; break; }
        }
        if (match) return i;
    }
    fprintf(stderr, "unknown dither '%s'\n", s);
    exit(2);
}

static int parse_palette(const char *s)
{
    for (int i = 0; i < GBCAM_PALETTE_COUNT; i++) {
        const char *n = gbcam_palette_name((gbcam_palette_t)i);
        int match = 1;
        for (int k = 0; n[k] || s[k]; k++) {
            char a = n[k], b = s[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (b == '_') b = ' ';
            if (a != b) { match = 0; break; }
        }
        if (match) return i;
    }
    fprintf(stderr, "unknown palette '%s'\n", s);
    exit(2);
}

static unsigned char *make_test_pattern(int *w, int *h)
{
    const int W = 640, H = 480;
    unsigned char *img = malloc((size_t)W * H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            double v = 255.0 * x / (W - 1);                        /* horizontal ramp */
            double dx = x - W * 0.35, dy = y - H * 0.5;
            if (dx * dx + dy * dy < 90.0 * 90.0) v = 255 - v;      /* inverted disc */
            if (y > H * 0.8) v = ((x / 16 + y / 16) & 1) ? 230 : 25; /* checker strip */
            if (x > W * 0.7 && y < H * 0.3) v = 128 + 100 * sin(x * 0.2); /* fine stripes */
            img[y * W + x] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
    *w = W;
    *h = H;
    return img;
}

static void run(const gbcam_frame_t *frame, const gbcam_settings_t *s, int iters)
{
    gbcam_init(&cam, s);
    gbcam_downsample(&cam, frame);
    for (int i = 0; i < iters; i++)
        gbcam_process_luma(&cam);
}

static void shades_to_rgb(const uint8_t *shades, gbcam_palette_t pal, int scale,
                          unsigned char *dst, int dst_w, int ox, int oy)
{
    for (int y = 0; y < GBCAM_H * scale; y++)
        for (int x = 0; x < GBCAM_W * scale; x++) {
            const uint8_t *c = gbcam_palette_rgb(pal, shades[(y / scale) * GBCAM_W + x / scale]);
            unsigned char *p = dst + ((size_t)(oy + y) * dst_w + ox + x) * 3;
            p[0] = c[0]; p[1] = c[1]; p[2] = c[2];
        }
}

static void usage(void)
{
    fprintf(stderr, "usage: gbcam_cli <image | --test-pattern> [-o prefix] [-b 0..16] [-c 0..15]\n"
                    "       [-d dither] [-e auto|none|h|2d] [-r 0..7] [-g gain] [-p palette]\n"
                    "       [-s scale] [-n iters] [--sweep contrast|dither]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    if (argc < 2) usage();

    gbcam_settings_t s;
    gbcam_default_settings(&s);
    const char *input = NULL, *prefix = NULL, *sweep = NULL;
    gbcam_palette_t pal = GBCAM_PALETTE_DEFAULT;
    int scale = 4, iters = 40;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--test-pattern")) { input = a; continue; }
        if (a[0] != '-') { input = a; continue; }
        if (!v) usage();
        if (!strcmp(a, "-o")) prefix = v;
        else if (!strcmp(a, "-b")) s.brightness = (uint8_t)atoi(v);
        else if (!strcmp(a, "-c")) s.contrast = (uint8_t)atoi(v);
        else if (!strcmp(a, "-d")) s.dither = (gbcam_dither_t)parse_dither(v);
        else if (!strcmp(a, "-e")) {
            s.edge_mode = !strcmp(v, "none") ? GBCAM_EDGE_NONE
                        : !strcmp(v, "h")    ? GBCAM_EDGE_HORIZONTAL
                        : !strcmp(v, "2d")   ? GBCAM_EDGE_2D : GBCAM_EDGE_AUTO;
        }
        else if (!strcmp(a, "-r")) s.edge_ratio = (uint8_t)atoi(v);
        else if (!strcmp(a, "-g")) { s.auto_exposure = false; s.manual_gain_q8 = (uint16_t)(atof(v) * 256.0 + 0.5); }
        else if (!strcmp(a, "-p")) pal = (gbcam_palette_t)parse_palette(v);
        else if (!strcmp(a, "-s")) scale = atoi(v);
        else if (!strcmp(a, "-n")) iters = atoi(v);
        else if (!strcmp(a, "--sweep")) sweep = v;
        else usage();
        i++;
    }
    if (!input) usage();
    if (scale < 1) scale = 1;

    int w, h, comp;
    unsigned char *img;
    if (!strcmp(input, "--test-pattern")) {
        img = make_test_pattern(&w, &h);
        if (!prefix) prefix = "test_pattern";
    } else {
        img = stbi_load(input, &w, &h, &comp, 1);
        if (!img) {
            fprintf(stderr, "cannot load %s: %s\n", input, stbi_failure_reason());
            return 1;
        }
    }

    char buf[1024];
    if (!prefix) {
        snprintf(buf, sizeof buf, "%s", input);
        char *dot = strrchr(buf, '.');
        if (dot) *dot = 0;
        prefix = buf;
    }

    gbcam_frame_t frame = {.data = img, .width = w, .height = h, .format = GBCAM_FMT_GREY8};
    char path[1100];

    if (sweep) {
        int is_contrast = !strcmp(sweep, "contrast");
        int count = is_contrast ? GBCAM_CONTRAST_LEVELS : GBCAM_DITHER_COUNT;
        int cols = 4, rows = (count + cols - 1) / cols, sc = 2, pad = 4;
        int cw = GBCAM_W * sc + pad, ch = GBCAM_H * sc + pad;
        int sw = cols * cw + pad, sh = rows * ch + pad;
        unsigned char *sheet = calloc((size_t)sw * sh * 3, 1);
        memset(sheet, 64, (size_t)sw * sh * 3);
        for (int k = 0; k < count; k++) {
            gbcam_settings_t t = s;
            if (is_contrast) t.contrast = (uint8_t)k; else t.dither = (gbcam_dither_t)k;
            run(&frame, &t, iters);
            shades_to_rgb(cam.shades, pal, sc, sheet, sw, pad + (k % cols) * cw, pad + (k / cols) * ch);
            printf("%s %2d: gain %.2f tier %d ae_err %d%s%s\n", sweep, k, cam.gain_q8 / 256.0, cam.tier,
                   cam.last_ae_error, is_contrast ? "" : " ", is_contrast ? "" : gbcam_dither_name((gbcam_dither_t)k));
        }
        snprintf(path, sizeof path, "%s_sweep_%s.png", prefix, sweep);
        stbi_write_png(path, sw, sh, 3, sheet, sw * 3);
        printf("wrote %s\n", path);
        free(sheet);
    } else {
        run(&frame, &s, iters);
        printf("gain %.2f  tier %d  high_light %d  ae_err %d  dither %s  contrast %d  brightness %d\n",
               cam.gain_q8 / 256.0, cam.tier, cam.high_light, cam.last_ae_error,
               gbcam_dither_name(s.dither), s.contrast, s.brightness);

        unsigned char *rgb = malloc((size_t)GBCAM_W * GBCAM_H * 3);
        shades_to_rgb(cam.shades, pal, 1, rgb, GBCAM_W, 0, 0);
        snprintf(path, sizeof path, "%s_gb.png", prefix);
        stbi_write_png(path, GBCAM_W, GBCAM_H, 3, rgb, GBCAM_W * 3);
        printf("wrote %s\n", path);
        free(rgb);

        rgb = malloc((size_t)GBCAM_W * scale * GBCAM_H * scale * 3);
        shades_to_rgb(cam.shades, pal, scale, rgb, GBCAM_W * scale, 0, 0);
        snprintf(path, sizeof path, "%s_x%d.png", prefix, scale);
        stbi_write_png(path, GBCAM_W * scale, GBCAM_H * scale, 3, rgb, GBCAM_W * scale * 3);
        printf("wrote %s\n", path);
        free(rgb);

        snprintf(path, sizeof path, "%s_luma.png", prefix);
        stbi_write_png(path, GBCAM_W, GBCAM_H, 1, cam.luma, GBCAM_W);
        printf("wrote %s\n", path);

        /* Round-trip check of the Game Boy tile encoding. */
        static uint8_t tiles[GBCAM_TILES_SIZE], back[GBCAM_PIXELS];
        gbcam_shades_to_tiles(cam.shades, tiles);
        gbcam_tiles_to_shades(tiles, back);
        if (memcmp(back, cam.shades, GBCAM_PIXELS) != 0) {
            fprintf(stderr, "tile round-trip FAILED\n");
            return 1;
        }
    }

    if (strcmp(input, "--test-pattern")) stbi_image_free(img); else free(img);
    return 0;
}
