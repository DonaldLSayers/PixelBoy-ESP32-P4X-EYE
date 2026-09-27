#pragma once
#include <stdbool.h>
#include "esp_err.h"

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
void usb_msc_tick(void);           /* call every frame - drives connect/disconnect detection */
bool usb_msc_prompt_pending(void); /* a host just connected - show the yes/no prompt */
bool usb_msc_active(void);         /* true while the real SD card is exposed to the host */
void usb_msc_accept(void);         /* hand the SD card over */
void usb_msc_decline(void);        /* dismiss the prompt, stay on the camera */
void usb_msc_exit(void);           /* force back to the camera app (e.g. Shutter pressed) */
