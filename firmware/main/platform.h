/*
 * Small platform layer so the app logic (app.c, app_display.c, app_storage.c)
 * builds both for the ESP32-P4 and for the PC simulator (tools/sim).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define PLOGI(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
#define PLOGW(tag, ...) ESP_LOGW(tag, __VA_ARGS__)
#define PLOGE(tag, ...) ESP_LOGE(tag, __VA_ARGS__)
#else
#include <stdio.h>
#define PLOG_(lvl, tag, ...) (printf("%s (%s) ", lvl, tag), printf(__VA_ARGS__), printf("\n"), fflush(stdout))
#define PLOGI(tag, ...) PLOG_("I", tag, __VA_ARGS__)
#define PLOGW(tag, ...) PLOG_("W", tag, __VA_ARGS__)
#define PLOGE(tag, ...) PLOG_("E", tag, __VA_ARGS__)
#endif

int64_t plat_now_us(void);
void plat_sleep_ms(uint32_t ms);

/* Zeroed allocation in fast (internal) RAM where the platform has it. */
void *plat_calloc_fast(size_t size);

/* Mount the photo storage (SD card); writes its root path, e.g. "/sdcard". */
esp_err_t plat_storage_mount(char *root, size_t len);
int plat_mkdir(const char *path);

/* Battery charge, 0..100, or -1 if there's no battery/gauge to read (the
 * simulator, or a board run purely on USB power with nothing plugged into
 * the battery header). */
int plat_battery_percent(void);

/* Cuts power to the camera/LCD/SD card and puts the chip into deep sleep,
 * waking only on the shutter button - a no-op on the simulator (nothing to
 * power down, nothing to wake from). Never returns on real hardware: a wake
 * is a full reset, same as power-on, so there's no separate "resume" path -
 * app_init() just runs again. */
void plat_enter_deep_sleep(void);
