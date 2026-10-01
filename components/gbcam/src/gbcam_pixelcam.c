/*
 * PIXEL CAM style: the GB mode of the PIXEL CAM project, ported to C from
 * its Python implementation (dither_gbcam(), _apply_levels(),
 * search_best_levels()).
 *
 *   gray  -> contrast pivot around mid-grey, clip, gamma      (_apply_levels)
 *         -> 128 + gray / 2 into the ROM thresholds' domain
 *         -> same 4x4x3 ROM threshold matrix as the hardware style
 *   levels (contrast, gamma) are picked by dithering a small preview at a
 *   7x5 grid of candidates and keeping the one whose shade histogram has the
 *   highest Shannon entropy (search_best_levels). In the viewfinder the
 *   result is smoothed from frame to frame so it doesn't flicker.
 *
 * Brightness (0..16, 8 = neutral) biases the chosen gamma by up to 1 EV-ish
 * either way; contrast picks the ROM pattern, as in the hardware style.
 */
#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "gbcam.h"

static const float search_contrast[] = {0.8f, 1.0f, 1.15f, 1.3f, 1.5f, 1.7f, 2.0f};
static const float search_gamma[] = {0.7f, 0.85f, 1.0f, 1.15f, 1.3f};
#define N_CONTRAST (sizeof search_contrast / sizeof search_contrast[0])
#define N_GAMMA (sizeof search_gamma / sizeof search_gamma[0])

#define SMOOTHING 0.25f  /* per-frame blend toward the new auto levels */
#define LOW_LIGHT_MEAN 96

/* luma -> floor(128 + levels(luma) / 2). Comparing that floor against the
 * integer thresholds gives exactly dither_gbcam()'s float `gray >= t`. */
static void build_lut(uint8_t lut[256], double contrast, double gamma)
{
    for (int i = 0; i < 256; i++) {
        double v = i / 255.0;
        v = (v - 0.5) * contrast + 0.5;
        v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        v = pow(v, gamma) * 255.0;
        lut[i] = (uint8_t)floor(128.0 + v * 0.5);
    }
}

static inline uint8_t shade_of(uint8_t code, const uint8_t *t)
{
    return code < t[0] ? 3 : code < t[1] ? 2 : code < t[2] ? 1 : 0;
}

static float entropy_of(const unsigned counts[4])
{
    unsigned total = counts[0] + counts[1] + counts[2] + counts[3];
    float h = 0.0f;
    for (int i = 0; i < 4; i++) {
        if (!counts[i]) continue;
        float p = (float)counts[i] / (float)total;
        h -= p * log2f(p);
    }
    return h;
}

/* A new candidate must beat the one being followed by this much entropy (bits)
 * before auto-levels switches to it; stops two near-equal candidates taking
 * turns and making the whole image pulse. */
#define SWITCH_MARGIN 0.03f
/* Once the smoothed levels are this close to the target, snap onto it so the
 * curve (and every pixel) stops moving. */
#define SNAP 0.01f

/* The 35 candidate curves never change: build them once. build_lut() uses
 * double-precision pow() (to match the Python exactly), which the ESP32-P4 can
 * only do in software, so rebuilding them every frame would cost tens of ms. */
static uint8_t (*candidate_luts)[256]; /* ~9KB - PSRAM instead of internal static, same reasoning as elsewhere */
static bool candidate_luts_built;

static void build_candidate_luts(void)
{
    if (!candidate_luts) candidate_luts = heap_caps_malloc(N_CONTRAST * N_GAMMA * 256, MALLOC_CAP_SPIRAM);
    if (!candidate_luts) return;
    for (size_t ci = 0; ci < N_CONTRAST; ci++)
        for (size_t gi = 0; gi < N_GAMMA; gi++)
            build_lut(candidate_luts[ci * N_GAMMA + gi], search_contrast[ci], search_gamma[gi]);
    candidate_luts_built = true;
}

/* Scores every candidate (index = ci * N_GAMMA + gi) and returns the best index.
 * Preview: every other pixel (64x56); the dither tile keeps its 4x4 period. */
static int search_levels(const gbcam_t *cam, float scores[N_CONTRAST * N_GAMMA])
{
    if (!candidate_luts_built) build_candidate_luts();
    float best_score = -1.0f;
    int best = 0;
    if (!candidate_luts) {
        memset(scores, 0, N_CONTRAST * N_GAMMA * sizeof scores[0]);
        return best;
    }
    for (size_t ci = 0; ci < N_CONTRAST; ci++) {
        for (size_t gi = 0; gi < N_GAMMA; gi++) {
            const uint8_t *lut = candidate_luts[ci * N_GAMMA + gi];
            unsigned counts[4] = {0, 0, 0, 0};
            for (int y = 0; y < GBCAM_H / 2; y++) {
                const uint8_t *row = cam->luma + (y * 2) * GBCAM_W;
                for (int x = 0; x < GBCAM_W / 2; x++) {
                    const uint8_t *t = cam->matrix + (((y & 3) << 2) | (x & 3)) * 3;
                    counts[shade_of(lut[row[x * 2]], t)]++;
                }
            }
            int idx = (int)(ci * N_GAMMA + gi);
            scores[idx] = entropy_of(counts);
            if (scores[idx] > best_score) {
                best_score = scores[idx];
                best = idx;
            }
        }
    }
    return best;
}

void gbcam_process_pixelcam(gbcam_t *cam)
{
    const gbcam_settings_t *s = &cam->settings;

    /* Light table: forced, or low-light when the frame is dark (dither_gbcam "auto"). */
    if (s->light_table == GBCAM_TABLE_AUTO) {
        unsigned long sum = 0;
        for (int i = 0; i < GBCAM_PIXELS; i++) sum += cam->luma[i];
        cam->high_light = (sum / GBCAM_PIXELS) >= LOW_LIGHT_MEAN;
    } else {
        cam->high_light = s->light_table == GBCAM_TABLE_HIGH_LIGHT;
    }
    cam->tier = cam->high_light ? 2 : 4;
    gbcam_update_matrix(cam);

    float c, g;
    if (s->auto_levels) {
        float scores[N_CONTRAST * N_GAMMA];
        int best = search_levels(cam, scores);
        if (cam->lv_target < 0 || scores[best] > scores[cam->lv_target] + SWITCH_MARGIN)
            cam->lv_target = (int8_t)best;
        float tc = search_contrast[cam->lv_target / N_GAMMA];
        float tg = search_gamma[cam->lv_target % N_GAMMA];
        if (cam->lv_valid) {
            c = cam->lv_contrast + (tc - cam->lv_contrast) * SMOOTHING;
            g = cam->lv_gamma + (tg - cam->lv_gamma) * SMOOTHING;
            if (fabsf(c - tc) < SNAP && fabsf(g - tg) < SNAP) {
                c = tc;
                g = tg;
            }
        } else {
            c = tc;
            g = tg;
        }
    } else {
        c = s->levels_contrast > 0.0f ? s->levels_contrast : 1.0f;
        g = s->levels_gamma > 0.0f ? s->levels_gamma : 1.0f;
    }
    cam->lv_contrast = c;
    cam->lv_gamma = g;
    cam->lv_valid = true;

    /* Brightness bias: 8 = as chosen; each step scales gamma by 2^(1/8). */
    int b = s->brightness > 16 ? 16 : s->brightness;
    float gamma = g * powf(2.0f, (8 - b) / 8.0f);
    if (gamma < 0.25f) gamma = 0.25f;
    if (gamma > 4.0f) gamma = 4.0f;

    /* Final curve: rebuilt only when contrast/gamma actually change (once
     * auto-levels has converged and snapped, that's never). Cached per-cam
     * (not a shared static) - Trichrome runs three gbcam_t instances back to
     * back, each converging to its own (contrast, gamma), which thrashed a
     * single shared cache every frame indefinitely. */
    if (c != cam->lv_lut_c || gamma != cam->lv_lut_g) {
        build_lut(cam->lv_lut, c, gamma);
        cam->lv_lut_c = c;
        cam->lv_lut_g = gamma;
    }
    const uint8_t *lut = cam->lv_lut;
    const uint8_t *m = cam->matrix;
    for (int y = 0; y < GBCAM_H; y++) {
        const uint8_t *row = cam->luma + y * GBCAM_W;
        uint8_t *out = cam->shades + y * GBCAM_W;
        for (int x = 0; x < GBCAM_W; x++)
            out[x] = shade_of(lut[row[x]], m + (((y & 3) << 2) | (x & 3)) * 3);
    }
}
