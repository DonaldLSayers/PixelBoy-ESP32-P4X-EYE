#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gbcam.h"

typedef enum {
    CAM_MODE_GB,        /* Game Boy Camera look (PIXEL CAM / HARDWARE style) */
    CAM_MODE_DITHER,    /* Dither Cam: arbitrary palette + dither */
    CAM_MODE_NORMAL,    /* plain colour preview, no quantizing */
    CAM_MODE_COUNT
} cam_mode_t;

typedef struct {
    uint8_t brightness;
    uint8_t contrast;
    uint8_t palette;   /* gbcam_palette_t */
    uint8_t dither;    /* gbcam_dither_t */
    uint8_t style;     /* gbcam_style_t */
    uint8_t denoise;   /* 0..3 */
    uint8_t cam_mode;  /* cam_mode_t */
    uint8_t dc_palette;
    uint8_t dc_method; /* dc_method_t */
    uint8_t dc_size;
    float dc_amount;
    uint8_t vf_scale;    /* GB Camera viewfinder: 0 = 2x cropped (224x224, default), 1 = 1:1 (128x112) */
    uint8_t normal_size; /* Normal Cam resolution, see normal_size() in dithercam.h */
    uint8_t frame;       /* GB Camera border: 0 = none, 1..FRAME_COUNT = frame_id_t + 1, see frames.h */
} app_settings_t;

/* Shared by the board and simulator back ends (app_settings_common.c). */
void settings_defaults(app_settings_t *s);
bool settings_valid(const app_settings_t *s);

/* Loads saved settings, or defaults if none are stored. */
void settings_load(app_settings_t *s);

/* Marks settings changed; they are written to flash a couple of seconds later. */
void settings_changed(const app_settings_t *s);

/* Call regularly; writes pending changes once they have settled. */
void settings_tick(void);
