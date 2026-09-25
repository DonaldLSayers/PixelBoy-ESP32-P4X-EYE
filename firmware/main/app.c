/*
 * GB Camera app logic (platform-neutral; runs on the ESP32-P4X-EYE and in the PC simulator).
 *
 * Boots straight into a viewfinder with three camera modes (Bottom button cycles):
 *   GB CAMERA    the Game Boy Camera look (PIXEL CAM / HARDWARE style), with
 *                on-screen brightness/contrast bars like the real camera
 *   PIXELBOY     PIXEL CAM's Dither Cam: arbitrary palette + dither pattern
 *   DIGICAM      plain colour preview, with brightness/contrast
 *
 * Controls:
 *   Encoder press    take a photo (shutter)
 *   Encoder turn     adjust whichever of the two settings is active (see Mode)
 *   Mode button      click: switch which setting the encoder adjusts
 *                    hold (GB Camera only): style, PIXEL CAM <-> HARDWARE
 *   Menu button      click: open/close the menu   hold: gallery
 *   Bottom button    click: next camera mode
 * In the menu: encoder turn moves the selection, encoder press activates the
 * row (cycles its value, or runs Gallery/Exit). The gallery lists every
 * photo from every mode, oldest first: encoder scrolls, encoder press goes
 * back, Menu button also goes back, and clicking the Menu button twice
 * deletes the current photo.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dithercam.h"
#include "gbcam.h"
#include "app.h"
#include "app_camera.h"
#include "app_display.h"
#include "app_frames.h"
#include "app_frames_sd.h"
#include "app_input.h"
#include "app_settings.h"
#include "app_storage.h"
#include "platform.h"

static const char *TAG = "gbcam";

#define OSD_MS 1200
#define OSD_BRIEF_MS 700
#define FREEZE_MS 600
#define DELETE_CONFIRM_MS 3000
#define FRAME_PREVIEW_MS 2000

typedef enum { SCREEN_VIEWFINDER, SCREEN_MENU, SCREEN_GALLERY } screen_t;
typedef enum { ADJUST_0, ADJUST_1, ADJUST_2, ADJUST_3 } adjust_t;

typedef struct {
    char line1[24];
    char line2[24];
    int64_t until_us;
} osd_t;

static gbcam_t *s_cam;
static app_settings_t s_set;
static screen_t s_screen = SCREEN_VIEWFINDER;
static adjust_t s_adjust = ADJUST_0;
static osd_t s_osd;

/* Dither Cam / Normal Cam working image: sized for the largest preset
 * (DC_MAX_W x DC_MAX_H), reused at whatever size is actually selected -
 * Normal Cam has its own "digicam" size list, normal_size() in dithercam.h. */
static uint8_t *s_dc_rgb;
static int s_dc_w, s_dc_h;

static uint8_t s_still[GBCAM_PIXELS];              /* frozen GB photo / gallery photo */
static uint8_t *s_still_rgb;                       /* frozen Dither/Normal Cam photo, DC_MAX_W*DC_MAX_H*3 */
static int64_t s_freeze_until_us;
static int64_t s_frame_preview_until_us;            /* see ROW_FRAME in activate_menu_row() */
static uint8_t s_frame_rgb[160 * 224 * 3];           /* GB Camera framed viewfinder/export canvas, worst case (Wild) */
static int s_gallery_pos;
static bool s_gallery_dirty;
static int64_t s_delete_armed_until_us;

/* ------------------------------------------------------------------- menu */

typedef enum {
    ROW_PALETTE, ROW_DITHER, ROW_STYLE, ROW_VF_SCALE, ROW_FRAME, ROW_DC_PALETTE, ROW_DC_METHOD, ROW_DC_SIZE,
    ROW_DC_AMOUNT,
    ROW_NORMAL_SIZE,
    ROW_DENOISE, ROW_GALLERY, ROW_EXIT
} menu_row_t;

static menu_row_t s_menu_rows[8];
static int s_menu_count;
static int s_menu_sel;

static int64_t now_us(void) { return plat_now_us(); }

static void osd_text(const char *l1, const char *l2)
{
    snprintf(s_osd.line1, sizeof s_osd.line1, "%s", l1 ? l1 : "");
    snprintf(s_osd.line2, sizeof s_osd.line2, "%s", l2 ? l2 : "");
    s_osd.until_us = now_us() + OSD_MS * 1000LL;
}

/* Short single-line message (e.g. which setting the encoder now adjusts). */
static void osd_brief(const char *l1)
{
    osd_text(l1, NULL);
    s_osd.until_us = now_us() + OSD_BRIEF_MS * 1000LL;
}

static void draw_osd(void)
{
    if (now_us() > s_osd.until_us) return;
    display_osd(s_osd.line1, s_osd.line2);
}

static void apply_settings(void)
{
    s_cam->settings.brightness = s_set.brightness;
    s_cam->settings.contrast = s_set.contrast;
    s_cam->settings.dither = (gbcam_dither_t)s_set.dither;
    s_cam->settings.style = (gbcam_style_t)s_set.style;
    s_cam->settings.denoise = s_set.denoise;
    gbcam_update_matrix(s_cam);
    settings_changed(&s_set);
}

/* Solid black strips behind white text, top and bottom. Used by the gallery
 * screen, which shows photos of any kind (GB Camera's own palette, or a
 * Dither/Normal Cam photo's arbitrary colours) - a fixed black/white overlay
 * stays readable regardless of what's under it, unlike the old palette-tinted
 * version this replaced. */
static void draw_gallery_overlay(const char *top_left, const char *top_right, const char *bottom)
{
    display_rect(0, 0, DISP_W, 8, 0, 0, 0);
    display_rect(0, DISP_H - 8, DISP_W, 8, 0, 0, 0);
    if (top_left) display_text(2, 0, 1, top_left, 255, 255, 255);
    if (top_right) display_text(DISP_W - 2 - display_text_width(top_right, 1), 0, 1, top_right, 255, 255, 255);
    if (bottom) display_text(2, DISP_H - 8, 1, bottom, 255, 255, 255);
}

/* Maps our 0..16 / 0..15 brightness/contrast fields onto a contrast pivot and
 * a gamma curve, the same shape gbcam_pixelcam.c uses for its brightness
 * bias. Used for Normal Cam's preview, where there's no ROM-style auto
 * exposure to lean on. */
static void brightness_contrast_to_levels(float *contrast, float *gamma)
{
    *contrast = 0.5f + (s_set.contrast / (float)(GBCAM_CONTRAST_LEVELS - 1)) * 1.5f;
    float g = powf(2.0f, (8 - (int)s_set.brightness) / 8.0f);
    *gamma = g < 0.25f ? 0.25f : (g > 4.0f ? 4.0f : g);
}

/* ------------------------------------------------------------------ actions */

static void take_photo(void)
{
    s_freeze_until_us = now_us() + FREEZE_MS * 1000LL;
    int n;
    if (s_set.cam_mode == CAM_MODE_GB) {
        memcpy(s_still, s_cam->shades, GBCAM_PIXELS);
        int frame = s_set.frame == 0 ? -1 : (int)s_set.frame - 1;
        n = storage_ready() ? storage_save(s_still, (gbcam_palette_t)s_set.palette, frame) : -1;
    } else {
        memcpy(s_still_rgb, s_dc_rgb, (size_t)s_dc_w * s_dc_h * 3);
        bool jpeg = s_set.cam_mode == CAM_MODE_NORMAL;
        n = storage_ready() ? storage_save_dc(s_still_rgb, s_dc_w, s_dc_h, jpeg) : -1;
    }

    if (!storage_ready()) {
        osd_text("NO SD CARD", NULL);
        return;
    }
    char buf[24];
    if (n < 0) {
        osd_text("SAVE FAILED", NULL);
    } else {
        snprintf(buf, sizeof buf, "#%d", n);
        osd_text("SAVED", buf);
    }
}

static void enter_gallery(void)
{
    if (!storage_ready()) {
        osd_text("NO SD CARD", NULL);
        return;
    }
    if (storage_count() == 0) {
        osd_text("NO PHOTOS", NULL);
        return;
    }
    camera_pause();  /* nothing live to show browsing photos - see app_camera.h */
    s_screen = SCREEN_GALLERY;
    s_gallery_pos = storage_count() - 1;
    s_gallery_dirty = true;
    s_delete_armed_until_us = 0;
}

static void leave_gallery(void)
{
    camera_resume();
    s_screen = SCREEN_VIEWFINDER;
    s_osd.until_us = 0;
}

/* --------------------------------------------------------------------- menu */

/* Menu row values (a longer frame/palette/pack name in particular) can run
 * past what's legible at the menu's fixed scale=1 font - truncate to what
 * the values[] buffer actually holds (see draw_menu()) rather than the
 * tighter 8 chars this used to use, so a name gets to use the row's real
 * width instead of being cut short of it. */
static void truncate_value(char *out, const char *in)
{
    snprintf(out, 12, "%s", in);
}

static void build_menu(void)
{
    s_menu_count = 0;
    if (s_set.cam_mode == CAM_MODE_GB) {
        s_menu_rows[s_menu_count++] = ROW_PALETTE;
        s_menu_rows[s_menu_count++] = ROW_DITHER;
        s_menu_rows[s_menu_count++] = ROW_STYLE;
        s_menu_rows[s_menu_count++] = ROW_VF_SCALE;
        s_menu_rows[s_menu_count++] = ROW_FRAME;
    } else if (s_set.cam_mode == CAM_MODE_DITHER) {
        s_menu_rows[s_menu_count++] = ROW_DC_PALETTE;
        s_menu_rows[s_menu_count++] = ROW_DC_METHOD;
        s_menu_rows[s_menu_count++] = ROW_DC_SIZE;
        s_menu_rows[s_menu_count++] = ROW_DC_AMOUNT;
    } else { /* CAM_MODE_NORMAL */
        s_menu_rows[s_menu_count++] = ROW_NORMAL_SIZE;
    }
    s_menu_rows[s_menu_count++] = ROW_DENOISE;
    s_menu_rows[s_menu_count++] = ROW_GALLERY;
    s_menu_rows[s_menu_count++] = ROW_EXIT;
    if (s_menu_sel >= s_menu_count) s_menu_sel = s_menu_count - 1;
}

static void enter_menu(void)
{
    camera_pause();  /* nothing live to show behind a full-screen menu - see app_camera.h */
    build_menu();
    s_menu_sel = 0;
    s_screen = SCREEN_MENU;
}

static void draw_menu(void)
{
    static const char *const dc_amount_labels[] = {"0%", "25%", "50%", "75%", "100%"};

    char labels[8][12], values[8][12];
    const char *label_ptrs[8], *value_ptrs[8];
    icon_id_t icons[8];
    for (int i = 0; i < s_menu_count; i++) {
        const char *val = NULL;
        switch (s_menu_rows[i]) {
        case ROW_PALETTE:
            snprintf(labels[i], sizeof labels[i], "PALETTE");
            truncate_value(values[i], gbcam_palette_name((gbcam_palette_t)s_set.palette));
            val = values[i];
            icons[i] = ICON_PALETTE;
            break;
        case ROW_DITHER:
            snprintf(labels[i], sizeof labels[i], "DITHER");
            truncate_value(values[i], gbcam_dither_name((gbcam_dither_t)s_set.dither));
            val = values[i];
            icons[i] = ICON_DITHER;
            break;
        case ROW_STYLE:
            snprintf(labels[i], sizeof labels[i], "STYLE");
            truncate_value(values[i], s_set.style == GBCAM_STYLE_PIXELCAM ? "PIXEL" : "HW");
            val = values[i];
            icons[i] = ICON_STYLE;
            break;
        case ROW_VF_SCALE:
            snprintf(labels[i], sizeof labels[i], "SCALE");
            snprintf(values[i], sizeof values[i], "%s", s_set.vf_scale ? "1:1" : "2X CROP");
            val = values[i];
            icons[i] = ICON_SCALE;
            break;
        case ROW_FRAME:
            snprintf(labels[i], sizeof labels[i], "FRAME");
            truncate_value(values[i], s_set.frame == 0 ? "NONE" : frames_get_name(s_set.frame - 1));
            val = values[i];
            icons[i] = ICON_FRAME;
            break;
        case ROW_DC_PALETTE:
            snprintf(labels[i], sizeof labels[i], "PALETTE");
            truncate_value(values[i], dc_palette_name(s_set.dc_palette));
            val = values[i];
            icons[i] = ICON_PALETTE;
            break;
        case ROW_DC_METHOD:
            snprintf(labels[i], sizeof labels[i], "METHOD");
            truncate_value(values[i], dc_method_name((dc_method_t)s_set.dc_method));
            val = values[i];
            icons[i] = ICON_METHOD;
            break;
        case ROW_DC_SIZE: {
            int w, h;
            dc_size(s_set.dc_size, &w, &h);
            snprintf(labels[i], sizeof labels[i], "SIZE");
            snprintf(values[i], sizeof values[i], "%dX%d", w, h);
            val = values[i];
            icons[i] = ICON_SIZE;
            break;
        }
        case ROW_DC_AMOUNT: {
            int idx = (int)(s_set.dc_amount * 4.0f + 0.5f);
            snprintf(labels[i], sizeof labels[i], "AMOUNT");
            val = dc_amount_labels[idx > 4 ? 4 : idx];
            icons[i] = ICON_AMOUNT;
            break;
        }
        case ROW_NORMAL_SIZE:
            snprintf(labels[i], sizeof labels[i], "SIZE");
            truncate_value(values[i], normal_size_name(s_set.normal_size));
            val = values[i];
            icons[i] = ICON_SIZE;
            break;
        case ROW_DENOISE:
            snprintf(labels[i], sizeof labels[i], "DENOISE");
            snprintf(values[i], sizeof values[i], "%d", s_set.denoise);
            val = values[i];
            icons[i] = ICON_DENOISE;
            break;
        case ROW_GALLERY:
            snprintf(labels[i], sizeof labels[i], "GALLERY");
            icons[i] = ICON_GALLERY;
            break;
        case ROW_EXIT:
            snprintf(labels[i], sizeof labels[i], "EXIT");
            icons[i] = ICON_EXIT;
            break;
        }
        label_ptrs[i] = labels[i];
        value_ptrs[i] = val;
    }
    const char *title = s_set.cam_mode == CAM_MODE_GB     ? "GB CAMERA"
                       : s_set.cam_mode == CAM_MODE_DITHER ? "PIXELBOY"
                                                            : "DIGICAM";
    display_menu(title, label_ptrs, value_ptrs, icons, s_menu_count, s_menu_sel);
}

static void activate_menu_row(void)
{
    switch (s_menu_rows[s_menu_sel]) {
    case ROW_PALETTE:
        s_set.palette = (uint8_t)((s_set.palette + 1) % GBCAM_PALETTE_COUNT);
        apply_settings();
        break;
    case ROW_DITHER:
        s_set.dither = (uint8_t)((s_set.dither + 1) % GBCAM_DITHER_COUNT);
        apply_settings();
        break;
    case ROW_STYLE:
        s_set.style = (uint8_t)((s_set.style + 1) % GBCAM_STYLE_COUNT);
        apply_settings();
        break;
    case ROW_VF_SCALE:
        s_set.vf_scale = (uint8_t)(s_set.vf_scale ? 0 : 1);
        settings_changed(&s_set);
        break;
    case ROW_FRAME:
        s_set.frame = (uint8_t)((s_set.frame + 1) % (frames_total() + 1));
        settings_changed(&s_set);
        s_frame_preview_until_us = now_us() + FRAME_PREVIEW_MS * 1000LL;
        break;
    case ROW_DC_PALETTE:
        s_set.dc_palette = (uint8_t)((s_set.dc_palette + 1) % DC_PALETTE_COUNT);
        settings_changed(&s_set);
        break;
    case ROW_DC_METHOD:
        s_set.dc_method = (uint8_t)((s_set.dc_method + 1) % DC_METHOD_COUNT);
        settings_changed(&s_set);
        break;
    case ROW_DC_SIZE:
        s_set.dc_size = (uint8_t)((s_set.dc_size + 1) % DC_SIZE_COUNT);
        settings_changed(&s_set);
        break;
    case ROW_DC_AMOUNT: {
        int idx = (int)(s_set.dc_amount * 4.0f + 0.5f);
        idx = (idx + 1) % 5;
        s_set.dc_amount = idx / 4.0f;
        settings_changed(&s_set);
        break;
    }
    case ROW_NORMAL_SIZE:
        s_set.normal_size = (uint8_t)((s_set.normal_size + 1) % NORMAL_SIZE_COUNT);
        settings_changed(&s_set);
        break;
    case ROW_DENOISE:
        s_set.denoise = (uint8_t)((s_set.denoise + 1) % 4);
        apply_settings();
        break;
    case ROW_GALLERY:
        s_screen = SCREEN_VIEWFINDER; /* enter_gallery() switches it again if it succeeds */
        enter_gallery();
        return;
    case ROW_EXIT:
        break;
    }
    if (s_menu_rows[s_menu_sel] == ROW_EXIT) {
        camera_resume();
        s_screen = SCREEN_VIEWFINDER;
    }
}

static void handle_menu_input(const input_event_t *ev)
{
    if (ev->type == INPUT_ROTATE) {
        int v = s_menu_sel + ev->value;
        s_menu_sel = v < 0 ? 0 : v > s_menu_count - 1 ? s_menu_count - 1 : v;
    } else if (ev->type == INPUT_PRESS && ev->button == BTN_SHUTTER) {
        activate_menu_row();
    } else if (ev->type == INPUT_CLICK && ev->button == BTN_MENU) {
        camera_resume();
        s_screen = SCREEN_VIEWFINDER;
    }
}

/* -------------------------------------------------------------- viewfinder */

static void cycle_cam_mode(void)
{
    s_set.cam_mode = (uint8_t)((s_set.cam_mode + 1) % CAM_MODE_COUNT);
    settings_changed(&s_set);
    s_adjust = ADJUST_0;
    osd_text(s_set.cam_mode == CAM_MODE_GB       ? "GB CAMERA"
             : s_set.cam_mode == CAM_MODE_DITHER ? "PIXELBOY"
                                                  : "DIGICAM",
             NULL);
}

/* Encoder turn outside the menu: what it adjusts depends on the camera mode
 * and which of the two settings Mode-button-click selected. */
static void rotate_viewfinder(int detents)
{
    if (s_set.cam_mode == CAM_MODE_GB) {
        if (s_adjust == ADJUST_0) {
            int v = s_set.brightness + detents;
            s_set.brightness = (uint8_t)(v < 0 ? 0 : v > GBCAM_BRIGHTNESS_LEVELS - 1 ? GBCAM_BRIGHTNESS_LEVELS - 1 : v);
            apply_settings(); /* no OSD: the bars on screen show the change */
        } else if (s_adjust == ADJUST_1) {
            int v = s_set.contrast + detents;
            s_set.contrast = (uint8_t)(v < 0 ? 0 : v > GBCAM_CONTRAST_LEVELS - 1 ? GBCAM_CONTRAST_LEVELS - 1 : v);
            apply_settings(); /* no OSD: the bars on screen show the change */
        } else if (s_adjust == ADJUST_2) {
            int v = (int)s_set.palette + detents;
            int n = GBCAM_PALETTE_COUNT;
            s_set.palette = (uint8_t)(((v % n) + n) % n);
            apply_settings();
            osd_brief(gbcam_palette_name((gbcam_palette_t)s_set.palette));
        } else {
            int v = (int)s_set.frame + detents;
            int n = frames_total() + 1;
            s_set.frame = (uint8_t)(((v % n) + n) % n);
            settings_changed(&s_set);
            s_frame_preview_until_us = now_us() + FRAME_PREVIEW_MS * 1000LL;
            osd_brief(s_set.frame == 0 ? "NONE" : frames_get_name(s_set.frame - 1));
        }
    } else if (s_set.cam_mode == CAM_MODE_DITHER) {
        if (s_adjust == ADJUST_0) {
            int idx = (int)(s_set.dc_amount * 4.0f + 0.5f) + detents;
            idx = idx < 0 ? 0 : idx > 4 ? 4 : idx;
            s_set.dc_amount = idx / 4.0f;
            char buf[8];
            snprintf(buf, sizeof buf, "%d%%", idx * 25);
            osd_brief(buf);
        } else if (s_adjust == ADJUST_1) {
            int v = (int)s_set.dc_palette + detents;
            int n = DC_PALETTE_COUNT;
            s_set.dc_palette = (uint8_t)(((v % n) + n) % n);
            osd_brief(dc_palette_name(s_set.dc_palette));
        } else if (s_adjust == ADJUST_2) {
            int v = (int)s_set.dc_size + detents;
            int n = DC_SIZE_COUNT;
            s_set.dc_size = (uint8_t)(((v % n) + n) % n);
            int w, h;
            dc_size(s_set.dc_size, &w, &h);
            char buf[16];
            snprintf(buf, sizeof buf, "%dX%d", w, h);
            osd_brief(buf);
        } else {
            int v = (int)s_set.dc_method + detents;
            int n = DC_METHOD_COUNT;
            s_set.dc_method = (uint8_t)(((v % n) + n) % n);
            osd_brief(dc_method_name((dc_method_t)s_set.dc_method));
        }
        settings_changed(&s_set);
    } else { /* CAM_MODE_NORMAL */
        if (s_adjust == ADJUST_0) {
            int v = s_set.brightness + detents;
            s_set.brightness = (uint8_t)(v < 0 ? 0 : v > GBCAM_BRIGHTNESS_LEVELS - 1 ? GBCAM_BRIGHTNESS_LEVELS - 1 : v);
        } else if (s_adjust == ADJUST_1) {
            int v = s_set.contrast + detents;
            s_set.contrast = (uint8_t)(v < 0 ? 0 : v > GBCAM_CONTRAST_LEVELS - 1 ? GBCAM_CONTRAST_LEVELS - 1 : v);
        } else {
            int v = (int)s_set.normal_size + detents;
            int n = NORMAL_SIZE_COUNT;
            s_set.normal_size = (uint8_t)(((v % n) + n) % n);
            osd_brief(normal_size_name(s_set.normal_size));
        }
        settings_changed(&s_set);
    }
}

static void handle_viewfinder_input(const input_event_t *ev)
{
    switch (ev->type) {
    case INPUT_PRESS:
        if (ev->button == BTN_SHUTTER) take_photo();
        break;
    case INPUT_CLICK:
        if (ev->button == BTN_MODE) {
            /* GB Camera and Dither Cam have four quick-adjust targets; Normal
             * Cam has three (BRIGHTNESS/CONTRAST/SIZE). */
            int count = s_set.cam_mode == CAM_MODE_NORMAL ? 3 : 4;
            s_adjust = (adjust_t)((s_adjust + 1) % count);
            if (s_set.cam_mode == CAM_MODE_DITHER)
                osd_brief(s_adjust == ADJUST_0 ? "AMOUNT" : s_adjust == ADJUST_1 ? "PALETTE"
                        : s_adjust == ADJUST_2 ? "SIZE" : "METHOD");
            else if (s_set.cam_mode == CAM_MODE_GB)
                osd_brief(s_adjust == ADJUST_0 ? "BRIGHTNESS" : s_adjust == ADJUST_1 ? "CONTRAST"
                        : s_adjust == ADJUST_2 ? "PALETTE" : "FRAME");
            else
                osd_brief(s_adjust == ADJUST_0 ? "BRIGHTNESS" : s_adjust == ADJUST_1 ? "CONTRAST" : "SIZE");
        } else if (ev->button == BTN_MENU) {
            enter_menu();
        } else if (ev->button == BTN_CAMMODE) {
            cycle_cam_mode();
        }
        break;
    case INPUT_LONG_PRESS:
        if (ev->button == BTN_MODE && s_set.cam_mode == CAM_MODE_GB) {
            s_set.style = (uint8_t)((s_set.style + 1) % GBCAM_STYLE_COUNT);
            apply_settings();
            osd_text("STYLE", gbcam_style_name((gbcam_style_t)s_set.style));
        } else if (ev->button == BTN_MENU) {
            enter_gallery();
        }
        break;
    case INPUT_ROTATE:
        rotate_viewfinder(ev->value);
        break;
    }
}

static void handle_gallery_input(const input_event_t *ev)
{
    if (ev->type == INPUT_ROTATE) {
        int pos = s_gallery_pos + ev->value;
        int max = storage_count() - 1;
        s_gallery_pos = pos < 0 ? 0 : pos > max ? max : pos;
        s_delete_armed_until_us = 0;
        s_osd.until_us = 0;
        s_gallery_dirty = true;
        return;
    }
    if (ev->type == INPUT_PRESS && ev->button == BTN_SHUTTER) {
        leave_gallery();
    } else if (ev->type == INPUT_LONG_PRESS && ev->button == BTN_MENU) {
        leave_gallery();
    } else if (ev->type == INPUT_CLICK && ev->button == BTN_MENU) {
        if (now_us() < s_delete_armed_until_us) {
            storage_delete(storage_number_at(s_gallery_pos), storage_is_dc_at(s_gallery_pos));
            s_delete_armed_until_us = 0;
            if (storage_count() == 0) {
                leave_gallery();
                osd_text("DELETED", "NO PHOTOS");
                return;
            }
            if (s_gallery_pos >= storage_count()) s_gallery_pos = storage_count() - 1;
            osd_text("DELETED", NULL);
        } else {
            s_delete_armed_until_us = now_us() + DELETE_CONFIRM_MS * 1000LL;
            snprintf(s_osd.line1, sizeof s_osd.line1, "DELETE?");
            snprintf(s_osd.line2, sizeof s_osd.line2, "PRESS AGAIN");
            s_osd.until_us = s_delete_armed_until_us;
        }
        s_gallery_dirty = true;
    }
}

/* -------------------------------------------------------------------- loops */

/* Viewfinder rate. Every camera frame is still taken from the driver (so the
 * picture is always the newest one), but only this many per second are
 * processed and drawn; the rest are handed straight back. */
#define TARGET_FPS 15
#define FRAME_INTERVAL_US (1000000 / TARGET_FPS)

/* Per-stage timing, logged every STATS_FRAMES processed frames. */
#define STATS_FRAMES 60
static struct {
    int64_t t0, wait, process_cam, process_look, draw;
    int frames, skipped;
} s_stats;
static int64_t s_next_frame_us;

static void log_stats(void)
{
    int64_t t = now_us();
    double n = s_stats.frames, secs = (double)(t - s_stats.t0) / 1e6;
    const char *mode = s_set.cam_mode == CAM_MODE_GB       ? gbcam_style_name(s_cam->settings.style)
                       : s_set.cam_mode == CAM_MODE_DITHER ? "PIXELBOY"
                                                            : "DIGICAM";
    PLOGI(TAG, "%.1f fps (%d skipped) | per frame: wait %.1f ms, sample %.1f ms, look %.1f ms, draw %.1f ms | %s",
          n / secs, s_stats.skipped, s_stats.wait / n / 1000.0, s_stats.process_cam / n / 1000.0,
          s_stats.process_look / n / 1000.0, s_stats.draw / n / 1000.0, mode);
    memset(&s_stats, 0, sizeof s_stats);
    s_stats.t0 = t;
}

static void gb_frame_cb(const gbcam_frame_t *f, void *ctx) { gbcam_downsample((gbcam_t *)ctx, f); }

typedef struct {
    uint8_t *rgb;
    int w, h;
    bool smooth;
} dc_ctx_t;

static void dc_frame_cb(const gbcam_frame_t *f, void *ctx)
{
    dc_ctx_t *c = (dc_ctx_t *)ctx;
    if (c->smooth) dc_sample_smooth(f, c->rgb, c->w, c->h);
    else dc_sample(f, c->rgb, c->w, c->h);
}

static void viewfinder_frame(void)
{
    int64_t t0 = now_us();
    if (t0 < s_next_frame_us) {
        if (camera_skip() != ESP_OK) plat_sleep_ms(5);
        s_stats.skipped++;
        return;
    }
    s_next_frame_us = (s_next_frame_us && t0 - s_next_frame_us < FRAME_INTERVAL_US)
                          ? s_next_frame_us + FRAME_INTERVAL_US : t0 + FRAME_INTERVAL_US;

    if (s_screen == SCREEN_MENU) {
        /* Camera is paused for the menu (see enter_menu()) - camera_grab()
         * below would just fail every time, so skip straight to drawing it;
         * the menu is full-screen and paints over the whole framebuffer
         * itself, so there's no image to put behind it. */
        display_begin_blank(0, 0, 0);
        draw_menu();
        display_end_frame();
        return;
    }

    esp_err_t err;
    if (s_set.cam_mode == CAM_MODE_GB) {
        err = camera_grab(gb_frame_cb, s_cam);
    } else {
        bool normal = s_set.cam_mode == CAM_MODE_NORMAL;
        if (normal) normal_size(s_set.normal_size, &s_dc_w, &s_dc_h);
        else dc_size(s_set.dc_size, &s_dc_w, &s_dc_h);
        dc_ctx_t ctx = {.rgb = s_dc_rgb, .w = s_dc_w, .h = s_dc_h, .smooth = normal};
        err = camera_grab(dc_frame_cb, &ctx);
    }
    if (err != ESP_OK) {
        plat_sleep_ms(10);
        return;
    }
    int64_t t1 = now_us();

    bool frozen = now_us() < s_freeze_until_us;
    if (s_set.cam_mode == CAM_MODE_GB) {
        gbcam_process_luma(s_cam);
    } else if (s_set.cam_mode == CAM_MODE_DITHER) {
        dc_quantize(s_dc_rgb, s_dc_w, s_dc_h, s_set.dc_palette, (dc_method_t)s_set.dc_method,
                   s_set.dc_amount, 1.0f, 1.0f, s_dc_rgb, NULL, NULL);
    } else {
        float contrast, gamma;
        brightness_contrast_to_levels(&contrast, &gamma);
        dc_levels(s_dc_rgb, s_dc_w, s_dc_h, contrast, gamma);
    }
    int64_t t2 = now_us();

    if (s_set.cam_mode == CAM_MODE_GB) {
        const uint8_t *shades = frozen ? s_still : s_cam->shades;
        /* The frame only shows at 1:1 (see FRAME_PREVIEW_MS above): a brief
         * flash at 1:1 when you just changed it even in 2x mode, so you can
         * see the new choice without leaving the crop you're framing with. */
        bool native1x = s_set.vf_scale != 0 || now_us() < s_frame_preview_until_us;
        bool show_frame = native1x && s_set.frame != 0;
        int framed_w = 0, framed_h = 0;
        if (show_frame) {
            const frame_meta_t *fm = frames_get(s_set.frame - 1);
            frame_size(fm, 1, &framed_w, &framed_h);
            frame_compose_rgb(fm, shades, (gbcam_palette_t)s_set.palette, 1, s_frame_rgb);
        }
        display_begin_viewfinder(shades, (gbcam_palette_t)s_set.palette,
                                 s_set.brightness, GBCAM_BRIGHTNESS_LEVELS - 1,
                                 s_set.contrast, GBCAM_CONTRAST_LEVELS - 1, (int)s_adjust,
                                 native1x, show_frame ? s_frame_rgb : NULL, framed_w, framed_h);
    } else {
        display_begin_camera(frozen ? s_still_rgb : s_dc_rgb, s_dc_w, s_dc_h,
                             s_set.cam_mode == CAM_MODE_NORMAL);
    }
    if (s_screen == SCREEN_MENU) draw_menu(); else draw_osd();
    display_end_frame();

    int64_t proc = camera_last_process_us();
    s_stats.process_cam += proc;
    s_stats.wait += (t1 - t0) - proc;
    s_stats.process_look += t2 - t1;
    s_stats.draw += now_us() - t2;
    if (++s_stats.frames == STATS_FRAMES) log_stats();
}

static void gallery_frame(void)
{
    bool osd_visible = now_us() <= s_osd.until_us;
    if (!s_gallery_dirty && !osd_visible) {
        plat_sleep_ms(20);
        return;
    }
    if (s_delete_armed_until_us && now_us() >= s_delete_armed_until_us) s_delete_armed_until_us = 0;

    int number = storage_number_at(s_gallery_pos);
    bool is_dc = storage_is_dc_at(s_gallery_pos);

    if (is_dc) {
        uint8_t *rgb;
        int w, h;
        if (storage_load_dc(number, &rgb, &w, &h) == ESP_OK) {
            display_begin_camera(rgb, w, h, false);
            storage_free_dc(rgb);
        } else {
            display_begin_blank(0, 0, 0);
        }
    } else {
        if (storage_load(number, s_still) != ESP_OK) memset(s_still, 0, sizeof s_still);
        /* Palette-free tiles, shown in whatever palette is currently
         * selected - not necessarily the one the photo was taken in. */
        display_begin_frame(s_still, (gbcam_palette_t)s_set.palette);
    }

    char right[16], bottom[24];
    snprintf(right, sizeof right, "%d/%d", s_gallery_pos + 1, storage_count());
    snprintf(bottom, sizeof bottom, "%s #%d", is_dc ? "PIXELBOY/DIGICAM" : "GB CAMERA", number);
    draw_gallery_overlay("GALLERY", right, bottom);
    draw_osd();
    display_end_frame();

    /* Keep redrawing while an OSD is up so it disappears on time. */
    s_gallery_dirty = osd_visible;
    plat_sleep_ms(20);
}

/* frames_sd_init()'s progress callback - shown while it's converting a new
 * ROM/pack (the first time only; every boot after that just loads the
 * cached .png, see app_frames_sd.h). */
static void frames_boot_progress(const char *l1, const char *l2)
{
    display_begin_blank(0, 0, 0);
    display_text(DISP_W / 2 - display_text_width(l1, 2) / 2, 100, 2, l1, 255, 255, 255);
    if (l2) display_text(DISP_W / 2 - display_text_width(l2, 1) / 2, 130, 1, l2, 200, 200, 200);
    display_end_frame();
}

esp_err_t app_init(void)
{
    settings_load(&s_set);

    s_cam = plat_calloc_fast(sizeof(gbcam_t));
    s_dc_rgb = calloc(1, (size_t)DC_MAX_W * DC_MAX_H * 3);
    s_still_rgb = calloc(1, (size_t)DC_MAX_W * DC_MAX_H * 3);
    if (!s_cam || !s_dc_rgb || !s_still_rgb) {
        PLOGE(TAG, "no memory");
        return ESP_ERR_NO_MEM;
    }
    gbcam_settings_t gs;
    gbcam_default_settings(&gs);
    /* 4x4 samples per output pixel: with temporal denoise this is plenty, and it
     * reads a quarter of the camera frame from PSRAM compared to 8x8. */
    gs.max_samples = 4;
    gbcam_init(s_cam, &gs);
    apply_settings();
    normal_size(s_set.normal_size, &s_dc_w, &s_dc_h);

    esp_err_t err = display_init();
    if (err != ESP_OK) {
        PLOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        return err;
    }
    display_begin_blank(0, 0, 0);
    display_text(DISP_W / 2 - display_text_width("GB CAMERA", 3) / 2, 100, 3, "GB CAMERA", 255, 255, 255);
    display_end_frame();

    err = input_init();
    if (err != ESP_OK) {
        PLOGE(TAG, "input init failed: %s", esp_err_to_name(err));
        return err;
    }
    storage_init(); /* runs without an SD card; photos are just unavailable */
    frames_sd_init(frames_boot_progress); /* SD card's /FRAMES and /ROMS, if any - see app_frames_sd.h */
    if (s_set.frame > (uint8_t)frames_total()) s_set.frame = 0; /* SD content may have changed since this was saved */

    err = camera_init();
    if (err != ESP_OK) {
        PLOGE(TAG, "camera init failed: %s", esp_err_to_name(err));
        display_begin_blank(0, 0, 0);
        display_text(20, 110, 2, "CAMERA ERROR", 255, 80, 80);
        display_end_frame();
        return err;
    }

    s_stats.t0 = now_us();
    return ESP_OK;
}

void app_step(void)
{
    input_event_t ev;
    while (input_get(&ev, 0)) {
        if (s_screen == SCREEN_VIEWFINDER) handle_viewfinder_input(&ev);
        else if (s_screen == SCREEN_MENU) handle_menu_input(&ev);
        else handle_gallery_input(&ev);
    }

    if (s_screen == SCREEN_GALLERY) gallery_frame();
    else viewfinder_frame(); /* also drives SCREEN_MENU, so the feed keeps live behind it */

    settings_tick();
}
