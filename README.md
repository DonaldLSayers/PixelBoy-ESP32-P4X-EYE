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

```powershell
. C:\Espressif\esp-idf\export.ps1
cd firmware
idf.py -p COM5 build flash monitor    # "Debug" USB-C port; check the COM number in Device Manager
```

`Ctrl+]` leaves the monitor. If flashing can't connect: hold Boot, press Reset, release
Boot, then flash again.

This flashes the P4 (bootloader, partition table, main app, plus the `c6fw`
staging partition below - all four are in one `flash_args`, one `idf.py
flash`). The onboard ESP32-C6 chip itself (WiFi Gallery's radio, see above) is
separate hardware with its own flash and isn't touched by this - its network
co-processor firmware only actually moves over once `wifi_gallery_diag()`
pushes it (next section), and stays put on the C6 across every ordinary P4
reflash after that.

### Flashing the C6 co-processor

The C6 has no exposed UART/USB of its own - its only external link is the
SDIO bus it already shares with the P4, so its firmware is flashed *through*
the P4's own USB port instead, over that same SDIO link via ESP-Hosted's own
OTA API (same idea as
[lboshuizen/crowpanel-p4-c6-sdio-ota](https://github.com/lboshuizen/crowpanel-p4-c6-sdio-ota)).
This project's version of that lives in `app_wifi_gallery.c`'s
`flash_coprocessor()`/`wifi_gallery_diag()`.

The C6 firmware image travels as its own dedicated flash partition (`c6fw` in
`partitions.csv`, built from whatever's in `firmware/c6fw_image/` - see the
top-level `CMakeLists.txt`) rather than a file dropped on the SD card by hand:

1. Build the co-processor firmware:
   ```powershell
   . C:\Espressif\esp-idf\export.ps1
   cd tools\c6_coprocessor
   idf.py set-target esp32c6
   idf.py build
   ```
   Output: `tools\c6_coprocessor\build\eh_cp_transport_sdcard.bin`.
2. Copy that file into `firmware\c6fw_image\c6_fw.bin` (create the folder if
   it's not there - it's gitignored, so nothing this big ends up committed).
3. Build/flash the P4 as usual (see above) - the `c6fw` partition gets
   written automatically alongside everything else, no extra step.
4. `wifi_gallery_diag()` (in `app_wifi_gallery.c`) is what actually pushes the
   C6 firmware from that partition over SDIO - it's not called from anywhere
   by default any more (it used to run on every boot, which powered the C6 up
   whether WiFi Gallery was used or not - see the power note above).
   Temporarily call it once from `main.c`'s `app_main()`, rebuild/reflash,
   then watch the `wifi_gallery` log tag (or the on-screen status - it shows
   "FLASHING C6...", then "C6 FLASHED OK" or "C6 WIFI FAILED"): it reports the
   C6's firmware version, flashes it if it's blank/mismatched, and confirms
   WiFi comes up. Remove the call again afterward. A successful flash also
   erases the `c6fw` partition on the P4 side automatically - nothing to
   clean up by hand.

Confirmed working end to end on real hardware: partition flashed
automatically, firmware streamed to the C6 byte-for-byte, activated, and the
C6 rebooted into it. One gotcha found along the way: mounting the `c6fw`
partition needs a free slot in the VFS registration table (`CONFIG_VFS_MAX_COUNT`,
default 8) - this project's SD card + USB-MSC + console mounts already used
all of them, so the mount failed with a misleading `ESP_ERR_NO_MEM` (not an
actual memory shortage). `sdkconfig.defaults` raises it to 10.

Only needed once, or if the co-processor firmware itself changes.

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
