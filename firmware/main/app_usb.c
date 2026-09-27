#include <stdlib.h>

#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "bsp/esp-bsp.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"

#include "app_usb.h"
#include "platform.h"

static const char *TAG = "usb_msc";

static wl_handle_t s_placeholder_wl = WL_INVALID_HANDLE;
static sd_pwr_ctrl_handle_t s_pwr_ctrl;
static sdmmc_host_t s_sd_host;
static sdmmc_card_t *s_sd_card;

static bool s_active;      /* real SD card handed to the host right now */
static bool s_prompt;      /* host just connected - waiting on Menu/Shutter */
static bool s_was_mounted; /* edge-detect for tud_mounted() */

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
    const tinyusb_config_t tusb_cfg = {0};
    return tinyusb_driver_install(&tusb_cfg);
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
    if (mounted && !s_was_mounted && !s_active) s_prompt = true;
    if (!mounted) s_prompt = false;
    /* tud_mounted() catches a host-initiated disconnect (Windows ejecting
     * the drive) fine, but NOT an actual cable pull - the vendored TinyUSB
     * DWC2 port has a literal "TODO check GINTSTS_DISCINT for disconnect
     * detection" left unimplemented (dcd_dwc2.c), so a real unplug leaves
     * tud_mounted() stuck true. Shutter (see app.c's handle_usb_input()) is
     * the reliable way out until that lands upstream. */
    if (s_active && !mounted) usb_msc_exit();
    s_was_mounted = mounted;
}
