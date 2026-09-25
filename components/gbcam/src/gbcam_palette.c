#include "gbcam.h"
#include "palettes_data.h"

/* Shade 0 = lightest .. 3 = darkest. RGB888. */
const uint8_t *gbcam_palette_rgb(gbcam_palette_t p, uint8_t shade)
{
    if (p >= GBCAM_PALETTE_COUNT) p = GBCAM_PALETTE_DEFAULT;
    return gb_palette_rgb[p][shade & 3];
}

const char *gbcam_palette_name(gbcam_palette_t p)
{
    return (p < GBCAM_PALETTE_COUNT) ? gb_palette_names[p] : "?";
}
