#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "gbcam.h"

/* Flip the image if the viewfinder comes out mirrored or upside down. */
#define CAMERA_MIRROR_X false
#define CAMERA_MIRROR_Y false

esp_err_t camera_init(void);

/* Stops streaming (V4L2 VIDIOC_STREAMOFF) - on the OV2710, this reaches the
 * sensor itself (ov2710_set_stream() writes register 0x3008 = 0x42, its
 * sleep-enable bit), not just a software pause, so it actually saves power
 * and MIPI/ISP processing while a menu or the gallery has nothing live to
 * show. camera_resume() (STREAMON) re-queues the capture buffers and wakes
 * the sensor (0x3008 = 0x02). Calling camera_grab()/camera_skip() while
 * paused fails harmlessly (ESP_FAIL) rather than crashing. */
esp_err_t camera_pause(void);
esp_err_t camera_resume(void);

/* Called with the raw camera frame while it's still valid (borrowed; don't
 * keep the pointer). Used for both the GB pipeline (gbcam_downsample) and
 * Dither Cam / Normal Cam (dc_sample / dc_sample_smooth) so there's one
 * DQBUF/QBUF path regardless of what happens with the frame. */
typedef void (*camera_frame_cb_t)(const gbcam_frame_t *frame, void *ctx);

/* Wait for the next frame, run cb on it, then return it to the driver. */
esp_err_t camera_grab(camera_frame_cb_t cb, void *ctx);

/* Wait for the next frame and hand it straight back (frame-rate pacing). */
esp_err_t camera_skip(void);

/* Time the last camera_grab() callback took, in microseconds
 * (the rest of camera_grab() is waiting for the frame). */
int64_t camera_last_process_us(void);
