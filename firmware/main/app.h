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
 * app_backlight_percent() is the duty (%) the user has set. It takes no idle
 * argument: there is no auto-dim any more (see app.c's backlight section), so
 * the level is the setting and nothing else. Never returns 0 - switching the
 * screen fully off is a standby decision each caller makes for itself, because
 * standby means different things in each loop (app_step() also stops the
 * camera, the emulator must not - see run_rom()'s standby block).
 *
 * app_standby_due() says whether that much idle has crossed the user's own
 * standby timeout (ROW_STANDBY; false when it's set to NEVER).
 *
 * app_sleep_due() is the other half of idle - the SLEEP timer (ROW_SLEEP),
 * which deep-sleeps the device rather than just darkening it. Unlike standby,
 * which each loop decides for itself, this one has to be a shared decision:
 * for the emulator it is not just a matter of stopping a loop but of tearing
 * the ROM down first (see run_rom()), so the caller passes the verdict on to
 * app_enter_sleep() only after it has finished with its own state. */
int app_backlight_percent(void);
bool app_standby_due(int64_t idle_us);
bool app_sleep_due(int64_t idle_us);

/* Show "SLEEPING", cut camera/LCD/SD/USB power and deep-sleep. Never returns -
 * a wake is a full reset. Callers must have nothing left to lose first: any
 * state they still hold in RAM is gone at this point, which for the emulator
 * means its cart RAM has already been written to the SD card. */
void app_enter_sleep(void);
