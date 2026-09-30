#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Set everything up (display, input, storage, camera). */
esp_err_t app_init(void);

/* One iteration of the main loop: handle input, show one frame. */
void app_step(void);

/* The idle policy, in the two pieces a loop that isn't app_step() needs.
 * Shared with app_gbemu.c, which runs its own input/loop and so never reaches
 * app_step() while a ROM is loaded - without these, a play session would hold
 * the screen fully lit and keep emulating forever, untouched.
 *
 * idle_us is the caller's own "time since last input", not a shared clock:
 * there is no one clock behind both loops.
 *
 * app_backlight_percent() is the duty (%) for that much idle, dimmed to match
 * the viewfinder's own idle-dim. Never returns 0 - switching the screen fully
 * off is a standby decision each caller makes for itself, because standby means
 * different things in each loop (app_step() also stops the camera, the emulator
 * must not - see run_rom()'s standby block).
 *
 * app_standby_due() says whether that much idle has crossed the user's own
 * standby timeout (ROW_STANDBY; false when it's set to NEVER). */
int app_backlight_percent(int64_t idle_us);
bool app_standby_due(int64_t idle_us);
