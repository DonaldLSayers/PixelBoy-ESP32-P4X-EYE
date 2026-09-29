#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Game Boy emulator (Peanut-GB, vendored in components/peanut_gb), including
 * real Game Boy Camera cartridge (mbc == 6) support: a legally-dumped GB
 * Camera ROM's sensor register interface is fed live frames from this
 * project's own gbcam capture pipeline instead of a real M64282FP sensor, so
 * it takes "photos" of whatever the P4's camera sees, run through actual
 * Game Boy hardware emulation (see app_gbemu.c).
 *
 * ROM source: every .gb/.gbc file anywhere under the SD card's /ROMS folder,
 * found recursively - including /ROMS/PROCESSED (see app_frames_sd.c),
 * where the frame-extraction feature archives a ROM once it's scanned it
 * once, untouched and just as playable as the original.
 *
 * gbemu_run() is reached from the real menu (ROW_GBEMU in app.c) or by
 * cycling the Bottom button past DIGICAM (CAM_MODE_EMULATOR in app.c) as a
 * self-contained blocking excursion: it shows a scrollable ROM list (encoder
 * scrolls, Shutter picks, Menu or CamMode cancels back to the camera app),
 * then for a GB Camera ROM a save-slot picker (any number of slots, one
 * .sav each - new photos in a slot auto-export into the regular /GBCAM
 * gallery every time you exit a session using it), then runs the chosen
 * ROM/save.
 *
 * While playing: Shutter is A, Mode is Start (true press/release, both held
 * as long as the physical button is). Menu click is B and CamMode click is
 * Select - both momentary taps rather than press/release, since Menu's and
 * CamMode's long-presses are already spoken for (exit, the axis toggle
 * below) and a real hold would never reach the game past 600ms either way.
 * The encoder's momentary taps cover one axis at a time - Left/Right by
 * default, or Up/Down after holding CamMode to toggle (same idiom as Mode-
 * button-click switching what the encoder adjusts in the normal camera
 * app). Holding Menu exits back to the camera app. */
void gbemu_run(void);
