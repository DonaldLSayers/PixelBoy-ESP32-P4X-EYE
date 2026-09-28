# PixelBoy (ESP32-P4X-EYE)

A pocket camera on an Espressif ESP32-P4X-EYE with three modes (Bottom button cycles):
- **GB CAMERA** — 128×112, 4 shades, the original Game Boy Camera's dithering/contrast/auto-exposure
- **PIXELBOY** — PIXEL CAM's Dither Cam: 32 palettes, 6 dither methods, 7 output sizes
- **DIGICAM** — plain colour preview with brightness/contrast

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

The **menu** lists the current mode's settings plus Sleep (auto-sleep timeout), Gallery
and Exit:
- **GB Camera:** Palette, Dither, Style, Scale, Frame, Auto (auto brightness/contrast)
- **PixelBoy:** Palette, Method, Size, Amount, Auto (auto-exposure), Edge (sharpen)
- **Digicam:** Size

The **gallery** lists every photo from every mode, oldest first. Photos save to the SD
card's `/GBCAM`, one shared numbering sequence: GB Camera as `GBnnnnn.PNG`+`.BIN`,
PixelBoy/Digicam as `DCnnnnn.PNG`.

### GB Camera frames

A decorative border, like the real Game Boy Camera - the Frame quick-adjust target or
menu row, shown live at 1:1. Baked into the saved `.PNG` only.

Built-in frames are PNGs in `assets/frames/` (160×144 or 160×224) - edit and rebuild with
`python tools\gen_frames.py`. Or drop `.png` files onto the SD card's `/FRAMES` folder
(no rebuild needed), or `.gb`/`.gbc`/`.zip` ROMs onto `/ROMS` to extract every frame a
commercial cartridge or [gb-photo](https://github.com/untoxa/gb-photo) ROM has. These
ROM/frame-pack files are copyrighted - never commit one (see `.gitignore`).

### Custom palettes

Drop `.hex` files onto the SD card's `/PALETTES` folder (loaded once at boot) - one RGB
colour per line, 6 hex digits (`#` prefix optional), blank/`#`-comment lines ignored. A
file with exactly 4 colours shows up as a GB Camera palette; 2-64 colours, as a Dither Cam
one (so a 4-colour file appears in both). Same format `tools\gen_palettes.py` reads.

## USB

Plugging the "USB" port (not "Debug") into a PC prompts on-device:
- **Drive** — SD card as a mass-storage drive
- **Mirror** — streams the device's own screen to the PC as a webcam
- **GB Webcam** — streams just the GB Camera photo at 3x (with its frame, if on)

Shutter cancels. Both webcam modes show up as a UVC webcam named "PIXELBOY".

## WiFi Gallery

Menu → WIFI GALLERY brings the onboard ESP32-C6 up as an access point (SSID
"PixelBoy Gallery", password "PIXELBOY1") and serves a photo gallery at
`http://192.168.4.1/` - browse/download from a phone, no cable. Shutter closes
it and powers the C6 back down.

## Build and flash

### First flash (new/fresh device)

A brand new board's onboard ESP32-C6 doesn't have ESP-Hosted's co-processor
firmware on it yet - without it the P4 side still builds and runs fine, WIFI
GALLERY in the menu just fails ("WIFI FAILED"). Do both chips in one go:

1. Build the C6's co-processor firmware - a separate ESP-IDF project, own
   chip target, not part of the P4 build:
   ```powershell
   . C:\Espressif\esp-idf\export.ps1
   cd tools\c6_coprocessor
   idf.py set-target esp32c6
   idf.py build
   ```
   Output: `tools\c6_coprocessor\build\eh_cp_transport_sdcard.bin`.
2. Copy that file into `firmware\c6fw_image\c6_fw.bin` (create the folder if
   it's not there - it's gitignored, so nothing this big ends up committed).
3. Build and flash the P4 - one command does both chips:
   ```powershell
   cd ..\..\firmware
   idf.py -p COM5 build flash monitor    # "Debug" USB-C port; check the COM number in Device Manager
   ```
   The `c6fw` staging partition (holding the file from step 2) gets written
   alongside the bootloader/partition table/app in that same flash, and
   `main.c` calls `wifi_gallery_check_c6_update()` on every boot: finding
   `c6_fw.bin` staged, it powers the C6 up, streams the firmware over SDIO,
   activates it, and lets ESP-Hosted force a clean host restart to boot into
   it (expected - a brief screen blank, not a crash). Watch the on-screen
   status ("CHECKING C6...", "FLASHING C6... DO NOT DISCONNECT", then "C6
   FLASHED OK") or the `wifi_gallery` serial log tag.
4. **Delete `firmware\c6fw_image\c6_fw.bin` once it's confirmed working** -
   see "Updating the C6 firmware later" below for why this matters.

`Ctrl+]` leaves the monitor. If flashing can't connect: hold Boot, press Reset, release
Boot, then flash again.

### Normal updates (P4 firmware changes)

Once the device is set up, day-to-day changes to this project's own firmware
(app.c, gbcam, etc) don't touch the C6 at all - just:

```powershell
. C:\Espressif\esp-idf\export.ps1
cd firmware
idf.py -p COM5 build flash monitor
```

With no `c6_fw.bin` staged, `wifi_gallery_check_c6_update()` is a near-zero-cost
no-op (it only reads the P4's own local flash to check) and boot stays fast -
the C6 keeps whatever firmware it already has.

### Updating the C6 firmware later

Same steps as "First flash" above (build `tools\c6_coprocessor`, copy the
`.bin` into `firmware\c6fw_image\c6_fw.bin`, reflash the P4) - it detects and
re-flashes the same way. Afterward, **delete
`firmware\c6fw_image\c6_fw.bin`** - the device erases its own on-flash copy
after a successful push, but that doesn't touch this source file, and leaving
it in place means the *next* `idf.py flash` (even an unrelated P4-only
change) re-stages it, so every boot after that reflashes the C6 again
(confirmed on real hardware: this is exactly why boot suddenly got slower and
kept showing a C6 status after an otherwise-unrelated reflash). Deleting the
file makes `idf.py flash` write an empty `c6fw` partition instead, and the
boot-time check goes back to normal.

### How the C6 flashing actually works

The C6 has no exposed UART/USB of its own - its only external link is the
SDIO bus it already shares with the P4, so its firmware is flashed *through*
the P4's own USB port, over that same SDIO link via ESP-Hosted's own OTA API
(same idea as
[lboshuizen/crowpanel-p4-c6-sdio-ota](https://github.com/lboshuizen/crowpanel-p4-c6-sdio-ota)).
This project's version of that lives in `app_wifi_gallery.c`'s
`flash_coprocessor()`/`wifi_gallery_diag()`/`wifi_gallery_check_c6_update()`;
the firmware image itself travels as its own dedicated flash partition
(`c6fw` in `partitions.csv`, built from `firmware/c6fw_image/` by the
top-level `CMakeLists.txt`) rather than a file dropped on the SD card by hand.

Confirmed working end to end on real hardware: partition flashed
automatically, firmware streamed to the C6 byte-for-byte, activated, and the
C6 rebooted into it. One gotcha found along the way: mounting the `c6fw`
partition needs a free slot in the VFS registration table (`CONFIG_VFS_MAX_COUNT`,
default 8) - this project's SD card + USB-MSC + console mounts already used
all of them, so the mount failed with a misleading `ESP_ERR_NO_MEM` (not an
actual memory shortage). `sdkconfig.defaults` raises it to 10.

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
components/stb     PNG writer
firmware/          ESP-IDF project
tools/host         PC test tools (photo files, live webcam preview)
```

Third-party code: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
