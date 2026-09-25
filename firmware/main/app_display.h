#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gbcam.h"

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
/* native1x: false shows the photo at 2x, cropped to VF_IMG_W/2 x GBCAM_H
 * source pixels (the default); true shows the full 128x112 sensor image at
 * 1x instead, centred in the same image area. Either way the bars/corner
 * label are unchanged - only how the photo itself is laid out changes.
 *
 * adjust: which of GB's three quick-adjust targets the encoder currently
 * moves - 0 brightness, 1 contrast, 2 palette. Lights the matching bar
 * (neither, for palette) and sets the bottom-left corner letter (B/C/P). */
void display_begin_viewfinder(const uint8_t *shades, gbcam_palette_t palette,
                              int brightness, int brightness_max,
                              int contrast, int contrast_max, int adjust,
                              bool native1x);

/* Start a new frame filled with one colour (RGB888). */
void display_begin_blank(uint8_t r, uint8_t g, uint8_t b);

/* Text, scaled (1 = 6x8 cell). Lower-case is drawn as upper case. */
void display_text(int x, int y, int scale, const char *s, uint8_t r, uint8_t g, uint8_t b);
int display_text_width(const char *s, int scale);

/* Filled rectangle (RGB888). */
void display_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);

/* Dither Cam / Normal Cam preview: an already-quantized/sampled w x h RGB888
 * image (see dithercam.h's DC_SIZE_COUNT presets). Either way it's a crop to
 * a clean integer scale (up if it's smaller than the 240x240 screen, down if
 * bigger), never a fractional resample - alias-free regardless of dither
 * method, since there's no shifting phase between the sampling grid and a
 * periodic pattern to beat against (see display_begin_camera's comment).
 * Letterboxed in black on whichever axis has room left over. No bars; use
 * display_osd() for feedback on changes. */
void display_begin_camera(const uint8_t *rgb888, int w, int h);

/* Menu box over the image: a title and up to 6 rows of "label   value",
 * the selected row drawn inverted. value may be NULL for action rows. */
void display_menu(const char *title, const char *const *labels, const char *const *values,
                  int count, int selected);

/* On-screen message box centred on the image, up to two lines. */
void display_osd(const char *line1, const char *line2);

/* Send the frame to the LCD (asynchronous; double buffered). */
void display_end_frame(void);
