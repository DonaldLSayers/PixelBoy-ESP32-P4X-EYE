#pragma once

#include "esp_err.h"

/* Registers the ESP32-P4 PPA (Pixel Processing Accelerator) as dithercam's
 * hardware resample hook - see camera_ppa_esp.c's file comment. Call once,
 * after camera_init(). Non-fatal on failure (falls back to the existing CPU
 * resample path, just slower) - check the return value only if you want to
 * know which path is active, not to gate startup on it. */
esp_err_t camera_ppa_init(void);
