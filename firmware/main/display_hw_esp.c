#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
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
    /* Not bsp_display_new(): the BSP's own version doesn't set
     * flags.psram_dma_direct below, which matters a lot given our
     * framebuffers live in PSRAM (see the calloc() below) - without it,
     * esp_lcd_panel_io_spi silently bounce-copies every single SPI DMA
     * transaction through a freshly malloc'd internal-RAM buffer first
     * (spi_master.c's setup_dma_priv_buffer(), since the source pointer
     * isn't "DMA capable" by its definition unless this flag says to trust
     * PSRAM directly) - wasted work every frame, and it's what silently
     * failed and blanked the screen the one time something else (USB MSC)
     * was also competing for that same tiny internal-DMA pool. Setting the
     * flag removes the bounce buffer entirely, not just the USB conflict.
     * Everything below mirrors bsp_display_new()'s own steps (esp32_p4_eye.c) -
     * pin/clock constants are all public BSP macros. */
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_LCD, true), TAG, "LCD feature enable");
    ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "brightness init");

    const spi_bus_config_t buscfg = {
        .sclk_io_num = BSP_LCD_PCLK,
        .mosi_io_num = BSP_LCD_DATA0,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISP_W * 40 * 2, /* esp_lcd splits each frame into chunks this size */
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI init");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BSP_LCD_DC,
        .cs_gpio_num = BSP_LCD_CS,
        .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .flags.psram_dma_direct = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &s_io), TAG, "panel IO");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .flags.reset_active_high = 0,
        .rgb_ele_order = BSP_LCD_COLOR_SPACE,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel), TAG, "panel");
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);

    /* PSRAM, not internal SRAM: the ESP32-P4's internal DMA-capable pool is
     * small, unlike classic ESP32's much larger internal SRAM - and now that
     * flags.psram_dma_direct above tells the panel IO to DMA from it
     * directly, this is no longer just tolerated but the whole point. */
    s_free = xSemaphoreCreateCounting(NUM_FB, NUM_FB);
    for (int i = 0; i < NUM_FB; i++) {
        s_fb[i] = heap_caps_calloc(1, FB_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
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
