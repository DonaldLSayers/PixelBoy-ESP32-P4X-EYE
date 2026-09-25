#include "app_settings.h"
#include "dithercam.h"

void settings_defaults(app_settings_t *s)
{
    s->brightness = GBCAM_DEFAULT_BRIGHTNESS;
    s->contrast = GBCAM_DEFAULT_CONTRAST;
    s->palette = GBCAM_PALETTE_DEFAULT;
    s->dither = GBCAM_DITHER_DEFAULT;
    s->style = GBCAM_STYLE_PIXELCAM;
    s->denoise = 2;
    s->cam_mode = CAM_MODE_GB;
    s->dc_palette = DC_PALETTE_DEFAULT;
    s->dc_method = DC_METHOD_DEFAULT;
    s->dc_size = DC_SIZE_DEFAULT;
    s->dc_amount = DC_AMOUNT_DEFAULT;
    s->vf_scale = 0;
    s->normal_size = NORMAL_SIZE_DEFAULT;
}

bool settings_valid(const app_settings_t *s)
{
    return s->brightness < GBCAM_BRIGHTNESS_LEVELS && s->contrast < GBCAM_CONTRAST_LEVELS &&
           s->palette < GBCAM_PALETTE_COUNT && s->dither < GBCAM_DITHER_COUNT &&
           s->style < GBCAM_STYLE_COUNT && s->denoise <= 3 && s->cam_mode < CAM_MODE_COUNT &&
           s->dc_palette < DC_PALETTE_COUNT && s->dc_method < DC_METHOD_COUNT &&
           s->dc_size < DC_SIZE_COUNT && s->dc_amount >= 0.0f && s->dc_amount <= 1.0f &&
           s->vf_scale <= 1 && s->normal_size < NORMAL_SIZE_COUNT;
}
