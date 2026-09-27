/* PNG/JPEG decoder for the firmware (gallery: loading a saved Dither Cam /
 * Normal Cam photo back to show it). Only these two are compiled in, to keep
 * code size down - PNG for GB Camera/Dither Cam and Normal Cam's PNG
 * (non-JPEG) save path, JPEG for Normal Cam's actual photo save path
 * (storage_save_dc()'s jpeg=true branch) - both are genuinely written by
 * this project, unlike a PNG-only build's stale claim that PNG was the only
 * one that mattered: that silently broke every Normal Cam gallery photo
 * (stbi_load() always failing on a .JPG, falling through to a blank black
 * screen) without ever failing loudly. stb_image: public domain / MIT (see
 * header). */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
