#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_settings.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "gbcam"
#define NVS_KEY "settings"
#define SETTLE_US (2 * 1000 * 1000)
#define SETTINGS_VERSION 9  /* bumped: added per-mode adjust[] */

typedef struct {
    uint8_t version;
    app_settings_t s;
} stored_t;

static stored_t s_pending;
static int64_t s_changed_at = -1;

void settings_load(app_settings_t *s)
{
    settings_defaults(s);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable: %s", esp_err_to_name(err));
        return;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    stored_t st;
    size_t len = sizeof st;
    if (nvs_get_blob(h, NVS_KEY, &st, &len) == ESP_OK && len == sizeof st &&
        st.version == SETTINGS_VERSION && settings_valid(&st.s))
        *s = st.s;
    nvs_close(h);
}

void settings_changed(const app_settings_t *s)
{
    s_pending.version = SETTINGS_VERSION;
    s_pending.s = *s;
    s_changed_at = esp_timer_get_time();
}

void settings_tick(void)
{
    if (s_changed_at < 0 || esp_timer_get_time() - s_changed_at < SETTLE_US) return;
    s_changed_at = -1;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, NVS_KEY, &s_pending, sizeof s_pending) == ESP_OK)
        nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved");
}
