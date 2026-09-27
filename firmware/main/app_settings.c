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
#define SETTINGS_VERSION 9  /* only bump on incompatible field reorder/removal - see settings_load()'s
                              * size-tolerant load for plain field additions, which need no bump */

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
    /* stored_t may be smaller than current (fields appended since it was
     * saved) - keep s's already-defaulted tail fields in that case, only
     * overwriting the prefix that was actually stored, instead of rejecting
     * the whole blob and losing every setting to a struct size bump. */
    stored_t st;
    memset(&st, 0, sizeof st);
    size_t len = sizeof st;
    esp_err_t rd = nvs_get_blob(h, NVS_KEY, &st, &len);
    if (rd == ESP_ERR_NVS_INVALID_LENGTH && len > 0 && len <= sizeof st) {
        /* stored blob is smaller than sizeof(stored_t) - len now holds its
         * real size (NVS convention); re-read into a correctly-sized area. */
        rd = nvs_get_blob(h, NVS_KEY, &st, &len);
    }
    if (rd == ESP_OK && len >= sizeof(uint8_t) && len <= sizeof st &&
        st.version == SETTINGS_VERSION) {
        app_settings_t merged = *s;
        memcpy(&merged, &st.s, len - sizeof(uint8_t));
        if (settings_valid(&merged)) *s = merged;
    }
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
