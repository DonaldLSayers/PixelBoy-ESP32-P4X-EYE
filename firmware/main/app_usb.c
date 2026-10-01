#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "bsp/esp-bsp.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "stb_image_write.h" /* implementation lives in components/stb/stb_write_impl.c - just declarations here */

#include "app_usb.h"
#include "app_usb_video_desc.h"
#include "platform.h"

static const char *TAG = "usb_msc";

static wl_handle_t s_placeholder_wl = WL_INVALID_HANDLE;
static sd_pwr_ctrl_handle_t s_pwr_ctrl;
static sdmmc_host_t s_sd_host;
static sdmmc_card_t *s_sd_card;

static bool s_active;      /* real SD card handed to the host right now */
static bool s_prompt;      /* host just connected - waiting on Menu/Shutter */
static bool s_was_mounted; /* edge-detect for tud_mounted() */

/* -------------------------------------------------------------- webcam (UVC) */

static bool s_webcam_active;
/* ~126KB (WEBCAM_FRAME_W*H) - too big for internal RAM (chronically ~51KB
 * free at boot on this board), so this comes from PSRAM instead of a plain
 * static array, same fix as the two prior internal-RAM-exhaustion bugs. */
static uint8_t *s_webcam_jpeg;
static bool s_webcam_tx_busy;

/* True if webcam mode actually started. It can fail for exactly one reason -
 * no PSRAM left for the ~126KB JPEG buffer (see s_webcam_jpeg) - and that has
 * to reach the caller: the alternative is an on-screen "MIRROR ON" over a
 * stream that will never send a frame, with nothing to retry from and no way
 * for the user to tell. */
bool usb_webcam_accept(void)
{
    if (!s_webcam_jpeg) {
        s_webcam_jpeg = heap_caps_malloc(WEBCAM_JPEG_MAX_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_webcam_jpeg) {
            PLOGE(TAG, "webcam mode failed - out of PSRAM for JPEG buffer");
            return false;
        }
    }
    s_prompt = false;
    s_webcam_active = true;
    PLOGI(TAG, "webcam mode - streaming the GB Camera view over USB");
    return true;
}

bool usb_webcam_active(void) { return s_webcam_active; }

void usb_webcam_exit(void)
{
    s_webcam_active = false;
    s_webcam_tx_busy = false;
}

typedef struct { uint8_t *buf; size_t len, cap; } jpeg_write_ctx_t;

static void jpeg_write_cb(void *ctx, void *data, int size)
{
    jpeg_write_ctx_t *c = ctx;
    if (c->len + (size_t)size > c->cap) return; /* shouldn't happen at this frame size/quality, but don't overrun if it ever does */
    memcpy(c->buf + c->len, data, (size_t)size);
    c->len += (size_t)size;
}

void usb_webcam_feed(const uint8_t *rgb888)
{
    if (!s_webcam_active || !tud_video_n_streaming(0, 0) || s_webcam_tx_busy) return;

    jpeg_write_ctx_t ctx = { .buf = s_webcam_jpeg, .len = 0, .cap = WEBCAM_JPEG_MAX_BYTES };
    if (!stbi_write_jpg_to_func(jpeg_write_cb, &ctx, WEBCAM_FRAME_W, WEBCAM_FRAME_H, 3, rgb888, 80)) return;

    s_webcam_tx_busy = true;
    if (!tud_video_n_frame_xfer(0, 0, s_webcam_jpeg, ctx.len)) s_webcam_tx_busy = false;
}

void tud_video_frame_xfer_complete_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx)
{
    (void)ctl_idx; (void)stm_idx;
    s_webcam_tx_busy = false;
}

/* Host negotiates the stream format before it starts pulling frames (UVC
 * VS_COMMIT_CONTROL) - only one format/frame size is offered (see
 * app_usb_video_desc.c), so there's nothing to actually pick here, just
 * accept whatever it asks to commit to. */
int tud_video_commit_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx, video_probe_and_commit_control_t const *parameters)
{
    (void)ctl_idx; (void)stm_idx; (void)parameters;
    return VIDEO_ERROR_NONE;
}

static esp_err_t placeholder_attach(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                            ESP_PARTITION_SUBTYPE_DATA_FAT, "usbmsc");
    if (!part) {
        PLOGW(TAG, "no usbmsc partition - USB port won't show a drive until SD is confirmed");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = wl_mount(part, &s_placeholder_wl);
    if (err != ESP_OK) return err;

    const tinyusb_msc_spiflash_config_t cfg = { .wl_handle = s_placeholder_wl };
    return tinyusb_msc_storage_init_spiflash(&cfg);
}

esp_err_t usb_msc_init(void)
{
    placeholder_attach();
    usb_video_desc_init();
    return tinyusb_driver_install(usb_video_tinyusb_config());
}

/* USB teardown for deep sleep. usb_msc_init() installs the TinyUSB driver on
 * every boot and nothing ever took it back down, so the OTG PHY, its
 * descriptors and the placeholder drive stayed live for the whole sleep, with
 * nothing plugged in - pure standby load on a device whose entire point is
 * running off a battery.
 *
 * Only reachable from plat_enter_deep_sleep(), which enter_sleep() only gets
 * to when the SD card is *not* handed to a host (SCREEN_USB) and webcam mode
 * is off (usb_webcam_active()) - so there is never a transfer in flight to
 * cut short. Nothing re-installs it on the way back up because there is no
 * way back up: wake is a reset (see plat_enter_deep_sleep()). */
void usb_msc_deinit(void)
{
    esp_err_t err = tinyusb_driver_uninstall();
    if (err != ESP_OK) PLOGW(TAG, "tinyusb uninstall: %s", esp_err_to_name(err));
}

static void sd_host_deinit(void)
{
    if (s_sd_host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) s_sd_host.deinit_p(s_sd_host.slot);
    else if (s_sd_host.deinit) s_sd_host.deinit();
}

void usb_msc_exit(void)
{
    if (!s_active) return;
    PLOGI(TAG, "USB exit - returning SD card to the camera app");
    tinyusb_msc_storage_deinit();
    if (s_sd_card) { free(s_sd_card); s_sd_card = NULL; }
    sd_host_deinit();
    if (s_pwr_ctrl) { sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl); s_pwr_ctrl = NULL; }
    s_active = false;

    bsp_sdcard_mount();    /* back to normal camera app access */
    placeholder_attach();  /* re-arm connect detection for next time */
}

void usb_msc_accept(void)
{
    s_prompt = false;

    /* Only one controller can own the physical SD bus at a time - release
     * the camera app's own mount, then bring the card back up ourselves
     * (mirroring bsp_sdcard_mount()'s own power/host setup - the BSP has no
     * API to hand off a card it already mounted) so tinyusb can serve it
     * directly to the host. */
    tinyusb_msc_storage_deinit(); /* drop the placeholder */
    bsp_sdcard_unmount();

    sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = 4 };
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_pwr_ctrl);
    if (err != ESP_OK) PLOGE(TAG, "sd_pwr_ctrl_new_on_chip_ldo: %s", esp_err_to_name(err));

    if (err == ESP_OK) {
        bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &s_sd_host);
        s_sd_host.pwr_ctrl_handle = s_pwr_ctrl;
        sdmmc_slot_config_t slot;
        bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &slot);

        s_sd_card = calloc(1, sizeof(*s_sd_card));
        err = s_sd_host.init();
        if (err != ESP_OK) PLOGE(TAG, "sd host.init: %s", esp_err_to_name(err));
        if (err == ESP_OK) {
            err = sdmmc_host_init_slot(s_sd_host.slot, &slot);
            if (err != ESP_OK) PLOGE(TAG, "sdmmc_host_init_slot: %s", esp_err_to_name(err));
        }
        if (err == ESP_OK) {
            err = sdmmc_card_init(&s_sd_host, s_sd_card);
            if (err != ESP_OK) PLOGE(TAG, "sdmmc_card_init: %s", esp_err_to_name(err));
        }
    }
    if (err == ESP_OK) {
        const tinyusb_msc_sdmmc_config_t cfg = { .card = s_sd_card };
        err = tinyusb_msc_storage_init_sdmmc(&cfg);
        if (err != ESP_OK) PLOGE(TAG, "tinyusb_msc_storage_init_sdmmc: %s", esp_err_to_name(err));
    }

    if (err != ESP_OK) {
        PLOGE(TAG, "USB SD handoff failed - back to the camera app");
        if (s_sd_card) { free(s_sd_card); s_sd_card = NULL; }
        sd_host_deinit();
        if (s_pwr_ctrl) { sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl); s_pwr_ctrl = NULL; }
        bsp_sdcard_mount();
        placeholder_attach();
        return;
    }
    PLOGI(TAG, "SD card handed to USB host");
    s_active = true;
}

void usb_msc_decline(void) { s_prompt = false; }
bool usb_msc_prompt_pending(void) { return s_prompt; }
bool usb_msc_active(void) { return s_active; }

void usb_msc_tick(void)
{
    bool mounted = tud_mounted();
    if (mounted && !s_was_mounted && !s_active && !s_webcam_active) s_prompt = true;
    if (!mounted) s_prompt = false;
    /* tud_mounted() catches a host-initiated disconnect (Windows ejecting
     * the drive, or closing the webcam app) fine, but NOT an actual cable
     * pull - the vendored TinyUSB DWC2 port has a literal "TODO check
     * GINTSTS_DISCINT for disconnect detection" left unimplemented
     * (dcd_dwc2.c), so a real unplug leaves tud_mounted() stuck true.
     * Shutter (see app.c's handle_usb_input()) is the reliable way out until
     * that lands upstream. */
    if (s_active && !mounted) usb_msc_exit();
    if (s_webcam_active && !mounted) usb_webcam_exit();
    s_was_mounted = mounted;
}
