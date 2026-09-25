#include <stdio.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"

#include "platform.h"

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
