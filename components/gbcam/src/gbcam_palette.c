#include <stddef.h>

#include "gbcam.h"
#include "palettes_data.h"

static int s_extra_count;
static gbcam_extra_palette_rgb_fn s_extra_rgb;
static gbcam_extra_palette_name_fn s_extra_name;

void gbcam_set_extra_palettes(int count, gbcam_extra_palette_rgb_fn rgb_fn, gbcam_extra_palette_name_fn name_fn)
{
    s_extra_count = count > 0 ? count : 0;
    s_extra_rgb = rgb_fn;
    s_extra_name = name_fn;
}

int gbcam_palette_count(void) { return GBCAM_PALETTE_COUNT + s_extra_count; }

/* Shade 0 = lightest .. 3 = darkest. RGB888. */
const uint8_t *gbcam_palette_rgb(gbcam_palette_t p, uint8_t shade)
{
    if (p < GBCAM_PALETTE_COUNT) return gb_palette_rgb[p][shade & 3];
    if (p < GBCAM_PALETTE_COUNT + s_extra_count && s_extra_rgb) return s_extra_rgb(p - GBCAM_PALETTE_COUNT, shade & 3);
    return gb_palette_rgb[GBCAM_PALETTE_DEFAULT][shade & 3];
}

const char *gbcam_palette_name(gbcam_palette_t p)
{
    if (p < GBCAM_PALETTE_COUNT) return gb_palette_names[p];
    if (p < GBCAM_PALETTE_COUNT + s_extra_count && s_extra_name) return s_extra_name(p - GBCAM_PALETTE_COUNT);
    return "?";
}
