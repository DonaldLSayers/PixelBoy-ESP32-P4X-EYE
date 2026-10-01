# PixelBoy (ESP32-P4X-EYE)

A pocket camera on an Espressif ESP32-P4X-EYE. Three modes (Bottom button cycles
through them), plus a built-in Game Boy emulator:

- **GB CAMERA** — 128×112, 4 shades, the original Game Boy Camera's dithering/contrast/auto-exposure
- **PIXELBOY** — PIXEL CAM's Dither Cam: 32 palettes, 6 dither methods, 7 output sizes
- **DIGICAM** — plain colour preview with brightness/contrast

Cycling the Bottom button past DIGICAM launches the **Game Boy emulator** - see
[GB Emulator](#gb-emulator) below, including real Game Boy Camera cartridge support that
feeds the P4's own camera into the game.

## Controls

| Input | Viewfinder | Menu | Gallery |
|---|---|---|---|
| Encoder press (shutter) | Take photo | Activate row | Back |
| Turn encoder | Adjust active setting | Move selection | Scroll photos |
| Mode button | Switch which setting the encoder adjusts | — | — |
| Hold Mode | GB Camera only: style, PIXEL CAM ↔ HARDWARE (default) | — | — |
| Menu button | Open the menu | Close | Delete (click twice) |
| Hold Menu | Open the gallery | — | Back |
| Bottom button | Next camera mode | — | — |

The **menu** lists the current mode's settings plus Backlight (panel brightness),
Standby (screen-off timeout), Sleep (auto-sleep timeout), Gallery, WiFi Gallery and
Exit:

- **GB Camera:** Palette, Dither, Style, Scale, Frame, Auto (auto brightness/contrast),
  AEB/HDR (see [AEB / HDR](#aeb--hdr) below)
- **PixelBoy:** Palette, Method, Size, Amount, Auto (auto-exposure), Edge (sharpen)
- **Digicam:** Size

The **gallery** lists every photo from every mode, oldest first, one shared numbering
sequence - see [Your SD card](#your-sd-card) for the filenames.

## GB Camera frames

A decorative border, like the real Game Boy Camera - the Frame quick-adjust target or
menu row, shown live at 1:1. Baked into the saved `.PNG` only.

Built-in frames are PNGs in `assets/frames/` (160×144 or 160×224) - edit and rebuild with
`python tools\gen_frames.py`. Or drop `.png` files onto the SD card's `/FRAMES` folder
and they load instead, no rebuild needed.

## Custom palettes

Drop `.hex` files onto the SD card's `/PALETTES` folder (loaded once at boot) - one RGB
colour per line, 6 hex digits (`#` prefix optional), blank and `#`-comment lines ignored.
A file with exactly 4 colours shows up as a GB Camera palette; 2-64 colours, as a Dither
Cam one, so a 4-colour file appears in both. Same format `tools\gen_palettes.py` reads.

## AEB / HDR

GB Camera's HARDWARE style only (the PIXEL CAM style has no single exposure value to
bracket around). The AEB/HDR menu row cycles OFF → 3 → 5 → 7 → 9 → 11 → 13 shots;
pressing the shutter then takes that many exposures in a burst, shown live as
"BRACKETING i/N", evenly spread across a fixed ±1.5 EV range around the live
auto-exposure's converged value. They're averaged into one continuous-tone photo, which
is the gallery entry (`AEBnnnnn.PNG`, not a 4-shade `GBnnnnn` one). The individual
exposures are kept as reference material in `/GBCAM/AEB` under that same gallery number
(`AEBnnnnn_+1.PNG`, ...). Works in the RGB palette (Trichrome) too, bracketing all three
colour channels in lockstep.

## GB Emulator

A built-in Game Boy emulator ([Peanut-GB](https://github.com/deltabeard/Peanut-GB), MIT -
see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)) for playing your own legally-dumped
`.gb`/`.gbc` ROMs. It really supports the Game Boy Camera cartridge: it feeds the P4's own
camera into the cartridge's sensor interface, so the actual GB Camera ROM takes "photos"
of whatever the P4 sees, dithered through real Game Boy hardware emulation - not a
screenshot of the P4's own viewfinder.

Reach it by cycling the Bottom button past DIGICAM. ROMs live in `/ROMS`
(see [Your SD card](#your-sd-card)) - never commit one (see `.gitignore`).

### Controls

| Input | ROM/save picker | Playing |
|---|---|---|
| Encoder press (Shutter) | Pick | A |
| Turn encoder | Scroll list | Left/Right (or Up/Down - see below), momentary taps |
| Mode button | — | Start |
| CamMode (Bottom) button | Cancel back to camera | Select (momentary tap) |
| Hold CamMode | — | Toggle the encoder between Left/Right and Up/Down |
| Menu button | Confirms slot 1 (save picker only) | B (momentary tap) |
| Hold Menu | — | Exit to the camera app |

There's no physical D-pad: the encoder's momentary taps cover one axis at a time
(Left/Right by default), toggled to Up/Down by holding CamMode.

### Saves

Choosing a ROM shows a save-slot picker: every existing save for that ROM, plus one "NEW
SAVE" row past them - pick that to start another slot, up to 30 saves per ROM. Saves are
plain `.sav` files next to the ROM on the SD card (`<rom>.sav`, `<rom>.2.sav`, ...), the
same convention most GB emulators use, so they're portable to/from other tools.

### Photo extraction

"PULL NEW PHOTOS" - the first row of the ROM list, above the ROMs themselves - scans
every save (all slots, every ROM) under `/ROMS` and pulls new photos out of each Game Boy
Camera save's internal album into the regular `/GBCAM` gallery, named `EMUnnnnn` instead
of `GBnnnnn` so they're easy to tell apart from the device's own camera captures.

It's a manual action, not automatic: a save slot number isn't a stable photo identity (a
ROM can free slots for reuse, so the next photo taken can land in one an earlier pull
already handled), so it compares photo bytes against everything already in the gallery
rather than tracking which slots it has seen. Works on any `.sav`, including one copied
from another emulator or a real cartridge dump.

## USB

Plugging the "USB" port (not "Debug") into a PC prompts on-device:

- **Drive** — SD card as a mass-storage drive
- **Mirror** — streams the device's own screen to the PC as a webcam
- **GB Webcam** — streams just the GB Camera photo at 3x (with its frame, if on)

Shutter cancels. Both webcam modes show up as a UVC webcam named "PIXELBOY". Hold Menu
turns a running webcam stream back off.

## WiFi Gallery

Menu → WIFI GALLERY brings the onboard ESP32-C6 up as an access point (SSID
"PixelBoy Gallery", password "PIXELBOY1") and serves a photo gallery at
`http://192.168.4.1/` - browse/download from a phone, no cable. Shutter closes it and
powers the C6 back down.

## Your SD card

| Folder | What's in it |
|---|---|
| `/GBCAM` | Every photo the device has taken, one shared numbering sequence across all modes and sources |
| `/GBCAM/AEB` | The individual bracket exposures behind each AEB/HDR photo |
| `/FRAMES` | Your own frame `.png`s |
| `/ROMS` | `.gb`/`.gbc` ROMs and their `.sav` saves (subfolders scanned too) |
| `/PALETTES` | Your own `.hex` palettes |

Photo filenames: `GBnnnnn.PNG`+`.BIN` (GB Camera), `GBnnnnn` also being what the gallery
reads back as tiles; `DCnnnnn.PNG` (PixelBoy), `DCnnnnn.JPG` (Digicam), `AEBnnnnn.PNG`
(AEB/HDR), `EMUnnnnn` (pulled out of a GB Camera save). ROMs and extracted frame packs
are copyrighted - never commit one (see `.gitignore`).

## Build and flash

Needs **ESP-IDF v5.5.5**.

### Day to day (P4 firmware)

Changes to this project's own firmware don't touch the C6 at all:

```powershell
. C:\Espressif\esp-idf\export.ps1
cd firmware
idf.py -p COM5 build flash monitor    # "Debug" USB-C port; check the COM number in Device Manager
```

`Ctrl+]` leaves the monitor. If flashing can't connect: hold Boot, press Reset, release
Boot, then flash again. With no `c6_fw.bin` staged, the boot-time C6 check is a
near-zero-cost no-op and boot stays fast.

### First flash / updating the C6

The onboard C6 has no exposed UART/USB of its own, so its firmware is flashed *through*
the P4's USB port, over the SDIO link the two already share, via ESP-Hosted's own OTA
API. Without it the P4 side still builds and runs fine - WIFI GALLERY in the menu just
fails ("WIFI FAILED"). On a fresh board, or when the C6 firmware itself changes:

1. Build the C6's co-processor firmware - a separate ESP-IDF project, own chip target,
   not part of the P4 build:
   ```powershell
   . C:\Espressif\esp-idf\export.ps1
   cd tools\c6_coprocessor
   idf.py set-target esp32c6
   idf.py build
   ```
   Output: `tools\c6_coprocessor\build\eh_cp_transport_sdcard.bin`.
2. Copy that file into `firmware\c6fw_image\c6_fw.bin` (create the folder if it's not
   there - it's gitignored, so nothing this big ends up committed).
3. Flash and monitor the P4 as above. The staged file rides along in the `c6fw`
   partition, and `main.c` calls `wifi_gallery_check_c6_update()` on every boot: finding
   it staged, it powers the C6 up, streams the firmware over SDIO, activates it, and lets
   ESP-Hosted force a clean host restart to boot into it (expected - a brief screen
   blank, not a crash). Watch the on-screen status ("CHECKING C6...", "FLASHING C6... DO
   NOT DISCONNECT", then "C6 FLASHED OK") or the `wifi_gallery` serial log tag.
4. **Delete `firmware\c6fw_image\c6_fw.bin` once it's confirmed working.** The device
   erases its own on-flash copy, but not this source file - leaving it in place means the
   next `idf.py flash` (even an unrelated P4-only change) re-stages it, so every boot
   after that reflashes the C6 again - the symptom is boot suddenly getting slower while
   still showing a C6 status.

## PC tools

Needs **ESP-IDF v5.5.5** and **Zig** (`winget install zig.zig`, used as the C compiler).

```powershell
.\tools\host\build.ps1
..\tools\host\gbcam_cli.exe myphoto.jpg                    # -> myphoto_gb.png, myphoto_x4.png
..\tools\host\gbcam_cli.exe myphoto.jpg --sweep contrast   # all 16 contrast levels
python tools\host\gbcam_live.py                            # live webcam preview, needs opencv-python/numpy
```

`gbcam_cli.exe` options: `-b` brightness 0–16, `-c` contrast 0–15, `-d` dither, `-p`
palette, `-s` scale. GB Camera has two styles: HARDWARE (default, models the real
M64282FP sensor, with edge outlines) and PIXEL CAM (auto contrast/gamma, no edge
enhancement, cleaner).

## Layout

```
assets/icons       menu row icons, 16x16 PNG (edit, then run tools/gen_icons.py)
assets/frames      GB Camera border art, 160x144/160x224 PNG
assets/fonts       menu font source (tools/gen_font.py)
components/gbcam   portable image core (PC + firmware)
components/peanut_gb  vendored Game Boy emulator core (single header)
components/stb     PNG writer
firmware/          ESP-IDF project
tools/host         PC test tools (photo files, live webcam preview)
```

Third-party code: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
