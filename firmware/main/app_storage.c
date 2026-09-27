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
} photo_t;

static bool s_ready;
static char s_root[224];  /* SD card mount root, e.g. "/sdcard" */
static char s_dir[256];   /* <mount root>/GBCAM */
static char s_bin_dir[276]; /* <s_dir>/BIN - GB Camera's raw tiles, kept out of the way of the browsable photos */
static char s_thumb_dir[276]; /* <s_dir>/THUMB - small pre-shrunk copies for the gallery grid, see make_thumbnail() */
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

/* GBnnnnn.BIN specifically lives in its own subfolder - it's the raw
 * palette-free tile data (re-render material, not something to look at
 * directly), unlike every other saved file (the .PNG/.JPG previews,
 * Dither/Normal Cam's own saves) which are all actual viewable photos side
 * by side in one folder. */
static void bin_path_for(char *out, size_t len, int number)
{
    snprintf(out, len, "%s/GB%05d.BIN", s_bin_dir, number);
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

/* One-time migration for a card that already has GBnnnnn.BIN files sitting
 * directly in s_dir from before this split existed - moves each into
 * s_bin_dir so scan() (which now only looks for them there) still finds
 * every existing photo instead of losing them. Best-effort: a rename
 * failure just leaves that one where it was, to be retried next boot. */
static void migrate_bin_files(void)
{
    DIR *d = opendir(s_dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int n;
        char ext[4] = {0};
        if (sscanf(e->d_name, "%*1[Gg]%*1[Bb]%5d.%3s", &n, ext) != 2) continue;
        if (!(ext[0] == 'B' || ext[0] == 'b')) continue;
        char old_path[300], new_path[300];
        snprintf(old_path, sizeof old_path, "%s/%s", s_dir, e->d_name);
        bin_path_for(new_path, sizeof new_path, n);
        if (rename(old_path, new_path) != 0) PLOGW(TAG, "couldn't migrate %s to BIN/", e->d_name);
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
            /* FAT may report names in upper or lower case. Each DC-prefixed
             * number has exactly one file - Dither Cam's .PNG or Normal
             * Cam's .JPG (see app_storage.h) - so matching either extension
             * still counts every photo exactly once. GB-prefixed entries
             * aren't counted here at all any more - see the BIN/ pass below. */
            if (sscanf(e->d_name, "%*1[Dd]%*1[Cc]%5d.%3s", &n, ext) != 2) continue;
            if (!(ext[0] == 'P' || ext[0] == 'p' || ext[0] == 'J' || ext[0] == 'j')) continue;
            if (n >= s_next_number) s_next_number = n + 1;
            if (s_count < MAX_PHOTOS) s_photos[s_count++] = (photo_t){.number = n, .is_dc = true};
        }
        closedir(d);
    }

    /* GB Camera photos are counted by their .BIN, which now lives in its own
     * subfolder (see bin_path_for()) - a separate pass, not a second pattern
     * in the loop above. */
    d = opendir(s_bin_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            int n;
            char ext[4] = {0};
            if (sscanf(e->d_name, "%*1[Gg]%*1[Bb]%5d.%3s", &n, ext) != 2) continue;
            if (!(ext[0] == 'B' || ext[0] == 'b')) continue;
            if (n >= s_next_number) s_next_number = n + 1;
            if (s_count < MAX_PHOTOS) s_photos[s_count++] = (photo_t){.number = n, .is_dc = false};
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
    migrate_bin_files();
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

int storage_save(const uint8_t *shades, gbcam_palette_t palette, int frame)
{
    int number = next_number();
    if (number < 0) return -1;
    char path[300];

    /* Palette-free tiles for the gallery (and future .sav export) - always
     * just the plain photo, regardless of frame (see app_storage.h). */
    static uint8_t tiles[GBCAM_TILES_SIZE];
    gbcam_shades_to_tiles(shades, tiles);
    bin_path_for(path, sizeof path, number);
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
        path_for(path, sizeof path, "GB", number, "PNG");
        if (!stbi_write_png(path, w, h, 3, rgb, w * 3))
            PLOGW(TAG, "PNG write failed: %s", path);
        write_thumbnail("GB", number, rgb, w, h);
        free(rgb);
    }

    s_photos[s_count++] = (photo_t){.number = number, .is_dc = false};
    s_next_number = number + 1;
    PLOGI(TAG, "saved photo GB%05d", number);
    return number;
}

esp_err_t storage_load(int number, uint8_t *shades)
{
    char path[300];
    static uint8_t tiles[GBCAM_TILES_SIZE];
    bin_path_for(path, sizeof path, number);
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    size_t n = fread(tiles, 1, sizeof tiles, f);
    fclose(f);
    if (n != sizeof tiles) return ESP_ERR_INVALID_SIZE;
    gbcam_tiles_to_shades(tiles, shades);
    return ESP_OK;
}

int storage_save_dc(const uint8_t *rgb888, int w, int h, bool jpeg)
{
    int number = next_number();
    if (number < 0) return -1;

    char path[300];
    bool ok;
    if (jpeg) {
        /* Normal Cam: a real photo, not pixel art - saved at its actual
         * captured size, no upscale (see app_storage.h). */
        path_for(path, sizeof path, "DC", number, "JPG");
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
        path_for(path, sizeof path, "DC", number, "PNG");
        ok = stbi_write_png(path, sw, sh, 3, big, sw * 3) != 0;
        free(big);
        if (!ok) PLOGE(TAG, "PNG write failed: %s", path);
    }
    if (!ok) return -1;
    /* From the original un-upscaled source either way (rgb888/w/h) - the
     * same box-average work either way, and DC_SAVE_SCALE's 4x nearest-
     * neighbour upscale in the PNG branch above has nothing left to add that
     * a thumbnail would keep anyway. */
    write_thumbnail("DC", number, rgb888, w, h);

    s_photos[s_count++] = (photo_t){.number = number, .is_dc = true};
    s_next_number = number + 1;
    PLOGI(TAG, "saved photo DC%05d", number);
    return number;
}

esp_err_t storage_load_dc(int number, uint8_t **out_rgb, int *out_w, int *out_h)
{
    char path[300];
    int w, h, comp;
    uint8_t *rgb = NULL;
    /* Don't know which extension this number was saved with (Dither Cam's
     * PNG or Normal Cam's JPG) without re-scanning the directory - trying
     * both is simpler and this only runs when opening a gallery photo. */
    path_for(path, sizeof path, "DC", number, "PNG");
    rgb = stbi_load(path, &w, &h, &comp, 3);
    if (!rgb) {
        path_for(path, sizeof path, "DC", number, "JPG");
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
    path_for(path, sizeof path, "GB", number, "PNG");
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
    thumb_path_for(path, sizeof path, is_dc ? "DC" : "GB", number);
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

esp_err_t storage_delete(int number, bool is_dc)
{
    char path[300];
    thumb_path_for(path, sizeof path, is_dc ? "DC" : "GB", number);
    remove(path); /* not every old photo has a thumbnail yet - a failed remove() here is expected, not logged */
    if (is_dc) {
        /* Only one of these exists for a given number (Dither Cam's .PNG or
         * Normal Cam's .JPG) - remove() failing on the other is a harmless
         * no-op, only warn if BOTH fail (the photo's actual file, whichever
         * format it is, didn't get removed). */
        path_for(path, sizeof path, "DC", number, "PNG");
        bool png_ok = remove(path) == 0;
        path_for(path, sizeof path, "DC", number, "JPG");
        bool jpg_ok = remove(path) == 0;
        if (!png_ok && !jpg_ok) PLOGW(TAG, "delete failed: DC%05d - neither .PNG nor .JPG removed", number);
    } else {
        bin_path_for(path, sizeof path, number);
        if (remove(path) != 0) PLOGW(TAG, "delete failed: %s", path);
        path_for(path, sizeof path, "GB", number, "PNG");
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
