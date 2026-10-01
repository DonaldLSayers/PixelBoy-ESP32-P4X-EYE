/*
 * GB Camera app logic.
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
 * row (cycles its value, or runs Gallery/Exit). The gallery opens on a 2x2
 * thumbnail grid of every photo from every mode, oldest first: encoder
 * scrolls, Menu button (top) selects - drills into the highlighted photo, or
 * backs out to the grid from a single photo. A single GB Camera photo opens
 * 2x cropped with any frame it was saved with stripped back out (matching
 * the viewfinder's own default view); Mode button (middle) cycles that to
 * 1x with its frame (if it was saved with one), then to fit-to-screen (same,
 * just scaled up as big as it goes instead of exact native size), then back
 * to 2x cropped. Bottom button: delete (click twice to confirm). Shutter
 * (encoder press) always leaves the gallery entirely and goes back to the
 * camera, from any view.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_system.h"
#endif
#include <string.h>

#include "dithercam.h"
#include "gbcam.h"
#include "app.h"
#include "app_camera.h"
#include "camera_ppa_esp.h"
#include "app_display.h"
#include "app_frames.h"
#include "app_frames_sd.h"
#include "app_gbemu.h"
#include "app_input.h"
#include "app_palettes_sd.h"
#include "app_settings.h"
#include "app_storage.h"
#include "app_usb.h"
#include "app_wifi_gallery.h"
#include "platform.h"

static const char *TAG = "gbcam";

#define OSD_MS 1200
#define OSD_BRIEF_MS 700
#define FREEZE_MS 600
#define DELETE_CONFIRM_MS 3000
#define FRAME_PREVIEW_MS 2000

typedef enum { SCREEN_VIEWFINDER, SCREEN_MENU, SCREEN_GALLERY, SCREEN_USB, SCREEN_WIFI } screen_t;
typedef enum { ADJUST_0, ADJUST_1, ADJUST_2, ADJUST_3, ADJUST_4, ADJUST_5 } adjust_t;

typedef struct {
    char line1[24];
    char line2[24];
    int64_t until_us;
} osd_t;

static gbcam_t *s_cam;
/* RGB palette (see ROW_PALETTE's sentinel value, rgb_mode_active()): the
 * same GB Camera pipeline (exposure, edge enhancement, threshold dither) run
 * independently on the sensor's red/green/blue channels instead of luma -
 * the digital equivalent of the real camera's physical-filter trick, minus
 * the physical filters. Own gbcam_t per channel so each gets its own
 * auto-exposure, same as each real filtered exposure would. */
static gbcam_t *s_cam_r, *s_cam_g, *s_cam_b;
static uint8_t *s_rgb_mode_rgb; /* combined RGB888 result, GBCAM_W x GBCAM_H x 3 */
static app_settings_t s_set;
static screen_t s_screen = SCREEN_VIEWFINDER;
static adjust_t s_adjust = ADJUST_0;
static osd_t s_osd;

/* Dither Cam / Normal Cam working image: sized for the largest preset
 * (DC_MAX_W x DC_MAX_H), reused at whatever size is actually selected -
 * Normal Cam has its own "digicam" size list, normal_size() in dithercam.h.
 * s_dc_w/h is always the full selected preset (what gets saved); the live
 * s_dc_rgb buffer itself may be smaller - see s_dc_live_w/h below. */
static uint8_t *s_dc_rgb;
static int s_dc_w, s_dc_h;
/* Normal Cam's live preview size, capped well below its largest presets
 * (480p/720p) so the viewfinder stays smooth at any preset - see
 * normal_live_size()'s comment. Same as s_dc_w/h for Dither Cam (already
 * small at every size) and whenever the selected preset is already under
 * the cap. take_photo() still captures at the full s_dc_w/h, straight from
 * a fresh camera frame (dc_capture_cb) - what you see live is a smaller
 * preview of the same framing, not what actually gets saved. */
static int s_dc_live_w, s_dc_live_h;

static uint8_t *s_still;                           /* GB photo capture buffer, staged for storage_save(), GBCAM_PIXELS */
static uint8_t *s_still_rgb;                       /* Dither/Normal Cam capture buffer, DC_MAX_W*DC_MAX_H*3 */
static float *s_dc_quant_work;                     /* dc_quantize()'s scratch buffer, DC_MAX_W*DC_MAX_H*3 floats -
                                                     * preallocated so the live viewfinder (PixelBoy) doesn't
                                                     * malloc/free it every single frame (dc_quantize() mallocs
                                                     * its own when passed NULL here, up to ~690KB at 15fps). */
static uint8_t *s_webcam_rgb;                       /* USB webcam output canvas, WEBCAM_FRAME_W*WEBCAM_FRAME_H*3 - see usb_webcam_feed_screen()/usb_webcam_feed_gb() */
static uint8_t *s_screen_rgb;                        /* RGB565->RGB888 scratch for mirror mode, DISP_W*DISP_H*3 - see usb_webcam_feed_screen() */
static uint8_t *s_frame_compose_scratch;             /* frame_compose_rgb()'s output before centring into s_webcam_rgb - see usb_webcam_feed_gb() */
static bool s_webcam_mirror;                         /* which usb_webcam_feed_*() app_step() calls - see handle_viewfinder_input()'s USB prompt */
static int64_t s_freeze_until_us;                  /* shutter animation window - see take_photo()/viewfinder_frame() */
static int64_t s_frame_preview_until_us;            /* see ROW_FRAME in activate_menu_row() */
static int64_t s_last_input_us;                    /* see ROW_SLEEP/app_step() */
static uint8_t *s_frame_rgb;                         /* GB Camera framed viewfinder/export canvas, worst case (Wild), 160*224*3 */
static int s_gallery_pos;
static bool s_gallery_dirty;
static bool s_gallery_grid = true;  /* Game Boy Camera's own album view - opens here, drill in with Menu */
/* single GB photo view: Mode button cycles GALLERY_VIEW_CROP2X (default,
 * no frame) -> GALLERY_VIEW_NATIVE1X (with frame, if saved with one) ->
 * GALLERY_VIEW_FIT (as big as it fits on screen, still with frame if the
 * photo has one) -> back to CROP2X. */
typedef enum { GALLERY_VIEW_CROP2X, GALLERY_VIEW_NATIVE1X, GALLERY_VIEW_FIT, GALLERY_VIEW_COUNT } gallery_view_t;
static gallery_view_t s_gallery_view;
static uint8_t *s_gallery_plain_rgb; /* scratch: a saved GB photo with any frame stripped back out, GBCAM_W*GBCAM_H*3 */
static int64_t s_delete_armed_until_us;

/* Decoded grid-thumbnail cache - see gallery_grid_frame(). */
static uint8_t *s_grid_cache_rgb[GALLERY_GRID_CELLS];
static int s_grid_cache_w[GALLERY_GRID_CELLS], s_grid_cache_h[GALLERY_GRID_CELLS];
static int s_grid_cache_page = -1;

/* ------------------------------------------------------- deep-sleep resume */

/* What the device should come back to after a deep-sleep wake.

 * A wake is a reset, so this file's own state machine is back at its cold-boot
 * default - the live viewfinder - by the time the user is looking at the screen
 * again, never whatever they were actually in the middle of. The camera modes
 * need none of this (they are SCREEN_VIEWFINDER, and cam_mode plus the per-mode
 * adjust target are already in the NVS settings blob), so what this covers is
 * what would otherwise be lost: the menu's position, the photo being browsed,
 * and - the one that matters most - a running GB ROM, which the emulator's own
 * .state file can already restore frame-exactly but which nothing would ever
 * relaunch.

 * RTC_DATA_ATTR is what makes the lifetime right. It puts the record in the
 * LP/RTC domain, the only thing still powered through deep sleep, so it survives
 * a wake - and since the chip has no battery of its own it is lost on a real
 * power-off, i.e. turning the device off starts it clean. (IDF re-loads this
 * segment from the image on every other reset - panic, watchdog, esp_restart -
 * so it cannot survive those either, which is what keeps the magic below an
 * honest check.) */
#define RESUME_MAGIC   0x52534D45u /* "RSME" */
#define RESUME_VERSION 1u
enum { RESUME_NONE = 0, RESUME_VIEWFINDER, RESUME_MENU, RESUME_GALLERY, RESUME_GBEMU };

typedef struct {
    uint32_t magic;       /* written last, checked first - a partial write is refused */
    uint16_t version;
    uint8_t kind;         /* RESUME_* */
    uint8_t gallery_grid;
    int32_t menu_sel;
    int32_t gallery_pos;
    int32_t gallery_view; /* gallery_view_t */
    int32_t emu_slot;
    char emu_rom[300];    /* deliberately rom_entry_t.path's own size (app_gbemu.c) */
} resume_rec_t;

static RTC_DATA_ATTR resume_rec_t s_resume;

/* The emulator's half of that, in ordinary RAM: run_rom() is the only thing
 * that knows its ROM and slot, app_enter_sleep() is what writes the record, and
 * app_resume_note_gbemu() is how the two are introduced (see app.h). Cleared by
 * the sleep that consumes it, so a later sleep from the camera app does not
 * resume a ROM that has since been exited. */
static bool s_resume_gbemu;
static char s_resume_rom[300];
static int s_resume_slot;
/* Whether that session's .state actually made it to the card - reported on the
 * sleep screen, since a sleep without it comes back to the ROM's title screen
 * with nothing else to say why (see app_resume_note_gbemu()). */
static bool s_resume_state_saved;

/* ------------------------------------------------------------------- menu */

typedef enum {
    ROW_PALETTE, ROW_DITHER, ROW_STYLE, ROW_VF_SCALE, ROW_FRAME, ROW_GB_AUTO, ROW_GB_AEB, ROW_DC_PALETTE, ROW_DC_METHOD,
    ROW_DC_SIZE,
    ROW_DC_AMOUNT,
    ROW_DC_AUTO,
    ROW_DC_EDGE,
    ROW_NORMAL_SIZE,
    ROW_BACKLIGHT,
    ROW_STANDBY,
    ROW_SLEEP,
    ROW_GALLERY, ROW_WIFI, ROW_EXIT
} menu_row_t;

/* Auto-sleep timeout choices (see SLEEP_OPTIONS_COUNT in app_settings.h,
 * ROW_SLEEP below, and app_step()'s idle check) - 0 = never. */
static const int SLEEP_MINUTES[SLEEP_OPTIONS_COUNT] = {0, 1, 2, 3, 5, 10};

/* LCD backlight duty (%) choices - see BACKLIGHT_OPTIONS_COUNT in
 * app_settings.h and ROW_BACKLIGHT below. Deliberately tops out at 100%, the
 * fixed level this always ran at before the setting existed, and 25% is the
 * lowest step that still reads easily outdoors-ish rather than a token "dim"
 * that's useless in daylight. */
static const int BACKLIGHT_PERCENT[BACKLIGHT_OPTIONS_COUNT] = {25, 50, 75, 100};

static menu_row_t s_menu_rows[13]; /* GB Camera's 7 mode rows (adding ROW_GB_AEB) + 6 shared rows */
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
    /* Top-to-bottom button order (MENU/MODE/CAMMODE - see app_input.h), not
     * action order - matches the physical layout so the prompt reads the
     * same order the buttons sit in, one per line. */
    if (usb_msc_prompt_pending()) { display_osd("MENU=DRIVE", "MODE=GB", "BTM=MIRROR"); return; }
    if (now_us() > s_osd.until_us) return;
    display_osd(s_osd.line1, s_osd.line2, NULL);
}

static void apply_settings_to(gbcam_t *cam)
{
    cam->settings.brightness = s_set.brightness;
    cam->settings.contrast = s_set.contrast;
    cam->settings.dither = (gbcam_dither_t)s_set.dither;
    cam->settings.style = (gbcam_style_t)s_set.style;
    cam->settings.auto_levels = s_set.gb_auto;
    if (!s_set.gb_auto) {
        /* Only the contrast half - brightness is applied afterward either
         * way, from settings.brightness above (see
         * gbcam_process_pixelcam()'s bias step), so folding it in here too
         * would double it up. */
        cam->settings.levels_contrast = 0.5f + (s_set.contrast / (float)(GBCAM_CONTRAST_LEVELS - 1)) * 1.5f;
        cam->settings.levels_gamma = 1.0f;
    }
    gbcam_update_matrix(cam);
}

static void apply_settings(void)
{
    apply_settings_to(s_cam);
    /* Keep the RGB palette's three per-channel instances (s_cam_r/g/b) in
     * lockstep with s_cam's own settings - only their gbcam_frame_t.channel
     * differs (see rgb_mode_active()'s callers). Lazily allocated (see
     * rgb_mode_ensure_ready()), so still NULL for anyone who's never
     * selected RGB this session - nothing to sync yet. */
    if (s_cam_r) {
        apply_settings_to(s_cam_r);
        apply_settings_to(s_cam_g);
        apply_settings_to(s_cam_b);
    }
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

/* PixelBoy's own auto-exposure (ROW_DC_AUTO) - not the same search-for-most-
 * detail approach GB Camera's PIXEL CAM style uses (gbcam_process_pixelcam.c's
 * auto_levels - scores 35 candidate curves against the dithered output's
 * entropy), which doesn't translate cleanly to Dither Cam's arbitrary
 * palette/quantizer. Simpler and cheaper: measure the live frame's average
 * luma and solve for the gamma that would pull it to a mid-grey target
 * ((mean/255)^(1/gamma) = target/255), smoothed frame to frame so it doesn't
 * hunt visibly. Contrast stays on the manual dial either way - this only
 * takes over brightness. */
#define DC_AUTO_TARGET_MEAN 128.0f
#define DC_AUTO_SMOOTHING 0.1f
static float dc_auto_compute_gamma(const uint8_t *rgb, int w, int h)
{
    static float s_gamma = 1.0f;
    long sum = 0;
    int n = w * h;
    for (int i = 0; i < n; i++) {
        const uint8_t *p = rgb + (size_t)i * 3;
        sum += (p[0] * 77 + p[1] * 151 + p[2] * 28) >> 8; /* ITU-R BT.601 luma */
    }
    float mean = (float)sum / n;
    if (mean < 1.0f) mean = 1.0f;
    if (mean > 254.0f) mean = 254.0f;
    float target = logf(DC_AUTO_TARGET_MEAN / 255.0f) / logf(mean / 255.0f);
    target = target < 0.3f ? 0.3f : (target > 3.0f ? 3.0f : target);
    s_gamma += (target - s_gamma) * DC_AUTO_SMOOTHING;
    return s_gamma;
}

/* GB Camera's M64282FP edge enhancement (apply_edge() in gbcam.c), generalized
 * from its fixed-size 1-channel shades[] to PixelBoy's variable-size RGB888
 * buffer: 2-D Laplacian on luma, same delta added to all 3 channels so colour
 * is preserved, each channel clamped independently instead of the original's
 * symmetric clamp around a grey midpoint (there's no "centre" to clamp around
 * once colour's involved). Runs before dc_quantize() so the sharpened edges
 * still go through dithering/palette mapping like everything else. */
#define DC_EDGE_RATIO_PCT 100
/* Unsharp mask, not a raw Laplacian tap: a 1-pixel-wide kernel reacts to
 * sensor noise exactly as strongly as to a real edge, which is what made the
 * first version (and its dead-zone-threshold patch) come out grainy instead
 * of sharp - GB Camera's own apply_edge() gets away with the raw tap only
 * because it runs on an already heavily quantized 4-shade image, not a live
 * noisy sensor feed. Blurring first averages the single-pixel noise down
 * before the edge is measured, while a real edge (many pixels wide) survives
 * the blur mostly intact - so the difference (luma - blurred) isolates real
 * edges far more cleanly than a small-kernel Laplacian ever can. */
static void edge_enhance_rgb(uint8_t *rgb, int w, int h)
{
    static uint8_t *s_luma, *s_blur;
    static int s_cap;
    int n = w * h;
    if (s_cap < n) {
        free(s_luma);
        free(s_blur);
        s_luma = malloc(n);
        s_blur = malloc(n);
        s_cap = (s_luma && s_blur) ? n : 0;
    }
    if (!s_luma || !s_blur) return;
    for (int i = 0; i < n; i++) {
        const uint8_t *p = rgb + (size_t)i * 3;
        s_luma[i] = (uint8_t)((p[0] * 77 + p[1] * 151 + p[2] * 28) >> 8);
    }
    /* 3x3 box blur, edge pixels clamped to the nearest in-bounds row/col. */
    for (int y = 0; y < h; y++) {
        const uint8_t *up = s_luma + (y > 0 ? y - 1 : y) * w;
        const uint8_t *mid = s_luma + y * w;
        const uint8_t *dn = s_luma + (y < h - 1 ? y + 1 : y) * w;
        uint8_t *out = s_blur + y * w;
        for (int x = 0; x < w; x++) {
            int lx = x > 0 ? x - 1 : x, rx = x < w - 1 ? x + 1 : x;
            int sum = up[lx] + up[x] + up[rx] + mid[lx] + mid[x] + mid[rx] + dn[lx] + dn[x] + dn[rx];
            out[x] = (uint8_t)(sum / 9);
        }
    }
    for (int i = 0; i < n; i++) {
        int delta = ((int)s_luma[i] - (int)s_blur[i]) * DC_EDGE_RATIO_PCT / 100;
        if (!delta) continue;
        uint8_t *px = rgb + (size_t)i * 3;
        for (int c = 0; c < 3; c++) {
            int v = px[c] + delta;
            px[c] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}

/* Normal Cam's live preview, capped to roughly 360p's pixel count (640x360 =
 * 230,400px) regardless of the selected preset, so 480p/720p stay smooth to
 * look through - those presets are still what actually gets saved (see
 * s_dc_live_w/h's comment), just not what the CPU has to touch every single
 * live frame. Keeps the preset's own aspect ratio (so the live view frames
 * the same shot the photo will be, just smaller) rather than hardcoding
 * 640x360 itself, which would only be right for the 16:9 presets. A preset
 * already at or under the cap (320x240, 360p itself) is used as-is. */
#define DC_LIVE_MAX_PIXELS (640 * 360)
static void normal_live_size(int preset_w, int preset_h, int *out_w, int *out_h)
{
    long px = (long)preset_w * preset_h;
    if (px <= DC_LIVE_MAX_PIXELS) {
        *out_w = preset_w;
        *out_h = preset_h;
        return;
    }
    float scale = sqrtf((float)DC_LIVE_MAX_PIXELS / (float)px);
    *out_w = (int)(preset_w * scale + 0.5f);
    *out_h = (int)(preset_h * scale + 0.5f);
    if (*out_w < 1) *out_w = 1;
    if (*out_h < 1) *out_h = 1;
}

/* ------------------------------------------------------------------ actions */

typedef struct {
    uint8_t *rgb;
    int w, h;
    bool denoise;
} dc_ctx_t;

static void gb_frame_cb(const gbcam_frame_t *f, void *ctx) { gbcam_downsample((gbcam_t *)ctx, f); }

/* RGB palette selected (see rgb_mode_active()) - three downsample passes
 * over the same grabbed frame, one per colour channel, instead of gb_frame_cb's
 * one luma pass. ctx unused (always s_cam_r/g/b, not whatever camera_grab()
 * was handed as ctx). */
static void gb_frame_cb_rgb(const gbcam_frame_t *f, void *ctx)
{
    (void)ctx;
    gbcam_frame_t cf = *f;
    cf.channel = GBCAM_CHANNEL_RED;   gbcam_downsample(s_cam_r, &cf);
    cf.channel = GBCAM_CHANNEL_GREEN; gbcam_downsample(s_cam_g, &cf);
    cf.channel = GBCAM_CHANNEL_BLUE;  gbcam_downsample(s_cam_b, &cf);
}

/* ROW_PALETTE cycles through gbcam_palette_count() real palettes plus this
 * one extra sentinel slot ("RGB" - see draw_menu()/activate_menu_row()),
 * rather than teaching the gbcam library's palette abstraction (a plain
 * shade -> colour lookup) about a mode that doesn't fit it: three
 * independent channels, not one shade. gbcam_palette_rgb() itself tolerates
 * being called with this out-of-range value regardless (falls back to the
 * default palette), so accidentally leaving a call site unguarded degrades
 * rather than crashes.
 *
 * The s_cam_r check matters at boot: s_set.palette is persisted, so a saved
 * RGB selection reloads as the sentinel value before anything's called
 * rgb_mode_ensure_ready() this session (that only happens from the menu/
 * quick-select actually cycling onto it - see activate_menu_row()/
 * rotate_viewfinder()). Without this check the very first viewfinder frame
 * would read s_cam_r/g/b as NULL and crash - which then reboots straight
 * back into the same persisted setting, a boot loop. Falling back to the
 * plain GB pipeline instead (with the sentinel value harmlessly hitting
 * gbcam_palette_rgb()'s own fallback above) lets the device boot; cycling
 * PALETTE again properly allocates and switches over. */
static bool rgb_mode_active(void)
{
    return s_set.cam_mode == CAM_MODE_GB && s_set.palette == (uint8_t)gbcam_palette_count() && s_cam_r;
}

/* s_cam_r/g/b/s_rgb_mode_rgb are allocated on first use, not at boot like
 * s_cam - s_cam itself uses plat_calloc_fast() (internal RAM, for the tight
 * per-pixel dither/exposure loop's sake), and this board's internal RAM is
 * tight enough (~220KB free after boot) that three more gbcam_t (~70KB each)
 * there would starve every other allocation, whether or not anyone ever
 * selects RGB. Plain calloc() instead - PSRAM-backed automatically past 16KB
 * (see app_init()'s own comment on s_still/s_frame_rgb/etc) - a bit slower
 * per pixel, an acceptable trade for a mode that's already paying 3x the
 * pipeline cost. Called from activate_menu_row() the moment PALETTE cycles
 * onto the RGB sentinel, so grab_process_draw()/take_photo() can assume
 * these are non-NULL whenever rgb_mode_active() is true. */
static bool rgb_mode_ensure_ready(void)
{
    if (s_cam_r) return true;
    gbcam_t *r = calloc(1, sizeof(gbcam_t));
    gbcam_t *g = calloc(1, sizeof(gbcam_t));
    gbcam_t *b = calloc(1, sizeof(gbcam_t));
    uint8_t *rgb = calloc(1, (size_t)GBCAM_W * GBCAM_H * 3);
    if (!r || !g || !b || !rgb) {
        free(r); free(g); free(b); free(rgb);
        return false;
    }
    s_cam_r = r;
    s_cam_g = g;
    s_cam_b = b;
    s_rgb_mode_rgb = rgb;
    gbcam_settings_t gs;
    gbcam_default_settings(&gs);
    gs.max_samples = 4; /* matches s_cam's own init in app_init() */
    gbcam_init(s_cam_r, &gs);
    gbcam_init(s_cam_g, &gs);
    gbcam_init(s_cam_b, &gs);
    apply_settings_to(s_cam_r);
    apply_settings_to(s_cam_g);
    apply_settings_to(s_cam_b);
    return true;
}

/* Combines s_cam_r/g/b's independently-dithered 0..3 shades into one
 * GBCAM_W x GBCAM_H RGB888 image, GB Camera's 4 levels per channel instead
 * of its usual 4-colour palette. gbcam's shade convention is 0 = white
 * (brightest) .. 3 = black (darkest) - see gbcam.h - the opposite of a
 * colour channel's own 0 = off .. 255 = full intensity, so each shade is
 * inverted (3 - shade) before scaling up to 0/85/170/255. */
static void rgb_mode_combine(void)
{
    for (int i = 0; i < GBCAM_PIXELS; i++) {
        s_rgb_mode_rgb[i * 3 + 0] = (uint8_t)((3 - s_cam_r->shades[i]) * 85);
        s_rgb_mode_rgb[i * 3 + 1] = (uint8_t)((3 - s_cam_g->shades[i]) * 85);
        s_rgb_mode_rgb[i * 3 + 2] = (uint8_t)((3 - s_cam_b->shades[i]) * 85);
    }
}

/* Live preview, both Dither Cam and Normal Cam: plain nearest-neighbour
 * sampling (dc_sample(), not dc_sample_smooth()) - cheap enough to stay
 * smooth at any Normal Cam size preset, up to 720p. Normal Cam's own
 * antialiasing happens once, at capture time, instead - see
 * dc_capture_cb() below; the live preview being a plainer resample doesn't
 * affect the saved photo's quality. */
static void dc_frame_cb(const gbcam_frame_t *f, void *ctx)
{
    dc_ctx_t *c = (dc_ctx_t *)ctx;
    dc_sample(f, c->rgb, c->w, c->h);
    /* Dither Cam only - see dc_temporal_denoise()'s comment in dithercam.h
     * for why. Runs on the raw sample, before dc_quantize(). Error diffusion
     * (Floyd-Steinberg/Atkinson/Sierra Lite, the last three dc_method_t
     * values) propagates one flipped pixel's error to everything after it
     * in scan order, so it needs stronger smoothing than Bayer/none to stay
     * stable. */
    if (c->denoise) {
        bool diffusion = s_set.dc_method >= DC_METHOD_FLOYD_STEINBERG;
        dc_temporal_denoise(c->rgb, c->w, c->h, diffusion ? 3 : 2);
    }
}

/* Normal Cam capture only (take_photo()): a fresh full-resolution frame
 * (the live preview may be a smaller capped size - see s_dc_live_w/h),
 * still nearest-neighbour like the live preview. Used to be
 * dc_sample_smooth() (box-averaged, antialiased) instead, on the reasoning
 * that a real photo deserves the quality the live preview no longer pays
 * for every frame - true in principle, but at 720p that box-average is the
 * same per-pixel PSRAM-bound cost that made the live preview slow in the
 * first place (see dc_temporal_denoise()'s comment history), just paid
 * once instead of every frame - "once" was still bad enough to make taking
 * a photo feel like it hangs. Not worth it for a marginal antialiasing
 * improvement on a downscaled photo. */
static void dc_capture_cb(const gbcam_frame_t *f, void *ctx)
{
    dc_ctx_t *c = (dc_ctx_t *)ctx;
    dc_sample(f, c->rgb, c->w, c->h);
}

/* Grabs one live camera frame, processes and draws it exactly like the main
 * viewfinder loop does (same GB/Dither/Normal branching), with the shutter
 * curtains overlaid at the given progress (0 = no curtains at all - see
 * display_shutter()). Shared by viewfinder_frame()'s own per-frame draw and
 * take_photo()'s closing-curtain animation below, so the preview stays
 * genuinely live (a fresh grab, not a redraw of a stale buffer) while the
 * curtains close, not just while they reopen afterward. Returns
 * camera_grab()'s result - the pacing/frame-skip logic around when to call
 * this at all stays in viewfinder_frame(), not here.
 * out_t_grabbed/out_t_processed (either or both may be NULL): timestamps
 * right after the camera grab and after the process step, for
 * viewfinder_frame()'s own per-stage perf stats - take_photo()'s animation
 * loop doesn't care and passes NULL. */
static esp_err_t grab_process_draw(float shutter_curtain, int64_t *out_t_grabbed, int64_t *out_t_processed)
{
    esp_err_t err;
    if (s_set.cam_mode == CAM_MODE_GB) {
        err = rgb_mode_active() ? camera_grab(gb_frame_cb_rgb, NULL) : camera_grab(gb_frame_cb, s_cam);
    } else {
        bool normal = s_set.cam_mode == CAM_MODE_NORMAL;
        if (normal) {
            normal_size(s_set.normal_size, &s_dc_w, &s_dc_h);
            normal_live_size(s_dc_w, s_dc_h, &s_dc_live_w, &s_dc_live_h);
        } else {
            dc_size(s_set.dc_size, &s_dc_w, &s_dc_h);
            s_dc_live_w = s_dc_w;
            s_dc_live_h = s_dc_h;
        }
        dc_ctx_t ctx = {.rgb = s_dc_rgb, .w = s_dc_live_w, .h = s_dc_live_h, .denoise = !normal};
        err = camera_grab(dc_frame_cb, &ctx);
    }
    if (err != ESP_OK) return err;
    if (out_t_grabbed) *out_t_grabbed = now_us();

    if (s_set.cam_mode == CAM_MODE_GB) {
        if (rgb_mode_active()) {
            gbcam_process_luma(s_cam_r);
            gbcam_process_luma(s_cam_g);
            gbcam_process_luma(s_cam_b);
            rgb_mode_combine();
        } else {
            gbcam_process_luma(s_cam);
        }
    } else if (s_set.cam_mode == CAM_MODE_DITHER) {
        float contrast, gamma;
        brightness_contrast_to_levels(&contrast, &gamma);
        if (s_set.dc_auto) gamma = dc_auto_compute_gamma(s_dc_rgb, s_dc_live_w, s_dc_live_h);
        if (s_set.dc_edge) edge_enhance_rgb(s_dc_rgb, s_dc_live_w, s_dc_live_h);
        dc_quantize(s_dc_rgb, s_dc_live_w, s_dc_live_h, s_set.dc_palette, (dc_method_t)s_set.dc_method,
                   s_set.dc_amount, contrast, gamma, s_dc_rgb, NULL, s_dc_quant_work);
    } else {
        float contrast, gamma;
        brightness_contrast_to_levels(&contrast, &gamma);
        dc_levels(s_dc_rgb, s_dc_live_w, s_dc_live_h, contrast, gamma);
    }
    if (out_t_processed) *out_t_processed = now_us();

    if (rgb_mode_active()) {
        /* Plain camera-style preview, like Dither/Normal Cam - RGB mode's
         * three independent channels don't fit display_begin_viewfinder()'s
         * single-shade-plus-palette shape (no brightness/contrast bars, no
         * frame overlay - frames are designed for GB Camera's indexed
         * palettes, not a true-colour image). */
        display_begin_camera(s_rgb_mode_rgb, GBCAM_W, GBCAM_H, false);
    } else if (s_set.cam_mode == CAM_MODE_GB) {
        const uint8_t *shades = s_cam->shades;
        /* The frame never shows cropped (see FRAME_PREVIEW_MS above): a
         * brief flash at 1:1 when you just changed it in 2x-crop mode (which
         * can't show a frame at all), so you can see the new choice without
         * leaving the crop you're framing with. Only overrides from CROP2X -
         * NATIVE1X/FIT already show frames just fine, forcing 1:1 there would
         * just be an unwanted zoom-out every time the frame changes. */
        bool frame_preview_active = now_us() < s_frame_preview_until_us;
        vf_scale_t vf_scale = (s_set.vf_scale == VF_SCALE_CROP2X && frame_preview_active)
                                   ? VF_SCALE_NATIVE1X
                                   : (vf_scale_t)s_set.vf_scale;
        /* frame_available(): an SD frame selected in a previous session has no
         * pixel data to draw until the card's list has loaded (app_frames.h) -
         * until then this draws the plain viewfinder, then the frame appears. */
        bool show_frame = vf_scale != VF_SCALE_CROP2X && frame_available(s_set.frame);
        int framed_w = 0, framed_h = 0;
        if (show_frame) {
            const frame_meta_t *fm = frames_get(s_set.frame - 1);
            frame_size(fm, 1, &framed_w, &framed_h);
            frame_compose_rgb(fm, shades, (gbcam_palette_t)s_set.palette, 1, s_frame_rgb);
        }
        display_begin_viewfinder(shades, (gbcam_palette_t)s_set.palette,
                                 s_set.brightness, GBCAM_BRIGHTNESS_LEVELS - 1,
                                 s_set.contrast, GBCAM_CONTRAST_LEVELS - 1, (int)s_adjust,
                                 vf_scale, frame_preview_active,
                                 show_frame ? s_frame_rgb : NULL, framed_w, framed_h);
    } else {
        display_begin_camera(s_dc_rgb, s_dc_live_w, s_dc_live_h, s_set.cam_mode == CAM_MODE_NORMAL);
    }
    display_shutter(shutter_curtain);
    draw_osd();
    display_end_frame();
    return ESP_OK;
}

/* Total bracket spread stays fixed at +-this many EV no matter how many shots
 * are selected - confirmed on real hardware that letting the range grow with
 * shot count (the previous design: +-(level*0.5) EV) pushes the outer shots
 * into near-total clipping once level gets past 2 or so, since gbcam's
 * exposure compression (gbcam.c's "code = 128 + (exposed-128)/8") still can't
 * prevent hard clipping once gain_q8 - already pushed fairly high by auto-
 * exposure in anything but bright light - gets multiplied several stops
 * further on top of that. More shots should mean finer sampling of a range
 * that's already known to look good, not an ever-wider one that clips at the
 * edges regardless of count. */
#define GB_AEB_MAX_EV 1.5f

/* Digital gain for a shot at a given EV offset from center, clamped to the
 * same range app_gbemu.c's own gain-seeding uses for the same reason (keeps
 * tier_from_gain() sane). powf() gives a fractional multiplier a plain bit-
 * shift never could (real AEB implementations bracket in fractional stops
 * for the same clipping reason as above). */
static uint16_t gb_aeb_gain_for_ev(uint16_t center_gain, float ev)
{
    float mult = powf(2.0f, ev);
    int32_t g = (int32_t)(center_gain * mult + 0.5f);
    if (g < 32) g = 32;
    if (g > 8192) g = 8192;
    return (uint16_t)g;
}

/* EV offset for shot i of count, evenly spaced across the fixed
 * +-GB_AEB_MAX_EV range regardless of count. */
static float gb_aeb_ev_for_index(int i, int count)
{
    return count <= 1 ? 0.0f : -GB_AEB_MAX_EV + i * (2.0f * GB_AEB_MAX_EV) / (float)(count - 1);
}

/* Live feedback during gb_aeb_capture()/gb_aeb_capture_rgb() in place of the
 * normal shutter curtain (which implies a single instant, not several) -
 * shows this step's actual dithered exposure (already rendered to RGB888 by
 * the caller - palette-mapped shades for plain GB, rgb_mode_combine()'s
 * output for RGB mode) full-screen with a "BRACKETING i/N" caption, so a
 * multi-shot burst reads as a burst instead of one long pause. */
static void display_aeb_progress(const uint8_t *rgb, int i, int count)
{
    display_begin_camera(rgb, GBCAM_W, GBCAM_H, false);
    char msg[20]; /* fits "BRACKETING 13/13" - the current max shot count */
    snprintf(msg, sizeof msg, "BRACKETING %d/%d", i + 1, count);
    osd_text(msg, NULL);
    draw_osd();
    display_end_frame();
}

/* Automatic Exposure Bracketing + Average (see ROW_GB_AEB) - HARDWARE style
 * only: works by directly forcing gbcam's digital exposure gain (gain_q8),
 * the same knob gb-photo's own AEB varies on real hardware over the M64282FP
 * sensor; PIXEL CAM style has no equivalent single exposure value to bracket
 * around. Reprocesses the same already-downsampled s_cam->luma at each step
 * (gbcam_expose_luma(), not a fresh camera_grab()) - gain_q8 is a post-
 * capture digital multiplier in this pipeline, not a real sensor integration
 * time, so every bracketed exposure comes from one live frame rather than N
 * separate captures.
 *
 * The "Average" part (named after gb-printer-web's own tool of the same
 * name - confirmed against its actual source, average.js: canvas alpha
 * compositing with globalAlpha = 1/(n+1), which is just a plain per-pixel
 * mean, nothing fancier) works on the DITHERED shade of each step (0..3), not
 * the pre-dither exposure data an earlier version of this averaged - fewer
 * shots landing on the same rounded shade at any one pixel is exactly what
 * lets the average land BETWEEN two shades, which is the whole point:
 * confirmed on real hardware that averaging several independently-dithered
 * exposures reconstructs more apparent grey levels than any single 4-shade
 * dither could show (temporal dithering/stacking, the same idea film grain
 * averaging uses) - a real, if modest, benefit even though every step still
 * comes from one single real capture (see above). Since the result isn't
 * confined to 4 discrete shades any more, it's saved as a continuous-tone
 * image (storage_save_dc(), PixelBoy/Digicam's own path) rather than a
 * "GB"-prefixed 4-shade one - interpolating linearly between the palette's
 * two nearest shade colours per pixel for the fractional average.
 *
 * That combined image is "the final file" and the only real gallery entry;
 * every individual step (including center) is instead kept as reference
 * material in its own AEB/ folder, tagged with the combined result's gallery
 * number, so the main gallery only ever shows one photo per shutter press.
 * Returns the number of files saved (0 on total failure), matching
 * take_photo()'s existing single-shot >= 0 success check. */
static int gb_aeb_capture(void)
{
    /* gb_aeb IS the bracket level - i.e. how many half-EV steps either side
     * of center (gb_aeb=1 -> -1..+1 -> 3 shots, gb_aeb=2 -> 5 shots, etc.) -
     * so step values need no lookup table, just an offset from the loop
     * index. */
    int level = s_set.gb_aeb;
    int count = level * 2 + 1;

    uint16_t center_gain = s_cam->gain_q8;
    s_cam->settings.auto_exposure = false;

    uint8_t *shade_sum = malloc(GBCAM_PIXELS); /* max count*3 stays well under 255 for any sane level */
    /* ~42KB - too big for internal RAM (chronically ~51KB free at boot), so
     * PSRAM instead of a plain static array, same fix as two prior
     * internal-RAM-exhaustion bugs elsewhere in this codebase. */
    static uint8_t *progress_rgb;
    if (!progress_rgb) progress_rgb = heap_caps_malloc(GBCAM_PIXELS * 3, MALLOC_CAP_SPIRAM);
    int saved = 0;
    if (shade_sum && progress_rgb) {
        memset(shade_sum, 0, GBCAM_PIXELS);
        for (int i = 0; i < count; i++) {
            s_cam->settings.manual_gain_q8 = gb_aeb_gain_for_ev(center_gain, gb_aeb_ev_for_index(i, count));
            gbcam_expose_luma(s_cam);
            gbcam_dither_work(s_cam);
            for (int p = 0; p < GBCAM_PIXELS; p++) shade_sum[p] = (uint8_t)(shade_sum[p] + s_cam->shades[p]);
            for (int p = 0; p < GBCAM_PIXELS; p++)
                memcpy(progress_rgb + p * 3, gbcam_palette_rgb((gbcam_palette_t)s_set.palette, s_cam->shades[p]), 3);
            display_aeb_progress(progress_rgb, i, count);
            /* Held on screen deliberately - exposing+dithering a 128x112
             * image is fast enough that the whole burst would otherwise
             * finish and flip past every step in a handful of milliseconds,
             * too fast to actually see as a bracketing sequence at all. */
            plat_sleep_ms(150);
        }

        uint8_t *rgb = malloc((size_t)GBCAM_PIXELS * 3);
        if (rgb) {
            gbcam_palette_t palette = (gbcam_palette_t)s_set.palette;
            for (int p = 0; p < GBCAM_PIXELS; p++) {
                /* Fixed-point average in eighths of a shade for interpolation
                 * precision, then linearly blend the two palette colours
                 * either side of it. */
                uint32_t avg8 = ((uint32_t)shade_sum[p] * 8) / (uint32_t)count;
                int lo = avg8 >> 3;
                if (lo > 2) lo = 2; /* keep lo+1 a valid shade (<=3) */
                int frac = avg8 & 7;
                const uint8_t *c0 = gbcam_palette_rgb(palette, (uint8_t)lo);
                const uint8_t *c1 = gbcam_palette_rgb(palette, (uint8_t)(lo + 1));
                for (int ch = 0; ch < 3; ch++)
                    rgb[p * 3 + ch] = (uint8_t)(c0[ch] + ((int)(c1[ch] - c0[ch]) * frac) / 8);
            }
            int number = storage_ready() ? storage_save_dc(rgb, GBCAM_W, GBCAM_H, false, "AEB") : -1;
            free(rgb);
            if (number >= 0) {
                saved++;
                for (int i = 0; i < count; i++) {
                    s_cam->settings.manual_gain_q8 = gb_aeb_gain_for_ev(center_gain, gb_aeb_ev_for_index(i, count));
                    gbcam_expose_luma(s_cam);
                    gbcam_dither_work(s_cam);
                    if (storage_save_aeb_extra(s_cam->shades, palette, number, i - level))
                        saved++;
                }
            }
        }
    }
    free(shade_sum);

    s_cam->settings.auto_exposure = true;
    /* Snap the live view straight back to the pre-burst exposure instead of
     * leaving gain_q8 sitting at the last (most extreme) bracket step and
     * making auto-exposure visibly hunt its way back over the next second.
     * s_cam->work/shades are left holding the last reference step's data,
     * not the combined result, but nothing reads either before the next
     * live frame (viewfinder_frame()'s regular camera_grab() path) repopulates
     * them properly on its own. */
    s_cam->gain_q8 = center_gain;
    return saved;
}

/* Same idea as gb_aeb_capture(), for the RGB palette (rgb_mode_active()) -
 * three independent gbcam_t instances (s_cam_r/g/b, one per colour channel),
 * each bracketed and averaged the same way, in lockstep (same relative EV
 * offset from gb_aeb_ev_for_index() applied to each channel's own current
 * gain, even though the three channels' absolute gains differ). No palette
 * interpolation needed here - rgb_mode_combine()'s (3 - shade) * 85 mapping
 * is already linear, so the fractional average shade converts to intensity
 * directly. */
static int gb_aeb_capture_rgb(void)
{
    int level = s_set.gb_aeb;
    int count = level * 2 + 1;

    uint16_t center_r = s_cam_r->gain_q8, center_g = s_cam_g->gain_q8, center_b = s_cam_b->gain_q8;
    s_cam_r->settings.auto_exposure = false;
    s_cam_g->settings.auto_exposure = false;
    s_cam_b->settings.auto_exposure = false;

    uint8_t *shade_sum = malloc((size_t)GBCAM_PIXELS * 3); /* interleaved r,g,b per pixel */
    int saved = 0;
    if (shade_sum) {
        memset(shade_sum, 0, (size_t)GBCAM_PIXELS * 3);
        for (int i = 0; i < count; i++) {
            s_cam_r->settings.manual_gain_q8 = gb_aeb_gain_for_ev(center_r, gb_aeb_ev_for_index(i, count));
            s_cam_g->settings.manual_gain_q8 = gb_aeb_gain_for_ev(center_g, gb_aeb_ev_for_index(i, count));
            s_cam_b->settings.manual_gain_q8 = gb_aeb_gain_for_ev(center_b, gb_aeb_ev_for_index(i, count));
            gbcam_expose_luma(s_cam_r); gbcam_dither_work(s_cam_r);
            gbcam_expose_luma(s_cam_g); gbcam_dither_work(s_cam_g);
            gbcam_expose_luma(s_cam_b); gbcam_dither_work(s_cam_b);
            for (int p = 0; p < GBCAM_PIXELS; p++) {
                shade_sum[p * 3 + 0] = (uint8_t)(shade_sum[p * 3 + 0] + s_cam_r->shades[p]);
                shade_sum[p * 3 + 1] = (uint8_t)(shade_sum[p * 3 + 1] + s_cam_g->shades[p]);
                shade_sum[p * 3 + 2] = (uint8_t)(shade_sum[p * 3 + 2] + s_cam_b->shades[p]);
            }
            rgb_mode_combine();
            display_aeb_progress(s_rgb_mode_rgb, i, count);
            plat_sleep_ms(150); /* see gb_aeb_capture()'s own comment on this */
        }

        uint8_t *rgb = malloc((size_t)GBCAM_PIXELS * 3);
        if (rgb) {
            for (int p = 0; p < GBCAM_PIXELS; p++)
                for (int ch = 0; ch < 3; ch++) {
                    uint32_t avg8 = ((uint32_t)shade_sum[p * 3 + ch] * 8) / (uint32_t)count; /* 0..24 */
                    rgb[p * 3 + ch] = (uint8_t)(((3 * 8 - (int)avg8) * 85) / 8);
                }
            int number = storage_ready() ? storage_save_dc(rgb, GBCAM_W, GBCAM_H, false, "AEB") : -1;
            free(rgb);
            if (number >= 0) {
                saved++;
                for (int i = 0; i < count; i++) {
                    int step = i - level;
                    s_cam_r->settings.manual_gain_q8 = gb_aeb_gain_for_ev(center_r, gb_aeb_ev_for_index(i, count));
                    s_cam_g->settings.manual_gain_q8 = gb_aeb_gain_for_ev(center_g, gb_aeb_ev_for_index(i, count));
                    s_cam_b->settings.manual_gain_q8 = gb_aeb_gain_for_ev(center_b, gb_aeb_ev_for_index(i, count));
                    gbcam_expose_luma(s_cam_r); gbcam_dither_work(s_cam_r);
                    gbcam_expose_luma(s_cam_g); gbcam_dither_work(s_cam_g);
                    gbcam_expose_luma(s_cam_b); gbcam_dither_work(s_cam_b);
                    rgb_mode_combine();
                    if (storage_save_aeb_extra_rgb(s_rgb_mode_rgb, number, step))
                        saved++;
                }
            }
        }
    }
    free(shade_sum);

    s_cam_r->settings.auto_exposure = true;
    s_cam_g->settings.auto_exposure = true;
    s_cam_b->settings.auto_exposure = true;
    s_cam_r->gain_q8 = center_r;
    s_cam_g->gain_q8 = center_g;
    s_cam_b->gain_q8 = center_b;
    return saved;
}

static void take_photo(void)
{
    /* Instant feedback the moment the shutter is pressed - the capture (a
     * fresh high-res grab for Normal Cam) and SD card save below are fully
     * synchronous and can take a real chunk of time, especially at 720p;
     * nothing renders again until this function returns, so without this
     * the screen just sits there with no visible response until it's all
     * done. A real closing sweep (curtains 0 -> 0.5, meeting in the middle),
     * not a hard cut to black - and the camera preview stays live through
     * it (grab_process_draw() grabs a fresh frame each step), not frozen on
     * whatever was on screen the instant the shutter was pressed. */
    /* AEB shows its own bracketing progress in place of the usual closing-
     * curtain animation (a single instant doesn't read right for a multi-shot
     * burst) - see gb_aeb_capture()/display_aeb_progress(). Before any of
     * that, give the live auto-exposure loop a few real seconds on the
     * current scene to settle before gb_aeb_capture() locks in "center" gain
     * for everything else to bracket around - auto_exposure_step() only
     * nudges gain_q8 a little each frame (see gbcam.c), so a shot taken right
     * after framing a new scene (or right after turning AEB on) could still
     * be mid-adjustment, which every bracket step would inherit. Live,
     * uncurtained (grab_process_draw(0, ...) - same call the normal
     * viewfinder uses) so the screen keeps showing real preview instead of
     * freezing, with an OSD refreshed every frame so it stays up for the
     * whole wait instead of OSD_MS's usual ~1s. */
    bool aeb_shot = s_set.cam_mode == CAM_MODE_GB && s_set.gb_aeb &&
                    (gbcam_style_t)s_set.style == GBCAM_STYLE_HARDWARE;
    if (aeb_shot) {
        int64_t stabilize_until = now_us() + 3 * 1000000LL;
        while (now_us() < stabilize_until) {
            osd_text("STABILIZING...", NULL);
            if (grab_process_draw(0, NULL, NULL) != ESP_OK) break;
        }
    } else {
        for (int i = 1; i <= 4; i++)
            if (grab_process_draw(i * 0.125f, NULL, NULL) != ESP_OK) break;
    }

    int n;
    if (rgb_mode_active()) {
        if (aeb_shot) {
            n = gb_aeb_capture_rgb() > 0 ? 0 : -1;
        } else {
            if (s_set.gb_aeb) osd_text("AEB NEEDS HW STYLE", NULL);
            /* True colour, not GB Camera's palette-mapped 2bpp tiles - saved
             * the same way as a Dither/Normal Cam photo (upscaled PNG), not
             * storage_save()'s .BIN+palette pair, since there's no single
             * shade per pixel to store that way. */
            n = storage_ready() ? storage_save_dc(s_rgb_mode_rgb, GBCAM_W, GBCAM_H, false, "DC") : -1;
        }
    } else if (s_set.cam_mode == CAM_MODE_GB) {
        /* A photo taken in the second or so the card's frame list is still
         * loading is saved without its border rather than not at all - the
         * frame it would have had isn't decoded yet to bake in (app_frames.h). */
        int frame = frame_available(s_set.frame) ? (int)s_set.frame - 1 : -1;
        if (aeb_shot) {
            n = gb_aeb_capture() > 0 ? 0 : -1;
        } else {
            if (s_set.gb_aeb) osd_text("AEB NEEDS HW STYLE", NULL);
            memcpy(s_still, s_cam->shades, GBCAM_PIXELS);
            n = storage_ready() ? storage_save(s_still, (gbcam_palette_t)s_set.palette, frame, "GB") : -1;
        }
    } else {
        bool jpeg = s_set.cam_mode == CAM_MODE_NORMAL;
        if (jpeg) {
            /* One properly antialiased resample straight from a fresh
             * camera frame - see dc_capture_cb()'s comment. Falls back to
             * the (plain-nearest) live preview buffer on a grab failure
             * rather than saving nothing. */
            dc_ctx_t ctx = {.rgb = s_still_rgb, .w = s_dc_w, .h = s_dc_h};
            if (camera_grab(dc_capture_cb, &ctx) != ESP_OK)
                memcpy(s_still_rgb, s_dc_rgb, (size_t)s_dc_w * s_dc_h * 3);
        } else {
            memcpy(s_still_rgb, s_dc_rgb, (size_t)s_dc_w * s_dc_h * 3);
        }
        n = storage_ready() ? storage_save_dc(s_still_rgb, s_dc_w, s_dc_h, jpeg, "DC") : -1;
    }
    /* Set only now, after the capture/save work above (which can still take
     * a real chunk of time - the SD card write especially) - otherwise that
     * work eats into the window before the shutter-opening animation
     * (display_shutter(), driven by this same window - see
     * viewfinder_frame()) even starts, and it either barely plays or
     * doesn't get a chance to render at all. */
    s_freeze_until_us = now_us() + FREEZE_MS * 1000LL;

    if (!storage_ready()) {
        osd_text("NO SD CARD", NULL);
        return;
    }
    /* No "SAVED" text on success - the shutter animation (display_shutter(),
     * driven by this same freeze window - see viewfinder_frame()) is the
     * feedback instead. A real failure past this point is rare enough
     * (SD write error, card full) that it's still worth a message. */
    if (n < 0) osd_text("SAVE FAILED", NULL);
}

void app_enter_sleep(void)
{
    /* The resume record, written here rather than at either call site: this is
     * the only way into deep sleep, so a third caller cannot forget to record
     * one (see s_resume). */
    memset(&s_resume, 0, sizeof s_resume);
    s_resume.version = RESUME_VERSION;
    if (s_resume_gbemu) {
        s_resume.kind = RESUME_GBEMU;
        s_resume.emu_slot = s_resume_slot;
        snprintf(s_resume.emu_rom, sizeof s_resume.emu_rom, "%s", s_resume_rom);
        s_resume_gbemu = false;
    } else if (s_screen == SCREEN_MENU) {
        s_resume.kind = RESUME_MENU;
        s_resume.menu_sel = s_menu_sel;
    } else if (s_screen == SCREEN_GALLERY) {
        s_resume.kind = RESUME_GALLERY;
        s_resume.gallery_pos = s_gallery_pos;
        s_resume.gallery_grid = s_gallery_grid ? 1 : 0;
        s_resume.gallery_view = (int)s_gallery_view;
    } else {
        /* Viewfinder - and USB/WiFi, which app_sleep_due() refuses to sleep
         * from at all, so they are unreachable here. */
        s_resume.kind = RESUME_VIEWFINDER;
    }
    s_resume.magic = RESUME_MAGIC; /* last: a torn record is one that is not resumed */

    /* Standby almost always got here first (the SLEEP timeouts are minutes
     * against standby's tens of seconds), which means the backlight is
     * already at 0 and the message below would be drawn onto a panel nobody
     * can see - making a device that slept successfully indistinguishable
     * from one that is merely standing by. Bring it up for the 400ms the
     * message is held; app_backlight_percent(0) is the user's own setting,
     * not a fixed level. */
    display_set_backlight(app_backlight_percent());
    ESP_LOGI(TAG, "deep sleep"); /* last line before the reset - a wake is a fresh boot */
    display_begin_blank(0, 0, 0);
    display_text(DISP_W / 2 - display_text_width("SLEEPING", 3) / 2, 100, 3, "SLEEPING", 255, 255, 255);
    /* Only for an emulator session, where there is a question to answer: the
     * game comes back on the frame it was left on, or it comes back on its
     * title screen. The card is powered down right after this, so this screen
     * is the only chance to see which. */
    if (s_resume.kind == RESUME_GBEMU) {
        const char *msg = s_resume_state_saved ? "GAME SAVED" : "GAME NOT SAVED";
        uint8_t r = s_resume_state_saved ? 160 : 255, g = s_resume_state_saved ? 160 : 80;
        display_text(DISP_W / 2 - display_text_width(msg, 1) / 2, 140, 1, msg, r, g, s_resume_state_saved ? 160 : 80);
    }
    display_end_frame();
    plat_sleep_ms(400); /* let it actually show before the screen cuts out */
    plat_enter_deep_sleep(); /* cuts camera/LCD/SD power; wakes (as a fresh boot) on Shutter - never returns */
}

/* True if the gallery actually opened. The false cases leave the caller's
 * screen untouched on purpose: the camera is paused for the whole time the
 * menu or the gallery is up, so a caller that had already switched to
 * SCREEN_VIEWFINDER would land on a black viewfinder it can't come back from
 * (nothing resumes the camera there) - see the ROW_GALLERY case. */
static bool enter_gallery(void)
{
    if (!storage_ready()) {
        osd_text("NO SD CARD", NULL);
        return false;
    }
    if (storage_count() == 0) {
        osd_text("NO PHOTOS", NULL);
        return false;
    }
    camera_pause();  /* nothing live to show browsing photos - see app_camera.h */
    s_screen = SCREEN_GALLERY;
    s_gallery_pos = storage_count() - 1;
    s_gallery_grid = true;
    s_grid_cache_page = -1; /* force a fresh decode - see gallery_grid_frame() */
    s_gallery_view = GALLERY_VIEW_CROP2X;
    s_gallery_dirty = true;
    s_delete_armed_until_us = 0;
    return true;
}

static void leave_gallery(void)
{
    camera_resume();
    s_screen = SCREEN_VIEWFINDER;
    s_osd.until_us = 0;
}

/* ----------------------------------------------------------------- USB MSC */

static void handle_usb_input(const input_event_t *ev)
{
    /* This is the RELIABLE way out, not just a manual shortcut - see
     * app_usb.c's usb_msc_tick() comment: the actual USB cable being pulled
     * isn't something this stack can currently detect (a real gap in
     * TinyUSB's DWC2 driver, not a bug here), so Shutter is what most users
     * will need every time. Host-initiated disconnects (Windows ejecting
     * the drive) do still get caught automatically. */
    if (ev->type == INPUT_CLICK && ev->button == BTN_SHUTTER) usb_msc_exit();
}

static void usb_screen_frame(void)
{
    display_begin_blank(0, 0, 0);
    const char *l1 = "USB MODE";
    const char *l2 = "SHUTTER TO EXIT";
    display_text(DISP_W / 2 - display_text_width(l1, 2) / 2, 90, 2, l1, 255, 255, 255);
    display_text(DISP_W / 2 - display_text_width(l2, 1) / 2, 120, 1, l2, 200, 200, 200);
    display_end_frame();
}

/* ------------------------------------------------------------- WiFi Gallery */

static void handle_wifi_input(const input_event_t *ev)
{
    /* INPUT_PRESS, not INPUT_CLICK - ROW_WIFI is activated by the menu's own
     * Shutter *press* (see activate_menu_row()), so that same press's later
     * release would otherwise arrive here as a CLICK on the very next
     * app_step() and instantly exit the screen it just opened. Matches
     * ROW_GALLERY's leave_gallery() trigger, which has the same "activated
     * by a Shutter press" shape and dodges it the same way. */
    if (ev->type == INPUT_PRESS && ev->button == BTN_SHUTTER) {
        wifi_gallery_stop();
        camera_resume();
        s_screen = SCREEN_VIEWFINDER;
    }
}

static void wifi_screen_frame(void)
{
    display_begin_blank(0, 0, 0);
    char l2[40], l3[40];
    snprintf(l2, sizeof l2, "SSID %s", WIFI_GALLERY_SSID);
    snprintf(l3, sizeof l3, "PASS %s", WIFI_GALLERY_PASS);
    const char *l1 = "WIFI GALLERY";
    const char *l4 = "http://192.168.4.1";
    const char *l5 = "SHUTTER TO EXIT";
    display_text(DISP_W / 2 - display_text_width(l1, 2) / 2, 70, 2, l1, 255, 255, 255);
    display_text(DISP_W / 2 - display_text_width(l2, 1) / 2, 100, 1, l2, 200, 200, 200);
    display_text(DISP_W / 2 - display_text_width(l3, 1) / 2, 115, 1, l3, 200, 200, 200);
    display_text(DISP_W / 2 - display_text_width(l4, 1) / 2, 135, 1, l4, 200, 200, 200);
    display_text(DISP_W / 2 - display_text_width(l5, 1) / 2, 160, 1, l5, 200, 200, 200);
    display_end_frame();
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

/* --------------------------------------------------------------- backlight */

/* There used to be an auto-dim tier here - 20s of idle pulled the backlight
 * to 15% before standby took it out entirely. Removed: a dimmed screen is
 * still a screen the user is looking at, so it bought almost no power while
 * making every press during that window ambiguous - was it meant for the game
 * or just to bring the light back? Losing it means the screen is either at the
 * user's own level or off, and "off" is the only state that has to intercept a
 * press (see the swallow in app_step()/run_rom()). Standby arrives sooner for
 * it - see STANDBY_SECONDS below. */

/* Auto-standby timeout choices - see STANDBY_OPTIONS_COUNT in app_settings.h
 * and ROW_STANDBY below - 0 = never. Not deep sleep: standby is the cheap,
 * instantly-reversible tier (screen out, camera stopped, straight back on the
 * next press), so having it user-tunable is reasonable in a way the one-way
 * ROW_SLEEP trip isn't. It does mean the pair can be set in either order; the
 * only consequence of a sleep timeout shorter than this one is that the
 * standby state is never reached, which costs nothing. */
static const int STANDBY_SECONDS[STANDBY_OPTIONS_COUNT] = {0, 15, 30, 60, 120, 300};
/* The same values again for the menu to print (see ROW_STANDBY in draw_menu())
 * - spelled out rather than formatted from STANDBY_SECONDS[] so the row reads
 * "1 MIN" and not "60 SEC"; keep the two in step. */
static const char *const STANDBY_LABELS[STANDBY_OPTIONS_COUNT] = {"NEVER", "15 SEC", "30 SEC", "1 MIN", "2 MIN", "5 MIN"};

/* Defined with the rest of the standby state below - update_backlight() only
 * needs to know whether the screen is meant to be out at all. */
static bool s_standby;

/* Set when a press is spent waking the screen rather than doing what it says
 * (see the drain in app_step()), and kept set until that same touch is over.
 * A press is not one event: releasing it posts a click, and holding it posts a
 * long-press, both hundreds of ms after the backlight is already back on - so
 * without this the press would wake the device and its own tail would then
 * open the menu, take a photo or exit a ROM. Same flag, same reason, in
 * app_gbemu.c's run_rom(). */
static bool s_swallow_press;

/* The same idea as the flag above, for the other kind of touch that was not
 * aimed at the app: the one that woke the device. A wake is a reset, so the
 * finger that pressed Shutter can still be on the button when input_init()
 * starts polling - and by the time a resumed screen is up, that press (and the
 * click ending it) is a photo, or an A press in a resumed ROM.

 * s_swallow_press cannot cover this: it is only armed by seeing an event, and it
 * clears the moment nothing is held, which is true on the very first app_step()
 * because the 20ms poll has not confirmed the held pin yet. This one is armed at
 * boot and expires on its own terms - see app_boot_swallow_poll(). */
static bool s_boot_swallow;
static int64_t s_boot_swallow_deadline_us;
/* Long enough to cover the two-sample debounce (~40ms) and a couple of poll
 * margins, short enough that the earliest plausible genuine press after a wake
 * is not eaten. Only reached when nothing at all was sampled - a button that is
 * actually down keeps the window open until it comes back up. */
#define BOOT_SWALLOW_MS 250

/* "Never" as far as the two functions below are concerned: a sentinel rather
 * than 0 because 0 is itself a meaningful entry in STANDBY_SECONDS[] (never),
 * not "unset". */
static int64_t standby_us(void)
{
    int sec = STANDBY_SECONDS[s_set.standby];
    return sec == 0 ? 0 : (int64_t)sec * 1000000LL;
}

/* The user's own level, shared with app_gbemu.c (see app.h). Never returns 0;
 * full-off belongs to whichever loop is doing the standby (app_step() also
 * stops the camera, the emulator doesn't).
 *
 * Takes no idle argument any more - the auto-dim that used it is gone (see the backlight
 * section above), so this is the setting and nothing else. Both loops still come through
 * here rather than reading BACKLIGHT_PERCENT[] directly because that table is private to
 * this file. */
int app_backlight_percent(void) { return BACKLIGHT_PERCENT[s_set.backlight]; }

/* The timeout half of that policy, shared with app_gbemu.c (see app.h). The
 * standby state itself is each loop's own business - what app_step() does
 * about it (stop the camera) is not what the emulator does (leave it alone). */
bool app_standby_due(int64_t idle_us)
{
    int64_t sb = standby_us();
    return sb != 0 && idle_us > sb;
}

/* The SLEEP timer (ROW_SLEEP), which deep-sleeps rather than just darkening.
 * sleep_min of 0 means "never" (see the row), not an instant sleep.
 *
 * The exemptions are the whole reason this lives here instead of in each
 * loop: nothing should cut power while a PC owns the SD card (mid-transfer),
 * while the WiFi gallery could be mid-download, or while mirror mode is
 * streaming the very screen that would go blank - and the emulator loop has
 * no way to know about any of those on its own (it never calls
 * usb_msc_tick(), so it wouldn't even notice the cable). */
bool app_sleep_due(int64_t idle_us)
{
    int sleep_min = SLEEP_MINUTES[s_set.sleep_min];
    return sleep_min != 0 && s_screen != SCREEN_USB && s_screen != SCREEN_WIFI && !usb_webcam_active() &&
           idle_us > (int64_t)sleep_min * 60 * 1000000LL;
}

/* Idempotent and cheap to call every app_step(): display_hw_set_backlight()
 * ignores a value it's already at, so a lit viewfinder doesn't retouch the
 * LEDC duty (or its INFO log) 15 times a second. */
static void update_backlight(void)
{
    display_set_backlight(s_standby ? 0 : app_backlight_percent());
}

/* ---------------------------------------------------------------- standby */

/* The one idle step: the backlight goes out
 * completely and, in the viewfinder, the camera stops streaming. Those are
 * the two biggest continuous loads on the board - the sensor/MIPI/ISP chain
 * especially, which runs at its own 25fps regardless of how few frames this
 * app actually consumes - and both are pure waste when nobody has touched the
 * thing for half a minute.
 *
 * Deliberately not deep sleep: that's a full reset on wake (see
 * plat_enter_deep_sleep()), so anything reached from here has to come back
 * without a reboot. camera_pause()/camera_resume() are exactly that - a
 * V4L2 STREAMOFF/STREAMON of buffers the driver already owns - so the cost of
 * waking is the ISP's own restart plus whatever the auto-exposure does
 * converging again, not a boot. It's also why this is a separate, much
 * earlier threshold than ROW_SLEEP's: it's meant to be entered and left
 * constantly, not to be a one-way trip.
 *
 * The timeout itself is the user's (ROW_STANDBY, see app_standby_due()) and
 * defaults to 30s - comfortably inside the shortest ROW_SLEEP timeout (1 min),
 * so the two are ordered by default rather than racing. */
#define STANDBY_POLL_MS 20

/* Whether *this* stopped the camera, as opposed to finding it already stopped
 * by a screen that manages it itself (enter_menu()/enter_gallery()). Only a
 * pause we made is ours to undo - without this, leaving standby from the
 * gallery would camera_resume() behind enter_gallery()'s back. */
static bool s_standby_paused_camera;

/* Screens that are being watched from somewhere other than this device:
 * handing the SD card to a PC (the host is driving that screen), the WiFi
 * gallery (a phone may be mid-download), and USB mirror mode (a host is
 * capturing the screen - blanking it would blank the stream). Same set
 * app_step()'s auto-sleep check excludes, for the same reasons. */
static bool standby_allowed(void)
{
    if (usb_webcam_active()) return false;
    return s_screen != SCREEN_USB && s_screen != SCREEN_WIFI;
}

/* Time-based only - a pending input event has already been folded into
 * s_last_input_us by the time app_step() calls this (see there), which is
 * what makes leaving standby happen before the event that caused it gets
 * dispatched. */
static void update_standby(void)
{
    bool want = app_standby_due(now_us() - s_last_input_us) && standby_allowed();
    if (want == s_standby) return;

    if (want) {
        if (s_screen == SCREEN_VIEWFINDER) {
            camera_pause();
            s_standby_paused_camera = true;
        }
        s_standby = true;
    } else {
        if (s_standby_paused_camera) {
            camera_resume();
            s_standby_paused_camera = false;
        }
        s_standby = false;
    }
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
        s_menu_rows[s_menu_count++] = ROW_GB_AUTO;
        s_menu_rows[s_menu_count++] = ROW_GB_AEB;
    } else if (s_set.cam_mode == CAM_MODE_DITHER) {
        s_menu_rows[s_menu_count++] = ROW_DC_PALETTE;
        s_menu_rows[s_menu_count++] = ROW_DC_METHOD;
        s_menu_rows[s_menu_count++] = ROW_DC_SIZE;
        s_menu_rows[s_menu_count++] = ROW_DC_AMOUNT;
        s_menu_rows[s_menu_count++] = ROW_DC_AUTO;
        s_menu_rows[s_menu_count++] = ROW_DC_EDGE;
    } else { /* CAM_MODE_NORMAL */
        s_menu_rows[s_menu_count++] = ROW_NORMAL_SIZE;
    }
    s_menu_rows[s_menu_count++] = ROW_BACKLIGHT;
    s_menu_rows[s_menu_count++] = ROW_STANDBY;
    s_menu_rows[s_menu_count++] = ROW_SLEEP;
    s_menu_rows[s_menu_count++] = ROW_GALLERY;
    s_menu_rows[s_menu_count++] = ROW_WIFI;
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

    /* Sized to match s_menu_rows[] - GB Camera's 7 mode rows + 6 shared rows
     * (Backlight/Standby/Sleep/Gallery/WiFi/Exit) = 13, the largest any mode
     * builds. */
    char labels[13][13], values[13][12]; /* labels: 13, fits "WIFI GALLERY" (12 chars) + null */
    const char *label_ptrs[13], *value_ptrs[13];
    icon_id_t icons[13];
    for (int i = 0; i < s_menu_count; i++) {
        const char *val = NULL;
        switch (s_menu_rows[i]) {
        case ROW_PALETTE:
            snprintf(labels[i], sizeof labels[i], "PALETTE");
            /* One slot past the real palettes is the RGB sentinel - see
             * rgb_mode_active(). */
            if (s_set.palette == (uint8_t)gbcam_palette_count()) truncate_value(values[i], "TRICHROME");
            else truncate_value(values[i], gbcam_palette_name((gbcam_palette_t)s_set.palette));
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
            snprintf(values[i], sizeof values[i], "%s",
                     s_set.vf_scale == VF_SCALE_NATIVE1X ? "1:1" :
                     s_set.vf_scale == VF_SCALE_FIT      ? "FIT" : "2X CROP");
            val = values[i];
            icons[i] = ICON_SIZE;
            break;
        case ROW_FRAME:
            snprintf(labels[i], sizeof labels[i], "FRAME");
            truncate_value(values[i], s_set.frame == 0 ? "NONE" : frames_get_name(s_set.frame - 1));
            val = values[i];
            icons[i] = ICON_FRAME;
            break;
        case ROW_GB_AUTO:
            snprintf(labels[i], sizeof labels[i], "AUTO");
            snprintf(values[i], sizeof values[i], "%s", s_set.gb_auto ? "ON" : "OFF");
            val = values[i];
            icons[i] = ICON_AUTO;
            break;
        case ROW_GB_AEB:
            snprintf(labels[i], sizeof labels[i], "AEB/HDR");
            if (s_set.gb_aeb == 0) snprintf(values[i], sizeof values[i], "OFF");
            else snprintf(values[i], sizeof values[i], "%d SHOTS", s_set.gb_aeb * 2 + 1);
            val = values[i];
            icons[i] = ICON_HDR;
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
            icons[i] = ICON_DITHER;
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
        case ROW_DC_AUTO:
            snprintf(labels[i], sizeof labels[i], "AUTO");
            snprintf(values[i], sizeof values[i], "%s", s_set.dc_auto ? "ON" : "OFF");
            val = values[i];
            icons[i] = ICON_AUTO;
            break;
        case ROW_DC_EDGE:
            snprintf(labels[i], sizeof labels[i], "EDGE");
            snprintf(values[i], sizeof values[i], "%s", s_set.dc_edge ? "ON" : "OFF");
            val = values[i];
            icons[i] = ICON_EDGE;
            break;
        case ROW_NORMAL_SIZE:
            snprintf(labels[i], sizeof labels[i], "SIZE");
            truncate_value(values[i], normal_size_name(s_set.normal_size));
            val = values[i];
            icons[i] = ICON_SIZE;
            break;
        case ROW_BACKLIGHT:
            /* "BACKLIGHT", not "BRIGHTNESS" - that name is already taken by the
             * viewfinder's own quick-adjust (the GB Camera's emulated exposure,
             * a pixel-level effect), and the two are easy to confuse. The icon
             * asset is brightness.png all the same (see tools/gen_icons.py). */
            snprintf(labels[i], sizeof labels[i], "BACKLIGHT");
            snprintf(values[i], sizeof values[i], "%d%%", BACKLIGHT_PERCENT[s_set.backlight]);
            val = values[i];
            icons[i] = ICON_BACKLIGHT;
            break;
        case ROW_STANDBY:
            /* "STANDBY" rather than "SCREEN OFF": this also stops the camera,
             * not just the panel, and it's the word the code and the comments
             * here already use. The value column carries the seconds, which is
             * what actually distinguishes it from SLEEP below - the two rows
             * sit together so they read as the two halves of one policy. */
            snprintf(labels[i], sizeof labels[i], "STANDBY");
            snprintf(values[i], sizeof values[i], "%s", STANDBY_LABELS[s_set.standby]);
            val = values[i];
            icons[i] = ICON_STANDBY;
            break;
        case ROW_SLEEP:
            snprintf(labels[i], sizeof labels[i], "SLEEP");
            if (SLEEP_MINUTES[s_set.sleep_min] == 0) snprintf(values[i], sizeof values[i], "NEVER");
            else snprintf(values[i], sizeof values[i], "%d MIN", SLEEP_MINUTES[s_set.sleep_min]);
            val = values[i];
            icons[i] = ICON_SLEEP;
            break;
        case ROW_GALLERY:
            snprintf(labels[i], sizeof labels[i], "GALLERY");
            icons[i] = ICON_GALLERY;
            break;
        case ROW_WIFI:
            snprintf(labels[i], sizeof labels[i], "WIFI GALLERY");
            icons[i] = ICON_WIFI;
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
        /* +1 slot for the RGB sentinel past the real palettes - see
         * rgb_mode_active(). */
        s_set.palette = (uint8_t)((s_set.palette + 1) % (gbcam_palette_count() + 1));
        if (s_set.palette == (uint8_t)gbcam_palette_count() && !rgb_mode_ensure_ready()) {
            /* Allocation failed (out of memory) - skip back to the first
             * real palette rather than leaving palette pointed at a sentinel
             * whose buffers don't exist. */
            s_set.palette = 0;
            osd_text("RGB UNAVAILABLE", "OUT OF MEMORY");
        }
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
        s_set.vf_scale = (uint8_t)((s_set.vf_scale + 1) % 3);
        settings_changed(&s_set);
        break;
    case ROW_FRAME:
        s_set.frame = (uint8_t)((s_set.frame + 1) % (frames_total() + 1));
        settings_changed(&s_set);
        s_frame_preview_until_us = now_us() + FRAME_PREVIEW_MS * 1000LL;
        break;
    case ROW_GB_AUTO:
        s_set.gb_auto = (uint8_t)(s_set.gb_auto ? 0 : 1);
        apply_settings();
        break;
    case ROW_GB_AEB:
        /* Only applied at capture time (take_photo()) - nothing to push into
         * s_cam's live settings here, unlike GB_AUTO above. */
        s_set.gb_aeb = (uint8_t)((s_set.gb_aeb + 1) % 7); /* 0 off, 1..6 -> 3..13 shots */
        settings_changed(&s_set);
        break;
    case ROW_DC_PALETTE:
        s_set.dc_palette = (uint8_t)((s_set.dc_palette + 1) % dc_palette_count());
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
    case ROW_DC_AUTO:
        s_set.dc_auto = (uint8_t)(s_set.dc_auto ? 0 : 1);
        settings_changed(&s_set);
        break;
    case ROW_DC_EDGE:
        s_set.dc_edge = (uint8_t)(s_set.dc_edge ? 0 : 1);
        settings_changed(&s_set);
        break;
    case ROW_NORMAL_SIZE:
        s_set.normal_size = (uint8_t)((s_set.normal_size + 1) % NORMAL_SIZE_COUNT);
        settings_changed(&s_set);
        break;
    case ROW_BACKLIGHT:
        s_set.backlight = (uint8_t)((s_set.backlight + 1) % BACKLIGHT_OPTIONS_COUNT);
        settings_changed(&s_set);
        /* Not just saved - applied now, so the new level is visible in the
         * menu the moment it's picked. update_backlight() won't undo it: the
         * Shutter press that got here already refreshed s_last_input_us, so
         * the idle window is nowhere near elapsed. */
        update_backlight();
        break;
    case ROW_STANDBY:
        s_set.standby = (uint8_t)((s_set.standby + 1) % STANDBY_OPTIONS_COUNT);
        settings_changed(&s_set);
        /* Same as ROW_BACKLIGHT: the Shutter press that got here just refreshed
         * s_last_input_us, so a shorter timeout can't fire out from under the
         * menu the moment it's picked. */
        update_standby();
        update_backlight();
        break;
    case ROW_SLEEP:
        s_set.sleep_min = (uint8_t)((s_set.sleep_min + 1) % SLEEP_OPTIONS_COUNT);
        settings_changed(&s_set);
        break;
    case ROW_GALLERY:
        /* Stay on the menu (camera still paused, message up) if it can't open -
         * the old "assume success and set the screen first" version left the
         * device on SCREEN_VIEWFINDER with the camera paused and nothing that
         * would ever resume it. */
        if (enter_gallery()) return;
        break;
    case ROW_WIFI:
        /* camera_pause() already happened entering the menu - stays paused
         * behind this screen, same as ROW_GALLERY, until handle_wifi_input()
         * exits back to the viewfinder. */
        wifi_gallery_start();
        if (wifi_gallery_active()) {
            s_screen = SCREEN_WIFI;
            return;
        }
        osd_text("WIFI FAILED", NULL);
        break;
    case ROW_EXIT:
        break;
    }
    if (s_menu_rows[s_menu_sel] == ROW_EXIT) {
        camera_resume();
        s_screen = SCREEN_VIEWFINDER;
    }
}

/* Webcam mode (mirror or GB) has no screen of its own, so its off switch rides
 * on Menu's long press, which is otherwise unused on every screen it can be
 * running behind. Menu-hold from the viewfinder would normally open the gallery
 * and in the gallery would normally re-select a photo - while a stream is live
 * it stops the stream instead (the gallery is still one menu row away).
 *
 * Not optional: without it the only ways out are the host releasing the
 * device or a replug, and usb_msc_tick()'s tud_mounted() check can't see a
 * bare cable pull (see there) - so a webcam session could sit there
 * streaming to a PC that isn't even connected, which is also what keeps
 * app_sleep_due()/standby_allowed() refusing to idle. Returns true if the
 * press was the exit gesture, so the caller doesn't also act on it. */
static bool webcam_exit_long_press(const input_event_t *ev)
{
    if (ev->type != INPUT_LONG_PRESS || ev->button != BTN_MENU || !usb_webcam_active()) return false;
    usb_webcam_exit();
    osd_brief("WEBCAM OFF");
    return true;
}

static void handle_menu_input(const input_event_t *ev)
{
    if (webcam_exit_long_press(ev)) return;
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

/* GB Camera and Dither Cam have four quick-adjust targets; Normal Cam has
 * three (BRIGHTNESS/CONTRAST/SIZE). */
static int adjust_count(uint8_t cam_mode)
{
    return cam_mode == CAM_MODE_NORMAL ? 3 : cam_mode == CAM_MODE_DITHER ? 6 : 4;
}

/* s_set.adjust[] is the saved target per mode; a stale value left over from
 * a mode with more targets (or a version mismatch) just clamps to 0 rather
 * than indexing past what that mode actually has. */
static adjust_t load_adjust(uint8_t cam_mode)
{
    int v = s_set.adjust[cam_mode];
    return (adjust_t)(v < adjust_count(cam_mode) ? v : 0);
}

static void cycle_cam_mode(void)
{
    s_set.cam_mode = (uint8_t)((s_set.cam_mode + 1) % CAM_MODE_COUNT);
    if (s_set.cam_mode == CAM_MODE_EMULATOR) {
        /* Launcher slot, not a mode to sit in - see CAM_MODE_EMULATOR's own
         * comment. Deliberately NOT paused first, unlike entering the menu -
         * a GB Camera ROM's own live viewfinder needs the ISP actually
         * streaming (camera_grab() while paused just returns nothing, seen
         * as the emulator's picture going black). camera_resume() after is
         * still worth keeping in case anything inside ever does pause it. */
        gbemu_run();
        camera_resume();
        /* A whole session just went by with s_last_input_us frozen (app_step()
         * only refreshes it when input is pending, and none is dispatched in
         * here), so without this the sleep check would see the entire session as
         * idle and deep-sleep on the very next iteration - exiting a ROM looking
         * like the device switching itself off. */
        s_last_input_us = now_us();
        s_set.cam_mode = CAM_MODE_GB;
    }
    s_adjust = load_adjust(s_set.cam_mode);
    settings_changed(&s_set);
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
            /* +1 slot for the RGB sentinel past the real palettes - see
             * rgb_mode_active() and ROW_PALETTE's own cycle in
             * activate_menu_row(), which this must match. */
            int n = gbcam_palette_count() + 1;
            int new_palette = ((v % n) + n) % n;
            if (new_palette == gbcam_palette_count() && !rgb_mode_ensure_ready()) {
                osd_brief("TRICHROME UNAVAILABLE");
            } else {
                s_set.palette = (uint8_t)new_palette;
                apply_settings();
                osd_brief(s_set.palette == (uint8_t)gbcam_palette_count() ? "TRICHROME"
                                                                          : gbcam_palette_name((gbcam_palette_t)s_set.palette));
            }
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
            int n = dc_palette_count();
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
        } else if (s_adjust == ADJUST_3) {
            int v = (int)s_set.dc_method + detents;
            int n = DC_METHOD_COUNT;
            s_set.dc_method = (uint8_t)(((v % n) + n) % n);
            osd_brief(dc_method_name((dc_method_t)s_set.dc_method));
        } else if (s_adjust == ADJUST_4) {
            int v = s_set.brightness + detents;
            s_set.brightness = (uint8_t)(v < 0 ? 0 : v > GBCAM_BRIGHTNESS_LEVELS - 1 ? GBCAM_BRIGHTNESS_LEVELS - 1 : v);
            /* no OSD: the live preview itself shows the change, same as GB/Normal Cam */
        } else {
            int v = s_set.contrast + detents;
            s_set.contrast = (uint8_t)(v < 0 ? 0 : v > GBCAM_CONTRAST_LEVELS - 1 ? GBCAM_CONTRAST_LEVELS - 1 : v);
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
    if (webcam_exit_long_press(ev)) return;

    if (usb_msc_prompt_pending()) {
        /* Steals input while the prompt is up, same as a normal button click
         * still would - a photo shouldn't fire underneath it. */
        if (ev->type == INPUT_CLICK && ev->button == BTN_MENU) {
            usb_msc_accept();
            if (usb_msc_active()) {
                camera_pause(); /* nothing live to show once the SD card's handed to the host */
                s_screen = SCREEN_USB;
            } else {
                osd_text("USB FAILED", NULL); /* see app_usb.c's log for why */
            }
        } else if (ev->type == INPUT_CLICK && ev->button == BTN_CAMMODE) {
            /* Mirror mode: no dedicated screen, no camera_pause() - it just
             * starts feeding whatever's already on screen (any camera mode,
             * the menu, the gallery...) to USB alongside normal use. See
             * usb_webcam_feed_screen() and app_usb.h's comment. */
            s_webcam_mirror = true;
            if (usb_webcam_accept()) osd_brief("MIRROR ON");
            else osd_text("WEBCAM FAILED", NULL); /* no PSRAM for the JPEG buffer - see app_usb.c */
        } else if (ev->type == INPUT_CLICK && ev->button == BTN_MODE) {
            /* GB Webcam: just the photo (+ frame, if on), see usb_webcam_feed_gb(). */
            s_webcam_mirror = false;
            if (usb_webcam_accept()) osd_brief("GB WEBCAM ON");
            else osd_text("WEBCAM FAILED", NULL);
        } else if (ev->type == INPUT_CLICK && ev->button == BTN_SHUTTER) {
            usb_msc_decline();
        }
        return;
    }
    switch (ev->type) {
    case INPUT_PRESS:
        if (ev->button == BTN_SHUTTER) take_photo();
        break;
    case INPUT_CLICK:
        if (ev->button == BTN_MODE) {
            s_adjust = (adjust_t)((s_adjust + 1) % adjust_count(s_set.cam_mode));
            s_set.adjust[s_set.cam_mode] = (uint8_t)s_adjust;
            settings_changed(&s_set);
            if (s_set.cam_mode == CAM_MODE_DITHER)
                osd_brief(s_adjust == ADJUST_0 ? "AMOUNT" : s_adjust == ADJUST_1 ? "PALETTE"
                        : s_adjust == ADJUST_2 ? "SIZE" : s_adjust == ADJUST_3 ? "METHOD"
                        : s_adjust == ADJUST_4 ? "BRIGHTNESS" : "CONTRAST");
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
    if (webcam_exit_long_press(ev)) return;
    if (ev->type == INPUT_ROTATE) {
        int pos = s_gallery_pos + ev->value;
        int max = storage_count() - 1;
        s_gallery_pos = pos < 0 ? 0 : pos > max ? max : pos;
        s_delete_armed_until_us = 0;
        s_osd.until_us = 0;
        s_gallery_view = GALLERY_VIEW_CROP2X;
        s_gallery_dirty = true;
        return;
    }
    if (ev->type == INPUT_PRESS && ev->button == BTN_SHUTTER) {
        leave_gallery();
    } else if (ev->type == INPUT_CLICK && ev->button == BTN_MENU) {
        /* Select: grid -> drill into the highlighted photo; single photo ->
         * back out to the grid, like the real Game Boy Camera's album. */
        s_gallery_grid = !s_gallery_grid;
        s_gallery_view = GALLERY_VIEW_CROP2X;
        s_gallery_dirty = true;
    } else if (ev->type == INPUT_CLICK && ev->button == BTN_MODE) {
        /* Cycles 2x crop (no frame) -> 1:1 native (with frame, if the photo
         * has one) -> fit-to-screen (as big as it goes, still with frame if
         * it has one) -> back to 2x crop. Dither/Normal Cam photos aren't
         * shades, there's nothing to frame/crop specially, so this is a
         * no-op for those. */
        if (!s_gallery_grid && !storage_is_dc_at(s_gallery_pos)) {
            s_gallery_view = (gallery_view_t)((s_gallery_view + 1) % GALLERY_VIEW_COUNT);
            s_gallery_dirty = true;
        }
    } else if (ev->type == INPUT_CLICK && ev->button == BTN_CAMMODE) {
        if (now_us() < s_delete_armed_until_us) {
            storage_delete(storage_number_at(s_gallery_pos), storage_is_dc_at(s_gallery_pos));
            s_grid_cache_page = -1; /* numbers on this page shifted - force a fresh decode */
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

/* Edge-triggered so it warns once per drop below the threshold, not every
 * call - and can warn again on a later drop (e.g. it recovered because the
 * board got plugged in to charge, then was unplugged and ran back down).
 *
 * The edge itself lives here rather than in the two places that show it
 * because both loops need the same answer: app_step() below, and the
 * emulator's own loop, which never reaches app_step() at all (see
 * app_gbemu.c's run_rom()) and is where the battery drains fastest. */
#define LOW_BATTERY_PCT 15
static bool s_low_battery_warned;

bool app_low_battery_edge(void)
{
    int pct = plat_battery_percent();
    if (pct < 0) return false; /* no gauge to read - nothing to warn about */
    if (pct > LOW_BATTERY_PCT) {
        s_low_battery_warned = false;
        return false;
    }
    if (s_low_battery_warned) return false;
    s_low_battery_warned = true;
    return true;
}

static void check_low_battery(void)
{
    if (app_low_battery_edge()) osd_text("LOW BATTERY", NULL);
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

    /* No freeze-frame review of the just-taken photo any more - just the
     * shutter animation over whatever's live right now (see take_photo()'s
     * comment). s_still/s_still_rgb still exist as the actual save buffers
     * (storage_save()/storage_save_dc() read from them), just not shown. */
    float shutter_curtain = 0.0f; /* 0 = no curtains - see display_shutter() */
    if (now_us() < s_freeze_until_us) {
        /* Opening half only (progress 0.5..1.0) - take_photo() already
         * closed the curtains itself (its own grab_process_draw() calls,
         * ramping 0 -> 0.5) before the slow capture/save work, so this just
         * continues smoothly from there to fully open, rather than
         * restarting the whole close-then-open cycle and visibly jumping
         * from closed straight to open on the first post-capture frame.
         * Runs out exactly when s_freeze_until_us does - same window, no
         * separate timer (see take_photo()/FREEZE_MS). */
        float frac = 1.0f - (float)(s_freeze_until_us - now_us()) / (FREEZE_MS * 1000.0f);
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        shutter_curtain = 0.5f + 0.5f * frac;
    }
    int64_t t1, t2;
    if (grab_process_draw(shutter_curtain, &t1, &t2) != ESP_OK) {
        plat_sleep_ms(10);
        return;
    }

    /* Unconditional, not just on the "we're ahead of schedule" path above -
     * when a frame genuinely takes longer than the pacing target (a slow
     * Digicam preset can run well behind it), t0 is never < s_next_frame_us
     * and that's the ONLY other place this loop ever sleeps, so without this
     * the idle task can go without a timeslice for as long as frames keep
     * arriving late - which is exactly when it's most likely to happen,
     * and starves the idle task's own watchdog reset until it fires. 1ms is
     * enough for the scheduler to run something else without being a real
     * frame-rate cost. */
    plat_sleep_ms(1);

    int64_t proc = camera_last_process_us();
    s_stats.process_cam += proc;
    s_stats.wait += (t1 - t0) - proc;
    s_stats.process_look += t2 - t1;
    s_stats.draw += now_us() - t2;
    if (++s_stats.frames == STATS_FRAMES) log_stats();
}

/* A saved GBnnnnn.PNG may have a frame baked in (storage_save()'s
 * frame_compose_rgb() call, at GB_PNG_SCALE) - the plain 128x112 photo sits
 * at a fixed, frame-geometry-dependent offset inside it (frames.h's
 * FRAME_PHOTO_X and frame_meta_t's photo_y - one of gen_frames.py's two
 * supported canvas heights, 144 or 224). Which offset applies is worked out
 * purely from the loaded PNG's own dimensions - nothing about which frame
 * (if any) was used needs to be known or stored per photo, since there are
 * only ever these three possible saved sizes. Fills s_gallery_plain_rgb
 * (128x112) with the plain photo, downsampling the 4x4 solid blocks
 * GB_PNG_SCALE's nearest-neighbour upscale left behind back to 1x1 each. */
static void extract_plain_photo(const uint8_t *rgb, int w, int h)
{
    int photo_x = 0, photo_y = 0;
    if (w == GBCAM_W * GB_PNG_SCALE && h == GBCAM_H * GB_PNG_SCALE) {
        /* unframed - already just the photo */
    } else if (h == 144 * GB_PNG_SCALE) {
        photo_x = FRAME_PHOTO_X * GB_PNG_SCALE;
        photo_y = 2 * 8 * GB_PNG_SCALE;
    } else if (h == 224 * GB_PNG_SCALE) {
        photo_x = FRAME_PHOTO_X * GB_PNG_SCALE;
        photo_y = 5 * 8 * GB_PNG_SCALE;
    } /* else: unrecognised size (corrupt/foreign file) - best effort, top-left crop */

    for (int oy = 0; oy < GBCAM_H; oy++) {
        const uint8_t *src = rgb + (size_t)(photo_y + oy * GB_PNG_SCALE) * w * 3 + (size_t)photo_x * 3;
        uint8_t *dst = s_gallery_plain_rgb + (size_t)oy * GBCAM_W * 3;
        for (int ox = 0; ox < GBCAM_W; ox++) {
            const uint8_t *s = src + (size_t)ox * GB_PNG_SCALE * 3;
            dst[ox * 3 + 0] = s[0];
            dst[ox * 3 + 1] = s[1];
            dst[ox * 3 + 2] = s[2];
        }
    }
}

/* The saved PNG is GB_PNG_SCALE upscaled (storage_save()) - "1x" for the
 * Mode-button gallery view means the original canvas resolution (the frame's
 * own 160x144/160x224, or 128x112 unframed), one canvas pixel per screen
 * pixel, not one (already 4x-oversized) PNG pixel per screen pixel - that
 * would just be a zoomed-in crop of a quarter of the image. Downscales by
 * picking one sample per GB_PNG_SCALE x GB_PNG_SCALE block (already solid
 * colour, from the original nearest-neighbour upscale) into s_frame_rgb,
 * reused here since its size (the biggest canvas, Wild frames) covers every
 * case. w/h out are the resulting native size.
 *
 * Only valid for one of the three sizes storage_save() writes - see
 * saved_png_size_known(), which the caller checks first. Anything else here
 * writes past the end of s_frame_rgb by construction. */

/* The three sizes a saved GB photo can be: the unframed 128x112 canvas, or a
 * framed one at *the frame canvas' 160 width, 144 or 224 tall (frames.h's two
 * supported heights), all at GB_PNG_SCALE. Both downscale_to_native() and
 * extract_plain_photo() index a photo with fixed offsets sized for exactly
 * these, so a .PNG that is none of them - a hand-made file dropped into
 * /GBCAM over the USB MSC drive it's presented as, or a corrupt one - has to
 * be shown whole instead of through either. Both of those read from a buffer
 * the decoder sized from the file's own dimensions, so an unrecognised size is
 * not a read bug; it's the fixed-size s_frame_rgb/s_gallery_plain_rgb writes
 * and the fixed crop offsets that don't hold. */
static bool saved_png_size_known(int w, int h)
{
    if (w == GBCAM_W * GB_PNG_SCALE && h == GBCAM_H * GB_PNG_SCALE) return true;
    if (w == 160 * GB_PNG_SCALE && (h == 144 * GB_PNG_SCALE || h == 224 * GB_PNG_SCALE)) return true;
    return false;
}

static void downscale_to_native(const uint8_t *rgb, int w, int h, int *out_w, int *out_h)
{
    int nw = w / GB_PNG_SCALE, nh = h / GB_PNG_SCALE;
    for (int y = 0; y < nh; y++) {
        const uint8_t *src = rgb + (size_t)(y * GB_PNG_SCALE) * w * 3;
        uint8_t *dst = s_frame_rgb + (size_t)y * nw * 3;
        for (int x = 0; x < nw; x++)
            memcpy(dst + (size_t)x * 3, src + (size_t)(x * GB_PNG_SCALE) * 3, 3);
    }
    *out_w = nw;
    *out_h = nh;
}

/* Decoding a page's thumbnails (SD read + PNG/JPEG decode, x4) is real work -
 * enough that redoing it on every single encoder detent (most of which just
 * move the highlight within the SAME page, not to a different one) made the
 * grid visibly lag behind the scroll wheel. Cached per cell, keyed by which
 * page is currently showing (s_grid_cache_* above); only actually redecoded
 * when the page changes (or the cache is explicitly dropped - a delete or
 * re-entering the gallery, since either can change what these photo numbers
 * even are). Moving the highlight within the same page just redraws the
 * same buffers. */
static void gallery_grid_cache_drop(void)
{
    for (int i = 0; i < GALLERY_GRID_CELLS; i++) {
        storage_free_dc(s_grid_cache_rgb[i]);
        s_grid_cache_rgb[i] = NULL;
    }
    s_grid_cache_page = -1;
}

static void gallery_grid_frame(void)
{
    display_begin_gallery_grid();
    int page = (s_gallery_pos / GALLERY_GRID_CELLS) * GALLERY_GRID_CELLS;
    if (page != s_grid_cache_page) {
        gallery_grid_cache_drop();
        for (int i = 0; i < GALLERY_GRID_CELLS; i++) {
            int pos = page + i;
            if (pos >= storage_count()) continue;
            int number = storage_number_at(pos);
            bool is_dc = storage_is_dc_at(pos);
            esp_err_t err = storage_load_thumb(number, is_dc, &s_grid_cache_rgb[i], &s_grid_cache_w[i], &s_grid_cache_h[i]);
            if (err != ESP_OK) s_grid_cache_rgb[i] = NULL;
        }
        s_grid_cache_page = page;
    }
    for (int i = 0; i < GALLERY_GRID_CELLS; i++) {
        int pos = page + i;
        bool selected = pos == s_gallery_pos;
        display_grid_cell(i, s_grid_cache_rgb[i], s_grid_cache_w[i], s_grid_cache_h[i], selected);
    }
}

static void gallery_frame(void)
{
    bool osd_visible = now_us() <= s_osd.until_us;
    if (!s_gallery_dirty && !osd_visible) {
        plat_sleep_ms(20);
        return;
    }
    if (s_delete_armed_until_us && now_us() >= s_delete_armed_until_us) s_delete_armed_until_us = 0;

    if (s_gallery_grid) {
        gallery_grid_frame();
        draw_osd();
        display_end_frame();
        s_gallery_dirty = osd_visible;
        plat_sleep_ms(20);
        return;
    }

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
        /* The actual saved GBnnnnn.PNG, not a recompose from the .BIN's
         * palette-free shades against whatever palette/frame happen to be
         * selected right now - this always shows the photo exactly as it
         * was saved, in every view. Default: the plain photo (any frame
         * stripped back out - see extract_plain_photo()), 2x cropped, like
         * the viewfinder's own default view. Mode button cycles to 1x (with
         * its frame if it was saved with one), then to fit-to-screen (same
         * idea, just scaled up as big as it goes instead of shown at exact
         * native size - whatever the PNG's own dimensions are, framed or
         * not, same area_fit()/PPA path the live viewfinder itself uses). */
        uint8_t *rgb;
        int w, h;
        if (storage_load_gb_png(number, &rgb, &w, &h) == ESP_OK) {
            if (!saved_png_size_known(w, h)) {
                /* Not a photo this firmware wrote - show it whole (the one
                 * generic path) rather than through the fixed offsets below,
                 * and say so: this is a file on a card that is also a USB
                 * drive, so it's something the user can act on. */
                PLOGW(TAG, "photo #%d is %dx%d, not a saved photo size - showing it scaled to fit", number, w, h);
                display_begin_camera(rgb, w, h, true);
            } else if (s_gallery_view == GALLERY_VIEW_FIT) {
                display_begin_camera(rgb, w, h, true);
            } else if (s_gallery_view == GALLERY_VIEW_NATIVE1X) {
                int nw, nh;
                downscale_to_native(rgb, w, h, &nw, &nh);
                display_begin_native(s_frame_rgb, nw, nh);
            } else {
                extract_plain_photo(rgb, w, h);
                display_begin_camera(s_gallery_plain_rgb, GBCAM_W, GBCAM_H, false);
            }
            storage_free_dc(rgb);
        } else {
            display_begin_blank(0, 0, 0);
        }
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

/* frames_sd_start()'s background scan, two things it can't do for itself: it
 * runs on its own task, so the display stays the main task's to draw (the scan
 * publishes text, this shows it as an OSD instead of letting another task
 * touch the panel mid-frame); and the frame list only exists once it
 * finishes, so the saved frame index can't be checked against it until then.
 * Both are no-ops once the scan is done. */
static void frames_boot_status(void)
{
    char l1[40], l2[40];
    if (frames_sd_status(l1, sizeof l1, l2, sizeof l2)) osd_text(l1, l2[0] ? l2 : NULL);
}

/* SD content may have changed since s_set.frame was saved, so the saved index
 * is checked against the finished list - but only once that list exists.
 * Doing it at boot, the way this used to work, would throw away a perfectly
 * good SD frame choice and fall back to no frame at all, because the list it
 * would be checked against is still loading. */
static void frames_settle(void)
{
    static bool settled;
    if (settled || !frames_sd_ready()) return;
    settled = true;
    if (!frame_available(s_set.frame)) {
        s_set.frame = 0;
        settings_changed(&s_set);
    }
}

/* ------------------------------------------------------------- boot timing */

/* The only way to tell where a slow boot actually goes: there is no board-side
 * profiler here, and the interesting part is spread across NVS, the SD card and
 * the camera ISP. esp_timer_get_time() starts counting in the second-stage
 * loader, so these are milliseconds since the chip came out of reset, bootloader
 * work included - not "time spent in app_init()". Stamped at the milestones in
 * app_init() below and printed once, before the resume there can block for a
 * whole session. */
#define BOOT_STAGES 9
static const char *const BOOT_STAGE_NAMES[BOOT_STAGES] = {"reset", "nvs",    "display", "input", "sd",
                                                          "frames", "palettes", "usb",  "camera"};
static int64_t s_boot_ms[BOOT_STAGES];
static int s_boot_stage;

static void boot_stamp(void)
{
    if (s_boot_stage < BOOT_STAGES) s_boot_ms[s_boot_stage++] = now_us() / 1000;
}

static void boot_log(void)
{
    char buf[160];
    int n = 0;
    for (int i = 1; i < s_boot_stage && n < (int)sizeof buf - 24; i++)
        n += snprintf(buf + n, sizeof buf - (size_t)n, " %s=%lld", BOOT_STAGE_NAMES[i],
                      (long long)(s_boot_ms[i] - s_boot_ms[i - 1]));
    PLOGI(TAG, "boot: %lldms total -%s", (long long)s_boot_ms[s_boot_stage - 1], buf);
}

/* ------------------------------------------------------- the waking touch */

static void app_boot_swallow_arm(void)
{
    s_boot_swallow = true;
    s_boot_swallow_deadline_us = now_us() + BOOT_SWALLOW_MS * 1000;
}

bool app_boot_swallow_poll(void)
{
    if (!s_boot_swallow) return false;
    /* Two conditions for the two halves of the same problem: the deadline covers
     * a finger already released before input_init() ran (nothing was ever
     * queued, so there is nothing left to swallow), and the held/queued test
     * covers one still down - which stays swallowed until the whole touch is
     * over, the same rule s_swallow_press follows and for the same reason (the
     * click that ends the press is queued by the poll that clears the state). */
    if (now_us() < s_boot_swallow_deadline_us) return true;
    if (input_any_held() || input_pending()) return true;
    s_boot_swallow = false;
    return false;
}

/* -------------------------------------------------------------- waking up */

void app_resume_note_gbemu(const char *rom_path, int slot, bool state_saved)
{
    s_resume_gbemu = true;
    s_resume_state_saved = state_saved;
    snprintf(s_resume_rom, sizeof s_resume_rom, "%s", rom_path ? rom_path : "");
    s_resume_slot = slot;
}

/* The wake side of the resume record. Called once, near the end of app_init(),
 * where display/input/storage/camera are all up - and deliberately before the
 * idle clock is stamped, because the emulator case blocks right here for as long
 * as the session lasts.
 *
 * Every way this can fail ends on the plain viewfinder, which is what a cold
 * boot shows anyway: no SD card, ROM deleted, gallery emptied, a record from
 * another build. Nothing here is worth refusing to boot over. */
static void resume_apply(void)
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_GPIO || s_resume.magic != RESUME_MAGIC ||
        s_resume.version != RESUME_VERSION) {
        /* Cold boot (the record is all zeros), a stale one, or a wake that
         * recorded nothing. Dropped either way - app_enter_sleep() writes a fresh
         * one every time it runs. */
        memset(&s_resume, 0, sizeof s_resume);
        return;
    }

    resume_rec_t r = s_resume;
    memset(&s_resume, 0, sizeof s_resume); /* one-shot: the next wake starts clean */
    PLOGI(TAG, "resuming, kind %d", (int)r.kind);

    switch (r.kind) {
    case RESUME_MENU:
        /* enter_menu()'s own body minus its s_menu_sel = 0: the position is the
         * whole point of restoring this screen. */
        camera_pause();
        build_menu();
        s_menu_sel = r.menu_sel < 0 ? 0 : r.menu_sel >= s_menu_count ? s_menu_count - 1 : (int)r.menu_sel;
        s_screen = SCREEN_MENU;
        break;

    case RESUME_GALLERY:
        /* enter_gallery()'s checks, but not its defaults: the photo list is
         * whatever the card holds now, so an index that no longer exists clamps
         * rather than being taken on trust. */
        if (!storage_ready()) break; /* "NO SD CARD" is already on screen - see app_init() */
        if (storage_count() == 0) {
            osd_text("NO PHOTOS", NULL);
            break;
        }
        camera_pause();
        s_gallery_pos = r.gallery_pos < 0 ? 0
                        : r.gallery_pos >= storage_count() ? storage_count() - 1
                                                           : (int)r.gallery_pos;
        s_gallery_grid = r.gallery_grid != 0;
        s_gallery_view = (r.gallery_view >= 0 && r.gallery_view < GALLERY_VIEW_COUNT)
                             ? (gallery_view_t)r.gallery_view
                             : GALLERY_VIEW_CROP2X;
        s_grid_cache_page = -1; /* force a fresh decode - see gallery_grid_frame() */
        s_gallery_dirty = true;
        s_delete_armed_until_us = 0;
        s_screen = SCREEN_GALLERY;
        break;

    case RESUME_GBEMU:
        /* Deliberately no camera_pause() first: a GB Camera ROM's viewfinder
         * needs the ISP actually streaming, the same reason cycle_cam_mode() does
         * not pause either. The epilogue afterwards is that same function's,
         * which this path would otherwise never reach - the device sleeps *inside*
         * gbemu_run(), so the cam_mode = CAM_MODE_GB reset after it never runs
         * and the in-RAM copy is still CAM_MODE_EMULATOR. (NVS never sees that
         * value: settings_changed() is only called once gbemu_run() has
         * returned.) */
        if (gbemu_run_rom(r.emu_rom, r.emu_slot)) {
            /* Ending that session lands on the ROM list, exactly like ending any
             * other one - the resumed session was an ordinary session from the
             * moment it started, and its exit should not be the one place that
             * drops out to the camera instead. Returns from here only once the
             * list itself is left. */
            gbemu_run();
            camera_resume();
            s_set.cam_mode = CAM_MODE_GB;
            s_adjust = load_adjust(s_set.cam_mode);
            settings_changed(&s_set);
            s_last_input_us = now_us(); /* a whole session is not idle time */
        }
        break;

    default: /* RESUME_VIEWFINDER: s_screen already is it */
        break;
    }
}

esp_err_t app_init(void)
{
    /* First thing, before anything can want the C6: the reset line may still
     * be held low from the last deep sleep, and an RTC hold outlives the wake
     * (see wifi_gallery_cp_release_hold()). */
    wifi_gallery_cp_release_hold();

    /* Say why this boot happened: the panel, the SD card and the C6 are in the
     * same state after a wake as after a power-on, so this line and the resume
     * record below are the only things that can tell the two apart - and it's
     * the only way to tell a sleep that worked from one that never fired at
     * all. WARN rather than INFO because a deep-sleep wake should be the common
     * case on a battery. */
    bool woke = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO;
    if (woke)
        ESP_LOGW(TAG, "boot: woke from deep sleep, gpio mask 0x%08llx",
                 (unsigned long long)esp_sleep_get_gpio_wakeup_status());
    else
        /* Both numbers, not just "not a sleep wake": a brownout (9) when the
         * rails are cut and a panic (4) inside esp_deep_sleep_start() would
         * otherwise read the same from here, and they need opposite fixes. */
        ESP_LOGW(TAG, "boot: not a gpio sleep wake (cause=%d reset=%d)",
                 (int)esp_sleep_get_wakeup_cause(), (int)esp_reset_reason());
    boot_stamp(); /* reset */

    settings_load(&s_set);
    s_adjust = load_adjust(s_set.cam_mode);
    boot_stamp(); /* nvs */

    s_cam = plat_calloc_fast(sizeof(gbcam_t));
    s_dc_rgb = calloc(1, (size_t)DC_MAX_W * DC_MAX_H * 3);
    s_still_rgb = calloc(1, (size_t)DC_MAX_W * DC_MAX_H * 3);
    s_dc_quant_work = calloc(1, (size_t)DC_MAX_W * DC_MAX_H * 3 * sizeof(float));
    s_webcam_rgb = calloc(1, (size_t)WEBCAM_FRAME_W * WEBCAM_FRAME_H * 3);
    s_screen_rgb = calloc(1, (size_t)DISP_W * DISP_H * 3);
    s_frame_compose_scratch = calloc(1, (size_t)WEBCAM_FRAME_W * WEBCAM_FRAME_H * 3);
    /* These three used to be plain static arrays (~165KB combined) - fine on
     * a desktop, but on this board that's ~165KB permanently reserved out of
     * a small internal RAM budget, whether or not a frame's actually being
     * processed right now. calloc() puts anything over 16KB in PSRAM
     * automatically (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL - see
     * sdkconfig.defaults), same as s_dc_rgb/s_still_rgb above already did. */
    s_still = calloc(1, GBCAM_PIXELS);
    s_frame_rgb = calloc(1, 160 * 224 * 3);
    s_gallery_plain_rgb = calloc(1, GBCAM_W * GBCAM_H * 3);
    if (!s_cam || !s_dc_rgb || !s_still_rgb || !s_still || !s_frame_rgb || !s_gallery_plain_rgb || !s_dc_quant_work ||
        !s_webcam_rgb || !s_screen_rgb || !s_frame_compose_scratch) {
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
    /* Restore RGB mode on boot if it was persisted - rgb_mode_active() can't
     * just check s_set.palette alone (see its own comment): s_cam_r/g/b are
     * lazily allocated, normally only by activate_menu_row() actually cycling
     * onto the RGB sentinel, so a fresh boot would otherwise silently render
     * the plain GB pipeline (with PALETTE still reading "TRICHROME" in the menu)
     * until the user cycled it again - confirmed on real hardware. */
    if (s_set.cam_mode == CAM_MODE_GB && s_set.palette == (uint8_t)gbcam_palette_count())
        rgb_mode_ensure_ready();
    normal_size(s_set.normal_size, &s_dc_w, &s_dc_h);

    esp_err_t err = display_init();
    if (err != ESP_OK) {
        PLOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        return err;
    }
    display_begin_blank(0, 0, 0);
    display_text(DISP_W / 2 - display_text_width("PIXELBOY", 3) / 2, 100, 3, "PIXELBOY", 255, 255, 255);
    display_end_frame();
    /* display_hw_init() leaves the panel at 100% - bring it straight to the
     * saved level instead, so the boot splash and everything after it are
     * already at the level the user picked rather than flashing full-bright. */
    display_set_backlight(BACKLIGHT_PERCENT[s_set.backlight]);
    boot_stamp(); /* display */

    err = input_init();
    if (err != ESP_OK) {
        PLOGE(TAG, "input init failed: %s", esp_err_to_name(err));
        return err;
    }
    boot_stamp(); /* input */
    /* The press that woke the device is not meant for the app - input_init() is
     * only now polling, so a finger still down would otherwise be the first
     * thing the resumed screen sees (see app_boot_swallow_poll()). */
    if (woke) app_boot_swallow_arm();
    storage_init(); /* runs without an SD card; photos are just unavailable */
    boot_stamp();   /* sd */    /* Warn now, not just the first time Shutter is pressed - so it's known
     * before shooting starts, not after. No SD card also means no /FRAMES,
     * /ROMS or /PALETTES to scan, so nothing slow follows this that could
     * make the message expire before the live viewfinder is even up. */
    if (!storage_ready()) osd_text("NO SD CARD", NULL);
    /* SD card's /FRAMES and /ROMS, if any - backgrounded, so this returns at
     * once and the viewfinder is up while the card is still being read (see
     * app_frames_sd.h). The saved frame index is checked against the finished
     * list later, in frames_settle(), not here - the list is empty here even
     * on a card full of frames. */
    frames_sd_start();
    boot_stamp(); /* frames */

    palettes_sd_init(); /* SD card's /PALETTES, if any - see app_palettes_sd.h */
    gbcam_set_extra_palettes(palettes_sd_gb_count(), palettes_sd_gb_rgb, palettes_sd_gb_name);
    dc_set_extra_palettes(palettes_sd_dc_count(), palettes_sd_dc_colors, palettes_sd_dc_name);
    dc_set_extra_lut_io(palettes_sd_load_lut, palettes_sd_save_lut); /* cache each one's nearest-colour LUT on the SD card, not RAM */
    /* > not >=: gbcam_palette_count() itself is the valid RGB sentinel (see
     * rgb_mode_active()), one slot past the real palettes. */
    if (s_set.palette > (uint8_t)gbcam_palette_count()) s_set.palette = GBCAM_PALETTE_DEFAULT;
    if (s_set.dc_palette >= (uint8_t)dc_palette_count()) s_set.dc_palette = DC_PALETTE_DEFAULT;
    boot_stamp(); /* palettes */

    err = usb_msc_init();
    if (err != ESP_OK) PLOGW(TAG, "USB MSC init failed: %s - USB photo browsing unavailable", esp_err_to_name(err));
    boot_stamp(); /* usb */

    err = camera_init();
    if (err != ESP_OK) {
        PLOGE(TAG, "camera init failed: %s", esp_err_to_name(err));
        display_begin_blank(0, 0, 0);
        display_text(20, 110, 2, "CAMERA ERROR", 255, 80, 80);
        display_end_frame();
        return err;
    }
    /* camera_ppa_init(); - disabled for now: real hardware showed visible
     * corruption (right/bottom of frame) with no measurable speed win over
     * the CPU path either, so this isn't earning its risk yet. Code stays
     * in place (camera_ppa_esp.c, and its include above is kept for this one
     * line) for a more careful follow-up pass - needs figuring out why before
     * it's worth turning back on. */

    boot_stamp(); /* camera */
    /* Printed here, before resume_apply() below: that call blocks for as long as
     * a resumed emulator session lasts, so a timeline printed after it would be
     * a timeline of the session, not of the boot. */
    boot_log();
    /* Where the app really comes back to. Blocks in the emulator case, and
     * deliberately before the idle clock is stamped below - see app.h. */
    resume_apply();

    s_stats.t0 = now_us();
    s_last_input_us = now_us(); /* don't count boot itself as idle time - see ROW_SLEEP */
#ifdef ESP_PLATFORM
    PLOGI(TAG, "free heap: %u KB internal, %u KB PSRAM",
          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
#endif
    return ESP_OK;
}

/* Blit an RGB888 src_w x src_h image into s_webcam_rgb (WEBCAM_FRAME_W x
 * WEBCAM_FRAME_H), centred on black - shared by both feed functions below,
 * since neither the 240x240 screen nor an arbitrary frame's canvas is
 * guaranteed to exactly fill the fixed webcam output size. Clears the whole
 * canvas first (not just the padding), same cost either way at this size. */
static void webcam_blit_centered(const uint8_t *src, int src_w, int src_h)
{
    memset(s_webcam_rgb, 0, (size_t)WEBCAM_FRAME_W * WEBCAM_FRAME_H * 3);
    int ox = (WEBCAM_FRAME_W - src_w) / 2, oy = (WEBCAM_FRAME_H - src_h) / 2;
    for (int y = 0; y < src_h; y++)
        memcpy(s_webcam_rgb + (size_t)((oy + y) * WEBCAM_FRAME_W + ox) * 3, src + (size_t)y * src_w * 3, (size_t)src_w * 3);
}

/* Mirror mode: convert whatever display_end_frame() just sent to the LCD
 * (RGB565) into RGB888 and hand it to the USB video interface. Cheap no-op
 * via usb_webcam_feed() itself when webcam mode's off or the host isn't
 * actively pulling frames, but skip the conversion work entirely in that
 * case too - it's real per-pixel work at 240x240, no point doing it unread. */
static void usb_webcam_feed_screen(void)
{
    if (!usb_webcam_active() || !s_webcam_mirror) return;
    const uint16_t *fb = display_last_frame();
    for (int i = 0; i < DISP_W * DISP_H; i++) {
        /* fb[] is byte-swapped for the panel (BSP_LCD_BIGENDIAN) - see
         * app_display.c's rgb565(); undo that first. */
        uint16_t p = (uint16_t)((fb[i] >> 8) | (fb[i] << 8));
        uint8_t *o = s_screen_rgb + (size_t)i * 3;
        o[0] = (uint8_t)((p >> 11) * 255 / 31);        /* 5-bit R */
        o[1] = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63); /* 6-bit G */
        o[2] = (uint8_t)((p & 0x1F) * 255 / 31);        /* 5-bit B */
    }
    webcam_blit_centered(s_screen_rgb, DISP_W, DISP_H);
    usb_webcam_feed(s_webcam_rgb);
}

/* GB Webcam: just the GB Camera photo (gbcam_t.shades, palette-mapped) at
 * 3x - an exact fit for WEBCAM_FRAME_W/H, no padding needed - with its
 * frame composited in if one's on (frame_compose_rgb(), scaled down to fit
 * the fixed canvas if it would've been bigger at 3x, then centred same as
 * the plain-photo case). shades[] is only refreshed while GB Camera is the
 * selected camera mode (see grab_process_draw()) - this just freezes on the
 * last GB Camera frame otherwise, same as a stale gallery/menu screen
 * behind Mirror mode would. */
static void usb_webcam_feed_gb(void)
{
    if (!usb_webcam_active() || s_webcam_mirror) return;
    if (!frame_available(s_set.frame)) {
        for (int y = 0; y < GBCAM_H; y++) {
            for (int x = 0; x < GBCAM_W; x++) {
                const uint8_t *c = gbcam_palette_rgb((gbcam_palette_t)s_set.palette, s_cam->shades[y * GBCAM_W + x]);
                for (int dy = 0; dy < 3; dy++) {
                    uint8_t *o = s_webcam_rgb + (size_t)((y * 3 + dy) * WEBCAM_FRAME_W + x * 3) * 3;
                    for (int dx = 0; dx < 3; dx++) { o[dx * 3] = c[0]; o[dx * 3 + 1] = c[1]; o[dx * 3 + 2] = c[2]; }
                }
            }
        }
    } else {
        const frame_meta_t *fm = frames_get(s_set.frame - 1);
        int fw, fh;
        frame_size(fm, 1, &fw, &fh);
        int scale = 3;
        while (scale > 1 && (fw * scale > WEBCAM_FRAME_W || fh * scale > WEBCAM_FRAME_H)) scale--;
        int ow, oh;
        frame_size(fm, scale, &ow, &oh);
        frame_compose_rgb(fm, s_cam->shades, (gbcam_palette_t)s_set.palette, scale, s_frame_compose_scratch);
        webcam_blit_centered(s_frame_compose_scratch, ow, oh);
    }
    usb_webcam_feed(s_webcam_rgb);
}

void app_step(void)
{
    /* The card's frames load in the background (app_frames_sd.h) - both are
     * one flag read once it's done. */
    frames_boot_status();
    frames_settle();

    usb_msc_tick();
    if (s_screen == SCREEN_USB && !usb_msc_active()) {
        /* Cable pulled, or the host ejected/released the drive on its own -
         * usb_msc_tick() already tore the handoff down, just leave the
         * dedicated screen. */
        camera_resume();
        s_screen = SCREEN_VIEWFINDER;
    }

    /* An event that's already waiting counts as activity now, before it gets
     * dispatched below: both update_backlight() and update_standby() read
     * s_last_input_us, and leaving standby has to have happened by the time an
     * event like "open the menu" runs its own camera_pause() - reacting a step
     * later would dispatch that press with the camera still stopped and the
     * screen still dark, and standby's own camera_resume() would then undo the
     * pause enter_menu() just made. */
    /* Read before the pending input below refreshes the idle clock - this is
     * "was the screen lit when the user touched it". */
    bool was_standby = s_standby;
    if (input_pending()) s_last_input_us = now_us();
    update_standby();
    update_backlight();
    /* Every screen, not just the viewfinder it used to be called from - see
     * its own comment. Cheap: one battery ADC read per app_step(). */
    check_low_battery();

    /* Unconditionally, once per iteration: this call is what expires the
     * wake-touch window, so it must not live inside the drain below (see
     * app_boot_swallow_poll()). */
    bool boot_swallow = app_boot_swallow_poll();

    input_event_t ev;
    while (input_get(&ev, 0)) {
        s_last_input_us = now_us();
        /* Consumed to wake the screen, not dispatched: standby is entered by
         * doing nothing, so whatever was pressed into it was asking to see the
         * screen again, not asking for what that button does - and the live
         * Shutter lands as a photo of a viewfinder the user couldn't see to
         * frame.
         *
         * The whole touch is swallowed, not just the press in it: s_swallow_press
         * stays set until the button comes back up (see below), because the
         * click or long-press that ends this press arrives after the backlight
         * is already on and would otherwise be dispatched as if the user had
         * meant it. Same rule and same flag as the emulator loop's (see
         * run_rom()); the next touch acts normally. */
        if (was_standby || s_swallow_press || boot_swallow) {
            /* Only the standby case latches the flag: the boot window expires by
             * itself (see app_boot_swallow_poll()), so latching it here would
             * keep swallowing past the end of it. */
            if (was_standby || s_swallow_press) s_swallow_press = true;
            continue;
        }
        if (s_screen == SCREEN_USB) handle_usb_input(&ev);
        else if (s_screen == SCREEN_VIEWFINDER) handle_viewfinder_input(&ev);
        else if (s_screen == SCREEN_MENU) handle_menu_input(&ev);
        else if (s_screen == SCREEN_WIFI) handle_wifi_input(&ev);
        else handle_gallery_input(&ev);
    }

    /* The waking touch is over once nothing is held and nothing is still
     * queued. Both conditions matter: the click that ends a press is put in
     * the queue by the same poll callback that clears the button state, so
     * checking the button alone would drop the flag in the window between the
     * two and let exactly the event this exists to swallow through. */
    if (s_swallow_press && !input_any_held() && !input_pending()) s_swallow_press = false;

    /* Why this fires and when it deliberately doesn't: see app_sleep_due().
     * Nothing to tear down here - app_step() owns no state that doesn't
     * survive being reset - so it goes straight to sleep. */
    if (app_sleep_due(now_us() - s_last_input_us)) app_enter_sleep();

    /* Nothing to draw and nothing to capture - the camera is stopped and the
     * backlight is out (see update_standby()). Sleep a whole poll interval
     * rather than running the frame path, which in the viewfinder would just
     * fail camera_grab() and sleep 10ms itself on every pass. Input arrives
     * through its own queue, so the next event still wakes this promptly on
     * the following iteration. */
    if (s_standby) {
        plat_sleep_ms(STANDBY_POLL_MS);
        settings_tick();
        return;
    }

    if (s_screen == SCREEN_GALLERY) gallery_frame();
    else if (s_screen == SCREEN_USB) usb_screen_frame();
    else if (s_screen == SCREEN_WIFI) wifi_screen_frame();
    else viewfinder_frame(); /* also drives SCREEN_MENU, so the feed keeps live behind it */
    usb_webcam_feed_screen(); /* after the draw above, whichever screen it was - see its own comment */
    usb_webcam_feed_gb();     /* only one of these two actually sends anything - see s_webcam_mirror */

    settings_tick();
}
