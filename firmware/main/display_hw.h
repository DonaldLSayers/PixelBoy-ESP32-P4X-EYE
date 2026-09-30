/* Display back end: the LCD on the board. */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Initialise the panel, clear it and switch the backlight on. */
esp_err_t display_hw_init(void);

/* Get a 240x240 big-endian RGB565 buffer to draw the next frame into.
 * Blocks until a buffer is free. */
uint16_t *display_hw_acquire(void);

/* Show a buffer returned by display_hw_acquire(). */
void display_hw_present(uint16_t *fb);

/* Panel backlight duty, 0..100 (%). Costs real battery - the backlight is one
 * of the few loads here that's on continuously regardless of what the CPU is
 * doing, so this is what the idle-dim in app.c's update_backlight() drives. */
void display_hw_set_backlight(int percent);
