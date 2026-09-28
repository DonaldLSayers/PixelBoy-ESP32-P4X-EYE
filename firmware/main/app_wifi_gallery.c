/* Diagnostic/bring-up only for now (see app_wifi_gallery.h) - checks
 * whether the onboard C6 is reachable over SDIO, and if it's running a
 * blank/stub image, pushes real coprocessor firmware to it over that same
 * link via ESP-Hosted's OTA API - no external adapter needed. Doesn't
 * bring up an AP or touch the gallery yet.
 *
 * esp_wifi_init() is called first (even though it may fail while the
 * coprocessor's still blank) rather than only the lower-level eh_host_init/
 * eh_host_connect_to_slave transport calls - esp_wifi_remote ships a weak
 * "unsupported" fallback for esp_wifi_remote_init() that only gets
 * overridden by esp_hosted's real one if something actually references it;
 * skipping esp_wifi_init() entirely (tried this first) left the linker with
 * only the weak stub, so a real coprocessor swapped in mid-session still
 * reported ESP_ERR_NOT_SUPPORTED afterwards. Calling it unconditionally,
 * success or not, is what keeps the real backend linked in. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "app_storage.h"
#include "app_wifi_gallery.h"

static const char *TAG = "wifi_gallery";

/* Matches eh_host_cp_ota_write()'s own EH_RPC_OTA_CHUNK_MAX. */
#define OTA_CHUNK_MAX 1536

static void log_cp_version(void)
{
    esp_hosted_coprocessor_fwver_t ver = {0};
    if (esp_hosted_get_coprocessor_fwversion(&ver) == ESP_OK)
        ESP_LOGW(TAG, "C6 firmware version: %" PRIu32 ".%" PRIu32 ".%" PRIu32, ver.major1, ver.minor1, ver.patch1);
    else
        ESP_LOGW(TAG, "C6 firmware version query failed (normal for very old/blank coprocessor firmware)");
}

/* Streams storage_root()/c6_fw.bin (dropped there by hand for now) to the
 * C6 over the SDIO/RPC link. Returns true if the whole file was written
 * and the image was marked pending-boot. */
static bool flash_coprocessor(void)
{
    char path[300];
    snprintf(path, sizeof path, "%s/c6_fw.bin", storage_root());

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "%s not found - drop the built coprocessor firmware there to flash it", path);
        return false;
    }

    esp_err_t err = esp_hosted_cp_ota_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_cp_ota_begin: %s", esp_err_to_name(err));
        fclose(f);
        return false;
    }

    uint8_t buf[OTA_CHUNK_MAX];
    size_t total = 0;
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        err = esp_hosted_cp_ota_write(buf, (uint32_t)n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_hosted_cp_ota_write at %u bytes: %s", (unsigned)total, esp_err_to_name(err));
            ok = false;
            break;
        }
        total += n;
    }
    fclose(f);
    if (!ok) return false;

    ESP_LOGI(TAG, "wrote %u bytes to the C6", (unsigned)total);

    err = esp_hosted_cp_ota_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_cp_ota_end: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_hosted_cp_ota_activate();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_cp_ota_activate: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "C6 firmware update staged - resetting the link to boot into it");
    return true;
}

void wifi_gallery_diag(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    ESP_LOGI(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
    log_cp_version();

    if (err == ESP_OK) {
        ESP_LOGW(TAG, "C6 WiFi is up and working!");
        return;
    }

    /* Not working yet - flash real coprocessor firmware over the transport
     * (already up as a side effect of esp_wifi_init() even though it
     * failed) and retry. */
    if (flash_coprocessor()) {
        esp_wifi_deinit();
        err = esp_wifi_init(&cfg);
        ESP_LOGI(TAG, "post-flash esp_wifi_init: %s", esp_err_to_name(err));
        log_cp_version();
        if (err == ESP_OK) ESP_LOGW(TAG, "C6 WiFi is up and working!");
    }
}
