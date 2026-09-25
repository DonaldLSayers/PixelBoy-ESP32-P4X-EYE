/*
 * Contrast tables, dither patterns and threshold-matrix generation.
 *
 * Ported from gb-photo (https://github.com/untoxa/gb-photo), src/dither_patterns.c
 *
 * MIT License
 * Copyright (c) 2022 Toxa
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "gbcam.h"

#define NUM_INTERVALS 3
#define NUM_BASE_VALUES 16
#define PATTERN_MATRIX_SIZE 16

/* Threshold ranges per contrast level: 4 values bound the 3 threshold intervals. */
static const uint8_t dither_high_light_values[GBCAM_CONTRAST_LEVELS][4] = {
    {0x80, 0x8F, 0xD0, 0xE6},
    {0x82, 0x90, 0xC8, 0xE3},
    {0x84, 0x90, 0xC0, 0xE0},
    {0x85, 0x91, 0xB8, 0xDD},
    {0x86, 0x91, 0xB1, 0xDB},
    {0x87, 0x92, 0xAA, 0xD8},
    {0x88, 0x92, 0xA5, 0xD5},
    {0x89, 0x92, 0xA2, 0xD2},
    {0x8A, 0x92, 0xA1, 0xC8},
    {0x8B, 0x92, 0xA0, 0xBE},
    {0x8C, 0x92, 0x9E, 0xB4},
    {0x8D, 0x92, 0x9C, 0xAC},
    {0x8E, 0x92, 0x9B, 0xA5},
    {0x8F, 0x92, 0x99, 0xA0},
    {0x90, 0x92, 0x97, 0x9A},
    {0x92, 0x92, 0x92, 0x92},
};

static const uint8_t dither_low_light_values[GBCAM_CONTRAST_LEVELS][4] = {
    {0x80, 0x94, 0xDC, 0xFF},
    {0x82, 0x95, 0xD2, 0xFF},
    {0x84, 0x96, 0xCA, 0xFF},
    {0x86, 0x96, 0xC4, 0xFF},
    {0x88, 0x97, 0xBE, 0xFF},
    {0x8A, 0x97, 0xB8, 0xFF},
    {0x8B, 0x98, 0xB2, 0xF5},
    {0x8C, 0x98, 0xAC, 0xEB},
    {0x8D, 0x98, 0xAA, 0xDD},
    {0x8E, 0x98, 0xA8, 0xD0},
    {0x8F, 0x98, 0xA6, 0xC4},
    {0x90, 0x98, 0xA4, 0xBA},
    {0x92, 0x98, 0xA1, 0xB2},
    {0x94, 0x98, 0x9D, 0xA8},
    {0x96, 0x98, 0x99, 0xA0},
    {0x98, 0x98, 0x98, 0x98},
};

static const uint8_t pattern_null[PATTERN_MATRIX_SIZE] = {
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

/* original (standard) */
static const uint8_t pattern_standard[PATTERN_MATRIX_SIZE] = {
    0x00, 0x0C, 0x03, 0x0F,
    0x08, 0x04, 0x0B, 0x07,
    0x02, 0x0E, 0x01, 0x0D,
    0x0A, 0x06, 0x09, 0x05,
};

static const uint8_t pattern_2x2[PATTERN_MATRIX_SIZE] = {
    0x01, 0x01, 0x0A, 0x0A,
    0x01, 0x01, 0x0A, 0x0A,
    0x0D, 0x0D, 0x03, 0x03,
    0x0D, 0x0D, 0x03, 0x03,
};

static const uint8_t pattern_grid[PATTERN_MATRIX_SIZE] = {
    0x0C, 0x08, 0x07, 0x0C,
    0x07, 0x01, 0x02, 0x07,
    0x07, 0x07, 0x02, 0x08,
    0x0D, 0x08, 0x08, 0x0C,
};

static const uint8_t pattern_maze[PATTERN_MATRIX_SIZE] = {
    0x00, 0x01, 0x03, 0x05,
    0x02, 0x0A, 0x0B, 0x0D,
    0x04, 0x0C, 0x07, 0x08,
    0x06, 0x0E, 0x09, 0x0F,
};

static const uint8_t pattern_nest[PATTERN_MATRIX_SIZE] = {
    0x00, 0x01, 0x08, 0x0B,
    0x02, 0x06, 0x0A, 0x0C,
    0x09, 0x0E, 0x03, 0x04,
    0x0D, 0x0F, 0x05, 0x07,
};

static const uint8_t pattern_fuzz[PATTERN_MATRIX_SIZE] = {
    0x00, 0x09, 0x0E, 0x07,
    0x04, 0x0D, 0x02, 0x0B,
    0x08, 0x01, 0x06, 0x0F,
    0x0C, 0x05, 0x0A, 0x03,
};

static const uint8_t pattern_vertical[PATTERN_MATRIX_SIZE] = {
    0x00, 0x0A, 0x07, 0x0D,
    0x01, 0x0B, 0x04, 0x0E,
    0x02, 0x08, 0x05, 0x0F,
    0x03, 0x09, 0x06, 0x0C,
};

static const uint8_t pattern_horizontal[PATTERN_MATRIX_SIZE] = {
    0x00, 0x01, 0x02, 0x03,
    0x0A, 0x0B, 0x08, 0x09,
    0x07, 0x04, 0x05, 0x06,
    0x0D, 0x0E, 0x0F, 0x0C,
};

static const uint8_t pattern_diagonal[PATTERN_MATRIX_SIZE] = {
    0x00, 0x08, 0x04, 0x0C,
    0x0D, 0x01, 0x09, 0x05,
    0x06, 0x0E, 0x02, 0x0A,
    0x0B, 0x07, 0x0F, 0x03,
};

static const uint8_t *const dithering_patterns[GBCAM_DITHER_COUNT][NUM_INTERVALS] = {
    [GBCAM_DITHER_OFF]        = {pattern_null,       pattern_null,       pattern_null},
    [GBCAM_DITHER_DEFAULT]    = {pattern_standard,   pattern_standard,   pattern_standard},
    [GBCAM_DITHER_2X2]        = {pattern_2x2,        pattern_2x2,        pattern_2x2},
    [GBCAM_DITHER_GRID]       = {pattern_grid,       pattern_grid,       pattern_grid},
    [GBCAM_DITHER_MAZE]       = {pattern_maze,       pattern_maze,       pattern_maze},
    [GBCAM_DITHER_NEST]       = {pattern_nest,       pattern_nest,       pattern_nest},
    [GBCAM_DITHER_FUZZ]       = {pattern_fuzz,       pattern_fuzz,       pattern_fuzz},
    [GBCAM_DITHER_VERTICAL]   = {pattern_vertical,   pattern_vertical,   pattern_vertical},
    [GBCAM_DITHER_HORIZONTAL] = {pattern_horizontal, pattern_horizontal, pattern_horizontal},
    [GBCAM_DITHER_MIX]        = {pattern_horizontal, pattern_diagonal,   pattern_vertical},
};

static const char *const dither_names[GBCAM_DITHER_COUNT] = {
    "Off", "Default", "2x2", "Grid", "Maze", "Nest", "Fuzz", "Vertical", "Horizontal", "Mix",
};

static void dither_gen_base_values(uint8_t a, uint8_t b, uint8_t *buffer)
{
    uint16_t start = (uint16_t)(a << 8);
    uint16_t step = (a < b) ? (uint16_t)(((uint16_t)(b << 8) - start) >> 4) : 0;
    for (int i = NUM_BASE_VALUES; i != 0; i--, start += step)
        *buffer++ = (uint8_t)(start >> 8);
}

void gbcam_build_matrix(uint8_t out[48], gbcam_dither_t dither, bool high_light, uint8_t contrast)
{
    uint8_t base[NUM_INTERVALS][NUM_BASE_VALUES];

    if (dither >= GBCAM_DITHER_COUNT)
        dither = GBCAM_DITHER_DEFAULT;
    if (contrast >= GBCAM_CONTRAST_LEVELS)
        contrast = GBCAM_CONTRAST_LEVELS - 1;

    const uint8_t *range = high_light ? dither_high_light_values[contrast]
                                      : dither_low_light_values[contrast];
    dither_gen_base_values(range[0], range[1], base[0]);
    dither_gen_base_values(range[1], range[2], base[1]);
    dither_gen_base_values(range[2], range[3], base[2]);

    uint8_t *dest = out;
    for (int i = 0; i != PATTERN_MATRIX_SIZE; i++) {
        *dest++ = base[0][dithering_patterns[dither][0][i]];
        *dest++ = base[1][dithering_patterns[dither][1][i]];
        *dest++ = base[2][dithering_patterns[dither][2][i]];
    }
}

const char *gbcam_dither_name(gbcam_dither_t d)
{
    return (d < GBCAM_DITHER_COUNT) ? dither_names[d] : "?";
}
