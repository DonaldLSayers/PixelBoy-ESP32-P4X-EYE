#pragma once

#include "esp_err.h"

/* Set everything up (display, input, storage, camera). */
esp_err_t app_init(void);

/* One iteration of the main loop: handle input, show one frame. */
void app_step(void);
