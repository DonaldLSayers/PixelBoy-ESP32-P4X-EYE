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
 * doing, so this is what app.c's update_backlight() drives. */
void display_hw_set_backlight(int percent);

/* Panel sleep-in and backlight off - the display driver's last act before deep
 * sleep cuts its rail. Nothing on the other side of it: a wake is a full
 * reset, so display_hw_init() runs again. */
void display_hw_sleep(void);
