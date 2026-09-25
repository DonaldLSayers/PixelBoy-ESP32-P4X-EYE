#pragma once

#include <stdint.h>

#include "frames.h"
#include "gbcam.h"

/* Composites a decorative border (a frame_meta_t - either one of frames.h's
 * built-ins, or one loaded from the SD card's /frames folder at boot, see
 * app_frames_sd.h) around a GB Camera photo, recoloured with the same
 * 4-colour palette the photo itself used - ported from PixelBoy's own
 * Android app (engine/frames/Frames.kt's compose()). `shades` is GBCAM_W x
 * GBCAM_H, values 0 (lightest) .. 3 (darkest), same convention as the
 * frame's own indices, so no reversal is needed. `scale` upscales the whole
 * composited canvas by an integer factor (nearest-neighbour) - 1 for a live
 * 1:1 viewfinder preview, 4 to match the existing GB Camera PNG export size.
 *
 * `out_rgb` must be pre-sized for the frame's canvas at that scale - call
 * frame_size() first to size it. Writes out_w x out_h RGB888 pixels. */
void frame_size(const frame_meta_t *frame, int scale, int *out_w, int *out_h);
void frame_compose_rgb(const frame_meta_t *frame, const uint8_t *shades, gbcam_palette_t palette,
                       int scale, uint8_t *out_rgb);

/* Every selectable frame: frames.h's built-ins (index 0..FRAME_COUNT-1)
 * followed by whatever tools/frames_sd_init() loaded from the SD card's
 * /frames folder at boot (see app_frames_sd.h) - one combined list so the
 * menu/dial don't need to know which source a frame came from. */
int frames_total(void);
const frame_meta_t *frames_get(int index);
const char *frames_get_name(int index);
