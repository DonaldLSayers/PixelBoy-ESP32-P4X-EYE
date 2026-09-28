#pragma once

#include <stdbool.h>

/* WPA2-PSK needs 8+ chars - fixed, not configurable on-device (there's no
 * text entry UI); good enough for a short-lived local gallery. Public so
 * app.c's SCREEN_WIFI info screen can show them alongside the toggle.
 * WIFI_GALLERY_PASS is all-caps on purpose: app_display.c's font is
 * uppercase-only (lowercase folds to caps for display), so a mixed-case
 * password would show wrong on screen - whatever's typed in from there
 * wouldn't match the real one. SSID doesn't have this problem since it's
 * picked from a scan list, not typed. */
#define WIFI_GALLERY_SSID "PixelBoy Gallery"
#define WIFI_GALLERY_PASS "PIXELBOY1"

/* Called unconditionally from main.c at every boot - cheap no-op in the
 * normal case (nothing staged in the c6fw partition, or a previous flash
 * already erased it): just reads the P4's own local flash, no C6/SDIO
 * power-up at all. Only calls through to wifi_gallery_diag() below (which
 * does power the C6 up - the slow, battery-costing part) if
 * firmware/c6fw_image/c6_fw.bin was actually built in and reflashed - see
 * the README's "Setting up a brand new device". One-step: drop the file in,
 * idf.py flash, it flashes the C6 and reboots on its own; next boot finds
 * nothing staged (the partition self-erases after a successful flash) and
 * skips straight past, back to normal fast boot. */
void wifi_gallery_check_c6_update(void);

/* Diagnostic - brings up just enough of esp_hosted/esp_wifi to tell whether
 * the C6 answers over SDIO at all, and if so what firmware it's running
 * (and, if it's a blank/stub image, flashes real coprocessor firmware to it
 * over that same SDIO link via ESP-Hosted's OTA API - see
 * flash_coprocessor() in app_wifi_gallery.c). Normally reached through
 * wifi_gallery_check_c6_update() above, not called directly - call it
 * yourself only for a manual one-off recheck (e.g. from a debug console).
 * Logs under tag "wifi_gallery" - watch the serial monitor, or the on-screen
 * status. */
void wifi_gallery_diag(void);

/* The real WiFi Gallery - brings the onboard ESP32-C6-MINI-1U up as a WiFi
 * access point and serves an HTTP photo gallery at http://192.168.4.1/
 * (reusing app_storage.c's existing index - one grid page of THUMB/
 * thumbnails, each linking to its full photo for download) so photos can be
 * browsed/downloaded from a phone with no cable. Wired up in app.c as a menu
 * row (ROW_WIFI, "WIFI GALLERY") that calls wifi_gallery_start() on click and
 * switches to SCREEN_WIFI; Shutter there calls wifi_gallery_stop() and
 * returns to the viewfinder, same shape as the USB picker. Confirmed working
 * on real hardware.
 *
 * The C6 draws no power except between these two calls: sdkconfig.defaults
 * turns off ESP-Hosted's own auto-init (which otherwise brings the C6 up
 * before app_main() even runs, whether the gallery's ever opened or not), so
 * wifi_gallery_start() explicitly powers the C6 up (esp_hosted_init() +
 * esp_hosted_connect_to_slave()) and wifi_gallery_stop() powers it back down
 * (esp_hosted_deinit(), then holds it in reset - EN low) every time. */
void wifi_gallery_start(void);
void wifi_gallery_stop(void);
bool wifi_gallery_active(void);

/* Hardware background, kept for context.
 *
 * The P4 has no WiFi radio of its own - it talks to the onboard C6 over
 * SDIO via ESP-Hosted (the C6 runs its own separate "network co-processor"
 * firmware, flashed independently of this project's own firmware). This is
 * NOT a simple "add a component + Kconfig flag" - confirmed by checking
 * Espressif's own esp-dev-kits factory_demo for this exact board, which
 * doesn't use the C6 at all (no wifi/hosted config anywhere in its
 * sdkconfig.defaults or source), and by esp-hosted-mcu's own docs, which
 * treat the C6 co-processor as an entirely separate firmware project (its
 * own `eh.py` build/flash tool, own port) rather than a component dropped
 * into this one.
 *
 * P4<->C6 SDIO wiring, confirmed from the real schematic (not guessed) -
 * ESP32-P4-EYE-MB v2.3, sheet "04_WiFi&BT", cross-checked against sheet
 * "02_ESP32-P4"'s own GPIO pin list (and sanity-checked against this
 * project's own app_input.h button pins, which matched exactly):
 *
 *   P4 GPIO9  -> C6_EN      (C6's enable/reset line)
 *   P4 GPIO27 -> SD2_CMD
 *   P4 GPIO28 -> SD2_CLK
 *   P4 GPIO29 -> SD2_D0
 *   P4 GPIO30 -> SD2_D1
 *   P4 GPIO31 -> SD2_D2
 *   P4 GPIO32 -> SD2_D3
 *   P4 GPIO33 -> C6_BOOT
 *   P4 GPIO35 -> C6_U0RXD   (shares the P4's own strapping/BOOT pin - only
 *                            matters while actually flashing the C6 over
 *                            UART, not during normal SDIO operation)
 *   P4 GPIO36 -> C6_U0TXD
 *
 * On the C6-MINI-1U side, these are its own default/fixed SDIO slave pins
 * (IO18=CMD, IO19=CLK, IO20=D0, IO21=D1, IO22=D2, IO23=D3, per the module's
 * own datasheet) - nothing unusual there, all the board-specific info is
 * the P4-side GPIO list above.
 *
 * The C6's own network co-processor firmware was flashed via
 * flash_coprocessor() below, over the SDIO link it already shares with the
 * P4 (same approach as github.com/lboshuizen/crowpanel-p4-c6-sdio-ota),
 * reading the image from its own dedicated c6fw flash partition (see
 * partitions.csv) rather than the SD card - built from whatever's in
 * firmware/c6fw_image/ and flashed automatically alongside the rest by
 * idf.py flash. tools/c6_coprocessor holds that build (espressif/esp-hosted-
 * mcu's mcu_hosted_sdio_sdmmc_combined example's "cp" project) - see the
 * README's "Setting up a brand new device" section for the actual steps
 * (build it, drop it in firmware/c6fw_image/, reflash the P4 -
 * wifi_gallery_check_c6_update() picks it up automatically).
 *
 * Two real-hardware bugs found and fixed getting here, both worth knowing
 * about if this ever regresses:
 *   - SCREEN_WIFI closed itself within about a second of opening: ROW_WIFI
 *     activates on the menu's Shutter *press* (app.c's activate_menu_row()),
 *     so that same press's later release arrived as an INPUT_CLICK on the
 *     very next app_step(), which the exit handler was also listening for -
 *     instantly closing the screen it had just opened. Fixed by listening
 *     for INPUT_PRESS instead (matches ROW_GALLERY's leave_gallery(), which
 *     has the same shape and already dodged this).
 *   - Loading the gallery page rebooted the board: esp_http_server's default
 *     task stack (4KB) blew its stack canary (Guru Meditation: Stack
 *     protection fault in task "httpd", inside snprintf) - our handlers'
 *     own snprintf() calls on top of the server's request parsing didn't
 *     fit. Fixed by setting httpd_config_t.stack_size = 8192 in
 *     wifi_gallery_start().
 */
