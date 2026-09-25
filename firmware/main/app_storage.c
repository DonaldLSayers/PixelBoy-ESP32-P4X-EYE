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
#define PNG_SCALE 4

typedef struct {
    int number;
    bool is_dc;
} photo_t;

static bool s_ready;
static char s_root[224];  /* SD card mount root, e.g. "/sdcard" */
static char s_dir[256];   /* <mount root>/GBCAM */
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

static void scan(void)
{
    s_count = 0;
    s_next_number = 1;
    DIR *d = opendir(s_dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int n;
        char ext[4] = {0};
        bool is_dc, matched;
        /* FAT may report names in upper or lower case. */
        if (sscanf(e->d_name, "%*1[Dd]%*1[Cc]%5d.%3s", &n, ext) == 2) {
            is_dc = true;
            /* Each DC-prefixed number has exactly one file - Dither Cam's
             * .PNG or Normal Cam's .JPG (see app_storage.h) - so matching
             * either extension still counts every photo exactly once. */
            matched = ext[0] == 'P' || ext[0] == 'p' || ext[0] == 'J' || ext[0] == 'j';
        } else if (sscanf(e->d_name, "%*1[Gg]%*1[Bb]%5d.%3s", &n, ext) == 2) {
            is_dc = false;
            matched = ext[0] == 'B' || ext[0] == 'b'; /* only count a GB photo by its .BIN */
        } else {
            matched = false;
        }
        if (!matched) continue;
        if (n >= s_next_number) s_next_number = n + 1;
        if (s_count < MAX_PHOTOS) s_photos[s_count++] = (photo_t){.number = n, .is_dc = is_dc};
    }
    closedir(d);
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
    path_for(path, sizeof path, "GB", number, "BIN");
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
    path_for(path, sizeof path, "GB", number, "BIN");
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

void storage_free_dc(uint8_t *rgb) { stbi_image_free(rgb); }

esp_err_t storage_delete(int number, bool is_dc)
{
    char path[300];
    if (is_dc) {
        /* Only one of these exists for a given number (Dither Cam's .PNG or
         * Normal Cam's .JPG) - remove() on the other is a harmless no-op. */
        path_for(path, sizeof path, "DC", number, "PNG");
        remove(path);
        path_for(path, sizeof path, "DC", number, "JPG");
        remove(path);
    } else {
        path_for(path, sizeof path, "GB", number, "BIN");
        remove(path);
        path_for(path, sizeof path, "GB", number, "PNG");
        remove(path);
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
