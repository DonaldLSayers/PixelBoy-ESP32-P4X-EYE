#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"

#include "app_input.h"
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
 * be tuned against the actual battery on real hardware. -1 (from either
 * call) means no gauge to read - shown as unavailable, not 0%. */
int plat_battery_percent(void)
{
    static bool s_inited;
    static bool s_ok;
    if (!s_inited) {
        s_inited = true;
        esp_err_t err = bsp_voltage_init();
        s_ok = err == ESP_OK;
        if (!s_ok) PLOGW(TAG, "bsp_voltage_init failed: %s - battery indicator unavailable", esp_err_to_name(err));
    }
    if (!s_ok) return -1;

    /* Confirmed dead end, kept for whoever looks at this next: the actual
     * schematic (SCH_ESP32-P4-EYE-MB V2.3, sheet 05_PWR_USB) shows VBAT
     * through R15 (1M) and R16 (332K) to GND, tapped between them for
     * BAT_ADC - a divide ratio of 332/(1000+332) = 1/4.012, not the BSP's
     * hardcoded BSP_BATTERY_VOLTAGE_DIV (2). That math is right (read
     * straight off the resistor values), and heavy oversampling/smoothing
     * ruled out ADC noise as the culprit too - but corrected, smoothed
     * readings still didn't track a battery known to be actively charging
     * (stayed pinned low instead of climbing). That points to something
     * physical - GPIO18 not actually reaching BAT_ADC on this unit, a cold
     * joint on R15/R16, or a board defect - not something fixable from here.
     * Next step is a multimeter on the BAT_ADC test point/GPIO18 itself, not
     * another firmware guess. Disabled rather than show a number this
     * unreliable. */
    return -1;
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
    bsp_sdcard_unmount();
    bsp_feature_enable(BSP_FEATURE_SD, false);
    bsp_feature_enable(BSP_FEATURE_CAMERA, false);
    bsp_feature_enable(BSP_FEATURE_LCD, false);

    esp_deep_sleep_enable_gpio_wakeup(BIT64(PIN_BTN_SHUTTER), ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start(); /* never returns - a wake is a full reset, same as power-on */
}
