/*
 * Flat C API over the gbcam core, built as gbcam.dll for the Python live viewer
 * (gbcam_live.py). Keeps Python away from the gbcam_t struct layout.
 */
#include <stdlib.h>

#include "gbcam.h"
#include "dithercam.h"

#ifdef _WIN32
#define API __declspec(dllexport)
#else
#define API __attribute__((visibility("default")))
#endif

API gbcam_t *gbc_create(void)
{
    gbcam_t *cam = calloc(1, sizeof(gbcam_t));
    if (cam) gbcam_init(cam, NULL);
    return cam;
}

API void gbc_destroy(gbcam_t *cam) { free(cam); }

API void gbc_set(gbcam_t *cam, int brightness, int contrast, int dither, int edge_mode, int edge_ratio)
{
    cam->settings.brightness = (uint8_t)brightness;
    cam->settings.contrast = (uint8_t)contrast;
    cam->settings.dither = (gbcam_dither_t)dither;
    cam->settings.edge_mode = (gbcam_edge_mode_t)edge_mode;
    cam->settings.edge_ratio = (uint8_t)edge_ratio;
    gbcam_update_matrix(cam);
}

/* 8-bit greyscale frame in, 128x112 shades (0 = white .. 3 = black) out. */
API const uint8_t *gbc_process_grey(gbcam_t *cam, const uint8_t *data, int width, int height, int stride, int mirror_x)
{
    const gbcam_frame_t f = {
        .data = data, .width = width, .height = height, .stride = stride,
        .format = GBCAM_FMT_GREY8, .mirror_x = mirror_x != 0,
    };
    gbcam_process(cam, &f);
    return cam->shades;
}

/* Fixed digital exposure (Q8, 256 = 1.0); 0 switches auto-exposure back on. */
API void gbc_set_manual_gain(gbcam_t *cam, int gain_q8)
{
    cam->settings.auto_exposure = gain_q8 == 0;
    cam->settings.manual_gain_q8 = (uint16_t)gain_q8;
}

/* Temporal noise reduction 0 (off) .. 3. */
API void gbc_set_denoise(gbcam_t *cam, int strength)
{
    cam->settings.denoise = (uint8_t)strength;
}

/* 0 = PIXEL CAM style, 1 = hardware style. */
API void gbc_set_style(gbcam_t *cam, int style)
{
    cam->settings.style = (gbcam_style_t)style;
}

API const char *gbc_style_name(int style) { return gbcam_style_name((gbcam_style_t)style); }

/* PIXEL CAM style levels: contrast/gamma <= 0 switches the automatic search back on. */
API void gbc_set_levels(gbcam_t *cam, double contrast, double gamma)
{
    cam->settings.auto_levels = contrast <= 0.0 || gamma <= 0.0;
    cam->settings.levels_contrast = (float)contrast;
    cam->settings.levels_gamma = (float)gamma;
}

API double gbc_levels_contrast(const gbcam_t *cam) { return cam->lv_contrast; }
API double gbc_levels_gamma(const gbcam_t *cam) { return cam->lv_gamma; }

/* Force the contrast table: 1 = high-light, 0 = low-light, -1 = automatic. */
API void gbc_force_table(gbcam_t *cam, int high)
{
    cam->settings.light_table = high < 0 ? GBCAM_TABLE_AUTO : high ? GBCAM_TABLE_HIGH_LIGHT : GBCAM_TABLE_LOW_LIGHT;
}

/* The 48 dither registers (A006-A035) for a dither pattern / table / contrast. */
API void gbc_matrix(int dither, int high, int contrast, uint8_t out[48])
{
    gbcam_build_matrix(out, (gbcam_dither_t)dither, high != 0, (uint8_t)contrast);
}

/* Dither Cam on an already-sized w x h RGB888 image. */
API void gbc_dc_quantize(const uint8_t *rgb, int w, int h, int palette, int method, double amount,
                         double contrast, double gamma, uint8_t *out_rgb)
{
    dc_quantize(rgb, w, h, palette, (dc_method_t)method, (float)amount, (float)contrast, (float)gamma,
                out_rgb, NULL, NULL);
}

API int gbc_dc_palette_count(void) { return DC_PALETTE_COUNT; }
API int gbc_palette_count(void) { return GBCAM_PALETTE_COUNT; }

API double gbc_gain(const gbcam_t *cam) { return cam->gain_q8 / 256.0; }
API int gbc_tier(const gbcam_t *cam) { return cam->tier; }
API int gbc_ae_error(const gbcam_t *cam) { return cam->last_ae_error; }

API const char *gbc_dither_name(int d) { return gbcam_dither_name((gbcam_dither_t)d); }
API const char *gbc_palette_name(int p) { return gbcam_palette_name((gbcam_palette_t)p); }
API int gbc_edge_ratio_pct(int idx) { return gbcam_edge_ratio_pct((uint8_t)idx); }

/* RGB888 of a palette shade, packed 0xRRGGBB. */
API int gbc_palette_rgb(int p, int shade)
{
    const uint8_t *c = gbcam_palette_rgb((gbcam_palette_t)p, (uint8_t)shade);
    return (c[0] << 16) | (c[1] << 8) | c[2];
}
