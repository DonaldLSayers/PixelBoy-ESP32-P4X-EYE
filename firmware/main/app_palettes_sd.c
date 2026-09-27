#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_palettes_sd.h"
#include "app_storage.h"
#include "dithercam.h"
#include "platform.h"

static const char *TAG = "palettes_sd";

/* Case-insensitive extension match; dot points at the '.' in the filename. */
static bool ext_is(const char *dot, const char *ext)
{
    size_t i = 0;
    for (; ext[i]; i++) {
        char a = dot[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != ext[i]) return false;
    }
    return dot[i] == 0;
}

typedef struct {
    uint8_t rgb[MAX_PALETTE_COLORS][3];
    uint8_t count;
    char name[13];  /* short uppercase display name - menu rows truncate further to fit anyway */
    char stem[48];  /* full original filename (no extension, original case) - for the .dclut cache path */
} sd_palette_t;

static sd_palette_t s_pal[MAX_SD_PALETTES];
static int s_total;
static int s_gb_map[MAX_SD_PALETTES]; /* indices into s_pal[] with exactly 4 colours */
static int s_gb_total;

/* line is one text line (already read, may have a trailing \r\n); accepts
 * exactly 6 hex digits, optionally followed by whitespace/EOL - anything
 * else (blank, '#'-led comment, junk) isn't a colour. */
static bool parse_hex_line(const char *line, uint8_t rgb[3])
{
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#') return false;
    int v = 0;
    for (int i = 0; i < 6; i++) {
        char c = line[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | d;
    }
    for (const char *p = line + 6; *p; p++)
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') return false;
    rgb[0] = (uint8_t)(v >> 16);
    rgb[1] = (uint8_t)(v >> 8);
    rgb[2] = (uint8_t)v;
    return true;
}

static void load_hex(const char *path, const char *stem)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    sd_palette_t pal = {0};
    char line[64];
    while (pal.count < MAX_PALETTE_COLORS && fgets(line, sizeof line, f))
        if (parse_hex_line(line, pal.rgb[pal.count])) pal.count++;
    fclose(f);

    if (pal.count < 2) {
        PLOGW(TAG, "%s: fewer than 2 colours, skipping", path);
        return;
    }
    if (s_total >= MAX_SD_PALETTES) {
        PLOGW(TAG, "cap of %d reached, skipping \"%s\"", MAX_SD_PALETTES, stem);
        return;
    }

    /* Display name: uppercase, and '-'/'_' (common word separators in a
     * filename) read as spaces - "nanner-pancakes" shows as "NANNER
     * PANCAKES", not "NANNER-PANCA". stem (the .dclut cache path) keeps the
     * real filename untouched. */
    snprintf(pal.name, sizeof pal.name, "%.*s", (int)(sizeof pal.name - 1), stem);
    for (char *c = pal.name; *c; c++)
        *c = (char)((*c == '-' || *c == '_') ? ' ' : toupper((unsigned char)*c));
    snprintf(pal.stem, sizeof pal.stem, "%s", stem);

    s_pal[s_total] = pal;
    if (pal.count == 4 && s_gb_total < MAX_SD_PALETTES) s_gb_map[s_gb_total++] = s_total;
    s_total++;
}

void palettes_sd_init(void)
{
    s_total = 0;
    s_gb_total = 0;
    const char *root = storage_root();
    if (!root[0]) return; /* no SD card */

    char dir[256];
    snprintf(dir, sizeof dir, "%s/PALETTES", root);
    DIR *d = opendir(dir);
    if (!d) return; /* folder doesn't exist - nothing to load, not an error */

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || !ext_is(dot, ".hex")) continue;

        char stem[48];
        size_t stemlen = (size_t)(dot - e->d_name);
        if (stemlen >= sizeof stem) stemlen = sizeof stem - 1;
        memcpy(stem, e->d_name, stemlen);
        stem[stemlen] = 0;

        char path[300];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        load_hex(path, stem);
    }
    closedir(d);
    PLOGI(TAG, "%d palette(s) from /PALETTES (%d usable for GB Camera)", s_total, s_gb_total);
}

int palettes_sd_gb_count(void) { return s_gb_total; }

const char *palettes_sd_gb_name(int index)
{
    return (index >= 0 && index < s_gb_total) ? s_pal[s_gb_map[index]].name : "?";
}

const uint8_t *palettes_sd_gb_rgb(int index, uint8_t shade)
{
    static const uint8_t fallback[3] = {128, 128, 128};
    if (index < 0 || index >= s_gb_total) return fallback;
    /* Files are dark -> light (see app_palettes_sd.h); gbcam shades run
     * 0 = lightest .. 3 = darkest, so reverse on read. */
    return s_pal[s_gb_map[index]].rgb[3 - (shade & 3)];
}

int palettes_sd_dc_count(void) { return s_total; }

const char *palettes_sd_dc_name(int index)
{
    return (index >= 0 && index < s_total) ? s_pal[index].name : "?";
}

int palettes_sd_dc_colors(int index, const uint8_t (**colors)[3])
{
    if (index < 0 || index >= s_total) {
        *colors = NULL;
        return 0;
    }
    *colors = s_pal[index].rgb;
    return s_pal[index].count;
}

/* <root>/PALETTES/<stem>.dclut - next to the source .hex, full original
 * filename (not the 12-char display name - that's truncated for the menu
 * and would collide between palettes that only differ after char 12). */
static void lut_path(char *out, size_t outsz, int index)
{
    snprintf(out, outsz, "%s/PALETTES/%s.dclut", storage_root(), s_pal[index].stem);
}

bool palettes_sd_load_lut(int index, uint8_t *out_lut)
{
    if (index < 0 || index >= s_total) return false;
    char path[300];
    lut_path(path, sizeof path, index);
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    uint8_t count = 0;
    uint8_t colors[MAX_PALETTE_COLORS][3];
    /* The palette's own colours are stored first - a .hex edited since this
     * was saved (different count or colours) fails this comparison, so a
     * stale LUT is never served; the caller rebuilds and overwrites it. */
    bool ok = fread(&count, 1, 1, f) == 1 && count == s_pal[index].count &&
              fread(colors, 3, count, f) == (size_t)count &&
              memcmp(colors, s_pal[index].rgb, (size_t)count * 3) == 0 &&
              fread(out_lut, 1, DC_LUT_SIZE, f) == DC_LUT_SIZE;
    fclose(f);
    return ok;
}

void palettes_sd_save_lut(int index, const uint8_t *lut)
{
    if (index < 0 || index >= s_total) return;
    char path[300];
    lut_path(path, sizeof path, index);
    FILE *f = fopen(path, "wb");
    if (!f) return; /* best-effort - read-only/full card just means rebuilding again next time */
    fwrite(&s_pal[index].count, 1, 1, f);
    fwrite(s_pal[index].rgb, 3, s_pal[index].count, f);
    fwrite(lut, 1, DC_LUT_SIZE, f);
    fclose(f);
}
