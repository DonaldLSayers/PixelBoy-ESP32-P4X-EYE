#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Registers the ESP32-P4 PPA (Pixel Processing Accelerator) as dithercam's
 * hardware resample hook - see camera_ppa_esp.c's file comment. Call once,
 * after camera_init(). Non-fatal on failure (falls back to the existing CPU
 * resample path, just slower) - check the return value only if you want to
 * know which path is active, not to gate startup on it. */
esp_err_t camera_ppa_init(void);

/* Scales a plain RGB888 source (sw x sh, e.g. the GB emulator's own rendered
 * frame - not a raw camera frame, see camera_ppa_resample() for that path)
 * into a big-endian RGB565 destination canvas (dw x dh, e.g. the display
 * framebuffer), aspect-preserved and centred like app_display.c's own
 * fit_rect()/area_fit() - same hardware DMA path as camera_ppa_resample(),
 * just RGB888->RGB565 instead of RGB888/565->RGB888. Returns false (nothing
 * written) if PPA isn't available/failed - caller falls back to its own CPU
 * path. Only the actual scaled block is written; any letterbox border is
 * left untouched, so the caller must have already cleared it. */
bool camera_ppa_scale_to_rgb565(const uint8_t *rgb888, int sw, int sh,
                                uint16_t *out_fb, int dw, int dh);
