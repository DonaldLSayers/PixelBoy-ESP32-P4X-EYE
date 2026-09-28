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
 * the viewfinder's own default view); Mode button (middle) toggles that to
 * 1x with its frame, if it was saved with one. Bottom button: delete (click
 * twice to confirm). Shutter (encoder press) always leaves the gallery
 * entirely and goes back to the camera, from either view.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
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
static bool s_gallery_framed;       /* single GB photo view: Mode button toggles 2x crop/no frame vs 1x/with frame */
static uint8_t *s_gallery_plain_rgb; /* scratch: a saved GB photo with any frame stripped back out, GBCAM_W*GBCAM_H*3 */
static int64_t s_delete_armed_until_us;

/* Decoded grid-thumbnail cache - see gallery_grid_frame(). */
static uint8_t *s_grid_cache_rgb[GALLERY_GRID_CELLS];
static int s_grid_cache_w[GALLERY_GRID_CELLS], s_grid_cache_h[GALLERY_GRID_CELLS];
static int s_grid_cache_page = -1;

/* ------------------------------------------------------------------- menu */

typedef enum {
    ROW_PALETTE, ROW_DITHER, ROW_STYLE, ROW_VF_SCALE, ROW_FRAME, ROW_GB_AUTO, ROW_DC_PALETTE, ROW_DC_METHOD,
    ROW_DC_SIZE,
    ROW_DC_AMOUNT,
    ROW_DC_AUTO,
    ROW_DC_EDGE,
    ROW_NORMAL_SIZE,
    ROW_SLEEP,
    ROW_GALLERY, ROW_WIFI, ROW_EXIT
} menu_row_t;

/* Auto-sleep timeout choices (see SLEEP_OPTIONS_COUNT in app_settings.h,
 * ROW_SLEEP below, and app_step()'s idle check) - 0 = never. */
static const int SLEEP_MINUTES[SLEEP_OPTIONS_COUNT] = {0, 1, 2, 3, 5, 10};

static menu_row_t s_menu_rows[10];
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
        display_begin_camera(s_dc_rgb, s_dc_live_w, s_dc_live_h, s_set.cam_mode == CAM_MODE_NORMAL);
    }
    display_shutter(shutter_curtain);
    draw_osd();
    display_end_frame();
    return ESP_OK;
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
    for (int i = 1; i <= 4; i++)
        if (grab_process_draw(i * 0.125f, NULL, NULL) != ESP_OK) break;

    int n;
    if (rgb_mode_active()) {
        /* True colour, not GB Camera's palette-mapped 2bpp tiles - saved the
         * same way as a Dither/Normal Cam photo (upscaled PNG), not
         * storage_save()'s .BIN+palette pair, since there's no single shade
         * per pixel to store that way. */
        n = storage_ready() ? storage_save_dc(s_rgb_mode_rgb, GBCAM_W, GBCAM_H, false) : -1;
    } else if (s_set.cam_mode == CAM_MODE_GB) {
        memcpy(s_still, s_cam->shades, GBCAM_PIXELS);
        int frame = s_set.frame == 0 ? -1 : (int)s_set.frame - 1;
        n = storage_ready() ? storage_save(s_still, (gbcam_palette_t)s_set.palette, frame) : -1;
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
        n = storage_ready() ? storage_save_dc(s_still_rgb, s_dc_w, s_dc_h, jpeg) : -1;
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

static void enter_sleep(void)
{
    display_begin_blank(0, 0, 0);
    display_text(DISP_W / 2 - display_text_width("SLEEPING", 3) / 2, 100, 3, "SLEEPING", 255, 255, 255);
    display_end_frame();
    plat_sleep_ms(400); /* let it actually show before the screen cuts out */
    plat_enter_deep_sleep(); /* cuts camera/LCD/SD power; wakes (as a fresh boot) on Shutter - never returns */
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
    s_gallery_grid = true;
    s_grid_cache_page = -1; /* force a fresh decode - see gallery_grid_frame() */
    s_gallery_framed = false;
    s_gallery_dirty = true;
    s_delete_armed_until_us = 0;
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

    char labels[10][13], values[10][12]; /* labels: 13, fits "WIFI GALLERY" (12 chars) + null */
    const char *label_ptrs[10], *value_ptrs[10];
    icon_id_t icons[10];
    for (int i = 0; i < s_menu_count; i++) {
        const char *val = NULL;
        switch (s_menu_rows[i]) {
        case ROW_PALETTE:
            snprintf(labels[i], sizeof labels[i], "PALETTE");
            /* One slot past the real palettes is the RGB sentinel - see
             * rgb_mode_active(). */
            if (s_set.palette == (uint8_t)gbcam_palette_count()) truncate_value(values[i], "RGB");
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
            snprintf(values[i], sizeof values[i], "%s", s_set.vf_scale ? "1:1" : "2X CROP");
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
        s_set.vf_scale = (uint8_t)(s_set.vf_scale ? 0 : 1);
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
    case ROW_SLEEP:
        s_set.sleep_min = (uint8_t)((s_set.sleep_min + 1) % SLEEP_OPTIONS_COUNT);
        settings_changed(&s_set);
        break;
    case ROW_GALLERY:
        s_screen = SCREEN_VIEWFINDER; /* enter_gallery() switches it again if it succeeds */
        enter_gallery();
        return;
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
                osd_brief("RGB UNAVAILABLE");
            } else {
                s_set.palette = (uint8_t)new_palette;
                apply_settings();
                osd_brief(s_set.palette == (uint8_t)gbcam_palette_count() ? "RGB"
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
            usb_webcam_accept();
            osd_brief("MIRROR ON");
        } else if (ev->type == INPUT_CLICK && ev->button == BTN_MODE) {
            /* GB Webcam: just the photo (+ frame, if on), see usb_webcam_feed_gb(). */
            s_webcam_mirror = false;
            usb_webcam_accept();
            osd_brief("GB WEBCAM ON");
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
    if (ev->type == INPUT_ROTATE) {
        int pos = s_gallery_pos + ev->value;
        int max = storage_count() - 1;
        s_gallery_pos = pos < 0 ? 0 : pos > max ? max : pos;
        s_delete_armed_until_us = 0;
        s_osd.until_us = 0;
        s_gallery_framed = false;
        s_gallery_dirty = true;
        return;
    }
    if (ev->type == INPUT_PRESS && ev->button == BTN_SHUTTER) {
        leave_gallery();
    } else if (ev->type == INPUT_CLICK && ev->button == BTN_MENU) {
        /* Select: grid -> drill into the highlighted photo; single photo ->
         * back out to the grid, like the real Game Boy Camera's album. */
        s_gallery_grid = !s_gallery_grid;
        s_gallery_framed = false;
        s_gallery_dirty = true;
    } else if (ev->type == INPUT_CLICK && ev->button == BTN_MODE) {
        /* Show the GB photo at 1:1, with its frame if one's currently
         * selected (s_set.frame) - Dither/Normal Cam photos aren't shades,
         * there's nothing to frame, so this is a no-op for those. */
        if (!s_gallery_grid && !storage_is_dc_at(s_gallery_pos)) {
            s_gallery_framed = !s_gallery_framed;
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
 * frame - and can warn again on a later drop (e.g. it recovered because the
 * board got plugged in to charge, then was unplugged and ran back down). */
#define LOW_BATTERY_PCT 15
static bool s_low_battery_warned;

static void check_low_battery(void)
{
    int pct = plat_battery_percent();
    if (pct < 0) return; /* no gauge to read - nothing to warn about */
    if (pct <= LOW_BATTERY_PCT) {
        if (!s_low_battery_warned) {
            s_low_battery_warned = true;
            osd_text("LOW BATTERY", NULL);
        }
    } else {
        s_low_battery_warned = false;
    }
}

static void viewfinder_frame(void)
{
    int64_t t0 = now_us();
    if (t0 < s_next_frame_us) {
        if (camera_skip() != ESP_OK) plat_sleep_ms(5);
        s_stats.skipped++;
        return;
    }
    check_low_battery();
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
 * case. w/h out are the resulting native size. */
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
         * was saved, in both views. Default: the plain photo (any frame
         * stripped back out - see extract_plain_photo()), 2x cropped, like
         * the viewfinder's own default view. Mode button (s_gallery_framed):
         * 1x, with its frame if it was saved with one. */
        uint8_t *rgb;
        int w, h;
        if (storage_load_gb_png(number, &rgb, &w, &h) == ESP_OK) {
            if (s_gallery_framed) {
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
    s_adjust = load_adjust(s_set.cam_mode);

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
    normal_size(s_set.normal_size, &s_dc_w, &s_dc_h);

    esp_err_t err = display_init();
    if (err != ESP_OK) {
        PLOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        return err;
    }
    display_begin_blank(0, 0, 0);
    display_text(DISP_W / 2 - display_text_width("PIXELBOY", 3) / 2, 100, 3, "PIXELBOY", 255, 255, 255);
    display_end_frame();

    err = input_init();
    if (err != ESP_OK) {
        PLOGE(TAG, "input init failed: %s", esp_err_to_name(err));
        return err;
    }
    storage_init(); /* runs without an SD card; photos are just unavailable */
    /* Warn now, not just the first time Shutter is pressed - so it's known
     * before shooting starts, not after. No SD card also means no /FRAMES,
     * /ROMS or /PALETTES to scan, so nothing slow follows this that could
     * make the message expire before the live viewfinder is even up. */
    if (!storage_ready()) osd_text("NO SD CARD", NULL);
    frames_sd_init(frames_boot_progress); /* SD card's /FRAMES and /ROMS, if any - see app_frames_sd.h */
    if (s_set.frame > (uint8_t)frames_total()) s_set.frame = 0; /* SD content may have changed since this was saved */

    palettes_sd_init(); /* SD card's /PALETTES, if any - see app_palettes_sd.h */
    gbcam_set_extra_palettes(palettes_sd_gb_count(), palettes_sd_gb_rgb, palettes_sd_gb_name);
    dc_set_extra_palettes(palettes_sd_dc_count(), palettes_sd_dc_colors, palettes_sd_dc_name);
    dc_set_extra_lut_io(palettes_sd_load_lut, palettes_sd_save_lut); /* cache each one's nearest-colour LUT on the SD card, not RAM */
    /* > not >=: gbcam_palette_count() itself is the valid RGB sentinel (see
     * rgb_mode_active()), one slot past the real palettes. */
    if (s_set.palette > (uint8_t)gbcam_palette_count()) s_set.palette = GBCAM_PALETTE_DEFAULT;
    if (s_set.dc_palette >= (uint8_t)dc_palette_count()) s_set.dc_palette = DC_PALETTE_DEFAULT;

    err = usb_msc_init();
    if (err != ESP_OK) PLOGW(TAG, "USB MSC init failed: %s - USB photo browsing unavailable", esp_err_to_name(err));

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
     * in place (camera_ppa_esp.c) for a more careful follow-up pass -
     * needs figuring out why before it's worth turning back on. */

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
    if (s_set.frame == 0) {
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
    usb_msc_tick();
    if (s_screen == SCREEN_USB && !usb_msc_active()) {
        /* Cable pulled, or the host ejected/released the drive on its own -
         * usb_msc_tick() already tore the handoff down, just leave the
         * dedicated screen. */
        camera_resume();
        s_screen = SCREEN_VIEWFINDER;
    }

    input_event_t ev;
    while (input_get(&ev, 0)) {
        s_last_input_us = now_us();
        if (s_screen == SCREEN_USB) handle_usb_input(&ev);
        else if (s_screen == SCREEN_VIEWFINDER) handle_viewfinder_input(&ev);
        else if (s_screen == SCREEN_MENU) handle_menu_input(&ev);
        else if (s_screen == SCREEN_WIFI) handle_wifi_input(&ev);
        else handle_gallery_input(&ev);
    }

    /* Not while the SD card's handed to a PC (s_screen==SCREEN_USB implies
     * usb_msc_active(), given app_step()'s own check above) - cutting power
     * mid-transfer would be a bad surprise, not just an inconvenience. Same
     * for mirror mode - sleeping would blank the very screen it's streaming.
     * Same for the WiFi gallery - a phone could be mid-download. sleep_min
     * of 0 means "never" (see ROW_SLEEP), not an instant sleep. */
    int sleep_min = SLEEP_MINUTES[s_set.sleep_min];
    if (sleep_min != 0 && s_screen != SCREEN_USB && s_screen != SCREEN_WIFI && !usb_webcam_active() &&
        now_us() - s_last_input_us > (int64_t)sleep_min * 60 * 1000000LL) enter_sleep();

    if (s_screen == SCREEN_GALLERY) gallery_frame();
    else if (s_screen == SCREEN_USB) usb_screen_frame();
    else if (s_screen == SCREEN_WIFI) wifi_screen_frame();
    else viewfinder_frame(); /* also drives SCREEN_MENU, so the feed keeps live behind it */
    usb_webcam_feed_screen(); /* after the draw above, whichever screen it was - see its own comment */
    usb_webcam_feed_gb();     /* only one of these two actually sends anything - see s_webcam_mirror */

    settings_tick();
}
