#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Set everything up (display, input, storage, camera). */
esp_err_t app_init(void);

/* One iteration of the main loop: handle input, show one frame. */
void app_step(void);

/* Backlight duty (%) for a caller that has been idle for idle_us, dimming to
 * match the idle-dim app_step() applies to the live viewfinder. Shared with
 * app_gbemu.c, which runs its own input/loop and so never reaches app_step()
 * while a ROM is loaded. Never returns 0 - switching the screen fully off is
 * a standby decision each caller makes for itself, since standby means
 * different things in each loop (app_step() also stops the camera).
 *
 * idle_us is the caller's own "time since last input", not a shared clock. */
int app_backlight_percent(int64_t idle_us);
