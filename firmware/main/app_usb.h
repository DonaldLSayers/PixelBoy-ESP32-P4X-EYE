#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_display.h" /* DISP_W/DISP_H */
#include "gbcam.h"        /* GBCAM_W/GBCAM_H - see WEBCAM_FRAME_W/H below */

/*
 * USB Mass Storage - browse SD card photos from a PC over the board's "USB"
 * port. This is a separate physical port/PHY from "Debug" (used for
 * flashing and the serial console), so this doesn't interfere with normal
 * development use of the debug port.
 *
 * A tiny placeholder drive (backed by an internal flash partition, not the
 * SD card) is exposed from boot purely to detect "something got plugged
 * into the USB port" - the real SD card can't be attached until the camera
 * app releases it (only one controller can own the physical SD bus at a
 * time), so usb_msc_accept() does that handoff only once the user confirms
 * on screen.
 */

esp_err_t usb_msc_init(void);      /* call once at boot */
/* Takes the USB device back down, for the deep-sleep path only - see the
 * implementation. Nothing needs to undo it: a wake is a full reset, so the
 * next boot installs it again. */
void usb_msc_deinit(void);
void usb_msc_tick(void);           /* call every frame - drives connect/disconnect detection */
bool usb_msc_prompt_pending(void); /* a host just connected - show the yes/no prompt */
bool usb_msc_active(void);         /* true while the real SD card is exposed to the host */
void usb_msc_accept(void);         /* hand the SD card over */
void usb_msc_decline(void);        /* dismiss the prompt, stay on the camera */
void usb_msc_exit(void);           /* force back to the camera app (e.g. Shutter pressed) */

/*
 * USB webcam (UVC) - the device always enumerates BOTH a Mass Storage
 * interface and a Video (webcam) interface at once (composite, see
 * app_usb_video_desc.c) - TinyUSB's descriptor set is fixed once installed,
 * so there's no cheap way to swap it live for one or the other. Which one
 * actually does anything is a choice made on-device at the same "USB
 * CONNECTED" prompt MSC already shows (see usb_msc_prompt_pending()):
 * accepting Photos behaves exactly as before; accepting Webcam instead
 * starts feeding the live camera view into the video interface, streamed as
 * a sequence of MJPEG frames (small/simple enough for Full Speed USB's bulk
 * endpoints - no isochronous alternate-setting interface needed). Declining
 * (or accepting Photos) leaves the video interface enumerated but idle - a
 * webcam app that opens it while in Photos mode would just see nothing,
 * same as any inactive UVC device.
 *
 * Two picks at the same prompt (see app.c's handle_viewfinder_input()):
 * CamMode-click starts Mirror, Mode-click starts GB Webcam. While either is
 * running there is no screen to leave, so Menu-hold is the off switch (see
 * usb_webcam_exit()).
 *   - Mirror: whatever's on the device's own 240x240 screen right now -
 *     viewfinder (any camera mode), the menu, the gallery, OSD prompts, all
 *     of it. Padded (not scaled) onto the fixed WEBCAM_FRAME_W/H canvas
 *     below, centred on black - see usb_webcam_feed_screen() in app.c.
 *   - GB Webcam: just the GB Camera photo itself at 3x (384x336, no padding
 *     needed - it's an exact fit), with its frame composited in if one's
 *     turned on (frame canvas scaled down to fit if it would've been
 *     bigger than 384x336, then centred/padded same as Mirror) - see
 *     usb_webcam_feed_gb() in app.c. Uses whatever's currently in
 *     gbcam_t.shades, which is only kept fresh while GB Camera is the
 *     selected camera mode (see grab_process_draw()) - picking this while
 *     PixelBoy/Digicam is selected just freezes on the last GB Camera frame.
 * Either way, app.c calls its feed function once per app_step() (after that
 * iteration's screen has drawn), which hands an RGB888 frame to
 * usb_webcam_feed() below. */
#define WEBCAM_FRAME_W (GBCAM_W * 3)
#define WEBCAM_FRAME_H (GBCAM_H * 3)
#define WEBCAM_FRAME_RATE 8
#define WEBCAM_JPEG_MAX_BYTES (WEBCAM_FRAME_W * WEBCAM_FRAME_H) /* generous MJPEG upper bound - mostly-flat pixel-art/UI screen compresses well */

/* Starts streaming over USB instead of handing off the SD card. False, having
 * started nothing, if its JPEG buffer couldn't be allocated - see the
 * implementation; the caller shows a failure message rather than a "MIRROR
 * ON" over a stream that will never send anything. */
bool usb_webcam_accept(void);
bool usb_webcam_active(void);       /* true while webcam mode is running */
/* Stop streaming, back to the camera app. Reached from Menu-hold (see app.c's
 * handle_*_input() - webcam mode has no screen of its own to exit from) and
 * from usb_msc_tick() when the host disconnects. */
void usb_webcam_exit(void);
/* Feed one RGB888 frame at WEBCAM_FRAME_W x WEBCAM_FRAME_H - call every
 * app_step() while usb_webcam_active() (see usb_webcam_feed_screen()/
 * usb_webcam_feed_gb() in app.c). JPEG-encodes and hands it to the video
 * interface if the host is actively streaming and the previous frame's
 * transfer has completed, otherwise it's a cheap no-op (no point encoding a
 * frame nothing will send). */
void usb_webcam_feed(const uint8_t *rgb888);
