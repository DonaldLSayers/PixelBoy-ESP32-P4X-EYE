#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* ESP32-P4X-EYE controls (pins from Espressif's P4-EYE factory-demo BSP).
 * Which physical button is which still needs confirming on the real board;
 * swap the GPIOs here if the roles land on the wrong buttons. */
/* Espressif's factory demo: button 1 (GPIO3) = Menu, 2 (GPIO4) = Up, 3 (GPIO5) = Down,
 * encoder push (GPIO2) = Select. Assumed top-to-bottom on the case; confirm on the board. */
#define PIN_BTN_MENU      3   /* top:    click open/close the menu, hold: gallery */
#define PIN_BTN_MODE      4   /* middle: click brightness <-> contrast bar */
#define PIN_BTN_CAMMODE   5   /* bottom: click GB Camera <-> Dither Cam <-> Normal Cam */
#define PIN_BTN_SHUTTER   2   /* the encoder's push switch: take a photo (select in the menu) */
#define PIN_ENCODER_A     48
#define PIN_ENCODER_B     47

/* Quadrature counts per detent (x4 decoding). Adjust if one click moves 2 steps or 0. */
#define ENCODER_COUNTS_PER_DETENT 4

typedef enum {
    BTN_MENU,
    BTN_MODE,
    BTN_CAMMODE,
    BTN_SHUTTER,   /* encoder push */
    BTN_COUNT
} button_id_t;

typedef enum {
    INPUT_PRESS,       /* button went down */
    INPUT_CLICK,       /* button released before it became a long press */
    INPUT_LONG_PRESS,  /* held for LONG_PRESS_MS (sent once, while still held) */
    INPUT_ROTATE,      /* encoder moved; value = detents (+ clockwise) */
} input_type_t;

typedef struct {
    input_type_t type;
    button_id_t button;
    int value;
} input_event_t;

esp_err_t input_init(void);

/* Wait up to timeout_ms for the next event. Returns false on timeout. */
bool input_get(input_event_t *ev, uint32_t timeout_ms);
