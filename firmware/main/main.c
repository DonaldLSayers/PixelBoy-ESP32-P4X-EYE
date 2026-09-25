/*
 * GB Camera for the ESP32-P4X-EYE: entry point. The app itself is in app.c
 * (shared with the PC simulator in tools/sim).
 *
 * Boots straight into a viewfinder with three camera modes (Bottom button cycles):
 *   GB CAMERA    the Game Boy Camera look (PIXEL CAM / HARDWARE style), with
 *                on-screen brightness/contrast bars like the real camera
 *   PIXELBOY     PIXEL CAM's Dither Cam: arbitrary palette + dither pattern
 *   DIGICAM      plain colour preview, with brightness/contrast
 *
 * Controls:
 *   Encoder press    take a photo (shutter)
 *   Encoder turn     adjust whichever of the two settings is active (see Mode)
 *   Mode button      click: switch which setting the encoder adjusts
 *                    hold (GB Camera only): style, PIXEL CAM <-> HARDWARE
 *   Menu button      click: open/close the menu   hold: gallery
 *   Bottom button    click: next camera mode
 * In the menu: encoder turn moves the selection, encoder press activates the
 * row (cycles its value, or runs Gallery/Exit). The gallery lists every
 * photo from every mode, oldest first: encoder scrolls, encoder press goes
 * back, Menu button also goes back, and clicking the Menu button twice
 * deletes the current photo.
 */
#include "app.h"

void app_main(void)
{
    if (app_init() != ESP_OK)
        return;
    for (;;)
        app_step();
}
