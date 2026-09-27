# GB Camera (ESP32-P4X-EYE)

A pocket camera on an Espressif ESP32-P4X-EYE with three modes (Bottom button cycles):
- **GB CAMERA** — 128×112, 4 shades, the original Game Boy Camera's dithering, contrast
  and (in HARDWARE style) auto-exposure
- **PIXELBOY** — PIXEL CAM's Dither Cam: any of 32 palettes, 6 dither/error-diffusion
  methods, and 7 output sizes (120×60 up to 240×240) — each one scales onto the screen by
  a clean 1x or 2x factor, so the preview is always pixel-identical to the saved photo
- **DIGICAM** — plain colour preview with brightness/contrast

## Controls

| Input | Viewfinder | Menu | Gallery |
|---|---|---|---|
| Encoder press (shutter) | Take photo | Activate row | Back |
| Turn encoder | Adjust the active setting (see Mode) | Move selection | Scroll photos |
| Mode button | Click: switch which setting the encoder adjusts | — | — |
| Hold Mode | GB Camera only: style, **PIXEL CAM** ↔ **HARDWARE** (default)| — | — |
| Menu button | Click: open the menu | Click: close | Click twice: delete |
| Hold Menu | Open the gallery | — | Back |
| Bottom button | Click: next camera mode | — | — |

The **gallery** lists every photo from every mode in one list, oldest first, regardless of
which mode took it.

**GB Camera** shows on-screen brightness/contrast bars like the real camera: a vertical
bar on the left (⊕ at the top) and a horizontal bar along the bottom (⊖ left, ⊕ right),
each with a knob; the active one has a brighter track, and the corner shows **B** or **C**.
**Dither Cam** and **Normal Cam** show a plain live preview instead, with brief on-screen
messages when a setting changes.

The **menu** (top button) lists the current mode's settings — palette, dither/method,
style, size, amount, and temporal noise reduction — plus **Gallery** and **Exit**. Turn
the encoder to move the selection, press it to change the row's value.

Photos are saved to the microSD card in `/GBCAM`, all sharing one numbering sequence so
the gallery lists them in the order they were taken:
- **GB Camera:** `GBnnnnn.PNG` (512×448, in the palette used, with the selected **Frame**
  baked in if one's set) and `GBnnnnn.BIN` (raw 2bpp tiles, always unframed — the gallery
  reads this back, not the PNG)
- **Dither Cam / Normal Cam:** `DCnnnnn.PNG` only (4× upscale) — the gallery decodes this
  PNG back to show it, since there's no separate tile form for full-colour photos

### GB Camera frames

Like the real Game Boy Camera, a decorative border can be added around the photo — it's
the fourth quick-adjust target (Mode button cycles Brightness/Contrast/Palette/**Frame**,
**NONE** by default), or the **Frame** row in the GB Camera menu. It shows live in the
viewfinder at **1:1** scale (not in the default 2x-cropped view), and flashes at 1:1 for a
couple of seconds whenever you change it so you can see the new choice without leaving 2x.
It's baked into the saved `.PNG` only — the gallery always shows the plain unframed photo.

Built-in frames are plain PNGs in `assets/frames/` (160×144 or 160×224, same idea as the
menu icons below) — paint your own, then bake them into the firmware:

```powershell
python tools\gen_frames.py
```

**Adding frames without rebuilding the firmware:** drop `.png` files (same 160×144/160×224
sizing) straight onto the SD card's `/FRAMES` folder — they're loaded once at boot and
appear after the built-in frames. The original cartridge's own frame packs
(`framegroup_xx.json` — copyrighted, never committed, see `.gitignore`) work too, dropped
onto `/FRAMES` as-is: the firmware decodes them (zlib + tile data) on the device, no PC
step needed. A pack with many frames (e.g. a full "International" or "Hello Kitty" set)
adds one selectable frame per entry it contains.

**Extracting frames straight from a ROM:** drop `.gb`/`.gbc` files, or `.zip` files
containing them, onto the SD card's `/ROMS` folder — every Standard/Wild frame the
commercial cartridge has (18 + 8, or the Hello Kitty release's own 25 + 6) is extracted on
the device at boot, and so is anything a [gb-photo](https://github.com/untoxa/gb-photo)
ROM's own frame table has that fits this pipeline's layout. Frames that turn out
pixel-identical to one already loaded (common across regions/revisions of the same ROM)
are skipped automatically. These ROMs are copyrighted — same rule as the frame packs,
never commit one.

## Tools (already installed on this PC)

- **ESP-IDF v5.5.5** in `C:\esp\esp-idf` (tools in `C:\esp\.espressif`)
- **Zig** (`winget install zig.zig`), used as the PC C compiler

## Build and flash the firmware

ESP-IDF can't build in a folder whose path contains spaces, so `tools\fw.ps1` copies
the project to `C:\esp\gbcam_build` and builds there. Always edit files here; the copy
is refreshed on every run.

```powershell
.\tools\fw.ps1 build
.\tools\fw.ps1 flash monitor -Port COM5   # use the "Debug" USB-C port; check the COM number in Device Manager
```

Press `Ctrl+]` to leave the monitor. If flashing can't connect: hold **Boot (⚙)**, press
**Reset (↻)**, release Boot, then flash again.

## Tune the look on the PC

```powershell
.\tools\host\build.ps1
cd samples
..\tools\host\gbcam_cli.exe myphoto.jpg                    # -> myphoto_gb.png, myphoto_x4.png
..\tools\host\gbcam_cli.exe myphoto.jpg --sweep contrast   # all 16 contrast levels
..\tools\host\gbcam_cli.exe myphoto.jpg --sweep dither -p dmg
..\tools\host\gbcam_cli.exe --test-pattern
```

### Live webcam preview

Runs your PC webcam through the same C image core as the camera (`gbcam.dll`, built by
`build.ps1`). Needs Python with `opencv-python` and `numpy`.

```powershell
python tools\host\gbcam_live.py            # --camera 1 for a second webcam, --scale 3 for a smaller window
```

Keys: **M** switches the arrow keys between brightness and contrast; **↑/↓** adjust it;
**P** palette; **D** dither; **E** edge mode; **R** edge ratio; **F** mirror;
**[ / ]** webcam exposure; **A** webcam auto-exposure on/off; **H** info bar;
**Space** saves to `samples/out`; **Q/Esc** quits. The webcam's own auto-exposure is
locked off while it runs.

### Styles and accuracy tests

Both styles use the same ROM-accurate threshold dither; they differ in how the image
is prepared first:
- **PIXEL CAM** (default): the GB mode from the PIXEL CAM project. It uses an automatic
  contrast/gamma curve (searched for the best spread across the 4 shades), and no edge
  enhancement. Cleaner, and uses all four shades.
- **HARDWARE:** a model of the real M64282FP sensor and MAC-GBD chip, with Game Boy Camera
  auto-exposure. Harsher, with the real camera's edge outlines.

Live viewer: **S** switches style.

```powershell
python tools\host\test_reference.py   # HARDWARE vs the Pan Docs reference + real register dumps
python tools\host\test_pixelcam.py    # PIXEL CAM style vs "PIXEL CAM/pixelboy/dither.py"
```

### Photo files

Options: `-b` brightness 0–16, `-c` contrast 0–15, `-d` dither, `-e auto|none|h|2d`,
`-r` edge ratio 0–7, `-g` fixed gain, `-p grey|dmg|pocket|sepia`, `-s` scale.

## Layout

```
assets/icons       menu row icons, 16x16 PNG (edit these, then run tools/gen_icons.py)
assets/frames      GB Camera border art, 160x144/160x224 PNG (see GB Camera frames above)
components/gbcam   portable image core (PC + firmware)
components/stb     PNG writer
firmware/          ESP-IDF project
tools/host         PC test tools (photo files, live webcam preview)
tools/fw.ps1       build/flash helper
samples/           your test photos (outputs go in samples/out)
```

## Menu icons

Each menu row's icon is a 16x16 PNG in `assets/icons/` (`palette.png`, `dither.png`,
`style.png`, `scale.png`, `method.png`, `size.png`, `amount.png`, `denoise.png`,
`gallery.png`, `exit.png`) - paint over them in any editor that keeps the size and
transparency, then bake them into the firmware:

```powershell
python tools\gen_icons.py
```

Third-party code: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
