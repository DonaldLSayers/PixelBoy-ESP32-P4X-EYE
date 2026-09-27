/*
 * SD card /FRAMES and /ROMS folders: user-supplied GB Camera borders. Boot
 * only scans for metadata (name, size, and the /FRAMES/<x>.png path each one
 * is backed by) - same reasoning as app_palettes_sd.c's LUT caching: with a
 * couple hundred frames possible across several ROMs/packs, keeping every
 * one's decoded pixels resident in RAM permanently adds up for something
 * only ever one of is on screen at a time. The actual pixel data is decoded
 * from that path on demand, the first time a given frame is selected
 * (frames_sd_get()), and only the single most-recently-selected frame's
 * decode is kept around - switching frames re-decodes (a plain PNG, cheap),
 * it doesn't touch the SD card or re-decode on every viewfinder frame after
 * that. Source formats:
 *
 *   /FRAMES - .png files the same shape as assets/frames (160x144 or
 *             160x224), quantized the same way tools/gen_frames.py does on
 *             the PC; .json files in the original cartridge ROM's own
 *             frame-pack format (zlib + a small tile-hex JSON schema, one
 *             file can hold many frames), decoded the same way tools/
 *             import_gb_frames.py does on the PC, just ported to C.
 *   /ROMS   - .gb, .gbc and .zip files: frames extracted directly from a
 *             real GB Camera cartridge ROM (18 Standard + 8 Wild slots, or
 *             the Hello Kitty release's own 25 + 6) - decoded the same way
 *             tools/extract_gb_frames.py does on the PC, again ported to
 *             C; a .zip's .gb/.gbc members are extracted in memory first
 *             (stored or deflate, the only two methods the format allows).
 *
 * zlib/deflate inflate reuses stb_image's own (already linked for PNG
 * decode - see stbi_zlib_decode_malloc()/stbi_zlib_decode_noheader_malloc());
 * JSON and ZIP parsing are small hand-rolled scanners for exactly these
 * schemas, not general libraries (nothing else here needs one).
 *
 * A file that doesn't decode, or whose canvas isn't one gen_frames.py
 * supports, is skipped with a log warning - one bad file shouldn't block
 * every other frame on the card.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image.h"
#include "stb_image_write.h"

#include "app_frames_sd.h"
#include "app_storage.h"
#include "platform.h"

static const char *TAG = "frames_sd";

#define CANVAS_W 160
#define CANVAS_W_TILES 20
#define PHOTO_W_TILES 16
#define PHOTO_H_TILES 14
#define PHOTO_X_PX 16

static frame_meta_t s_meta[MAX_SD_FRAMES]; /* .indices is NULL until frames_sd_get() loads it on demand */
static char s_names[MAX_SD_FRAMES][32]; /* fits the longest FRAME_NAME_OVERRIDES entry (22 chars) with room to spare */
/* Just the filename, not the full path - every SD frame lives directly in
 * /FRAMES (see add_frame_ex()), so the directory half is the same for all of
 * them and not worth repeating 200 times over; frames_sd_get() rebuilds the
 * full path from s_root when it actually needs it. */
static char s_filenames[MAX_SD_FRAMES][48];
static int s_count;
static char s_root[224];               /* SD mount root, set at the top of frames_sd_init() */
static frames_sd_progress_cb s_progress;

/* The one frame currently decoded into RAM (whichever was last asked for by
 * frames_sd_get()) - freed/replaced, never more than one at a time. */
static int s_cached_index = -1;
static uint8_t *s_cached_indices;

/* SD filenames often carry underscores in place of spaces (common ROM-set
 * naming) and "(J)"/"(V1.1)"-style region/version tags - fine as a
 * filename, just noise in the boot toast, so for display only: swap
 * underscores for spaces, drop anything in parens, and collapse the
 * leftover runs of spaces. Nothing that builds paths or file_stem sees
 * this copy. */
static void report(const char *l1, const char *l2)
{
    if (!s_progress) return;
    if (!l2) { s_progress(l1, l2); return; }
    char pretty[40];
    size_t n = 0;
    int depth = 0;
    for (const char *p = l2; *p && n < sizeof pretty - 1; p++) {
        if (*p == '(') { depth++; continue; }
        if (*p == ')') { if (depth > 0) depth--; continue; }
        if (depth > 0) continue;
        char ch = *p == '_' ? ' ' : *p;
        if (ch == ' ' && (n == 0 || pretty[n - 1] == ' ')) continue;
        pretty[n++] = ch;
    }
    while (n > 0 && pretty[n - 1] == ' ') n--;
    pretty[n] = 0;
    s_progress(l1, pretty);
}

/* hardware shade index (0=lightest..3=darkest) -> grayscale byte that
 * round-trips losslessly back through load_png()'s round(gray/64)
 * quantizer - same mapping tools/import_gb_frames.py uses, so a frame
 * cached here looks identical whether it's reloaded from this .png or (as
 * it was this boot) decoded straight from its ROM/pack. */
static const uint8_t LEVEL_GRAY[4] = {192, 128, 64, 0};

/* Persists a newly-decoded ROM/pack frame to /FRAMES/<file_stem>.png so
 * future boots load it as a plain PNG instead of re-decoding its source -
 * see app_frames_sd.h. file_stem is the short display name itself (e.g.
 * "GAMEBOY1", "Tiger") - the filename IS the name a later boot reads back
 * (see load_png()), no separate sidecar to keep in sync. Trade-off: two
 * different sources that happen to produce the same short name (two
 * different ROMs' "GAMEBOY1", say) collide on the same cache file - judged
 * less important than every cached frame just reading right on the SD card
 * without an extra file next to it. Best-effort: a write failure (e.g. no
 * SD, card full) just means it gets re-decoded again next boot, not a hard
 * error.
 *
 * out_path/out_path_sz: filled with the .png path written (even on a write
 * failure - the caller still needs a path to remember, and everything after
 * a failed write just fails again the same way if that path's later read
 * back, no worse off than before this was cached at all). */
static void write_frame_png(const char *file_stem, int w, int h, const uint8_t *indices,
                            char *out_path, size_t out_path_sz)
{
    char safe[48];
    size_t n = 0;
    for (const char *c = file_stem; *c && n < sizeof safe - 1; c++) {
        char ch = *c;
        safe[n++] = (isalnum((unsigned char)ch) || ch == '-') ? ch : '_';
    }
    safe[n] = 0;
    snprintf(out_path, out_path_sz, "%s/FRAMES/%s.png", s_root, safe);
    if (!s_root[0]) return;

    uint8_t *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return;
    for (int i = 0; i < w * h; i++) {
        uint8_t g = LEVEL_GRAY[indices[i] & 3];
        rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = g;
    }
    if (!stbi_write_png(out_path, w, h, 3, rgb, w * 3))
        PLOGW(TAG, "couldn't write %s", out_path);
    free(rgb);
}

/* Same flatten-onto-white + PIL luma weights + round(gray/64) quantizer as
 * tools/gen_frames.py's quantize(), so a frame looks the same whether it was
 * baked in at build time or dropped on/decoded onto the SD card. Shared by
 * the initial scan (PNG sources decode fully up front, to dedupe by pixel
 * content) and frames_sd_get()'s on-demand reload of whichever frame is
 * currently selected. */
static uint8_t *decode_png_to_indices(const char *path, int *out_w, int *out_h, int *out_photo_y)
{
    int w, h, comp;
    uint8_t *rgba = stbi_load(path, &w, &h, &comp, 4);
    if (!rgba) {
        PLOGW(TAG, "%s: not a readable PNG", path);
        return NULL;
    }
    if (w != CANVAS_W || (h != 144 && h != 224)) {
        PLOGW(TAG, "%s: %dx%d, expected 160x144 or 160x224", path, w, h);
        stbi_image_free(rgba);
        return NULL;
    }
    uint8_t *indices = malloc((size_t)w * h);
    if (!indices) {
        stbi_image_free(rgba);
        return NULL;
    }
    for (int i = 0; i < w * h; i++) {
        uint8_t r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
        float fr = (r * a + 255.0f * (255 - a)) / 255.0f;
        float fg = (g * a + 255.0f * (255 - a)) / 255.0f;
        float fb = (b * a + 255.0f * (255 - a)) / 255.0f;
        float gray = fr * 0.299f + fg * 0.587f + fb * 0.114f;
        int level = (int)(gray / 64.0f + 0.5f);
        level = level < 0 ? 0 : level > 3 ? 3 : level;
        indices[i] = (uint8_t)(3 - level);
    }
    stbi_image_free(rgba);
    *out_w = w;
    *out_h = h;
    *out_photo_y = (h == 144 ? 2 : 5) * 8;
    return indices;
}

int frames_sd_count(void) { return s_count; }
const char *frames_sd_name(int index) { return (index >= 0 && index < s_count) ? s_names[index] : "?"; }

/* Decodes whichever frame is asked for from its /FRAMES/<x>.png (see the
 * file comment) unless it's already the one cached from the last call -
 * switching frames re-decodes a plain PNG (cheap), it doesn't re-decode on
 * every call, so this is fine to call once per viewfinder frame the way
 * app.c already does. */
const frame_meta_t *frames_sd_get(int index)
{
    static const frame_meta_t empty = {CANVAS_W, 144, 16, NULL};
    if (index < 0 || index >= s_count) return &empty;
    if (index != s_cached_index) {
        free(s_cached_indices);
        char path[300];
        snprintf(path, sizeof path, "%s/FRAMES/%s", s_root, s_filenames[index]);
        int w, h, photo_y;
        s_cached_indices = decode_png_to_indices(path, &w, &h, &photo_y);
        s_cached_index = s_cached_indices ? index : -1;
    }
    s_meta[index].indices = s_cached_indices;
    return &s_meta[index];
}

/* Different ROMs/packs (regions, revisions, a .zip vs. a loose .gb of the
 * same release) routinely share identical frames - dedupe by actual pixel
 * content (against frames.h's built-ins too, in case a dropped-in PNG
 * happens to match one) so pointing this at a whole folder of ROMs doesn't
 * fill the picker with repeats. */
static bool same_canvas(int aw, int ah, const uint8_t *a, int bw, int bh, const uint8_t *b)
{
    return aw == bw && ah == bh && a && b && memcmp(a, b, (size_t)bw * bh) == 0;
}

static const char *find_duplicate(int w, int h, const uint8_t *indices)
{
    for (int i = 0; i < FRAME_COUNT; i++)
        if (same_canvas(frame_meta[i].w, frame_meta[i].h, frame_meta[i].indices, w, h, indices))
            return frame_names[i];
    for (int i = 0; i < s_count; i++)
        if (same_canvas(s_meta[i].w, s_meta[i].h, s_meta[i].indices, w, h, indices))
            return s_names[i];
    return NULL;
}

/* name is the short (<=12 char) name shown in the menu/dial - put whatever
 * actually distinguishes this frame from its siblings first (a slot number,
 * a caption), since it's what survives truncation; a source ROM's own name
 * is usually too long to fit at all, let alone leave room for that.
 * file_stem (only used when persist=true) is the /FRAMES/<file_stem>.png
 * filename, which has much more room - the natural place to keep the source
 * name for anyone browsing the SD card later. `indices` is only needed
 * transiently here (for the dedupe check, and to persist=true sources'
 * write_frame_png()) - it's freed before returning either way, since the
 * resident copy going forward is whatever frames_sd_get() lazily decodes
 * from `path`/the written cache file, not this buffer. path is only used
 * when persist=false (a source that's already its own standalone .png, e.g.
 * one the user dropped onto the card - nothing to write). */
static bool add_frame_ex(const char *name, const char *path, const char *file_stem, int w, int h, int photo_y,
                         uint8_t *indices, bool persist)
{
    const char *dup = find_duplicate(w, h, indices);
    if (dup) {
        PLOGI(TAG, "\"%s\" is a duplicate of \"%s\", skipping", name, dup);
        free(indices);
        return false;
    }
    if (s_count >= MAX_SD_FRAMES) {
        PLOGW(TAG, "cap of %d reached, skipping \"%s\"", MAX_SD_FRAMES, name);
        free(indices);
        return false;
    }
    char written[300];
    if (persist) {
        write_frame_png(file_stem, w, h, indices, written, sizeof written);
        path = written;
    }
    s_meta[s_count] = (frame_meta_t){w, h, photo_y, NULL}; /* loaded on demand - see frames_sd_get() */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(s_filenames[s_count], sizeof s_filenames[s_count], "%s", base);
    free(indices);
    /* Uppercase, and '-'/'_' (common word separators in a filename) read as
     * spaces - "game_boy_camera" shows as "GAME BOY CAMERA" (then truncated
     * to fit, same as any other menu value), not "GAME_BOY_CA". Only ever
     * affects PNG/JSON sources, whose display name comes from a filename or
     * pack field - ROM sources' names here are always "STD"/"WILD" + a
     * number, nothing to replace. */
    snprintf(s_names[s_count], sizeof s_names[s_count], "%.*s", (int)(sizeof s_names[s_count] - 1), name);
    for (char *c = s_names[s_count]; *c; c++)
        *c = (char)((*c == '-' || *c == '_') ? ' ' : toupper((unsigned char)*c));
    s_count++;
    return true;
}

static bool add_frame_from_file(const char *name, const char *path, int w, int h, int photo_y, uint8_t *indices)
{
    return add_frame_ex(name, path, NULL, w, h, photo_y, indices, false);
}

static bool add_frame_persist(const char *name, const char *file_stem, int w, int h, int photo_y, uint8_t *indices)
{
    return add_frame_ex(name, NULL, file_stem, w, h, photo_y, indices, true);
}

/* ---------------------------------------------------------------- PNG in */

static void load_png(const char *path, const char *stem)
{
    int w, h, photo_y;
    uint8_t *indices = decode_png_to_indices(path, &w, &h, &photo_y);
    if (!indices) return;
    /* The filename itself is the display name (write_frame_png() names the
     * cache file that way already) - no separate sidecar to check. */
    add_frame_from_file(stem, path, w, h, photo_y, indices);
}

/* --------------------------------------------------------- JSON pack in */
/* Minimal hand-rolled scanner for exactly framegroup_*.json's shape - see
 * tools/import_gb_frames.py's module docstring for the format background
 * and tools/host FramePackParser.kt (the original Kotlin port) it mirrors. */

static const char *json_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *json_skip_string(const char *p) /* p at the opening quote */
{
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p += 2; else p++;
    }
    return *p == '"' ? p + 1 : p;
}

/* Value start (after any leading whitespace) to just past it - handles
 * strings, objects/arrays (brace/bracket depth, string-aware), and bare
 * literals (numbers/true/false/null). */
static const char *json_skip_value(const char *p)
{
    p = json_skip_ws(p);
    if (*p == '"') return json_skip_string(p);
    if (*p == '{' || *p == '[') {
        char open = *p, close = open == '{' ? '}' : ']';
        int depth = 1;
        p++;
        while (*p && depth) {
            if (*p == '"') { p = json_skip_string(p); continue; }
            if (*p == open) depth++;
            else if (*p == close) depth--;
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) p++;
    return p;
}

/* Finds "key" as an object member anywhere in [start,end) - not a full
 * parse, just a bounded scan; the caller bounds the span to one object so
 * sibling objects' same-named fields can't be picked up by mistake. */
static const char *json_find_key(const char *start, const char *end, const char *key)
{
    size_t klen = strlen(key);
    for (const char *p = start; p + klen + 2 < end; p++) {
        if (p[0] == '"' && memcmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            const char *q = json_skip_ws(p + 2 + klen);
            if (*q == ':') return json_skip_ws(q + 1);
        }
    }
    return NULL;
}

/* Parses a JSON string literal at *pp (which must point at the opening
 * quote), unescaping \n \t \r \b \f \\ \" \/ and \uXXXX (low byte only -
 * every character in these files is <256, encoded that way on purpose, see
 * import_gb_frames.py). Advances *pp past the closing quote. Returns the
 * decoded length, or -1 if *pp isn't a string / outcap is too small. */
static int json_parse_string(const char **pp, char *out, int outcap)
{
    const char *p = json_skip_ws(*pp);
    if (*p != '"') return -1;
    p++;
    int n = 0;
    while (*p && *p != '"') {
        unsigned char c;
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': c = '\n'; p++; break;
            case 't': c = '\t'; p++; break;
            case 'r': c = '\r'; p++; break;
            case 'b': c = '\b'; p++; break;
            case 'f': c = '\f'; p++; break;
            case 'u': {
                p++;
                int v = 0;
                for (int i = 0; i < 4 && *p; i++, p++) {
                    char h = *p;
                    int d = (h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                           : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : 0;
                    v = v * 16 + d;
                }
                c = (unsigned char)(v & 0xFF);
                break;
            }
            default: c = (unsigned char)*p; p++; break;
            }
        } else {
            /* Every payload byte is <256 by construction (see the docstring
             * above), but the JSON file itself is UTF-8 text, so any code
             * point >=0x80 is stored on disk as a 2-4 byte UTF-8 sequence,
             * not a raw byte - decode it back to that one code point rather
             * than copying its encoded bytes verbatim. */
            unsigned char b0 = (unsigned char)*p;
            int cp, len;
            if (b0 < 0x80) { cp = b0; len = 1; }
            else if ((b0 & 0xE0) == 0xC0 && p[1]) { cp = ((b0 & 0x1F) << 6) | (p[1] & 0x3F); len = 2; }
            else if ((b0 & 0xF0) == 0xE0 && p[1] && p[2]) {
                cp = ((b0 & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
                len = 3;
            } else if ((b0 & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
                cp = ((b0 & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
                len = 4;
            } else { cp = b0; len = 1; } /* malformed UTF-8 - fall back to the raw byte */
            c = (unsigned char)(cp & 0xFF);
            p += len;
        }
        if (n < outcap - 1) out[n++] = (char)c;
    }
    if (*p != '"') return -1;
    if (out) out[n < outcap ? n : outcap - 1] = 0;
    *pp = p + 1;
    return n;
}

static void decode_tile(const char *hex, uint8_t out[8][8])
{
    uint8_t b[16] = {0};
    int bi = 0;
    for (const char *c = hex; *c && bi < 16;) {
        while (*c == ' ') c++;
        if (!c[0] || !c[1]) break;
        int hi = isdigit((unsigned char)c[0]) ? c[0] - '0' : (tolower((unsigned char)c[0]) - 'a' + 10);
        int lo = isdigit((unsigned char)c[1]) ? c[1] - '0' : (tolower((unsigned char)c[1]) - 'a' + 10);
        b[bi++] = (uint8_t)((hi << 4) | lo);
        c += 2;
    }
    for (int r = 0; r < 8; r++) {
        uint8_t loB = b[r * 2], hiB = b[r * 2 + 1];
        for (int c = 0; c < 8; c++) {
            int bit = 7 - c;
            out[r][c] = (uint8_t)((((hiB >> bit) & 1) << 1) | ((loB >> bit) & 1));
        }
    }
}

static void paste_tile(uint8_t *canvas, int canvas_w, uint8_t tile[8][8], int tx, int ty)
{
    for (int r = 0; r < 8; r++)
        memcpy(canvas + (size_t)(ty * 8 + r) * canvas_w + (size_t)tx * 8, tile[r], 8);
}

/* Counts a flat array's string elements (upper/lower) without decoding
 * them - needed first to know upper_rows/lower_rows, hence canvas size,
 * before there's a canvas to paste into. */
static int json_count_array(const char *arr)
{
    const char *p = arr + 1;
    int n = 0;
    for (;;) {
        p = json_skip_ws(p);
        if (*p == ']' || !*p) break;
        p = json_skip_value(p);
        n++;
        p = json_skip_ws(p);
        if (*p == ',') p++;
    }
    return n;
}

static void decode_flat_section(const char *arr, uint8_t *canvas, int y0_tiles, int row_stride_tiles)
{
    const char *p = arr + 1;
    char hex[80];
    int i = 0;
    for (;;) {
        p = json_skip_ws(p);
        if (*p == ']' || !*p) break;
        const char *sp = p;
        if (json_parse_string(&sp, hex, sizeof hex) >= 0) {
            uint8_t tile[8][8];
            decode_tile(hex, tile);
            paste_tile(canvas, CANVAS_W, tile, i % CANVAS_W_TILES, y0_tiles + i / row_stride_tiles);
            i++;
            p = sp;
        } else {
            p = json_skip_value(p);
        }
        p = json_skip_ws(p);
        if (*p == ',') p++;
    }
}

static void decode_margin_section(const char *arr, uint8_t *canvas, int x0_tiles, int y0_tiles)
{
    const char *p = arr + 1;
    char hex[80];
    int row = 0;
    for (;;) {
        p = json_skip_ws(p);
        if (*p == ']' || !*p) break;
        if (*p == '[') {
            const char *pp = p + 1;
            int col = 0;
            for (;;) {
                pp = json_skip_ws(pp);
                if (*pp == ']' || !*pp) break;
                const char *sp = pp;
                if (json_parse_string(&sp, hex, sizeof hex) >= 0) {
                    uint8_t tile[8][8];
                    decode_tile(hex, tile);
                    paste_tile(canvas, CANVAS_W, tile, x0_tiles + col, y0_tiles + row);
                    col++;
                    pp = sp;
                } else {
                    pp = json_skip_value(pp);
                }
                pp = json_skip_ws(pp);
                if (*pp == ',') pp++;
            }
            row++;
            p = json_skip_value(p);
        } else {
            p = json_skip_value(p);
        }
        p = json_skip_ws(p);
        if (*p == ',') p++;
    }
}

static bool decode_sections(const char *buf, const char *end, const char *name, const char *file_stem)
{
    const char *upper = json_find_key(buf, end, "upper");
    const char *lower = json_find_key(buf, end, "lower");
    const char *left = json_find_key(buf, end, "left");
    const char *right = json_find_key(buf, end, "right");
    if (!upper || !lower || !left || !right || *upper != '[' || *lower != '[' || *left != '[' || *right != '[') {
        PLOGW(TAG, "frame \"%s\": missing upper/lower/left/right", name);
        return false;
    }

    int upper_rows = json_count_array(upper) / CANVAS_W_TILES;
    int lower_rows = json_count_array(lower) / CANVAS_W_TILES;
    int canvas_h = (upper_rows + PHOTO_H_TILES + lower_rows) * 8;
    /* gen_frames.py assumes a fixed photo_y per canvas height - reject
     * anything else rather than silently misplacing the border art (see
     * tools/import_gb_frames.py's identical check). */
    if ((canvas_h != 144 || upper_rows != 2) && (canvas_h != 224 || upper_rows != 5)) {
        PLOGW(TAG, "frame \"%s\": 160x%d canvas (upper_rows=%d), unsupported layout",
              name, canvas_h, upper_rows);
        return false;
    }

    uint8_t *canvas = calloc((size_t)CANVAS_W * (size_t)canvas_h, 1);
    if (!canvas) return false;

    decode_flat_section(upper, canvas, 0, CANVAS_W_TILES);
    decode_flat_section(lower, canvas, upper_rows + PHOTO_H_TILES, CANVAS_W_TILES);
    decode_margin_section(left, canvas, 0, upper_rows);
    decode_margin_section(right, canvas, (PHOTO_X_PX / 8) + PHOTO_W_TILES, upper_rows);

    return add_frame_persist(name, file_stem, CANVAS_W, canvas_h, upper_rows * 8, canvas);
}

static bool decode_one_frame(const char *p, const char *name, const char *file_stem)
{
    /* p is at the opening quote of the payload string. First pass just
     * measures the raw (still-escaped) span so the buffer can be sized
     * safely - decoding never makes a string longer. */
    const char *q = p + 1;
    while (*q && *q != '"') { if (*q == '\\' && q[1]) q += 2; else q++; }
    size_t span = (size_t)(q - (p + 1));
    char *payload = malloc(span + 1);
    if (!payload) return false;
    const char *pp = p;
    int plen = json_parse_string(&pp, payload, (int)span + 1);
    if (plen < 0) { free(payload); return false; }

    int outlen = 0;
    char *inflated = stbi_zlib_decode_malloc(payload, plen, &outlen);
    free(payload);
    if (!inflated) {
        PLOGW(TAG, "frame \"%s\": zlib inflate failed", name);
        return false;
    }

    bool ok = decode_sections(inflated, inflated + outlen, name, file_stem);
    free(inflated);
    return ok;
}

/* Friendly names for the original cartridge frame packs' own ids, ported
 * verbatim from PixelBoy's Android app (engine/frames/FrameNaming.kt's
 * NAME_OVERRIDES, itself ported from pixelboy/frames.py) - real names like
 * "GameBoy" or "Mario Kart 64" are fine here since they describe the user's
 * own imported content, unlike anything this project authors/ships itself.
 * Falls back to the pack's own id (see load_json()) for anything not
 * listed here. */
static const struct { const char *id, *name; } FRAME_NAME_OVERRIDES[] = {
    {"int01", "GameBoy"}, {"int02", "Dashes"}, {"int03", "Marbled"},
    {"int04", "Film Strip"}, {"int05", "Picture Frame"}, {"int06", "Squiggles"},
    {"int07", "Diamonds"}, {"int08", "X-Mas"}, {"int09", "Caution"},
    {"int10", "Bricks"}, {"int11", "Meandering Line"}, {"int12", "Television"},
    {"int13", "White"}, {"int14", "Black"}, {"int15", "Postage Stamp"},
    {"int16", "Kitty and flowers"}, {"int17", "Plaid"}, {"int18", "Pattern"},
    {"jp01", "Pocket Camera"}, {"jp02", "Round Pocket Camera"},
    {"jp07", "Nintendo Pocket Camera"},
    {"wi01", "Mario and Luigi"}, {"wi02", "Super Mario World"},
    {"wi03", "Game Boy Camera"}, {"wi04", "Yoshi"}, {"wi05", "Legend of Zelda"},
    {"wi06", "Wario"}, {"wi07", "Mario Kart 64"}, {"wi09", "Pokemon Trainer"},
    {"wi10", "Pocket Camera"}, {"wi12", "Blastoise"},
    {"wi13", "Pikachu and Clefairy"}, {"wi14", "Gakkyu-oh Yamazaki"},
    {"wi15", "Bakusou Kyoudai"}, {"wi16", "Hello Kitty Pattern"},
    {"wi17", "Hello Kitty Comic"}, {"wi18", "Hello Kitty Memo"},
    {"wi19", "Kitty's Family"}, {"wi20", "Sanrio Friends"},
    {"wi21", "Hello Kitty House"}, {"wi22", "GameBoy"},
};

static const char *frame_name_override(const char *id)
{
    for (size_t i = 0; i < sizeof FRAME_NAME_OVERRIDES / sizeof *FRAME_NAME_OVERRIDES; i++)
        if (strcmp(FRAME_NAME_OVERRIDES[i].id, id) == 0) return FRAME_NAME_OVERRIDES[i].name;
    return NULL;
}

static void load_json(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return; }
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return; }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = 0;
    const char *end = buf + got;

    const char *state = json_find_key(buf, end, "state");
    const char *state_end = state ? json_skip_value(state) : NULL;
    const char *frames = state ? json_find_key(state, state_end, "frames") : NULL;
    if (!frames || *frames != '[') {
        PLOGW(TAG, "%s: missing state.frames - not a recognized frame-pack file", path);
        free(buf);
        return;
    }

    int n = 0;
    const char *p = frames + 1;
    for (;;) {
        p = json_skip_ws(p);
        if (*p == ']' || !*p) break;
        const char *obj_start = p;
        const char *obj_end = json_skip_value(p);

        char id[16] = {0}, name[40] = {0}, hash[48] = {0};
        const char *v;
        if ((v = json_find_key(obj_start, obj_end, "id"))) json_parse_string(&v, id, sizeof id);
        if ((v = json_find_key(obj_start, obj_end, "name"))) json_parse_string(&v, name, sizeof name);
        if ((v = json_find_key(obj_start, obj_end, "hash"))) json_parse_string(&v, hash, sizeof hash);
        if (id[0] && hash[0]) {
            char key[64];
            snprintf(key, sizeof key, "frame-%s", hash);
            const char *fv = json_find_key(buf, end, key);
            /* A curated friendly name (see FRAME_NAME_OVERRIDES) if this id
             * has one; otherwise the pack's own "name" field - this newer
             * framegroup format's names are already short and meant to be
             * read ("Tiger", "Waves"), unlike the older commercial-cartridge
             * pack format this override table was originally written for,
             * where "name" could be a much longer descriptive string (e.g.
             * "International 01") and the short id ("wi01") was the better
             * bet to survive the menu's 12-char display width. Falling back
             * to the id only if this particular entry has no name at all. */
            const char *display = frame_name_override(id);
            if (!display) display = name[0] ? name : id;
            if (fv && *fv == '"' && decode_one_frame(fv, display, display)) n++;
        }

        p = json_skip_ws(obj_end);
        if (*p == ',') p++;
    }
    PLOGI(TAG, "%s: %d frame(s)", path, n);
    free(buf);
}

/* --------------------------------------------------------------- ROM in */
/* Extracts the built-in frames straight from a real cartridge ROM. Offsets
 * and the tile-position layout are ported from tools/extract_gb_frames.py
 * (see that script's docstring for where they come from and how they were
 * verified against real ROMs - this is the same algorithm, just in C). */

#define ROM_TITLE_OFFSET 0x134
#define ROM_TITLE_LENGTH 0xF
#define ROM_DEST_CODE_OFFSET 0x14A /* standard GB header field (Pan Docs) - 0x00 Japan, 0x01 overseas */
#define STANDARD_FRAME_OFFSET 0xD0000
#define STANDARD_FRAME_LENGTH 0x600      /* 96 tiles, 16 bytes each */
#define STANDARD_FRAME_MAP_LENGTH 0x88   /* 136 bytes, one tile index (0-95) per border position */
#define STANDARD_SLOT_LENGTH (STANDARD_FRAME_LENGTH + STANDARD_FRAME_MAP_LENGTH)
#define STANDARD_SLOTS 18
#define WILD_FRAME_OFFSET 0xC4000
#define WILD_FRAME_LENGTH 0x1800         /* enough for 384 tiles; only the first 336 are ever placed */
#define WILD_SLOTS 8
#define ROM_BANK_SHIFT 0x4000
#define MIN_ROM_SIZE 0x100000            /* every known GB Camera release is exactly 1MB */
#define TILE_LENGTH 16

static bool ext_is(const char *dot, const char *ext); /* defined further down, used by load_zip() */

/* Border tile positions (1-indexed, row-major in a 20-tile-wide grid), in
 * the order they're actually packed into the ROM: top/bottom positions in
 * ascending order, then side positions ALSO in ascending order (not grouped
 * by column - the ROM was written by a scan over ascending tile position,
 * which interleaves left/right pairs row by row; see tools/
 * extract_gb_frames.py's build_canvas() comment for how this was found). */
static const int16_t STD_POSITIONS[136] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
    27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 321, 322, 323, 324, 325, 326, 327, 328,
    329, 330, 331, 332, 333, 334, 335, 336, 337, 338, 339, 340, 341, 342, 343, 344, 345, 346, 347,
    348, 349, 350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 360, 41, 42, 59, 60, 61, 62, 79, 80,
    81, 82, 99, 100, 101, 102, 119, 120, 121, 122, 139, 140, 141, 142, 159, 160, 161, 162, 179, 180,
    181, 182, 199, 200, 201, 202, 219, 220, 221, 222, 239, 240, 241, 242, 259, 260, 261, 262, 279,
    280, 281, 282, 299, 300, 301, 302, 319, 320,
};
static const int16_t WILD_POSITIONS[336] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
    27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50,
    51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74,
    75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98,
    99, 100, 381, 382, 383, 384, 385, 386, 387, 388, 389, 390, 391, 392, 393, 394, 395, 396, 397,
    398, 399, 400, 401, 402, 403, 404, 405, 406, 407, 408, 409, 410, 411, 412, 413, 414, 415, 416,
    417, 418, 419, 420, 421, 422, 423, 424, 425, 426, 427, 428, 429, 430, 431, 432, 433, 434, 435,
    436, 437, 438, 439, 440, 441, 442, 443, 444, 445, 446, 447, 448, 449, 450, 451, 452, 453, 454,
    455, 456, 457, 458, 459, 460, 461, 462, 463, 464, 465, 466, 467, 468, 469, 470, 471, 472, 473,
    474, 475, 476, 477, 478, 479, 480, 481, 482, 483, 484, 485, 486, 487, 488, 489, 490, 491, 492,
    493, 494, 495, 496, 497, 498, 499, 500, 501, 502, 503, 504, 505, 506, 507, 508, 509, 510, 511,
    512, 513, 514, 515, 516, 517, 518, 519, 520, 521, 522, 523, 524, 525, 526, 527, 528, 529, 530,
    531, 532, 533, 534, 535, 536, 537, 538, 539, 540, 541, 542, 543, 544, 545, 546, 547, 548, 549,
    550, 551, 552, 553, 554, 555, 556, 557, 558, 559, 560, 101, 102, 119, 120, 121, 122, 139, 140,
    141, 142, 159, 160, 161, 162, 179, 180, 181, 182, 199, 200, 201, 202, 219, 220, 221, 222, 239,
    240, 241, 242, 259, 260, 261, 262, 279, 280, 281, 282, 299, 300, 301, 302, 319, 320, 321, 322,
    339, 340, 341, 342, 359, 360, 361, 362, 379, 380,
};

static const uint8_t HELLO_KITTY_TITLE[] = "POCKETCAMERA_SN"; /* ROM_TITLE_LENGTH (15) chars + a
                                                                  null the memcmp below never reads */

/* Each pair is {tile data offset, tile map offset} - the Hello Kitty
 * release doesn't lay its frames out in neat contiguous slots like every
 * other release, so these are simply the individual addresses found. */
static const uint32_t HK_STD_OFFSETS[25][2] = {
    {0xC6C70, 0xCF5D0}, {0xC3B80, 0xCF548}, {0xCBEC0, 0xCF4C0}, {0xC5F10, 0xCF658},
    {0xCF210, 0xCF7F0}, {0xC73A0, 0xCF768}, {0xB7420, 0xCF6E0}, {0xBE3E0, 0xCF438},
    {0xB3CD0, 0xC7EF0}, {0xB2B80, 0xCF3B0}, {0x8FD50, 0xC7F78}, {0xC3800, 0xD7800},
    {0xBDC00, 0xD3F70}, {0xD7F70, 0xD7888}, {0xC5C00, 0xD7998}, {0xB7C20, 0xD7910},
    {0xC3ED0, 0xD3D50}, {0x33F80, 0xD3CC8}, {0xDB800, 0xD3DD8}, {0xB2200, 0xD3EE8},
    {0xB34D0, 0xD3E60}, {0xB3030, 0xD7A20}, {0x93E00, 0xD7D50}, {0x77FE0, 0xCFCB8},
    {0x77FF0, 0xCFDC4},
};
static const uint32_t HK_WILD_OFFSETS[6] = {0x6C000, 0x60000, 0x64000, 0x65800, 0x69800, 0x68000};

static void decode_tile_bytes(const uint8_t *b16, uint8_t out[8][8])
{
    for (int r = 0; r < 8; r++) {
        uint8_t lo = b16[r * 2], hi = b16[r * 2 + 1];
        for (int c = 0; c < 8; c++) {
            int bit = 7 - c;
            out[r][c] = (uint8_t)((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
        }
    }
}

/* tile_offset/map_offset are byte offsets into `rom` (already resolved -
 * bank shifts etc. are the caller's job). map_offset < 0 means "no map, one
 * tile per position in `positions` order" (Wild frames); otherwise it's a
 * byte per position (0..n_unique-1) indexing the n_unique tiles stored
 * at tile_offset (Standard frames). canvas_h_tiles/photo_y_tiles must
 * match one of gen_frames.py's LAYOUTS (18/2 for Standard, 28/5 for Wild). */
static bool rom_build_and_add(const uint8_t *rom, size_t rom_len, const char *name,
                              const char *file_stem,
                              long tile_offset, long map_offset, int n_unique,
                              const int16_t *positions, int n_positions,
                              int canvas_h_tiles, int photo_y_tiles)
{
    long need = tile_offset + (long)n_unique * TILE_LENGTH;
    if (map_offset >= 0) need = need > map_offset + n_positions ? need : map_offset + n_positions;
    if (tile_offset < 0 || need > (long)rom_len) {
        PLOGW(TAG, "\"%s\": offset out of range for a %zu-byte file", name, rom_len);
        return false;
    }

    int w = CANVAS_W_TILES * 8, h = canvas_h_tiles * 8;
    uint8_t *canvas = calloc((size_t)w * (size_t)h, 1);
    if (!canvas) return false;

    if (map_offset >= 0) {
        for (int i = 0; i < n_positions; i++) {
            int idx = rom[map_offset + i];
            if (idx >= n_unique) idx = 0;
            uint8_t tile[8][8];
            decode_tile_bytes(rom + tile_offset + (size_t)idx * TILE_LENGTH, tile);
            int p = positions[i] - 1;
            paste_tile(canvas, w, tile, p % CANVAS_W_TILES, p / CANVAS_W_TILES);
        }
    } else {
        for (int i = 0; i < n_positions; i++) {
            uint8_t tile[8][8];
            decode_tile_bytes(rom + tile_offset + (size_t)i * TILE_LENGTH, tile);
            int p = positions[i] - 1;
            paste_tile(canvas, w, tile, p % CANVAS_W_TILES, p / CANVAS_W_TILES);
        }
    }

    return add_frame_persist(name, file_stem, w, h, photo_y_tiles * 8, canvas);
}

static void extract_rom_frames(const uint8_t *rom, size_t rom_len, const char *label)
{
    if (rom_len < MIN_ROM_SIZE) {
        PLOGW(TAG, "%s: %zu bytes, too small to be a GB Camera ROM (expected %d)",
              label, rom_len, MIN_ROM_SIZE);
        return;
    }
    bool is_hk = memcmp(rom + ROM_TITLE_OFFSET, HELLO_KITTY_TITLE, ROM_TITLE_LENGTH) == 0;
    /* Standard GB cartridge header field, not a guess - see Pan Docs. Doesn't
     * tell us anything about which named frame is which (no verified mapping
     * from ROM slot order to the community "int01".."int18" list exists, or
     * this would use it to pick real names, not just log/tag the region). */
    const char *region = rom[ROM_DEST_CODE_OFFSET] == 0x00 ? "POCKET" : "GAMEBOY"; /* Japan's "Pocket Camera" vs the international "Game Boy Camera" release */
    PLOGI(TAG, "%s: %s Camera cartridge (destination code 0x%02X)", label, region, rom[ROM_DEST_CODE_OFFSET]);
    /* Region prefix in the name itself - "POCKET1"/"GAMEBOY W3", not
     * "STD1"/"WILD3" - since a Pocket Camera (JP) and Game Boy Camera
     * (overseas) ROM's slots aren't guaranteed to hold the same frame at the
     * same slot number, this at least tells them apart at a glance instead
     * of implying they're identical. Longest case ("GAMEBOY W1", 10 chars)
     * is well under the menu's 12-char display width. */
    char name[24];
    int found = 0;

    /* file_stem == name (the display name itself doubles as the filename,
     * e.g. "GAMEBOY1.png"/"POCKET_W3.png" - write_frame_png() turns the
     * space before a Wild slot's "W3" into an underscore, FAT filenames
     * being safest without spaces) rather than a longer name carrying the
     * source ROM's own filename too. Simpler, at the cost of two different
     * ROMs of the same region (e.g. two distinct Game Boy Camera releases)
     * colliding on the same cache filename if both are ever loaded - an
     * edge case judged less important than every ROM's frames just reading
     * "GAMEBOY1" on the SD card the way they do in the menu. */
    if (is_hk) {
        for (int i = 0; i < 25; i++) {
            snprintf(name, sizeof name, "%s %d", region, i + 1);
            if (rom_build_and_add(rom, rom_len, name, name, HK_STD_OFFSETS[i][0], HK_STD_OFFSETS[i][1],
                                  96, STD_POSITIONS, 136, 18, 2))
                found++;
        }
        for (int i = 0; i < 6; i++) {
            snprintf(name, sizeof name, "%s W%d", region, i + 1);
            if (rom_build_and_add(rom, rom_len, name, name, HK_WILD_OFFSETS[i], -1,
                                  336, WILD_POSITIONS, 336, 28, 5))
                found++;
        }
    } else {
        for (int slot = 0; slot < STANDARD_SLOTS; slot++) {
            long bank = slot >= 9 ? ROM_BANK_SHIFT : 0;
            int idx = slot < 9 ? slot : slot - 9;
            long base = STANDARD_FRAME_OFFSET + bank + (long)STANDARD_SLOT_LENGTH * idx;
            snprintf(name, sizeof name, "%s %d", region, slot + 1);
            if (rom_build_and_add(rom, rom_len, name, name, base, base + STANDARD_FRAME_LENGTH,
                                  96, STD_POSITIONS, 136, 18, 2))
                found++;
        }
        for (int slot = 0; slot < WILD_SLOTS; slot++) { /* always slot<9, no bank shift needed */
            long base = WILD_FRAME_OFFSET + (long)WILD_FRAME_LENGTH * slot;
            snprintf(name, sizeof name, "%s W%d", region, slot + 1);
            if (rom_build_and_add(rom, rom_len, name, name, base, -1, 336, WILD_POSITIONS, 336, 28, 5))
                found++;
        }
    }
    PLOGI(TAG, "%s: %d frame(s)", label, found);
}

/* --------------------------------------------------- untoxa/gb-photo ROM */
/* An open-source GB Camera-alike (https://github.com/untoxa/gb-photo) -
 * a completely different ROM from the commercial cartridge above, built
 * with GBDK/SDCC, whose linker assigns bank/address per build - there's no
 * fixed offset to hardcode the way the commercial ROM's is. Instead this
 * locates its print_frames[] table (src/print_frames.c upstream) by its
 * own distinctive byte signature: a frame_desc_t entry (include/
 * print_frames.h upstream) with every pointer/bank field zeroed - the
 * always-first "No Frame" placeholder - so it works across any build.
 * Ported from tools/extract_gb_frames.py's extract_gbphoto_rom(), which has
 * the fuller story and was verified against a real ROM first. */
static const uint8_t GBPHOTO_ANCHOR[12] = {0x0E, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02};
#define GBPHOTO_MAP_WIDTH 20 /* PRN_TILE_WIDTH upstream */

static long gbphoto_resolve(int bank, int ptr)
{
    return ptr < 0x4000 ? -1 : (long)bank * 0x4000 + (ptr - 0x4000);
}

/* height/image_y/image_x are in tiles - only two combinations match a
 * canvas size and photo position this pipeline supports (gen_frames.py's
 * Standard/Wild); gb-photo itself allows arbitrary placement, so anything
 * else (its own "GB Camera" frame, notably) is skipped rather than forced. */
static bool gbphoto_layout_ok(int height, int image_y, int image_x)
{
    return (height == 18 && image_y == 2 && image_x == 2) ||
           (height == 28 && image_y == 5 && image_x == 2);
}

static bool try_gbphoto_rom(const uint8_t *rom, size_t rom_len, const char *label)
{
    long anchor = -1, bank = 0;
    for (long i = 0; i + 16 <= (long)rom_len; i++) {
        if (memcmp(rom + i, GBPHOTO_ANCHOR, sizeof GBPHOTO_ANCHOR) != 0) continue;
        bank = i / 0x4000;
        int cap_ptr = rom[i + 12] | (rom[i + 13] << 8);
        long cap_off = gbphoto_resolve((int)bank, cap_ptr);
        if (cap_off >= 0 && cap_off + 8 <= (long)rom_len && memcmp(rom + cap_off, "No Frame", 8) == 0) {
            anchor = i;
            break;
        }
    }
    if (anchor < 0) return false; /* not a gb-photo ROM (or none we recognize) */

    int num_banks = (int)(rom_len / 0x4000);
    int found = 0;

    for (int idx = 1;; idx++) { /* entry 0 is the anchor itself - no graphics to extract */
        long off = anchor + (long)idx * 16;
        if (off + 16 > (long)rom_len) break;
        const uint8_t *e = rom + off;
        int height = e[0];
        int map_ptr = e[1] | (e[2] << 8), map_bank = e[3];
        int tiles_ptr = e[4] | (e[5] << 8), tiles_bank = e[6];
        int image_y = e[10], image_x = e[11];
        int cap_ptr = e[12] | (e[13] << 8);

        if (!(map_bank > 0 && map_bank < num_banks && tiles_bank > 0 && tiles_bank < num_banks &&
              map_ptr >= 0x4000 && tiles_ptr >= 0x4000))
            break; /* past the end of the real table */

        long cap_off = gbphoto_resolve((int)bank, cap_ptr);
        char caption[24] = {0};
        size_t n = 0;
        while (cap_off >= 0 && n < sizeof caption - 1 && cap_off + (long)n < (long)rom_len && rom[cap_off + n]) {
            caption[n] = (char)rom[cap_off + n];
            n++;
        }
        if (!caption[0]) snprintf(caption, sizeof caption, "frame%d", idx);

        if (!gbphoto_layout_ok(height, image_y, image_x)) {
            PLOGI(TAG, "%s: \"%s\" is %dpx tall, photo at (%d,%d)px - unsupported layout, skipping",
                  label, caption, height * 8, image_x * 8, image_y * 8);
            continue;
        }

        long map_off = gbphoto_resolve(map_bank, map_ptr);
        long tiles_off = gbphoto_resolve(tiles_bank, tiles_ptr);
        int w = GBPHOTO_MAP_WIDTH * 8, h = height * 8;
        int map_len = GBPHOTO_MAP_WIDTH * height;
        if (map_off < 0 || map_off + map_len > (long)rom_len) continue;

        uint8_t *canvas = calloc((size_t)w * (size_t)h, 1);
        if (!canvas) continue;
        bool ok = true;
        for (int p = 0; p < map_len; p++) {
            int tile_idx = rom[map_off + p];
            long toff = tiles_off + (long)tile_idx * TILE_LENGTH;
            if (toff < 0 || toff + TILE_LENGTH > (long)rom_len) { ok = false; break; }
            uint8_t tile[8][8];
            decode_tile_bytes(rom + toff, tile);
            paste_tile(canvas, w, tile, p % GBPHOTO_MAP_WIDTH, p / GBPHOTO_MAP_WIDTH);
        }
        if (!ok) { free(canvas); continue; }

        if (add_frame_persist(caption, caption, w, h, image_y * 8, canvas)) found++;
    }
    PLOGI(TAG, "%s: gb-photo ROM, %d frame(s)", label, found);
    return true;
}

static void extract_any_rom(const uint8_t *rom, size_t rom_len, const char *label)
{
    if (try_gbphoto_rom(rom, rom_len, label)) return;
    extract_rom_frames(rom, rom_len, label);
}

/* Moves an already-processed ROM/zip out of /ROMS into /ROMS/PROCESSED, so
 * it's never re-decoded on a later boot (its frames are already cached as
 * plain .png files under /FRAMES by then - see write_frame_png()). Silently
 * does nothing if the move fails (e.g. read-only card) - worst case, that
 * one file just gets processed again next boot. */
static void move_to_processed(const char *path)
{
    if (!s_root[0]) return;
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;

    char dir[256];
    snprintf(dir, sizeof dir, "%s/ROMS/PROCESSED", s_root);
    plat_mkdir(dir);

    char dest[300];
    snprintf(dest, sizeof dest, "%s/%s", dir, base);
    rename(path, dest);
}

static void load_gb(const char *path, const char *label)
{
    report("CONVERTING", label);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return; }
    uint8_t *rom = malloc((size_t)size);
    if (!rom) { fclose(f); return; }
    size_t got = fread(rom, 1, (size_t)size, f);
    fclose(f);
    extract_any_rom(rom, got, label);
    free(rom);
    move_to_processed(path);
}

/* ------------------------------------------------------------- ZIP in */
/* Just enough of the ZIP format to find named entries in the central
 * directory and decompress them (method 0 "stored" or 8 "deflate" - the
 * only two anything actually producing these files would use), so a whole
 * ROM zip can be dropped onto the card as-is. Not a general unzip: no
 * zip64, no encryption, no multi-disk archives. */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void load_zip(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize < 22) { fclose(f); return; }

    /* End Of Central Directory record: fixed 22 bytes plus an optional
     * comment, so scan backward for its signature within a bounded tail
     * (64KB is the max a standard comment can be). */
    long tail = fsize < 65557 ? fsize : 65557;
    uint8_t *buf = malloc((size_t)tail);
    if (!buf) { fclose(f); return; }
    fseek(f, fsize - tail, SEEK_SET);
    if (fread(buf, 1, (size_t)tail, f) != (size_t)tail) { free(buf); fclose(f); return; }

    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--) {
        if (buf[i] == 0x50 && buf[i + 1] == 0x4B && buf[i + 2] == 0x05 && buf[i + 3] == 0x06) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) { PLOGW(TAG, "%s: not a zip file (no end-of-central-directory)", path); free(buf); fclose(f); return; }

    uint16_t entries = rd16(buf + eocd + 10);
    uint32_t cd_offset = rd32(buf + eocd + 16);
    free(buf);

    for (uint16_t e = 0; e < entries; e++) {
        uint8_t hdr[46];
        fseek(f, (long)cd_offset, SEEK_SET);
        if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || rd32(hdr) != 0x02014b50u) break;

        uint16_t method = rd16(hdr + 10);
        uint32_t comp_size = rd32(hdr + 20);
        uint32_t uncomp_size = rd32(hdr + 24);
        uint16_t name_len = rd16(hdr + 28);
        uint16_t extra_len = rd16(hdr + 30);
        uint16_t comment_len = rd16(hdr + 32);
        uint32_t local_offset = rd32(hdr + 42);

        char name[80];
        size_t nl = name_len < sizeof name - 1 ? name_len : sizeof name - 1;
        if (fread(name, 1, nl, f) != nl) break;
        name[nl] = 0;
        const char *dot = strrchr(name, '.');
        bool is_gb = dot && (ext_is(dot, ".gb") || ext_is(dot, ".gbc"));

        if (is_gb && uncomp_size > 0 && uncomp_size < 8 * 1024 * 1024) {
            report("CONVERTING", name);
            uint8_t lhdr[30];
            fseek(f, (long)local_offset, SEEK_SET);
            if (fread(lhdr, 1, sizeof lhdr, f) == sizeof lhdr && rd32(lhdr) == 0x04034b50u) {
                uint16_t lname_len = rd16(lhdr + 26), lextra_len = rd16(lhdr + 28);
                long data_off = (long)local_offset + 30 + lname_len + lextra_len;
                uint8_t *comp = malloc(comp_size ? comp_size : 1);
                if (comp) {
                    fseek(f, data_off, SEEK_SET);
                    if (fread(comp, 1, comp_size, f) == comp_size) {
                        if (method == 0 && comp_size == uncomp_size) {
                            extract_any_rom(comp, comp_size, name);
                        } else if (method == 8) {
                            int outlen = 0;
                            char *rom = stbi_zlib_decode_noheader_malloc((char *)comp, (int)comp_size, &outlen);
                            if (rom) {
                                extract_any_rom((const uint8_t *)rom, (size_t)outlen, name);
                                free(rom);
                            } else {
                                PLOGW(TAG, "%s: couldn't inflate %s", path, name);
                            }
                        } else {
                            PLOGW(TAG, "%s: %s uses unsupported zip compression method %u", path, name, method);
                        }
                    }
                    free(comp);
                }
            }
        }

        fseek(f, (long)cd_offset + 46 + name_len + extra_len + comment_len, SEEK_SET);
        cd_offset = (uint32_t)ftell(f);
    }
    fclose(f);
    move_to_processed(path);
}

/* -------------------------------------------------------------------- */

static bool ext_is(const char *dot, const char *ext) /* both start with '.', case-insensitive */
{
    size_t i = 0;
    for (; ext[i]; i++) {
        char a = dot[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != ext[i]) return false;
    }
    return dot[i] == 0;
}

static void scan_folder(const char *root, const char *name,
                        void (*on_png)(const char *path, const char *stem),
                        void (*on_json)(const char *path),
                        void (*on_gb)(const char *path, const char *stem),
                        void (*on_zip)(const char *path))
{
    char dir[256];
    snprintf(dir, sizeof dir, "%s/%s", root, name);
    DIR *d = opendir(dir);
    if (!d) return; /* folder doesn't exist - nothing to load, not an error */

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot) continue;
        char stem[48];
        size_t stemlen = (size_t)(dot - e->d_name);
        if (stemlen >= sizeof stem) stemlen = sizeof stem - 1;
        memcpy(stem, e->d_name, stemlen);
        stem[stemlen] = 0;

        char path[300];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);

        if (on_png && ext_is(dot, ".png")) on_png(path, stem);
        else if (on_json && ext_is(dot, ".json")) on_json(path);
        else if (on_gb && (ext_is(dot, ".gb") || ext_is(dot, ".gbc"))) on_gb(path, stem);
        else if (on_zip && ext_is(dot, ".zip")) on_zip(path);
    }
    closedir(d);
}

void frames_sd_init(frames_sd_progress_cb progress)
{
    s_progress = progress;
    const char *root = storage_root();
    if (!root[0]) return; /* no SD card */
    snprintf(s_root, sizeof s_root, "%s", root);

    char dir[256];
    snprintf(dir, sizeof dir, "%s/FRAMES", root);
    plat_mkdir(dir);
    snprintf(dir, sizeof dir, "%s/ROMS", root);
    plat_mkdir(dir);

    scan_folder(root, "FRAMES", load_png, load_json, NULL, NULL);
    int after_frames = s_count;
    scan_folder(root, "ROMS", NULL, NULL, load_gb, load_zip);
    int from_roms = s_count - after_frames;

    PLOGI(TAG, "%d frame(s) from /FRAMES, %d from /ROMS", after_frames, from_roms);
    if (from_roms > 0) {
        char line[24];
        snprintf(line, sizeof line, "%d NEW FRAME%s", from_roms, from_roms == 1 ? "" : "S");
        report("CONVERTED", line);
    }
    s_progress = NULL;
}
