#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gbcam.h"
#include "icons.h"

/* 240x240 LCD. The 128x112 image is shown at 2x (256x224) with 8 px cropped
 * from each side so it fills 240x224, leaving an 8 px bar above and below. */
#define DISP_W 240
#define DISP_H 240
#define DISP_IMG_Y 8

esp_err_t display_init(void);

/* Start a new frame: draws the image (shades 0..3) with the given palette. */
void display_begin_frame(const uint8_t *shades, gbcam_palette_t palette);

/* Viewfinder layout, like the Game Boy Camera's shooting screen: the photo at 2x
 * (x 16..239, y 0..223, showing source columns 8..119), a vertical brightness
 * bar on the left (+ at the top) and a horizontal contrast bar along the bottom
 * (- left, + right), drawn in the palette's shades on a dark border. The bottom-
 * left corner shows B or C for the bar the encoder currently adjusts. */
#define VF_IMG_X 16
#define VF_IMG_W 224
#define VF_IMG_H 224
/* vf_scale: VF_SCALE_CROP2X shows the photo at 2x, cropped to VF_IMG_W/2 x
 * GBCAM_H source pixels (the default, full sensor width not shown);
 * VF_SCALE_NATIVE1X shows the full 128x112 sensor image at 1x instead,
 * centred in the same image area (small letterbox bars either side);
 * VF_SCALE_FIT shows the same full image as VF_SCALE_NATIVE1X but scaled up
 * as big as it fits (area-averaged, not a crop) - the full field of view,
 * just bigger than a bare 1x. Either way the bars/corner label are
 * unchanged - only how the photo itself is laid out changes.
 *
 * adjust: which of GB's three quick-adjust targets the encoder currently
 * moves - 0 brightness, 1 contrast, 2 palette. Lights the matching bar
 * (neither, for palette) and sets the bottom-left corner letter (B/C/P).
 *
 * framed_rgb888: NULL for the plain shades-only draw above; otherwise an
 * already-composed frame_compose_rgb() canvas (see app_frames.h) to show
 * instead, centred/scaled the same way - only valid alongside
 * VF_SCALE_NATIVE1X or VF_SCALE_FIT (a frame never shows cropped, see
 * FRAME_PREVIEW_MS in app.c).
 *
 * frame_preview_active: only matters for VF_SCALE_FIT with a frame - a tall
 * ("Wild", 160x224) frame's own aspect ratio is narrower than the square
 * image area, so scaling it to show the whole thing uncropped (what
 * frame_preview_active does, true while FRAME_PREVIEW_MS's brief post-
 * change flash is up) lands the photo itself noticeably smaller than a
 * normal (160x144) frame gets. Once that expires, the steady-state view
 * instead scales by width alone (matching what a normal frame's own natural
 * fit already works out to) and centre-crops the frame's top/bottom to fit -
 * same photo size regardless of which frame is active, at the cost of
 * cropping a tall frame's own border art down to a 160x160 square. A normal
 * (144-tall) frame never needs that crop in the first place, so both phases
 * already look identical for one - no visible jump, only tall frames
 * actually zoom in after the preview window. */
typedef enum { VF_SCALE_CROP2X, VF_SCALE_NATIVE1X, VF_SCALE_FIT } vf_scale_t;
void display_begin_viewfinder(const uint8_t *shades, gbcam_palette_t palette,
                              int brightness, int brightness_max,
                              int contrast, int contrast_max, int adjust,
                              vf_scale_t vf_scale, bool frame_preview_active,
                              const uint8_t *framed_rgb888, int framed_w, int framed_h);

/* Start a new frame filled with one colour (RGB888). */
void display_begin_blank(uint8_t r, uint8_t g, uint8_t b);

/* True 1:1 (no scaling), centred - cropped if bigger than the screen,
 * letterboxed in black if smaller. See display_begin_camera() for the
 * fit-to-screen alternative. */
void display_begin_native(const uint8_t *rgb888, int w, int h);

/* Text, scaled (1 = 6x8 cell). Lower-case is drawn as upper case. */
void display_text(int x, int y, int scale, const char *s, uint8_t r, uint8_t g, uint8_t b);
int display_text_width(const char *s, int scale);

/* Filled rectangle (RGB888). */
void display_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);

/* A 16x16 icon (see icons.h / icons_data.h, generated from assets/icons
 * by tools/gen_icons.py), alpha-blitted (no blending - opaque or nothing,
 * see gen_icons.py) at x,y. invert flips every opaque pixel's colour
 * (~RGB565) - the source art is dark-on-transparent, so an unselected
 * menu row (black background) needs it to stay visible; the selected
 * row's white background doesn't. */
void display_icon(int x, int y, icon_id_t id, bool invert);

/* Dither Cam / Normal Cam preview: an already-quantized/sampled w x h RGB888
 * image (see dithercam.h's DC_SIZE_COUNT presets). With fill=false it's a
 * crop to a clean integer scale (up if it's smaller than the 240x240 screen,
 * down if bigger), never a fractional resample - alias-free regardless of
 * dither method, since there's no shifting phase between the sampling grid
 * and a periodic pattern to beat against (see display_begin_camera's
 * comment). Letterboxed in black on whichever axis has room left over.
 * With fill=true (Normal Cam's live preview - not pixel art, so there's no
 * aliasing risk to avoid) it instead area-averages the full frame, aspect
 * preserved, up to whichever of DISP_W/DISP_H it hits first - the same
 * letterboxing as fill=false, just a fractional fit instead of an integer
 * one. No bars; use display_osd() for feedback on changes. */
void display_begin_camera(const uint8_t *rgb888, int w, int h, bool fill);

/* Gallery grid view: a thumbnail album, like the real Game Boy Camera's own
 * (3x3 there; bigger cells read better on this screen, so 2x2 here).
 * display_begin_gallery_grid() clears the screen, then call
 * display_grid_cell() once per cell (row-major, 0..GALLERY_GRID_CELLS-1)
 * before display_end_frame(). A cell's rgb888/w/h may be NULL/0 to leave it
 * blank (a page with fewer than GALLERY_GRID_CELLS photos left over).
 * selected draws a highlight border around that cell. */
#define GALLERY_GRID_COLS 2
#define GALLERY_GRID_ROWS 2
#define GALLERY_GRID_CELLS (GALLERY_GRID_COLS * GALLERY_GRID_ROWS)
void display_begin_gallery_grid(void);
void display_grid_cell(int index, const uint8_t *rgb888, int w, int h, bool selected);

/* Menu box over the image: a title and up to 6 rows of "icon  label   value",
 * the selected row drawn inverted. values may be NULL, or an individual
 * value NULL, for action rows (e.g. GALLERY, EXIT). */
void display_menu(const char *title, const char *const *labels, const char *const *values,
                  const icon_id_t *icons, int count, int selected);

/* On-screen message box centred on the image, up to three lines - line3 NULL
 * (or "") for the usual one/two-line case. */
void display_osd(const char *line1, const char *line2, const char *line3);

/* Shutter-closing-then-opening animation over whatever's already drawn this
 * frame (the frozen just-taken photo) - two black curtains sliding in from
 * the top and bottom edges to meet at the centre, then back out again.
 * progress runs 0..1 across the whole animation (closed at 0.5); the caller
 * drives it (e.g. from how much of the post-shutter freeze window is left)
 * instead of this owning its own timer, so it always finishes exactly when
 * the freeze does. Draws nothing at progress <= 0 or >= 1. */
void display_shutter(float progress);

/* Send the frame to the LCD (asynchronous; double buffered). */
void display_end_frame(void);

/* The RGB565 buffer display_end_frame() just sent - whatever's actually on
 * screen right now (viewfinder, menu, gallery, OSD prompts, all of it),
 * valid until the next display_begin_*() call. Used for USB "mirror mode"
 * (see usb_webcam_feed() in app_usb.h) - not needed for normal rendering. */
const uint16_t *display_last_frame(void);

/* Panel backlight duty, 0..100 (%) - the app's idle-dim and its BACKLIGHT
 * setting both go through here (see update_backlight()/ROW_BACKLIGHT in
 * app.c). Pixel content is untouched: this is only the LED behind the panel. */
void display_set_backlight(int percent);
