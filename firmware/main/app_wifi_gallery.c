/* Diagnostic-only for now (see app_wifi_gallery.h) - just checks whether
 * the onboard C6 is reachable at all over the already-wired SDIO link, and
 * if so, what firmware it's already running, before assuming it needs
 * flashing via the TP44-48 test points. Doesn't bring up an AP or touch
 * the gallery yet. */
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "app_wifi_gallery.h"

static const char *TAG = "wifi_gallery";

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
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init (C6 link up) failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "esp_wifi_init OK - C6 SDIO link is up");

    esp_hosted_app_desc_t desc = {0};
    if (esp_hosted_get_coprocessor_app_desc(&desc) == ESP_OK) {
        ESP_LOGW(TAG, "C6 firmware - project: %s", desc.project_name);
        ESP_LOGW(TAG, "C6 firmware - version: %s", desc.version);
        ESP_LOGW(TAG, "C6 firmware - idf ver: %s", desc.idf_ver);
        ESP_LOGW(TAG, "C6 firmware - built:   %s %s", desc.date, desc.time);
    } else {
        ESP_LOGW(TAG, "C6 didn't report an app description (likely not ESP-Hosted CP firmware)");
    }
}
