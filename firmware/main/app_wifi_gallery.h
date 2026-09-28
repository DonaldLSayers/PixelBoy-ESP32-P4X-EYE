#pragma once

/* NOT YET IMPLEMENTED - not in main/CMakeLists.txt's SRCS, this branch is
 * just parking the hardware research needed before writing it.
 *
 * Goal: an on-demand "WiFi Gallery" menu toggle (same pattern as USB's
 * Drive/Mirror/GB Webcam picker) that brings up the board's onboard
 * ESP32-C6-MINI-1U as a WiFi access point and serves a small HTTP photo
 * gallery (reusing app_storage.c's existing index + pre-generated THUMB
 * files) so photos can be browsed/downloaded from a phone with no cable.
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
 * Next steps when actually building this:
 *   1. Get/build the C6's own esp-hosted network-co-processor firmware
 *      (espressif/esp-hosted-mcu's examples/*/cp project, target esp32c6,
 *      SDIO transport) and flash it via the C6's own UART pins (GPIO35/36
 *      above) - a one-time step, separate from this project's own flashing.
 *   2. Add the esp_hosted component + SDIO host config to this project
 *      (firmware/main/idf_component.yml, sdkconfig.defaults) using the pin
 *      list above.
 *   3. Bring up WiFi AP mode + esp_http_server once esp_hosted reports the
 *      link up; verify with a trivial "hello" page before building the
 *      actual gallery.
 *   4. Gallery page: reuse storage_count()/storage_number_at()/
 *      storage_is_dc_at()/storage_load_thumb() (app_storage.h) - the same
 *      index and pre-generated thumbnails the on-device gallery already
 *      uses - plus a download route serving the full photo file.
 *   5. Wire a menu toggle (ROW_WIFI, same shape as the USB picker) in app.c.
 */
