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

/* Tell app_enter_sleep() that this sleep is a GB emulator session, so the
 * resume record points back at this ROM + save slot instead of at whatever
 * screen is behind it. The one piece of the resume record that can't be read
 * off app.c's own state, since the emulator is the caller holding it.
 *
 * state_saved is the caller's own answer for whether this session's .state
 * actually reached the card - it is what the sleep screen reports. A sleep with
 * the record but without the file would come back to this ROM on its title
 * screen instead of the frame the player left, and nothing else would ever say
 * why: the card is powered down a moment later, so the file cannot be looked at
 * afterwards.
 *
 * Call it immediately before app_enter_sleep(), and only on the sleep path -
 * a deliberate exit deletes that slot's .state, so coming back to it would
 * have nothing to resume anyway. */
void app_resume_note_gbemu(const char *rom_path, int slot, bool state_saved);

/* True once, on the minute the battery crosses below the low-battery
 * threshold - app_step() shows it as an OSD, and the emulator loop does the
 * same with its own (see run_rom()). Both need the same edge, and each
 * separately calling the gauge would mean two warnings for one drop, so the
 * latch is here. False again after the board charges back above the
 * threshold, so a second drop warns too. */
bool app_low_battery_edge(void);

/* Whether this iteration's input should be discarded: true from a deep-sleep
 * GPIO wake until the button that caused it has been seen and released. A wake
 * is a reset, so the finger that pressed Shutter may still be down when the
 * resumed screen starts, and an unswallowed Shutter is a photo (or an A press
 * in a resumed ROM) the user never asked for.
 *
 * Call it once per loop iteration and OR it into whatever decides to drop an
 * event - calling it is what ages the window out, so a loop that only consulted
 * it inside its drain would swallow the first genuine press minutes later. */
bool app_boot_swallow_poll(void);

/* Show "SLEEPING", cut camera/LCD/SD/USB power and deep-sleep. Never returns -
 * a wake is a full reset. Callers must have nothing left to lose first: any
 * state they still hold in RAM is gone at this point, which for the emulator
 * means its cart RAM has already been written to the SD card.
 *
 * This is also where the resume record is written (see s_resume in app.c) - the
 * one choke point every sleep goes through, so a new caller can't forget it. */
void app_enter_sleep(void);
