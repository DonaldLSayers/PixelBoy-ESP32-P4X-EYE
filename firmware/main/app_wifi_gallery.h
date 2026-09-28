#pragma once

/* Diagnostic-only for now - brings up just enough of esp_hosted/esp_wifi to
 * tell whether the C6 answers over SDIO at all, and if so what firmware
 * it's already running. Call once at boot; see app_wifi_gallery.c. Logs
 * under tag "wifi_gallery" - watch the serial monitor. */
void wifi_gallery_diag(void);

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
 * STATUS: host side is DONE and builds clean - main/idf_component.yml pulls
 * espressif/esp_hosted (host role, MCU type, ESP32C6 target, SDIO transport,
 * slot 1 since the real SD card already owns slot 0), and
 * sdkconfig.defaults' CONFIG_ESP32P4_EYE_C6_BOARD=y selects this exact
 * board's pin preset - which, reassuringly, matches the schematic-derived
 * pin list above exactly (CMD=27, CLK=28, D0-D3=29-32, EN=9). No app code
 * calls into it yet, so nothing actually runs.
 *
 * REMAINING BLOCKER: the C6 needs its own separate co-processor firmware
 * (a small, fixed "network adapter" image, not our code) flashed onto it
 * before any of this can talk to it - and the P4's single USB port can't
 * reach the C6 for that. The board's own user guide confirms dedicated
 * test points exist for exactly this ("Provides access points for
 * programming and testing the ESP32-C6-MINI-1U; can be connected via
 * Dupont wires"), and the schematic (same "04_WiFi&BT" sheet) names them:
 *
 *   TP44 = C6_EN
 *   TP45 = C6_BOOT
 *   TP46 = C6_U0RXD  (C6's RX - drive from the adapter's TX)
 *   TP47 = C6_U0TXD  (C6's TX - feeds the adapter's RX)
 *   TP48 = GND
 *
 * Needs a 3.3V USB-to-serial adapter (e.g. CP2102/FTDI) jumper-wired to
 * those five points - not available in this session, since it requires
 * physically wiring the real board.
 *
 * Next steps once that adapter/wiring is in hand:
 *   1. Build espressif/esp-hosted-mcu's coprocessor firmware (the
 *      mcu_hosted_sdio_sdmmc_combined example's "cp" project, which matches
 *      our exact setup - SD card + C6 sharing one SDMMC controller), target
 *      esp32c6, SDIO transport, flash via TP46/47/44/45 above using its own
 *      eh.py, pointed at the adapter's port - a one-time step, wholly
 *      separate from this project's own P4 flashing.
 *   2. Add app code that actually calls esp_hosted/esp_wifi (AP mode) and
 *      esp_http_server; verify the host<->C6 link comes up with a trivial
 *      "hello" page before building the real gallery. Watch for the known
 *      SDMMC-controller-sharing quirk esp-idf#16233 flags (workaround shown
 *      in that combined example's main/esp_hosted_wifi.c).
 *   3. Gallery page: reuse storage_count()/storage_number_at()/
 *      storage_is_dc_at()/storage_load_thumb() (app_storage.h) - the same
 *      index and pre-generated thumbnails the on-device gallery already
 *      uses - plus a download route serving the full photo file.
 *   4. Wire a menu toggle (ROW_WIFI, same shape as the USB picker) in app.c.
 */
