#include <stdbool.h>
#include <stddef.h>

#include "app_frames.h"
#include "app_frames_sd.h"
#include "frames_data.h"

void frame_size(const frame_meta_t *frame, int scale, int *out_w, int *out_h)
{
    *out_w = frame->w * scale;
    *out_h = frame->h * scale;
}

void frame_compose_rgb(const frame_meta_t *frame, const uint8_t *shades, gbcam_palette_t palette,
                       int scale, uint8_t *out_rgb)
{
    const int canvas_w = frame->w * scale;
    const int photo_x = FRAME_PHOTO_X * scale, photo_y = frame->photo_y * scale;
    const int photo_w = FRAME_PHOTO_W * scale, photo_h = FRAME_PHOTO_H * scale;

    for (int y = 0; y < frame->h * scale; y++) {
        uint8_t *dst = out_rgb + (size_t)y * canvas_w * 3;
        bool in_photo_rows = y >= photo_y && y < photo_y + photo_h;
        const uint8_t *src_row = frame->indices + (size_t)(y / scale) * frame->w;
        for (int x = 0; x < canvas_w; x++) {
            uint8_t shade;
            if (in_photo_rows && x >= photo_x && x < photo_x + photo_w)
                shade = shades[(size_t)((y - photo_y) / scale) * FRAME_PHOTO_W + (x - photo_x) / scale];
            else
                shade = src_row[x / scale];
            const uint8_t *c = gbcam_palette_rgb(palette, shade);
            dst[x * 3] = c[0];
            dst[x * 3 + 1] = c[1];
            dst[x * 3 + 2] = c[2];
        }
    }
}

int frames_total(void)
{
    /* frames_sd_count() is 0 until the card's frames have finished loading in
     * the background (app_frames_sd.h) - so this is just the built-ins until
     * then, which is exactly what every index that has to be valid right now
     * should be measured against. */
    return FRAME_COUNT + frames_sd_count();
}

bool frame_available(int frame)
{
    /* A 0 (no frame) or an index past the end of the list - which includes
     * every SD frame while the background load is still running - would hand
     * frames_get() an entry with no pixel data, and frame_compose_rgb()
     * dereferences that. Callers that are about to draw or photograph a frame
     * that may have come from the card ask this first.
     *
     * The indices check is the same bug by the other road: an SD frame whose
     * .png is missing, corrupt, or named longer than s_filenames holds decodes
     * to NULL, and frames_sd_get() returns the entry with .indices still NULL.
     * Decoding here is not wasted work - frames_get() is exactly what the
     * caller is about to do, and frames_sd_get() caches it. */
    if (frame <= 0 || frame > frames_total()) return false;
    return frames_get(frame - 1)->indices != NULL;
}

const frame_meta_t *frames_get(int index)
{
    if (index < 0) index = 0;
    return index < FRAME_COUNT ? &frame_meta[index] : frames_sd_get(index - FRAME_COUNT);
}

const char *frames_get_name(int index)
{
    if (index < 0) index = 0;
    return index < FRAME_COUNT ? frame_names[index] : frames_sd_name(index - FRAME_COUNT);
}
