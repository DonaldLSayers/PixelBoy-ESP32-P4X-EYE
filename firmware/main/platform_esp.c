#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "app_input.h"
#include "app_usb.h"
#include "app_wifi_gallery.h"
#include "platform.h"

static const char *TAG = "platform_esp";

int64_t plat_now_us(void) { return esp_timer_get_time(); }

void plat_sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void *plat_calloc_fast(size_t size)
{
    return heap_caps_calloc(1, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

esp_err_t plat_storage_mount(char *root, size_t len)
{
    esp_err_t err = bsp_sdcard_mount();
    if (err == ESP_OK) snprintf(root, len, "%s", BSP_SD_MOUNT_POINT);
    return err;
}

int plat_mkdir(const char *path) { return mkdir(path, 0777); }

/* Simple 2-point LiPo curve (3300mV empty .. 4200mV full), linear - real
 * discharge isn't linear, but this is a reasonable placeholder until it can
 * be tuned against more real hardware. -1 (from either call) means no gauge
 * to read - shown as unavailable, not 0%.
 *
 * Three prior attempts at this (see git history) probed the wrong pin.
 * First: GPIO18 (that's BSP_LCD_CS in this same BSP header, unrelated).
 * Second: the BSP's own BSP_BATTERY_VOLTAGE_CHANNEL (ADC_CHANNEL_2 on
 * ADC_UNIT_2 = GPIO51 on ESP32-P4) - plausible since it's the BSP's declared
 * battery channel, but real hardware gave noisy, multimeter-confirmed
 * near-zero readings there, i.e. a floating pin. Third: GPIO50 (ADC2
 * channel 1), read off the schematic image (SCH_ESP32-P4-EYE-MB, sheet
 * 02_ESP32-P4X) by eye - close, but one pin off; still read low with a real,
 * confirmed-charged battery connected. Settled by sweeping every ADC2
 * channel (0-5, GPIO49-54) on hardware with the battery connected and the
 * charger LED confirmed green (full charge, ~4.2V): only channel 0
 * (GPIO49) read a plausible value (~1037mV, matching 4.2V through the
 * divider below). The BSP's BSP_BATTERY_VOLTAGE_CHANNEL constant is simply
 * wrong for this board revision, so it can't be used - ADC2 channel 0 is
 * read directly instead. Divider is R15 301K / R16 100K (schematic sheet
 * 05_PWR_USB) -> 100/401 = 1/4.01. */
#define BATTERY_ADC_CHANNEL     ADC_CHANNEL_0  /* GPIO49, BAT_ADC net - NOT BSP_BATTERY_VOLTAGE_CHANNEL */
#define BATTERY_DIVIDER_RATIO   4.01f
#define BATTERY_MV_EMPTY        3300
/* Nominal LiPo full charge is 4200mV, but this board's charger LED going
 * solid green (i.e. actually done charging, confirmed on hardware) measured
 * as 4158mV through this divider/ADC/calibration chain - close to nominal
 * but not exact, and it's this chain's own reading that has to hit 100%,
 * not the theoretical number. Topping out at the real measured value here
 * instead of 4200 is what makes a genuinely full battery show 100%. */
#define BATTERY_MV_FULL         4158
#define BATTERY_SAMPLE_COUNT    16
/* check_low_battery() polls plat_battery_percent() every viewfinder frame
 * (~15-30Hz) - re-sampling the ADC that often is wasteful and, since each
 * batch of samples has some natural spread, made the displayed number
 * visibly jitter by a point or two. Only take a fresh reading this often;
 * every call in between returns the last one. */
#define BATTERY_REFRESH_US      (4 * 1000 * 1000)

static adc_cali_handle_t s_battery_cali;
static SemaphoreHandle_t s_battery_mutex;
static int s_cached_pct = -1;
static int64_t s_cached_at_us;

static int battery_sample_percent(void)
{
    static bool s_inited;
    static bool s_ok;
    if (!s_inited) {
        s_inited = true;
        s_battery_mutex = xSemaphoreCreateMutex();
        esp_err_t err = bsp_adc_initialize();
        if (err == ESP_OK) {
            const adc_oneshot_chan_cfg_t chan_config = {
                .bitwidth = ADC_BITWIDTH_DEFAULT,
                .atten = ADC_ATTEN_DB_12,
            };
            err = adc_oneshot_config_channel(bsp_adc_get_handle(), BATTERY_ADC_CHANNEL, &chan_config);
        }
        if (err == ESP_OK) {
            const adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = BSP_ADC_UNIT,
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            err = adc_cali_create_scheme_curve_fitting(&cali_config, &s_battery_cali);
        }
        s_ok = err == ESP_OK;
        if (!s_ok) PLOGW(TAG, "battery ADC init failed: %s - battery indicator unavailable", esp_err_to_name(err));
    }
    if (!s_ok) return -1;

    /* bsp_adc_get_handle() is shared with anything else on ADC_UNIT_2, and
     * adc_oneshot_read() is not safe to call concurrently from two tasks -
     * this is called from both app.c and app_display.c, and without this
     * lock their reads interleave and corrupt each other (seen on hardware
     * as one call series pinned low while the other looked fine). */
    xSemaphoreTake(s_battery_mutex, portMAX_DELAY);
    long sum_mv = 0;
    int n = 0;
    for (int i = 0; i < BATTERY_SAMPLE_COUNT; i++) {
        int adc_raw, adc_mv;
        if (adc_oneshot_read(bsp_adc_get_handle(), BATTERY_ADC_CHANNEL, &adc_raw) != ESP_OK) continue;
        if (adc_cali_raw_to_voltage(s_battery_cali, adc_raw, &adc_mv) != ESP_OK) continue;
        sum_mv += adc_mv;
        n++;
    }
    xSemaphoreGive(s_battery_mutex);
    if (n == 0) return -1;

    float vbat_mv = (sum_mv / (float)n) * BATTERY_DIVIDER_RATIO;
    if (vbat_mv <= BATTERY_MV_EMPTY) return 0;
    if (vbat_mv >= BATTERY_MV_FULL) return 100;
    return (int)((vbat_mv - BATTERY_MV_EMPTY) * 100 / (BATTERY_MV_FULL - BATTERY_MV_EMPTY));
}

int plat_battery_percent(void)
{
    int64_t now = plat_now_us();
    if (s_cached_pct >= 0 && now - s_cached_at_us < BATTERY_REFRESH_US) return s_cached_pct;

    int pct = battery_sample_percent();
    s_cached_pct = pct;
    s_cached_at_us = now;
    return pct;
}

void plat_enter_deep_sleep(void)
{
    /* Camera and LCD share one physical power-enable pin on this board
     * (BSP_CAMERA_EN and BSP_LCD_EN are both GPIO12), so these two calls
     * gate the same rail - kept as two calls anyway since that's what the
     * BSP's own API expects, and it's harmless. SD gets its own unmount
     * (releases the on-chip LDO channel bsp_sdcard_mount() claimed) plus its
     * own enable pin (BSP_SD_EN). RTC GPIO holds keep these levels through
     * deep sleep, so the power actually stays cut, not just logically off. */
    /* Everything below only makes sense while a rail is still up, so the order
     * here is the order things stop working in, not an arbitrary one.
     *
     * The panel and its backlight are the visible half of this: bsp_display_
     * enter_sleep() is the BSP's own "you're about to lose power" call (panel
     * sleep-in command, backlight off). Cutting BSP_LCD_EN below probably
     * takes the whole rail with it, but "probably" isn't worth leaving a
     * backlight driver latched at whatever duty the last frame left it at -
     * and if the backlight boost turns out not to be on that rail at all,
     * this is the difference between a dark screen and a lit one all night. */
    bsp_display_enter_sleep();

    /* The USB PHY, unlike the rails below, has no enable pin to cut - only
     * the driver keeps it alive, so it has to be told to let go (see
     * usb_msc_deinit()). */
    usb_msc_deinit();

    bsp_sdcard_unmount();
    bsp_feature_enable(BSP_FEATURE_SD, false);
    bsp_feature_enable(BSP_FEATURE_CAMERA, false);
    bsp_feature_enable(BSP_FEATURE_LCD, false);
    /* The C6 has no power rail of its own to cut - its reset line has to be
     * held for the whole sleep instead (see app_wifi_gallery.c), which is also
     * why this is not just another bsp_feature_enable() call. */
    wifi_gallery_cp_hold_reset();

    esp_deep_sleep_enable_gpio_wakeup(BIT64(PIN_BTN_SHUTTER), ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start(); /* never returns - a wake is a full reset, same as power-on */
}
