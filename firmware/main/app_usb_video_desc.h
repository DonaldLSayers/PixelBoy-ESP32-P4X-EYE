#pragma once

/* ESP-only: builds the composite MSC+UVC descriptor set app_usb.c installs
 * (see app_usb_video_desc.c). Not in app_usb.h - that header is also
 * compiled by the simulator, which has no tinyusb of its own. */
#include "tinyusb.h"

void usb_video_desc_init(void);                          /* call once at boot, before usb_msc_init() */
const tinyusb_config_t *usb_video_tinyusb_config(void);   /* composite MSC+video descriptor set, passed to tinyusb_driver_install() */
