#include "app_settings.h"
#include "app_frames_sd.h"
#include "app_palettes_sd.h"
#include "dithercam.h"
#include "frames.h"

void settings_defaults(app_settings_t *s)
{
    s->brightness = GBCAM_DEFAULT_BRIGHTNESS;
    s->contrast = GBCAM_DEFAULT_CONTRAST;
    s->palette = GBCAM_PALETTE_DEFAULT;
    s->dither = GBCAM_DITHER_DEFAULT;
    s->style = GBCAM_STYLE_HARDWARE;
    s->cam_mode = CAM_MODE_GB;
    s->dc_palette = DC_PALETTE_DEFAULT;
    s->dc_method = DC_METHOD_DEFAULT;
    s->dc_size = DC_SIZE_DEFAULT;
    s->dc_amount = DC_AMOUNT_DEFAULT;
    s->vf_scale = 0;
    s->normal_size = NORMAL_SIZE_DEFAULT;
    s->frame = 0;
    for (int i = 0; i < CAM_MODE_COUNT; i++) s->adjust[i] = 0;
    s->sleep_min = 3; /* index into SLEEP_MINUTES[] (app.c) - {0,1,2,3,5,10}[3] = 3 minutes */
    s->gb_auto = 1; /* matches gbcam_default_settings()'s own auto_levels=true - no behaviour change by default */
    s->dc_auto = 0; /* new, opt-in - PixelBoy never had auto-exposure before this */
    s->dc_edge = 0; /* new, opt-in */
    s->gb_aeb = 0; /* new, opt-in */
    s->backlight = BACKLIGHT_OPTIONS_COUNT - 1; /* last entry = 100% - matches the fixed
                                                 * full-on backlight every earlier version
                                                 * had, so existing units look unchanged */
    s->standby = 2; /* index into STANDBY_SECONDS[] (app.c) - {0,15,30,60,120,300}[2] = 30s, the
                     * fixed threshold this ran at before the setting existed */
}

bool settings_valid(const app_settings_t *s)
{
    /* palette/dc_palette bounds are upper-only (built-in count + the SD cap),
     * same reasoning as frame/MAX_SD_FRAMES below - the actual current count
     * isn't known yet this early (palettes_sd_init() runs after storage_init(),
     * which runs after settings_load()), and a stale value past the real
     * count is clamped again once it is (app.c's app_init()). */
    if (!(s->brightness < GBCAM_BRIGHTNESS_LEVELS && s->contrast < GBCAM_CONTRAST_LEVELS &&
          s->palette < GBCAM_PALETTE_COUNT + MAX_SD_PALETTES && s->dither < GBCAM_DITHER_COUNT &&
          s->style < GBCAM_STYLE_COUNT && s->cam_mode < CAM_MODE_COUNT &&
          s->dc_palette < DC_PALETTE_COUNT + MAX_SD_PALETTES && s->dc_method < DC_METHOD_COUNT &&
          s->dc_size < DC_SIZE_COUNT && s->dc_amount >= 0.0f && s->dc_amount <= 1.0f &&
          s->vf_scale <= 2 && s->normal_size < NORMAL_SIZE_COUNT &&
          s->frame <= FRAME_COUNT + MAX_SD_FRAMES && s->sleep_min < SLEEP_OPTIONS_COUNT &&
          s->gb_auto <= 1 && s->dc_auto <= 1 && s->dc_edge <= 1 && s->gb_aeb <= 6 &&
          s->backlight < BACKLIGHT_OPTIONS_COUNT && s->standby < STANDBY_OPTIONS_COUNT))
        return false;
    /* Upper bound only - GB has 4 quick-adjust targets, Dither has 6, Normal
     * has 3; the exact per-mode count is clamped again where it's read
     * (app.c's load_adjust()), so a value that's in range here but stale for
     * a mode with fewer targets just clamps to 0. */
    for (int i = 0; i < CAM_MODE_COUNT; i++)
        if (s->adjust[i] >= 6) return false;
    return true;
}
