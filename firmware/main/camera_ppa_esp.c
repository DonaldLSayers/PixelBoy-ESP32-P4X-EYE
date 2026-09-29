/*
 * Hardware-accelerated crop+scale+format-convert for Dither Cam/Normal Cam's
 * live preview and capture, using the ESP32-P4's PPA (Pixel Processing
 * Accelerator) peripheral instead of the CPU touching every source pixel
 * itself - see dc_set_hw_resample()'s comment in dithercam.h for why: at
 * larger preview sizes (Normal Cam's 480p/720p presets) the CPU path spends
 * most of its time on essentially-random PSRAM reads, one per output pixel,
 * not on the (simple) resample arithmetic itself - PPA does the whole crop+
 * scale+convert as a single hardware DMA operation instead.
 *
 * Registered once at boot (see app.c's app_init()) via dc_set_hw_resample();
 * dithercam.c's dc_sample()/dc_sample_smooth() call back into camera_ppa_resample()
 * below for every frame, falling back to their own CPU loop if it returns
 * false (not yet initialized, or an unsupported source pixel format).
 */
#include <string.h>

#include "driver/ppa.h"
#include "esp_check.h"
#include "esp_private/esp_cache_private.h" /* esp_cache_get_alignment() - not in the public esp_cache.h */
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "camera_ppa_esp.h"
#include "dithercam.h"
#include "platform.h"

static const char *TAG = "camera_ppa";

static ppa_client_handle_t s_client;
static uint8_t *s_scratch;      /* PPA output lands here first - see camera_ppa_init()'s comment */
static size_t s_scratch_size;

static bool camera_ppa_resample(const gbcam_frame_t *f, int cx0, int cy0, int cw, int ch,
                                uint8_t *out_rgb, int out_w, int out_h);

esp_err_t camera_ppa_init(void)
{
    const ppa_client_config_t cfg = {.oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1};
    esp_err_t err = ppa_register_client(&cfg, &s_client);
    if (err != ESP_OK) {
        PLOGW(TAG, "no PPA client (%s) - falling back to CPU resample", esp_err_to_name(err));
        return err;
    }

    /* PPA's output buffer must be cache-line aligned for both the internal
     * and external (PSRAM) cache - s_dc_rgb/s_still_rgb (app.c) are plain
     * calloc()s with no such guarantee, and touched from many other places
     * that don't need it, so rather than change how those are allocated,
     * PPA writes here (a buffer sized and aligned just for this) and a
     * plain contiguous memcpy - fast, PSRAM bandwidth is fine for straight
     * linear access, unlike the scattered per-pixel reads this replaces -
     * lands the result wherever the caller actually wanted it. Sized for
     * the largest case (DC_MAX_W x DC_MAX_H RGB888, Normal Cam's 720p). */
    size_t alignment = 0;
    ESP_RETURN_ON_ERROR(esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &alignment), TAG, "cache align");
    s_scratch_size = (size_t)DC_MAX_W * DC_MAX_H * 3;
    s_scratch = heap_caps_aligned_alloc(alignment, s_scratch_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_scratch) {
        PLOGW(TAG, "no memory for PPA scratch buffer - falling back to CPU resample");
        ppa_unregister_client(s_client);
        s_client = NULL;
        return ESP_ERR_NO_MEM;
    }

    dc_set_hw_resample(camera_ppa_resample);
    PLOGI(TAG, "ready (cache alignment %u bytes)", (unsigned)alignment);
    return ESP_OK;
}

static bool camera_ppa_resample(const gbcam_frame_t *f, int cx0, int cy0, int cw, int ch,
                                uint8_t *out_rgb, int out_w, int out_h)
{
    if (!s_client) return false;

    ppa_srm_color_mode_t in_cm;
    bool byte_swap = false;
    switch (f->format) {
    case GBCAM_FMT_RGB565_LE: in_cm = PPA_SRM_COLOR_MODE_RGB565; byte_swap = false; break;
    case GBCAM_FMT_RGB565_BE: in_cm = PPA_SRM_COLOR_MODE_RGB565; byte_swap = true; break;
    case GBCAM_FMT_RGB888:    in_cm = PPA_SRM_COLOR_MODE_RGB888; break;
    default: return false; /* GREY8 (GB Camera path never calls dc_sample*, so this is unused there anyway) */
    }

    size_t need = (size_t)out_w * out_h * 3;
    if (need > s_scratch_size) return false; /* larger than any real preset - shouldn't happen, just don't overrun */

    const ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = f->data,
            .pic_w = (uint32_t)f->width,
            .pic_h = (uint32_t)f->height,
            .block_w = (uint32_t)cw,
            .block_h = (uint32_t)ch,
            .block_offset_x = (uint32_t)cx0,
            .block_offset_y = (uint32_t)cy0,
            .srm_cm = in_cm,
        },
        .out = {
            .buffer = s_scratch,
            .buffer_size = s_scratch_size,
            .pic_w = (uint32_t)out_w,
            .pic_h = (uint32_t)out_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = (float)out_w / (float)cw,
        .scale_y = (float)out_h / (float)ch,
        .mirror_x = f->mirror_x,
        .mirror_y = f->mirror_y,
        .byte_swap = byte_swap,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(s_client, &cfg) != ESP_OK) return false;

    memcpy(out_rgb, s_scratch, need);
    return true;
}

bool camera_ppa_scale_to_rgb565(const uint8_t *rgb888, int sw, int sh,
                                uint16_t *out_fb, int dw, int dh)
{
    if (!s_client) return false;

    int ow, oh;
    if ((int64_t)sw * dh > (int64_t)sh * dw) {
        ow = dw;
        oh = (int)((int64_t)sh * dw / sw);
    } else {
        oh = dh;
        ow = (int)((int64_t)sw * dh / sh);
    }
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
    int ox = (dw - ow) / 2, oy = (dh - oh) / 2;

    size_t need = (size_t)dw * dh * 2;
    if (need > s_scratch_size) return false; /* larger than any real preset - shouldn't happen */

    const ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = rgb888,
            .pic_w = (uint32_t)sw,
            .pic_h = (uint32_t)sh,
            .block_w = (uint32_t)sw,
            .block_h = (uint32_t)sh,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .out = {
            .buffer = s_scratch,
            .buffer_size = s_scratch_size,
            .pic_w = (uint32_t)dw,
            .pic_h = (uint32_t)dh,
            .block_offset_x = (uint32_t)ox,
            .block_offset_y = (uint32_t)oy,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = (float)ow / (float)sw,
        .scale_y = (float)oh / (float)sh,
        .mirror_x = false,
        .mirror_y = false,
        /* The panel wants big-endian RGB565 (see app_display.c's rgb565())
         * - PPA's native RGB565 output is little-endian, so this flag asks
         * it to swap on the way out instead of a separate CPU pass after. */
        .byte_swap = true,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(s_client, &cfg) != ESP_OK) return false;

    /* Only the block PPA actually touched - any letterbox border outside it
     * is left as whatever the caller already cleared it to. One contiguous
     * copy when the scaled image is full-width (every real caller so far -
     * the source is always wider-than-square, constraining on width),
     * otherwise per-row so a narrower block doesn't smear into its side
     * borders. */
    if (ow == dw) {
        memcpy((uint8_t *)out_fb + (size_t)oy * dw * 2,
               s_scratch + (size_t)oy * dw * 2,
               (size_t)oh * dw * 2);
    } else {
        for (int y = 0; y < oh; y++)
            memcpy((uint8_t *)out_fb + (size_t)(oy + y) * dw * 2 + (size_t)ox * 2,
                   s_scratch + (size_t)(oy + y) * dw * 2 + (size_t)ox * 2,
                   (size_t)ow * 2);
    }
    return true;
}
