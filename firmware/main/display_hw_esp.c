#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "app_display.h"
#include "display_hw.h"

static const char *TAG = "display_hw";

#define NUM_FB 2
#define FB_BYTES (DISP_W * DISP_H * 2)

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static uint16_t *s_fb[NUM_FB];
static int s_cur;
static SemaphoreHandle_t s_free; /* counts framebuffers not being sent */

static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ed, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_free, &woken);
    return woken == pdTRUE;
}

esp_err_t display_hw_init(void)
{
    /* esp_lcd splits each frame into chunks of this size and signals completion
     * once, after the last chunk. Keep it under the SPI DMA per-transaction limit. */
    const bsp_display_config_t cfg = {.max_transfer_sz = DISP_W * 40 * 2};
    ESP_RETURN_ON_ERROR(bsp_display_new(&cfg, &s_panel, &s_io), TAG, "display_new");

    s_free = xSemaphoreCreateCounting(NUM_FB, NUM_FB);
    for (int i = 0; i < NUM_FB; i++) {
        s_fb[i] = heap_caps_calloc(1, FB_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ESP_RETURN_ON_FALSE(s_fb[i], ESP_ERR_NO_MEM, TAG, "no memory for framebuffer");
    }

    const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = on_trans_done};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, NULL), TAG, "callbacks");

    /* Send one black frame, then turn the panel and backlight on. */
    display_hw_present(display_hw_acquire());
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");
    return bsp_display_backlight_on();
}

uint16_t *display_hw_acquire(void)
{
    s_cur = (s_cur + 1) % NUM_FB;
    xSemaphoreTake(s_free, portMAX_DELAY);
    return s_fb[s_cur];
}

void display_hw_present(uint16_t *fb)
{
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, DISP_W, DISP_H, fb);
}
