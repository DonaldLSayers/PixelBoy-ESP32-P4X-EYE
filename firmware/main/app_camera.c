#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "linux/videodev2.h"
#include "esp_video_device.h"
#include "bsp/esp-bsp.h"

#include "app_camera.h"

static const char *TAG = "camera";

#define NUM_BUFS 2

static int s_fd = -1;
static uint8_t *s_buf[NUM_BUFS];
static uint32_t s_width, s_height, s_stride;
static gbcam_pixfmt_t s_format;
static int64_t s_process_us;
static bool s_paused;

int64_t camera_last_process_us(void) { return s_process_us; }

static void log_fourcc(const char *what, uint32_t f)
{
    ESP_LOGI(TAG, "%s: %c%c%c%c", what, (int)(f & 0xFF), (int)((f >> 8) & 0xFF),
             (int)((f >> 16) & 0xFF), (int)((f >> 24) & 0xFF));
}

static bool map_format(uint32_t fourcc, gbcam_pixfmt_t *out)
{
    switch (fourcc) {
    case V4L2_PIX_FMT_GREY:    *out = GBCAM_FMT_GREY8;     return true;
    case V4L2_PIX_FMT_RGB565:  *out = GBCAM_FMT_RGB565_LE; return true;
    case V4L2_PIX_FMT_RGB565X: *out = GBCAM_FMT_RGB565_BE; return true;
    case V4L2_PIX_FMT_RGB24:   *out = GBCAM_FMT_RGB888;    return true;
    default:                   return false;
    }
}

esp_err_t camera_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_camera_start(NULL), TAG, "bsp_camera_start");

    s_fd = open(BSP_CAMERA_DEVICE, O_RDONLY);
    ESP_RETURN_ON_FALSE(s_fd >= 0, ESP_FAIL, TAG, "open %s", BSP_CAMERA_DEVICE);

    struct v4l2_capability cap = {0};
    if (ioctl(s_fd, VIDIOC_QUERYCAP, &cap) == 0)
        ESP_LOGI(TAG, "driver %s, card %s", cap.driver, cap.card);

    struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_G_FMT, &fmt) == 0, ESP_FAIL, TAG, "G_FMT");
    ESP_LOGI(TAG, "default %" PRIu32 "x%" PRIu32, fmt.fmt.pix.width, fmt.fmt.pix.height);
    log_fourcc("default format", fmt.fmt.pix.pixelformat);

    /* Prefer colour: Dither Cam and Normal Cam need it, and the GB pipeline
     * reads luma out of RGB565 just as well as out of GREY. Only fall back
     * to GREY (half the bytes/pixel) if the sensor truly can't do colour. */
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        const uint32_t wanted[] = {V4L2_PIX_FMT_RGB565, V4L2_PIX_FMT_GREY};
        for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
            struct v4l2_format f = fmt;
            f.fmt.pix.pixelformat = wanted[i];
            if (ioctl(s_fd, VIDIOC_S_FMT, &f) == 0) break;
        }
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_G_FMT, &fmt) == 0, ESP_FAIL, TAG, "G_FMT");
    log_fourcc("capture format", fmt.fmt.pix.pixelformat);
    ESP_RETURN_ON_FALSE(map_format(fmt.fmt.pix.pixelformat, &s_format), ESP_ERR_NOT_SUPPORTED, TAG,
                        "unsupported pixel format");
    s_width = fmt.fmt.pix.width;
    s_height = fmt.fmt.pix.height;
    s_stride = fmt.fmt.pix.bytesperline;

    struct v4l2_requestbuffers req = {
        .count = NUM_BUFS,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_REQBUFS, &req) == 0, ESP_FAIL, TAG, "REQBUFS");

    for (int i = 0; i < NUM_BUFS; i++) {
        struct v4l2_buffer buf = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i};
        ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_QUERYBUF, &buf) == 0, ESP_FAIL, TAG, "QUERYBUF");
        s_buf[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd, buf.m.offset);
        ESP_RETURN_ON_FALSE(s_buf[i] != NULL, ESP_FAIL, TAG, "mmap");
        ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_QBUF, &buf) == 0, ESP_FAIL, TAG, "QBUF");
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_STREAMON, &type) == 0, ESP_FAIL, TAG, "STREAMON");
    ESP_LOGI(TAG, "streaming %" PRIu32 "x%" PRIu32 ", stride %" PRIu32, s_width, s_height, s_stride);
    return ESP_OK;
}

esp_err_t camera_pause(void)
{
    if (s_paused) return ESP_OK;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_STREAMOFF, &type) == 0, ESP_FAIL, TAG, "STREAMOFF");
    s_paused = true;
    return ESP_OK;
}

esp_err_t camera_resume(void)
{
    if (!s_paused) return ESP_OK;
    /* STREAMOFF dequeues every buffer back to driver ownership - they need
     * re-queueing before STREAMON, same as the initial setup in camera_init(). */
    for (int i = 0; i < NUM_BUFS; i++) {
        struct v4l2_buffer buf = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i};
        ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_QBUF, &buf) == 0, ESP_FAIL, TAG, "QBUF");
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_STREAMON, &type) == 0, ESP_FAIL, TAG, "STREAMON");
    s_paused = false;
    return ESP_OK;
}

esp_err_t camera_skip(void)
{
    if (s_paused) return ESP_FAIL;
    struct v4l2_buffer buf = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) return ESP_FAIL;
    return ioctl(s_fd, VIDIOC_QBUF, &buf) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t camera_grab(camera_frame_cb_t cb, void *ctx)
{
    if (s_paused) return ESP_FAIL;
    struct v4l2_buffer buf = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) {
        ESP_LOGE(TAG, "DQBUF failed");
        return ESP_FAIL;
    }

    const gbcam_frame_t frame = {
        .data = s_buf[buf.index],
        .width = (int)s_width,
        .height = (int)s_height,
        .stride = (int)s_stride,
        .format = s_format,
        .mirror_x = CAMERA_MIRROR_X,
        .mirror_y = CAMERA_MIRROR_Y,
    };
    int64_t t = esp_timer_get_time();
    cb(&frame, ctx);
    s_process_us = esp_timer_get_time() - t;

    if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGE(TAG, "QBUF failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}
