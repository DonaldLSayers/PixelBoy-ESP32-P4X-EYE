/* PNG decoder for the firmware (gallery: loading a saved Dither Cam / Normal
 * Cam photo back to show it). Only PNG is compiled in - it's the only format
 * this project ever writes or needs to read back - to keep code size down.
 * stb_image: public domain / MIT (see header). */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
