#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gbcam.h"

/* Photos live in /sdcard/GBCAM. GB Camera photos are GBnnnnn.BIN (3584-byte
 * Game Boy 2bpp tiles, palette-free, used by the gallery) plus GBnnnnn.PNG
 * (4x upscale, shareable); photos pulled out of a GB emulator .sav (see
 * app_gbemu.c's export_photos_from_sav()) are the same pair of files but
 * EMUnnnnn instead, so they're easy to tell apart on the card. Dither Cam
 * photos are DCnnnnn.PNG (full colour, nearest-neighbour upscaled by
 * DC_SAVE_SCALE - pixel art, so the upscale and PNG's lossless colours both
 * matter). Normal Cam photos are DCnnnnn.JPG instead: it's a real photo, not
 * limited-palette pixel art, so there's nothing for the 4x upscale to
 * preserve and JPEG's lossy compression costs little quality for a lot less
 * SD space. The gallery decodes either back to show them (stb_image reads
 * both), there's no separate tile form for either. All kinds share one
 * numbering sequence, so the gallery lists every photo in the order it was
 * taken regardless of which mode (or the emulator) produced it. */

esp_err_t storage_init(void);
bool storage_ready(void);
const char *storage_root(void);

/* Number of photos, and the photo number / kind at a gallery position
 * (0 = oldest). */
int storage_count(void);
int storage_number_at(int pos);
bool storage_is_dc_at(int pos);

/* Save the shades; returns the new photo number, or -1 on error. frame is
 * -1 for no border, or a frame_id_t (see frames.h) to bake one into the
 * saved .PNG (frame_compose_rgb(), scaled to GB_PNG_SCALE) - the .BIN tiles
 * the gallery reads back are always just the plain 128x112 photo, unframed.
 * prefix is "GB" for a real camera capture, or "EMU" for a photo pulled out
 * of a GB emulator .sav (see app_gbemu.c) - see the file-format comment
 * above. */
#define GB_PNG_SCALE 4
int storage_save(const uint8_t *shades, gbcam_palette_t palette, int frame, const char *prefix);

esp_err_t storage_load(int number, uint8_t *shades);

/* Decodes a saved GBnnnnn.PNG back to RGB888 for the gallery's framed 1:1
 * view - the palette and frame (if any) actually baked in at save time,
 * unlike storage_load()'s palette-free .BIN tiles. *out_rgb must be freed
 * with storage_free_dc(). */
esp_err_t storage_load_gb_png(int number, uint8_t **out_rgb, int *out_w, int *out_h);

/* Dither Cam: nearest-neighbour upscaled by DC_SAVE_SCALE and saved as PNG.
 * Normal Cam (jpeg=true): saved at its real captured size, no upscale, as a
 * quality-90 JPEG - see the file-format comment above. Returns the new photo
 * number, or -1 on error. */
#define DC_SAVE_SCALE 4
#define NORMAL_JPEG_QUALITY 90
int storage_save_dc(const uint8_t *rgb888, int w, int h, bool jpeg);

/* Decodes a saved DCnnnnn.PNG/.JPG back to RGB888 for the gallery (already at
 * its saved size - typically far bigger than the screen, so display it with
 * display_begin_camera() the same as the live Dither/Normal Cam view).
 * *out_rgb must be freed with storage_free_dc(). */
esp_err_t storage_load_dc(int number, uint8_t **out_rgb, int *out_w, int *out_h);

/* Small pre-shrunk copy for the gallery grid (see app_storage.c's
 * THUMB_MAX_DIM) - much cheaper to decode than the full photo every time a
 * thumbnail's redrawn. Generated at save time going forward; a photo saved
 * before this existed gets one generated (and cached to disk) the first
 * time it's viewed. Frees the same way as storage_load_dc(). */
esp_err_t storage_load_thumb(int number, bool is_dc, uint8_t **out_rgb, int *out_w, int *out_h);

/* Also frees a storage_load_gb_png()/storage_load_thumb() result - same
 * stbi_image_free() either way. */
void storage_free_dc(uint8_t *rgb);

esp_err_t storage_delete(int number, bool is_dc);

/* Path accessors for serving files directly over HTTP (WiFi Gallery) - no
 * decode/re-encode needed, the file's already a viewable image. Both return
 * false (path left untouched) if nothing's found.
 *   storage_photo_path()  the full photo, for download.
 *   storage_thumb_path()  the small THUMB/ grid copy (generated first if not
 *                         already cached, same fallback storage_load_thumb()
 *                         uses) - the full photos turned out too slow to
 *                         load a whole grid of them over the AP's WiFi link. */
bool storage_photo_path(int number, bool is_dc, char *out, size_t len, const char **out_content_type);
bool storage_thumb_path(int number, bool is_dc, char *out, size_t len);
