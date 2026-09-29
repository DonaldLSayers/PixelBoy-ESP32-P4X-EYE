#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image.h"
#include "stb_image_write.h"

#include "app_frames.h"
#include "app_storage.h"
#include "platform.h"

static const char *TAG = "storage";

#define MAX_PHOTOS 4096
#define PNG_SCALE GB_PNG_SCALE /* public in app_storage.h - the gallery needs it too, to strip a frame back out */

typedef struct {
    int number;
    bool is_dc;
    char prefix[4]; /* "GB" (real camera) or "EMU" (pulled from a GB emulator .sav) - unused/blank for is_dc */
} photo_t;

static bool s_ready;
static char s_root[224];  /* SD card mount root, e.g. "/sdcard" */
static char s_dir[256];   /* <mount root>/GBCAM */
static char s_bin_dir[276]; /* <s_dir>/BIN - GB Camera's raw tiles, kept out of the way of the browsable photos */
static char s_thumb_dir[276]; /* <s_dir>/THUMB - small pre-shrunk copies for the gallery grid, see make_thumbnail() */
static char s_aeb_dir[276]; /* <s_dir>/AEB - AEB's non-center bracket exposures, see storage_save_aeb_extra() */
static photo_t *s_photos; /* sorted ascending by number (one shared sequence) */
static int s_count;
static int s_next_number = 1;

static int cmp_photo(const void *a, const void *b)
{
    return ((const photo_t *)a)->number - ((const photo_t *)b)->number;
}

static void path_for(char *out, size_t len, const char *prefix, int number, const char *ext)
{
    snprintf(out, len, "%s/%s%05d.%s", s_dir, prefix, number, ext);
}

/* GBnnnnn.BIN/EMUnnnnn.BIN specifically live in their own subfolder - it's
 * the raw palette-free tile data (re-render material, not something to look
 * at directly), unlike every other saved file (the .PNG/.JPG previews,
 * Dither/Normal Cam's own saves) which are all actual viewable photos side
 * by side in one folder. */
static void bin_path_for(char *out, size_t len, const char *prefix, int number)
{
    snprintf(out, len, "%s/%s%05d.BIN", s_bin_dir, prefix, number);
}

/* Looks up which prefix ("GB" or "EMU") a given non-DC photo number was
 * saved under - every read-side path below needs this instead of assuming
 * "GB", now that GB Camera's own captures and photos pulled out of an
 * emulator .sav (see app_gbemu.c) share one numbering sequence but use
 * different prefixes on disk. Falls back to "GB" if the number isn't in
 * s_photos yet (shouldn't happen for anything the gallery can reach). */
static const char *gb_prefix_for(int number)
{
    for (int i = 0; i < s_count; i++)
        if (s_photos[i].number == number && !s_photos[i].is_dc)
            return s_photos[i].prefix;
    return "GB";
}

/* Same idea for is_dc entries - "DC" (Dither Cam/Normal Cam) or "AEB" (an
 * AEB/HDR combined result, see app.c's gb_aeb_capture()), both continuous-
 * tone images saved the same way (storage_save_dc()) under one shared numbered
 * sequence. */
static const char *dc_prefix_for(int number)
{
    for (int i = 0; i < s_count; i++)
        if (s_photos[i].number == number && s_photos[i].is_dc)
            return s_photos[i].prefix;
    return "DC";
}

/* thumbnails are always .PNG regardless of the source's own format (Normal
 * Cam's full photo is .JPG) - lossy JPEG artifacts get a lot more visible
 * once you're down to thumbnail size, and PNG's simple/fast to write/read
 * for something this small anyway. */
static void thumb_path_for(char *out, size_t len, const char *prefix, int number)
{
    snprintf(out, len, "%s/%s%05d.PNG", s_thumb_dir, prefix, number);
}

/* Box-average downscale of src (sw x sh RGB888) into a freshly malloc'd
 * buffer, longer side capped at THUMB_MAX_DIM, aspect preserved. NULL on
 * allocation failure. Written once per photo at save time (or once per
 * already-saved photo the first time it's viewed - see
 * storage_load_thumb()), so simplicity matters more here than raw speed. */
#define THUMB_MAX_DIM 128
static uint8_t *make_thumbnail(const uint8_t *src, int sw, int sh, int *out_w, int *out_h)
{
    int tw, th;
    if (sw >= sh) {
        tw = sw < THUMB_MAX_DIM ? sw : THUMB_MAX_DIM;
        th = (int)((long)sh * tw / sw);
    } else {
        th = sh < THUMB_MAX_DIM ? sh : THUMB_MAX_DIM;
        tw = (int)((long)sw * th / sh);
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    uint8_t *dst = malloc((size_t)tw * th * 3);
    if (!dst) return NULL;
    for (int dy = 0; dy < th; dy++) {
        int ya = dy * sh / th, yb = (dy + 1) * sh / th;
        if (yb <= ya) yb = ya + 1;
        if (yb > sh) yb = sh;
        for (int dx = 0; dx < tw; dx++) {
            int xa = dx * sw / tw, xb = (dx + 1) * sw / tw;
            if (xb <= xa) xb = xa + 1;
            if (xb > sw) xb = sw;
            unsigned sum[3] = {0, 0, 0};
            for (int sy = ya; sy < yb; sy++) {
                const uint8_t *row = src + (size_t)sy * sw * 3;
                for (int sx = xa; sx < xb; sx++) {
                    sum[0] += row[sx * 3];
                    sum[1] += row[sx * 3 + 1];
                    sum[2] += row[sx * 3 + 2];
                }
            }
            unsigned n = (unsigned)((yb - ya) * (xb - xa));
            uint8_t *o = dst + ((size_t)dy * tw + dx) * 3;
            o[0] = (uint8_t)(sum[0] / n);
            o[1] = (uint8_t)(sum[1] / n);
            o[2] = (uint8_t)(sum[2] / n);
        }
    }
    *out_w = tw;
    *out_h = th;
    return dst;
}

/* Writes rgb888 (w x h) as a downscaled thumbnail PNG - best-effort, same
 * reasoning as write_frame_png() in app_frames_sd.c (a write failure just
 * means the gallery falls back to the slow path again next time, not a hard
 * error). */
static void write_thumbnail(const char *prefix, int number, const uint8_t *rgb888, int w, int h)
{
    int tw, th;
    uint8_t *thumb = make_thumbnail(rgb888, w, h, &tw, &th);
    if (!thumb) return;
    char path[300];
    thumb_path_for(path, sizeof path, prefix, number);
    if (!stbi_write_png(path, tw, th, 3, thumb, tw * 3))
        PLOGW(TAG, "thumbnail write failed: %s", path);
    free(thumb);
}

/* GB Camera's own captures are GBnnnnn, photos pulled out of a GB emulator
 * .sav (see app_gbemu.c's export_photos_from_sav()) are EMUnnnnn - both
 * live in the same BIN/ subfolder and share one numbering sequence, just
 * distinguished by this prefix. Matches name against prefix case-
 * insensitively, then parses the number/extension after it. */
static bool parse_numbered_file(const char *name, const char *prefix, int *out_n, char *ext)
{
    size_t plen = strlen(prefix);
    if (strncasecmp(name, prefix, plen) != 0) return false;
    return sscanf(name + plen, "%5d.%3s", out_n, ext) == 2;
}

/* One-time migration for a card that already has GBnnnnn.BIN files sitting
 * directly in s_dir from before this split existed - moves each into
 * s_bin_dir so scan() (which now only looks for them there) still finds
 * every existing photo instead of losing them. Best-effort: a rename
 * failure just leaves that one where it was, to be retried next boot.
 * Only ever "GB" - EMU-prefixed exports are new enough to never have
 * predated this split. */
static void migrate_bin_files(void)
{
    DIR *d = opendir(s_dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int n;
        char ext[4] = {0};
        if (!parse_numbered_file(e->d_name, "GB", &n, ext)) continue;
        if (!(ext[0] == 'B' || ext[0] == 'b')) continue;
        char old_path[300], new_path[300];
        snprintf(old_path, sizeof old_path, "%s/%s", s_dir, e->d_name);
        bin_path_for(new_path, sizeof new_path, "GB", n);
        if (rename(old_path, new_path) != 0) PLOGW(TAG, "couldn't migrate %s to BIN/", e->d_name);
    }
    closedir(d);
}

/* GBnnnnn/EMUnnnnn's full-size .PNG (in s_dir) is the actual viewable photo -
 * .BIN (in s_bin_dir) is just re-render material and THUMB/ just a cache,
 * but scan() below only checks .BIN to decide a numbered photo "exists".
 * If the .PNG gets deleted directly (e.g. over USB mass storage, not
 * through this app), that leaves a gallery entry with nothing to actually
 * show - so before counting anything, drop any .BIN/thumbnail whose .PNG is
 * gone, the same self-healing spirit as migrate_bin_files() above. */
static void prune_orphaned_bins(void)
{
    DIR *d = opendir(s_bin_dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int n;
        char ext[4] = {0};
        const char *prefix = "GB";
        if (!parse_numbered_file(e->d_name, "GB", &n, ext)) {
            prefix = "EMU";
            if (!parse_numbered_file(e->d_name, "EMU", &n, ext)) continue;
        }
        if (!(ext[0] == 'B' || ext[0] == 'b')) continue;

        char png_path[300];
        path_for(png_path, sizeof png_path, prefix, n, "PNG");
        FILE *check = fopen(png_path, "rb");
        if (check) {
            fclose(check);
            continue; /* PNG still there - a real photo, keep it */
        }

        char bin_path[300], thumb_path[300];
        bin_path_for(bin_path, sizeof bin_path, prefix, n);
        thumb_path_for(thumb_path, sizeof thumb_path, prefix, n);
        remove(bin_path);
        remove(thumb_path);
        PLOGI(TAG, "pruned orphaned %s%05d (PNG missing)", prefix, n);
    }
    closedir(d);
}

static void scan(void)
{
    s_count = 0;
    s_next_number = 1;
    DIR *d = opendir(s_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            int n;
            char ext[4] = {0};
            /* FAT may report names in upper or lower case. Each DC/AEB-
             * prefixed number has exactly one file - Dither Cam's .PNG,
             * Normal Cam's .JPG, or an AEB/HDR combined result's .PNG (see
             * app_storage.h) - so matching either extension still counts
             * every photo exactly once. GB-prefixed entries aren't counted
             * here at all any more - see the BIN/ pass below. */
            const char *dc_prefix = "DC";
            if (sscanf(e->d_name, "%*1[Dd]%*1[Cc]%5d.%3s", &n, ext) != 2) {
                dc_prefix = "AEB";
                if (sscanf(e->d_name, "AEB%5d.%3s", &n, ext) != 2) continue;
            }
            if (!(ext[0] == 'P' || ext[0] == 'p' || ext[0] == 'J' || ext[0] == 'j')) continue;
            if (n >= s_next_number) s_next_number = n + 1;
            if (s_count < MAX_PHOTOS) {
                photo_t *p = &s_photos[s_count++];
                *p = (photo_t){.number = n, .is_dc = true};
                snprintf(p->prefix, sizeof p->prefix, "%s", dc_prefix);
            }
        }
        closedir(d);
    }

    /* GB Camera / emulator-exported photos are counted by their .BIN, which
     * now lives in its own subfolder (see bin_path_for()) - a separate pass,
     * not a second pattern in the loop above. */
    d = opendir(s_bin_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            int n;
            char ext[4] = {0};
            const char *prefix = "GB";
            if (!parse_numbered_file(e->d_name, "GB", &n, ext)) {
                prefix = "EMU";
                if (!parse_numbered_file(e->d_name, "EMU", &n, ext)) continue;
            }
            if (!(ext[0] == 'B' || ext[0] == 'b')) continue;
            if (n >= s_next_number) s_next_number = n + 1;
            if (s_count < MAX_PHOTOS) {
                photo_t *p = &s_photos[s_count++];
                *p = (photo_t){.number = n, .is_dc = false};
                snprintf(p->prefix, sizeof p->prefix, "%s", prefix);
            }
        }
        closedir(d);
    }

    qsort(s_photos, (size_t)s_count, sizeof(photo_t), cmp_photo);
}

esp_err_t storage_init(void)
{
    s_photos = calloc(MAX_PHOTOS, sizeof(photo_t));
    if (!s_photos) return ESP_ERR_NO_MEM;

    esp_err_t err = plat_storage_mount(s_root, sizeof s_root);
    if (err != ESP_OK) {
        PLOGW(TAG, "no SD card (%s)", esp_err_to_name(err));
        return err;
    }
    snprintf(s_dir, sizeof s_dir, "%s/GBCAM", s_root);
    plat_mkdir(s_dir);
    snprintf(s_bin_dir, sizeof s_bin_dir, "%s/BIN", s_dir);
    plat_mkdir(s_bin_dir);
    snprintf(s_thumb_dir, sizeof s_thumb_dir, "%s/THUMB", s_dir);
    plat_mkdir(s_thumb_dir);
    snprintf(s_aeb_dir, sizeof s_aeb_dir, "%s/AEB", s_dir);
    plat_mkdir(s_aeb_dir);
    migrate_bin_files();
    prune_orphaned_bins();
    scan();
    s_ready = true;
    PLOGI(TAG, "%d photos in %s", s_count, s_dir);
    return ESP_OK;
}

bool storage_ready(void) { return s_ready; }
/* SD card mount root (e.g. "/sdcard"), or "" if there's no card - for other
 * SD-backed features (frames_sd_init()'s /frames folder) that need it but
 * shouldn't mount the card a second time. */
const char *storage_root(void) { return s_ready ? s_root : ""; }
int storage_count(void) { return s_count; }
int storage_number_at(int pos) { return (pos >= 0 && pos < s_count) ? s_photos[pos].number : -1; }
bool storage_is_dc_at(int pos) { return (pos >= 0 && pos < s_count) && s_photos[pos].is_dc; }

static int next_number(void)
{
    if (!s_ready || s_count >= MAX_PHOTOS || s_next_number > 99999) return -1;
    return s_next_number;
}

int storage_save(const uint8_t *shades, gbcam_palette_t palette, int frame, const char *prefix)
{
    int number = next_number();
    if (number < 0) return -1;
    char path[300];

    /* Palette-free tiles for the gallery (and future .sav export) - always
     * just the plain photo, regardless of frame (see app_storage.h). */
    static uint8_t tiles[GBCAM_TILES_SIZE];
    gbcam_shades_to_tiles(shades, tiles);
    bin_path_for(path, sizeof path, prefix, number);
    FILE *f = fopen(path, "wb");
    if (!f) {
        PLOGE(TAG, "cannot write %s", path);
        return -1;
    }
    size_t written = fwrite(tiles, 1, sizeof tiles, f);
    fclose(f);
    if (written != sizeof tiles) {
        remove(path);
        return -1;
    }

    /* Upscaled PNG in the current palette, framed if requested. */
    int w, h;
    uint8_t *rgb;
    if (frame >= 0) {
        const frame_meta_t *fm = frames_get(frame);
        frame_size(fm, PNG_SCALE, &w, &h);
        rgb = malloc((size_t)w * h * 3);
        if (rgb) frame_compose_rgb(fm, shades, palette, PNG_SCALE, rgb);
    } else {
        w = GBCAM_W * PNG_SCALE;
        h = GBCAM_H * PNG_SCALE;
        rgb = malloc((size_t)w * h * 3);
        if (rgb) {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    const uint8_t *c = gbcam_palette_rgb(palette, shades[(y / PNG_SCALE) * GBCAM_W + x / PNG_SCALE]);
                    memcpy(rgb + ((size_t)y * w + x) * 3, c, 3);
                }
        }
    }
    if (rgb) {
        path_for(path, sizeof path, prefix, number, "PNG");
        if (!stbi_write_png(path, w, h, 3, rgb, w * 3))
            PLOGW(TAG, "PNG write failed: %s", path);
        write_thumbnail(prefix, number, rgb, w, h);
        free(rgb);
    }

    photo_t *p = &s_photos[s_count++];
    *p = (photo_t){.number = number, .is_dc = false};
    snprintf(p->prefix, sizeof p->prefix, "%s", prefix);
    s_next_number = number + 1;
    PLOGI(TAG, "saved photo %s%05d", prefix, number);
    return number;
}

/* Shared by storage_save_aeb_extra()/storage_save_aeb_extra_rgb() - both just
 * differ in how they get to a plain RGB888 buffer to hand off here. */
static bool write_aeb_png(const uint8_t *rgb, int w, int h, int number, int step)
{
    char path[320];
    snprintf(path, sizeof path, "%s/AEB%05d_%+d.PNG", s_aeb_dir, number, step);
    bool ok = stbi_write_png(path, w, h, 3, rgb, w * 3) != 0;
    if (!ok) PLOGW(TAG, "AEB PNG write failed: %s", path);
    return ok;
}

/* Saves one of AEB's individual bracket exposures (see app.c's
 * gb_aeb_capture()) into its own AEB/ subfolder, tagged with the combined/
 * averaged photo's own gallery number - so a whole bracket set sits
 * together, findable from the number already visible in the gallery, instead
 * of cluttering the main numbered sequence/gallery grid with every source
 * exposure (only the combined result is a real gallery entry). No .BIN or
 * thumbnail - these aren't gallery entries, just an upscaled PNG in the
 * current palette (unframed, same rendering as storage_save()'s frame < 0
 * path) for viewing directly or feeding to an external HDR tool. */
bool storage_save_aeb_extra(const uint8_t *shades, gbcam_palette_t palette, int number, int step)
{
    if (!s_ready) return false;

    int w = GBCAM_W * PNG_SCALE, h = GBCAM_H * PNG_SCALE;
    uint8_t *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return false;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *c = gbcam_palette_rgb(palette, shades[(y / PNG_SCALE) * GBCAM_W + x / PNG_SCALE]);
            memcpy(rgb + ((size_t)y * w + x) * 3, c, 3);
        }
    bool ok = write_aeb_png(rgb, w, h, number, step);
    free(rgb);
    return ok;
}

/* Same as storage_save_aeb_extra(), for RGB mode's already-combined
 * per-channel-dithered image (rgb_mode_combine()'s output) instead of a
 * single palette-indexed shade buffer - nearest-neighbour upscaled the same
 * PNG_SCALE amount, no palette lookup needed since it's already RGB888. */
bool storage_save_aeb_extra_rgb(const uint8_t *rgb888, int number, int step)
{
    if (!s_ready) return false;

    int w = GBCAM_W * PNG_SCALE, h = GBCAM_H * PNG_SCALE;
    uint8_t *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return false;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            memcpy(rgb + ((size_t)y * w + x) * 3, rgb888 + ((size_t)(y / PNG_SCALE) * GBCAM_W + x / PNG_SCALE) * 3, 3);
    bool ok = write_aeb_png(rgb, w, h, number, step);
    free(rgb);
    return ok;
}

esp_err_t storage_load(int number, uint8_t *shades)
{
    char path[300];
    static uint8_t tiles[GBCAM_TILES_SIZE];
    bin_path_for(path, sizeof path, gb_prefix_for(number), number);
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    size_t n = fread(tiles, 1, sizeof tiles, f);
    fclose(f);
    if (n != sizeof tiles) return ESP_ERR_INVALID_SIZE;
    gbcam_tiles_to_shades(tiles, shades);
    return ESP_OK;
}

/* Byte-for-byte match against every existing GB/EMU photo's .BIN (skips DC,
 * which has no .BIN) - used by app_gbemu.c's "pull new photos" to tell a
 * genuinely new capture apart from one it's already saved. Content-based
 * rather than tracking which .sav slot a photo came from: a GB Camera-alike
 * ROM's save file reuses physical slot numbers once a roll is cleared (gb-
 * photo confirmed on real hardware), so the same slot index can hold a
 * different photo from one run to the next - comparing actual tile bytes is
 * the only check that's still correct after that. */
bool storage_has_duplicate_gb_tiles(const uint8_t tiles[GBCAM_TILES_SIZE])
{
    static uint8_t existing[GBCAM_TILES_SIZE];
    char path[300];
    for (int i = 0; i < s_count; i++) {
        if (s_photos[i].is_dc) continue;
        bin_path_for(path, sizeof path, s_photos[i].prefix, s_photos[i].number);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        size_t n = fread(existing, 1, sizeof existing, f);
        fclose(f);
        if (n == sizeof existing && memcmp(existing, tiles, sizeof existing) == 0) return true;
    }
    return false;
}

int storage_save_dc(const uint8_t *rgb888, int w, int h, bool jpeg, const char *prefix)
{
    int number = next_number();
    if (number < 0) return -1;

    char path[300];
    bool ok;
    if (jpeg) {
        /* Normal Cam: a real photo, not pixel art - saved at its actual
         * captured size, no upscale (see app_storage.h). */
        path_for(path, sizeof path, prefix, number, "JPG");
        ok = stbi_write_jpg(path, w, h, 3, rgb888, NORMAL_JPEG_QUALITY) != 0;
        if (!ok) PLOGE(TAG, "JPEG write failed: %s", path);
    } else {
        const int sw = w * DC_SAVE_SCALE, sh = h * DC_SAVE_SCALE;
        uint8_t *big = malloc((size_t)sw * sh * 3);
        if (!big) return -1;
        for (int y = 0; y < sh; y++) {
            const uint8_t *src = rgb888 + (size_t)(y / DC_SAVE_SCALE) * w * 3;
            uint8_t *dst = big + (size_t)y * sw * 3;
            for (int x = 0; x < sw; x++)
                memcpy(dst + (size_t)x * 3, src + (size_t)(x / DC_SAVE_SCALE) * 3, 3);
        }
        path_for(path, sizeof path, prefix, number, "PNG");
        ok = stbi_write_png(path, sw, sh, 3, big, sw * 3) != 0;
        free(big);
        if (!ok) PLOGE(TAG, "PNG write failed: %s", path);
    }
    if (!ok) return -1;
    /* From the original un-upscaled source either way (rgb888/w/h) - the
     * same box-average work either way, and DC_SAVE_SCALE's 4x nearest-
     * neighbour upscale in the PNG branch above has nothing left to add that
     * a thumbnail would keep anyway. */
    write_thumbnail(prefix, number, rgb888, w, h);

    photo_t *p = &s_photos[s_count++];
    *p = (photo_t){.number = number, .is_dc = true};
    snprintf(p->prefix, sizeof p->prefix, "%s", prefix);
    s_next_number = number + 1;
    PLOGI(TAG, "saved photo %s%05d", prefix, number);
    return number;
}

esp_err_t storage_load_dc(int number, uint8_t **out_rgb, int *out_w, int *out_h)
{
    char path[300];
    int w, h, comp;
    uint8_t *rgb = NULL;
    const char *prefix = dc_prefix_for(number);
    /* Don't know which extension this number was saved with (Dither Cam's
     * PNG or Normal Cam's JPG) without re-scanning the directory - trying
     * both is simpler and this only runs when opening a gallery photo. */
    path_for(path, sizeof path, prefix, number, "PNG");
    rgb = stbi_load(path, &w, &h, &comp, 3);
    if (!rgb) {
        path_for(path, sizeof path, prefix, number, "JPG");
        rgb = stbi_load(path, &w, &h, &comp, 3);
    }
    if (!rgb) return ESP_ERR_NOT_FOUND;
    *out_rgb = rgb;
    *out_w = w;
    *out_h = h;
    return ESP_OK;
}

/* Decodes a saved GBnnnnn.PNG back to RGB888 - unlike storage_load()'s .BIN
 * tiles, this is the actual upscaled export: whatever palette and frame (if
 * any) the photo was saved with, baked in, not the .BIN's palette-free
 * shades re-rendered with whatever's currently selected. Frees the same way
 * as storage_load_dc(). */
esp_err_t storage_load_gb_png(int number, uint8_t **out_rgb, int *out_w, int *out_h)
{
    char path[300];
    path_for(path, sizeof path, gb_prefix_for(number), number, "PNG");
    int w, h, comp;
    uint8_t *rgb = stbi_load(path, &w, &h, &comp, 3);
    if (!rgb) return ESP_ERR_NOT_FOUND;
    *out_rgb = rgb;
    *out_w = w;
    *out_h = h;
    return ESP_OK;
}

void storage_free_dc(uint8_t *rgb) { stbi_image_free(rgb); }

/* Gallery grid thumbnail - THUMB/ if one's already there (the normal case
 * for anything saved since thumbnails existed), otherwise falls back to
 * decoding the full photo and shrinking it down (a photo saved before this
 * existed), writing the result to THUMB/ so that fallback only has to
 * happen once per old photo, not every time it's scrolled past. Frees the
 * same way as storage_load_dc(). */
esp_err_t storage_load_thumb(int number, bool is_dc, uint8_t **out_rgb, int *out_w, int *out_h)
{
    char path[300];
    thumb_path_for(path, sizeof path, is_dc ? dc_prefix_for(number) : gb_prefix_for(number), number);
    int comp;
    uint8_t *rgb = stbi_load(path, out_w, out_h, &comp, 3);
    if (rgb) {
        *out_rgb = rgb;
        return ESP_OK;
    }

    uint8_t *full;
    int fw, fh;
    esp_err_t err = is_dc ? storage_load_dc(number, &full, &fw, &fh)
                          : storage_load_gb_png(number, &full, &fw, &fh);
    if (err != ESP_OK) return err;

    int tw, th;
    uint8_t *thumb = make_thumbnail(full, fw, fh, &tw, &th);
    storage_free_dc(full);
    if (!thumb) return ESP_ERR_NO_MEM;
    if (!stbi_write_png(path, tw, th, 3, thumb, tw * 3))
        PLOGW(TAG, "thumbnail write failed: %s", path);
    *out_rgb = thumb;
    *out_w = tw;
    *out_h = th;
    return ESP_OK;
}

bool storage_photo_path(int number, bool is_dc, char *out, size_t len, const char **out_content_type)
{
    FILE *f;
    if (is_dc) {
        /* Dither Cam/AEB's PNG or Normal Cam's JPG - see app_storage.h. */
        const char *prefix = dc_prefix_for(number);
        path_for(out, len, prefix, number, "PNG");
        if ((f = fopen(out, "rb")) != NULL) {
            fclose(f);
            *out_content_type = "image/png";
            return true;
        }
        path_for(out, len, prefix, number, "JPG");
        if ((f = fopen(out, "rb")) != NULL) {
            fclose(f);
            *out_content_type = "image/jpeg";
            return true;
        }
        return false;
    }
    path_for(out, len, gb_prefix_for(number), number, "PNG");
    if ((f = fopen(out, "rb")) == NULL) return false;
    fclose(f);
    *out_content_type = "image/png";
    return true;
}

bool storage_thumb_path(int number, bool is_dc, char *out, size_t len)
{
    thumb_path_for(out, len, is_dc ? dc_prefix_for(number) : gb_prefix_for(number), number);
    FILE *f = fopen(out, "rb");
    if (f) {
        fclose(f);
        return true;
    }
    /* Not cached yet - storage_load_thumb() generates and writes it to
     * THUMB/ as a side effect; the decoded pixels it hands back aren't
     * needed here, just the file it left behind. */
    uint8_t *rgb;
    int w, h;
    if (storage_load_thumb(number, is_dc, &rgb, &w, &h) != ESP_OK) return false;
    storage_free_dc(rgb);
    return true;
}

esp_err_t storage_delete(int number, bool is_dc)
{
    char path[300];
    const char *prefix = is_dc ? dc_prefix_for(number) : gb_prefix_for(number);
    thumb_path_for(path, sizeof path, prefix, number);
    remove(path); /* not every old photo has a thumbnail yet - a failed remove() here is expected, not logged */
    if (is_dc) {
        /* Only one of these exists for a given number (Dither Cam's .PNG,
         * Normal Cam's .JPG, or an AEB combined result's .PNG) - remove()
         * failing on the other is a harmless no-op, only warn if BOTH fail
         * (the photo's actual file, whichever format it is, didn't get
         * removed). */
        path_for(path, sizeof path, prefix, number, "PNG");
        bool png_ok = remove(path) == 0;
        path_for(path, sizeof path, prefix, number, "JPG");
        bool jpg_ok = remove(path) == 0;
        if (!png_ok && !jpg_ok) PLOGW(TAG, "delete failed: %s%05d - neither .PNG nor .JPG removed", prefix, number);
    } else {
        bin_path_for(path, sizeof path, prefix, number);
        if (remove(path) != 0) PLOGW(TAG, "delete failed: %s", path);
        path_for(path, sizeof path, prefix, number, "PNG");
        if (remove(path) != 0) PLOGW(TAG, "delete failed: %s", path);
    }
    for (int i = 0; i < s_count; i++) {
        if (s_photos[i].number == number && s_photos[i].is_dc == is_dc) {
            memmove(&s_photos[i], &s_photos[i + 1], (size_t)(s_count - i - 1) * sizeof(photo_t));
            s_count--;
            break;
        }
    }
    return ESP_OK;
}
