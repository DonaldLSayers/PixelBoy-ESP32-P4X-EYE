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
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "eh_host_transport_config.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "app_display.h"
#include "app_storage.h"
#include "app_wifi_gallery.h"
#include "platform.h"

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

/* Brief on-screen status - wifi_gallery_diag() is normally only wired in
 * temporarily (see app_wifi_gallery.h), so whoever's doing that is watching
 * the screen, not necessarily a serial monitor. */
static void screen_status(const char *l1, const char *l2)
{
    display_begin_blank(0, 0, 0);
    display_text(DISP_W / 2 - display_text_width(l1, 2) / 2, 100, 2, l1, 255, 255, 255);
    if (l2) display_text(DISP_W / 2 - display_text_width(l2, 1) / 2, 130, 1, l2, 200, 200, 200);
    display_end_frame();
}

#define C6FW_MOUNT "/c6fw"
#define C6FW_LABEL "c6fw"

/* Streams firmware/c6fw_image/c6_fw.bin (built into the c6fw partition by
 * the top-level CMakeLists.txt - see partitions.csv) to the C6 over the
 * SDIO/RPC link. Returns true if the whole file was written and the image
 * was marked pending-boot. */
static bool flash_coprocessor(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = C6FW_MOUNT,
        .partition_label = C6FW_LABEL,
        .max_files = 1,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "c6fw partition mount failed: %s - nothing to flash the C6 with "
                      "(drop the built firmware in firmware/c6fw_image/, see README)", esp_err_to_name(err));
        return false;
    }

    char path[64];
    snprintf(path, sizeof path, "%s/c6_fw.bin", C6FW_MOUNT);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "%s not found - drop the built coprocessor firmware in firmware/c6fw_image/ and reflash to update it", path);
        esp_vfs_spiffs_unregister(C6FW_LABEL);
        return false;
    }

    screen_status("FLASHING C6...", "DO NOT DISCONNECT");

    err = esp_hosted_cp_ota_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_cp_ota_begin: %s", esp_err_to_name(err));
        fclose(f);
        esp_vfs_spiffs_unregister(C6FW_LABEL);
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
    esp_vfs_spiffs_unregister(C6FW_LABEL);
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

/* Reclaim the c6fw partition - the staged image (if any) has done its job,
 * whether that meant actually flashing the C6 or finding it already working
 * without needing to. Not required for correctness (the same image would
 * just get rewritten harmlessly on the next flash), but skipping it after a
 * "already working, nothing to flash" outcome specifically was a real bug:
 * wifi_gallery_diag() would then leave c6_fw.bin staged forever (only
 * flash_coprocessor()'s own success path erased it), so every single boot
 * kept re-detecting it and re-running this whole check - confirmed on real
 * hardware (a freshly-flashed board whose C6 answered fine on the very
 * first try, since it wasn't truly blank). */
static void erase_c6fw_partition(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, C6FW_LABEL);
    if (part) esp_partition_erase_range(part, 0, part->size);
}

/* nvs/netif/event-loop bring-up shared by wifi_gallery_diag() (STA, for
 * hardware bring-up) and wifi_gallery_start() (AP, the real gallery) -
 * either one might run first, so this tolerates being called twice. */
static bool s_base_init;

static bool ensure_base_init(void)
{
    if (s_base_init) return true;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);

    s_base_init = true;
    return true;
}

/* C6 power gating - CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN is off
 * (sdkconfig.defaults), so nothing brings the C6/SDIO link up on its own any
 * more; cp_power_up()/cp_power_down() do it on demand instead, so the C6
 * draws no power except while the gallery (or the diagnostic) is actually
 * using it. */
static bool s_cp_gpio_ready;
static eh_gpio_pin_t s_cp_reset_pin;

static bool cp_power_up(void)
{
    if (esp_hosted_init() != 0) {
        ESP_LOGE(TAG, "esp_hosted_init failed");
        return false;
    }
    if (esp_hosted_connect_to_slave() != 0) {
        ESP_LOGE(TAG, "esp_hosted_connect_to_slave failed");
        esp_hosted_deinit();
        return false;
    }
    return true;
}

/* Tears down the RPC/transport link, then holds the C6 in reset (EN low) -
 * matches esp_hosted's own power_save/cp/shut_down_cp_when_unused example.
 * eh_host_connect_to_slave() releases EN again on the next cp_power_up(), as
 * part of its own SDIO bring-up (see the "Reset co-processor using GPIO[9]"
 * log line) - no matching manual release needed here. */
static void cp_power_down(void)
{
    esp_hosted_deinit();

    if (!s_cp_gpio_ready) {
        if (eh_host_transport_get_reset_config(&s_cp_reset_pin) != EH_HOST_TRANSPORT_RC_OK) {
            ESP_LOGW(TAG, "no reset GPIO config - can't power the C6 down");
            return;
        }
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << s_cp_reset_pin.pin,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io_conf);
        s_cp_gpio_ready = true;
    }
    gpio_set_level(s_cp_reset_pin.pin, 0);
}

/* One-step flash: main.c calls this unconditionally at every boot. Reading
 * the c6fw partition is just the P4's own local flash - no C6/SDIO power-up
 * needed for that part - so checking "is anything actually staged" is cheap
 * enough to always do. Only powers the C6 up and runs the real check-and-
 * flash (wifi_gallery_diag(), the slow/battery-costing part) if
 * firmware/c6fw_image/c6_fw.bin was actually dropped in and reflashed;
 * otherwise (the normal case - nothing staged, or a previous flash already
 * erased it) this returns almost immediately and boot stays fast. */
void wifi_gallery_check_c6_update(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = C6FW_MOUNT,
        .partition_label = C6FW_LABEL,
        .max_files = 1,
        .format_if_mount_failed = false,
    };
    if (esp_vfs_spiffs_register(&conf) != ESP_OK) return; /* nothing staged */

    char path[64];
    snprintf(path, sizeof path, "%s/c6_fw.bin", C6FW_MOUNT);
    FILE *f = fopen(path, "rb");
    bool staged = f != NULL;
    if (f) fclose(f);
    esp_vfs_spiffs_unregister(C6FW_LABEL);
    if (!staged) return;

    wifi_gallery_diag();
}

void wifi_gallery_diag(void)
{
    if (!ensure_base_init()) return;
    if (!cp_power_up()) return;

    screen_status("CHECKING C6...", NULL);
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    ESP_LOGI(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
    log_cp_version();

    bool flashed = false;
    if (err != ESP_OK) {
        /* Not working yet - flash real coprocessor firmware over the
         * transport (already up as a side effect of esp_wifi_init() even
         * though it failed) and retry. */
        flashed = flash_coprocessor();
        if (flashed) {
            esp_wifi_deinit();
            err = esp_wifi_init(&cfg);
            ESP_LOGI(TAG, "post-flash esp_wifi_init: %s", esp_err_to_name(err));
            log_cp_version();
        }
    }
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "C6 WiFi is up and working!");
        esp_wifi_deinit();
        erase_c6fw_partition();
        screen_status(flashed ? "C6 FLASHED OK" : "C6 WIFI OK", NULL);
    } else {
        screen_status("C6 WIFI FAILED", flashed ? "FLASHED, BUT WIFI STILL FAILS" : "SEE SERIAL LOG");
    }
    plat_sleep_ms(2000); /* let the result actually be read before app_step()'s own drawing resumes */
    /* Diagnostic-only - nothing keeps running after this, so the C6 goes
     * back off the same as wifi_gallery_stop() does. */
    cp_power_down();
}

/* ---- Gallery HTTP server ------------------------------------------------
 * One page: a grid of every photo, newest first (unlike the on-device
 * gallery, which is oldest first). Grid thumbnails - the full photos loaded
 * too slowly over the AP's WiFi link for a whole grid of them at once, even
 * though the files themselves are small; each still links to /photo (the
 * full file) for download. Each <img loading=lazy> defers its own fetch
 * until scrolled near-visible - native browser behaviour, no extra JS. */

static httpd_handle_t s_httpd;
static esp_netif_t *s_ap_netif;
static bool s_active;

bool wifi_gallery_active(void) { return s_active; }

/* Streams a file straight to the response - the file's already the final
 * viewable image, nothing to decode/re-encode. */
static esp_err_t send_file(httpd_req_t *req, const char *path, const char *content_type)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, content_type);
    char buf[2048];
    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

/* Gallery position ("n" in the query string, 0 = oldest - see
 * storage_number_at()) for a request, or -1 if missing/malformed. */
static int query_pos(httpd_req_t *req)
{
    char query[32], val[16];
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK) return -1;
    if (httpd_query_key_value(query, "n", val, sizeof val) != ESP_OK) return -1;
    return atoi(val);
}

static esp_err_t photo_handler(httpd_req_t *req)
{
    int pos = query_pos(req);
    int number = storage_number_at(pos);
    if (number < 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    char path[300];
    const char *content_type;
    if (!storage_photo_path(number, storage_is_dc_at(pos), path, sizeof path, &content_type)) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    /* Without this the browser has nothing to name the download but the URL
     * itself ("/photo?n=5"), and saves it as a generic "photo.png". path's
     * basename is the real GBnnnnn.PNG/DCnnnnn.PNG/.JPG filename already. */
    const char *slash = strrchr(path, '/');
    char disposition[64];
    snprintf(disposition, sizeof disposition, "attachment; filename=\"%s\"", slash ? slash + 1 : path);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    return send_file(req, path, content_type);
}

/* Grid thumbnail - the full photos turned out too slow to load a whole grid
 * of over the AP's WiFi link (see index_handler()). Always PNG - see
 * app_storage.c's thumb_path_for(). */
static esp_err_t thumb_handler(httpd_req_t *req)
{
    int pos = query_pos(req);
    int number = storage_number_at(pos);
    if (number < 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    char path[300];
    if (!storage_thumb_path(number, storage_is_dc_at(pos), path, sizeof path)) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    return send_file(req, path, "image/png");
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    static const char head[] =
        "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>PixelBoy Gallery</title><style>"
        "body{background:#111;color:#eee;font-family:sans-serif;margin:0;padding:12px}"
        "h1{font-size:16px;font-weight:normal}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(110px,1fr));gap:8px}"
        ".grid a{display:block}"
        ".grid img{width:100%;display:block;border-radius:4px;image-rendering:pixelated}"
        "</style></head><body>";
    httpd_resp_send_chunk(req, head, sizeof(head) - 1);

    int count = storage_count();
    char title[64];
    int tn = snprintf(title, sizeof title, "<h1>%d photo%s</h1><div class=grid>", count, count == 1 ? "" : "s");
    httpd_resp_send_chunk(req, title, tn);

    /* Newest first (storage_number_at() itself is oldest-first, position 0 -
     * see app_storage.h), unlike the on-device gallery. */
    char row[128];
    for (int i = count - 1; i >= 0; i--) {
        int n = snprintf(row, sizeof row, "<a href='/photo?n=%d' download><img src='/thumb?n=%d' loading=lazy></a>", i, i);
        httpd_resp_send_chunk(req, row, n);
    }

    static const char tail[] = "</div></body></html>";
    httpd_resp_send_chunk(req, tail, sizeof(tail) - 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

void wifi_gallery_start(void)
{
    if (s_active) return;
    if (!storage_ready()) {
        ESP_LOGW(TAG, "no SD card - nothing to serve");
        return;
    }
    if (!ensure_base_init()) return;
    if (!cp_power_up()) return;

    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        cp_power_down();
        return;
    }

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = WIFI_GALLERY_SSID,
            .ssid_len = sizeof(WIFI_GALLERY_SSID) - 1,
            .password = WIFI_GALLERY_PASS,
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        cp_power_down();
        return;
    }

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    /* Default (4KB) blew its stack canary (Guru Meditation: Stack protection
     * fault in task "httpd", inside snprintf) on real hardware - our
     * handlers' own snprintf() calls stacked on top of the server's request
     * parsing don't fit in that little. */
    http_cfg.stack_size = 8192;
    if (httpd_start(&s_httpd, &http_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        esp_wifi_stop();
        esp_wifi_deinit();
        cp_power_down();
        return;
    }
    httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    httpd_uri_t photo_uri = {.uri = "/photo", .method = HTTP_GET, .handler = photo_handler};
    httpd_uri_t thumb_uri = {.uri = "/thumb", .method = HTTP_GET, .handler = thumb_handler};
    httpd_register_uri_handler(s_httpd, &index_uri);
    httpd_register_uri_handler(s_httpd, &photo_uri);
    httpd_register_uri_handler(s_httpd, &thumb_uri);

    s_active = true;
    ESP_LOGW(TAG, "gallery AP up: SSID \"%s\" pass \"%s\" - browse http://192.168.4.1/", WIFI_GALLERY_SSID, WIFI_GALLERY_PASS);
}

void wifi_gallery_stop(void)
{
    if (!s_active) return;
    httpd_stop(s_httpd);
    s_httpd = NULL;
    esp_wifi_stop();
    esp_wifi_deinit();
    cp_power_down();
    s_active = false;
}
