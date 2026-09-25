#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_check.h"
#include "esp_timer.h"

#include "app_input.h"

static const char *TAG = "input";

#define POLL_MS 5
#define DEBOUNCE_SAMPLES 3
#define LONG_PRESS_MS 600

static const int s_pins[BTN_COUNT] = {PIN_BTN_MENU, PIN_BTN_MODE, PIN_BTN_CAMMODE, PIN_BTN_SHUTTER};

typedef struct {
    bool pressed;
    uint8_t stable_count;
    uint32_t held_ms;
    bool long_sent;
} button_state_t;

static button_state_t s_btn[BTN_COUNT];
static QueueHandle_t s_queue;
static pcnt_unit_handle_t s_pcnt;
static int s_enc_last;

static void post(input_type_t type, button_id_t button, int value)
{
    input_event_t ev = {.type = type, .button = button, .value = value};
    xQueueSend(s_queue, &ev, 0);
}

static void poll_cb(void *arg)
{
    for (int i = 0; i < BTN_COUNT; i++) {
        button_state_t *b = &s_btn[i];
        bool down = gpio_get_level(s_pins[i]) == 0; /* active low */
        if (down != b->pressed) {
            if (++b->stable_count >= DEBOUNCE_SAMPLES) {
                if (down) post(INPUT_PRESS, (button_id_t)i, 0);
                else if (!b->long_sent) post(INPUT_CLICK, (button_id_t)i, 0);
                b->pressed = down;
                b->stable_count = 0;
                b->held_ms = 0;
                b->long_sent = false;
            }
        } else {
            b->stable_count = 0;
            if (b->pressed) {
                b->held_ms += POLL_MS;
                if (!b->long_sent && b->held_ms >= LONG_PRESS_MS) {
                    b->long_sent = true;
                    post(INPUT_LONG_PRESS, (button_id_t)i, 0);
                }
            }
        }
    }

    int count = 0;
    if (pcnt_unit_get_count(s_pcnt, &count) == ESP_OK) {
        int detents = (count - s_enc_last) / ENCODER_COUNTS_PER_DETENT;
        if (detents != 0) {
            s_enc_last += detents * ENCODER_COUNTS_PER_DETENT;
            post(INPUT_ROTATE, BTN_COUNT, detents);
        }
    }
}

static esp_err_t encoder_init(void)
{
    pcnt_unit_config_t unit_cfg = {.low_limit = -30000, .high_limit = 30000};
    unit_cfg.flags.accum_count = 1;
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_cfg, &s_pcnt), TAG, "pcnt unit");

    pcnt_glitch_filter_config_t filter = {.max_glitch_ns = 1000};
    ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(s_pcnt, &filter), TAG, "pcnt filter");

    pcnt_chan_config_t a_cfg = {.edge_gpio_num = PIN_ENCODER_A, .level_gpio_num = PIN_ENCODER_B};
    pcnt_chan_config_t b_cfg = {.edge_gpio_num = PIN_ENCODER_B, .level_gpio_num = PIN_ENCODER_A};
    pcnt_channel_handle_t ca, cb;
    ESP_RETURN_ON_ERROR(pcnt_new_channel(s_pcnt, &a_cfg, &ca), TAG, "pcnt chan a");
    ESP_RETURN_ON_ERROR(pcnt_new_channel(s_pcnt, &b_cfg, &cb), TAG, "pcnt chan b");

    /* Standard x4 quadrature decoding (IDF rotary encoder example). */
    pcnt_channel_set_edge_action(ca, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(ca, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_channel_set_edge_action(cb, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(cb, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    /* accum_count needs watch points at the limits. */
    pcnt_unit_add_watch_point(s_pcnt, unit_cfg.high_limit);
    pcnt_unit_add_watch_point(s_pcnt, unit_cfg.low_limit);

    gpio_pullup_en(PIN_ENCODER_A);
    gpio_pullup_en(PIN_ENCODER_B);

    ESP_RETURN_ON_ERROR(pcnt_unit_enable(s_pcnt), TAG, "pcnt enable");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(s_pcnt), TAG, "pcnt clear");
    return pcnt_unit_start(s_pcnt);
}

esp_err_t input_init(void)
{
    s_queue = xQueueCreate(16, sizeof(input_event_t));

    uint64_t mask = 0;
    for (int i = 0; i < BTN_COUNT; i++) mask |= 1ULL << s_pins[i];
    const gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");
    ESP_RETURN_ON_ERROR(encoder_init(), TAG, "encoder");

    const esp_timer_create_args_t targs = {.callback = poll_cb, .name = "input"};
    esp_timer_handle_t timer;
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &timer), TAG, "timer");
    return esp_timer_start_periodic(timer, POLL_MS * 1000);
}

bool input_get(input_event_t *ev, uint32_t timeout_ms)
{
    return xQueueReceive(s_queue, ev, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
