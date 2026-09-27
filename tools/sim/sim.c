/*
 * PC simulator back end for the GB Camera firmware.
 *
 * Builds the real app code (firmware/main/app.c, app_display.c, app_storage.c and
 * the gbcam core) into gbcam_sim.dll, and replaces the hardware parts:
 *   display_hw  -> framebuffer read by the Python window
 *   camera      -> frames pushed from the webcam
 *   input       -> events pushed from keyboard / mouse
 *   settings    -> settings.bin file instead of NVS
 *   storage     -> a folder instead of the SD card
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define API __declspec(dllexport)
#else
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define API __attribute__((visibility("default")))
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG /* Normal Cam saves .JPG (storage_save_dc()) - see components/stb/stb_image_impl.c's comment */
#include "stb_image.h"

#include "app.h"
#include "app_camera.h"
#include "app_display.h"
#include "app_input.h"
#include "app_settings.h"
#include "camera_ppa_esp.h"
#include "app_usb.h"
#include "display_hw.h"
#include "platform.h"

static char s_data_dir[512];
static char s_sd_root[512];
static bool s_sd_present = true;

/* ------------------------------------------------------------------ esp_err */

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    default: return "UNKNOWN";
    }
}

/* ----------------------------------------------------------------- platform */

int64_t plat_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    /* now.QuadPart counts from system boot, not process start - on a PC with
     * more than ~10.7 days of uptime (at a typical 10MHz QPC frequency),
     * now.QuadPart * 1000000LL overflows int64_t and wraps negative, which
     * permanently breaks the 15fps throttle in app.c's viewfinder_frame()
     * (t0 stays negative forever, so it never clears "t0 < s_next_frame_us"
     * and every frame gets throttle-skipped - the viewfinder looks frozen on
     * whatever was last drawn, e.g. app_init()'s startup splash). Splitting
     * into whole seconds + remainder before multiplying avoids the overflow
     * for any realistic uptime. */
    int64_t whole = now.QuadPart / freq.QuadPart;
    int64_t rem = now.QuadPart % freq.QuadPart;
    return whole * 1000000LL + (rem * 1000000LL) / freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
#endif
}

void plat_sleep_ms(uint32_t ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

void *plat_calloc_fast(size_t size) { return calloc(1, size); }

int plat_mkdir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0777);
#endif
}

esp_err_t plat_storage_mount(char *root, size_t len)
{
    if (!s_sd_present) return ESP_ERR_NOT_FOUND;
    snprintf(root, len, "%s", s_sd_root);
    return ESP_OK;
}

/* No real battery to read on a PC - a fixed stand-in so the on-screen
 * indicator can actually be seen and checked in the simulator; real hardware
 * uses the actual gauge (platform_esp.c's plat_battery_percent()). */
int plat_battery_percent(void) { return 76; }

/* Nothing to power down or wake from on a PC. */
void plat_enter_deep_sleep(void) {}

/* No PPA hardware on a PC - dithercam.c's own CPU resample path is used
 * for everything in the simulator, same as it always has been. */
esp_err_t camera_ppa_init(void) { return ESP_ERR_NOT_SUPPORTED; }

/* No USB port to plug a PC into on a PC - always inert in the simulator;
 * real hardware uses app_usb.c. */
esp_err_t usb_msc_init(void) { return ESP_OK; }
void usb_msc_tick(void) {}
bool usb_msc_prompt_pending(void) { return false; }
bool usb_msc_active(void) { return false; }
void usb_msc_accept(void) {}
void usb_msc_decline(void) {}
void usb_msc_exit(void) {}
void usb_webcam_accept(void) {}
bool usb_webcam_active(void) { return false; }
void usb_webcam_exit(void) {}
void usb_webcam_feed(const uint8_t *rgb888) { (void)rgb888; }

/* --------------------------------------------------------------- display_hw */

static uint16_t s_fb[2][DISP_W * DISP_H];
static int s_back;
static uint16_t s_front[DISP_W * DISP_H];
static int s_presented;

esp_err_t display_hw_init(void)
{
    memset(s_front, 0, sizeof s_front);
    return ESP_OK;
}

uint16_t *display_hw_acquire(void)
{
    s_back ^= 1;
    return s_fb[s_back];
}

void display_hw_present(uint16_t *fb)
{
    memcpy(s_front, fb, sizeof s_front);
    s_presented++;
}

/* ------------------------------------------------------------------- camera */

static uint8_t *s_frame;
static int s_frame_w, s_frame_h, s_frame_stride;
static gbcam_pixfmt_t s_frame_format = GBCAM_FMT_GREY8;
static bool s_camera_fail;
static bool s_camera_paused;

esp_err_t camera_init(void)
{
    return s_camera_fail ? ESP_FAIL : ESP_OK;
}

/* No real sensor to sleep in the simulator - just mirrors the real driver's
 * paused/DQBUF-fails-while-off behaviour so app.c's menu/gallery pause logic
 * exercises the same code paths here as on the board. */
esp_err_t camera_pause(void) { s_camera_paused = true; return ESP_OK; }
esp_err_t camera_resume(void) { s_camera_paused = false; return ESP_OK; }

static int64_t s_process_us;

int64_t camera_last_process_us(void) { return s_process_us; }

esp_err_t camera_skip(void)
{
    if (s_camera_paused) return ESP_FAIL;
    return s_frame ? ESP_OK : ESP_FAIL;
}

esp_err_t camera_grab(camera_frame_cb_t cb, void *ctx)
{
    if (s_camera_paused || !s_frame) return ESP_FAIL;
    const gbcam_frame_t f = {
        .data = s_frame,
        .width = s_frame_w,
        .height = s_frame_h,
        .stride = s_frame_stride,
        .format = s_frame_format,
        .mirror_x = CAMERA_MIRROR_X,
        .mirror_y = CAMERA_MIRROR_Y,
    };
    int64_t t = plat_now_us();
    cb(&f, ctx);
    s_process_us = plat_now_us() - t;
    return ESP_OK;
}

/* -------------------------------------------------------------------- input */

#define QUEUE_LEN 64
static input_event_t s_queue[QUEUE_LEN];
static int s_q_head, s_q_tail;

esp_err_t input_init(void) { return ESP_OK; }

bool input_get(input_event_t *ev, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (s_q_head == s_q_tail) return false;
    *ev = s_queue[s_q_tail];
    s_q_tail = (s_q_tail + 1) % QUEUE_LEN;
    return true;
}

/* ----------------------------------------------------------------- settings */

#define SETTLE_US (2 * 1000 * 1000)
static app_settings_t s_pending;
static int64_t s_changed_at = -1;

static void settings_path(char *out, size_t len)
{
    snprintf(out, len, "%s/settings.bin", s_data_dir);
}

void settings_load(app_settings_t *s)
{
    settings_defaults(s);

    char path[600];
    settings_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    app_settings_t st;
    /* An older, shorter settings.bin reads short and falls back to defaults. */
    if (fread(&st, 1, sizeof st, f) == sizeof st && fgetc(f) == EOF && settings_valid(&st))
        *s = st;
    fclose(f);
}

void settings_changed(const app_settings_t *s)
{
    s_pending = *s;
    s_changed_at = plat_now_us();
}

void settings_tick(void)
{
    if (s_changed_at < 0 || plat_now_us() - s_changed_at < SETTLE_US) return;
    s_changed_at = -1;
    char path[600];
    settings_path(path, sizeof path);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(&s_pending, 1, sizeof s_pending, f);
    fclose(f);
    PLOGI("settings", "saved");
}

/* ---------------------------------------------------------------------- API */

/* data_dir holds settings.bin and sdcard/ (the simulated SD card). */
API int sim_init(const char *data_dir, int sd_present, int camera_fail)
{
    snprintf(s_data_dir, sizeof s_data_dir, "%s", data_dir);
    snprintf(s_sd_root, sizeof s_sd_root, "%s/sdcard", data_dir);
    plat_mkdir(s_data_dir);
    plat_mkdir(s_sd_root);
    s_sd_present = sd_present != 0;
    s_camera_fail = camera_fail != 0;
    return app_init();
}

/* 8-bit greyscale webcam frame; copied, used by the next camera_grab().
 * Kept for callers that only have a greyscale source (e.g. a locked-exposure
 * mono capture); Dither Cam / Normal Cam will show it as neutral grey, same
 * as real hardware falling back to a monochrome sensor format. */
API void sim_push_frame(const uint8_t *grey, int width, int height, int stride)
{
    size_t need = (size_t)width * height;
    if (!s_frame || width != s_frame_w || height != s_frame_h || s_frame_format != GBCAM_FMT_GREY8) {
        free(s_frame);
        s_frame = malloc(need);
        s_frame_w = width;
        s_frame_h = height;
    }
    s_frame_format = GBCAM_FMT_GREY8;
    for (int y = 0; y < height; y++)
        memcpy(s_frame + (size_t)y * width, grey + (size_t)y * stride, (size_t)width);
    s_frame_stride = width;
}

/* RGB888 webcam frame; copied, used by the next camera_grab(). This is what a
 * real ESP32-P4X-EYE frame looks like (see app_camera.c's format negotiation),
 * so use this for anything that should show correctly in all three camera
 * modes, not just sim_push_frame()'s greyscale. */
API void sim_push_frame_rgb(const uint8_t *rgb, int width, int height, int stride)
{
    size_t need = (size_t)width * height * 3;
    if (!s_frame || width != s_frame_w || height != s_frame_h || s_frame_format != GBCAM_FMT_RGB888) {
        free(s_frame);
        s_frame = malloc(need);
        s_frame_w = width;
        s_frame_h = height;
    }
    s_frame_format = GBCAM_FMT_RGB888;
    for (int y = 0; y < height; y++)
        memcpy(s_frame + (size_t)y * width * 3, rgb + (size_t)y * stride, (size_t)width * 3);
    s_frame_stride = width * 3;
}

/* type: 0 press, 1 click, 2 long press, 3 rotate. button: 0 menu, 1 mode, 2 cam mode, 3 encoder/shutter. */
API void sim_input(int type, int button, int value)
{
    int next = (s_q_head + 1) % QUEUE_LEN;
    if (next == s_q_tail) return;
    s_queue[s_q_head] = (input_event_t){.type = (input_type_t)type, .button = (button_id_t)button, .value = value};
    s_q_head = next;
}

API void sim_step(void) { app_step(); }

/* 240x240 big-endian RGB565, exactly what the LCD would receive. */
API const uint16_t *sim_framebuffer(void) { return s_front; }
API int sim_frames_presented(void) { return s_presented; }
API const char *sim_sd_root(void) { return s_sd_root; }
