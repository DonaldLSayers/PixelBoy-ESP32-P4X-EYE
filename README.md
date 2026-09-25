# GB Camera (ESP32-P4X-EYE)

A pocket camera on an Espressif ESP32-P4X-EYE with three modes (Bottom button cycles):
- **GB CAMERA** — 128×112, 4 shades, the original Game Boy Camera's dithering, contrast
  and (in HARDWARE style) auto-exposure
- **DITHER CAM** — PIXEL CAM's Dither Cam: any of 32 palettes, 6 dither/error-diffusion
  methods, and the same 6 output sizes as the Android/Pi app (32×24 up to 320×240) — sizes
  too big for the screen show a smaller same-aspect-ratio preview but still capture full-size
- **NORMAL CAM** — plain colour preview with brightness/contrast

Plan and status: [PLAN.md](PLAN.md).

## Controls

| Input | Viewfinder | Menu | Gallery |
|---|---|---|---|
| Encoder press (shutter) | Take photo | Activate row | Back |
| Turn encoder | Adjust the active setting (see Mode) | Move selection | Scroll photos |
| Mode button | Click: switch which setting the encoder adjusts | — | — |
| Hold Mode | GB Camera only: style, **PIXEL CAM** (default) ↔ **HARDWARE** | — | — |
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
- **GB Camera:** `GBnnnnn.PNG` (512×448, in the palette used) and `GBnnnnn.BIN` (raw
  2bpp tiles — the gallery reads this back, not the PNG)
- **Dither Cam / Normal Cam:** `DCnnnnn.PNG` only (4× upscale) — the gallery decodes this
  PNG back to show it, since there's no separate tile form for full-colour photos

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

## Simulator: try the camera UI on the PC

Runs the **real firmware app code** (`firmware/main/app.c`, screen drawing, storage) with
your webcam as the camera, pushed as a full-colour frame — the same format the
ESP32-P4X-EYE's own camera negotiates — so all three camera modes work in the simulator.
The window shows exactly what the 240×240 LCD would show.

```powershell
.\tools\sim\build.ps1                     # rebuild after changing firmware or gbcam code
python tools\sim\gbcam_sim.py             # --test-pattern (no webcam), --no-sd, --camera-error, --scale 2
```

| Key / mouse | Control |
|---|---|
| Space, Enter, E, or left-click the knob | Encoder press (shutter) |
| Arrows or mouse wheel | Turn the encoder |
| 1, or click MENU | Menu button (open/close the menu) |
| L, or right-click MENU | Menu long press (gallery) |
| M, 2 | Mode button |
| N, or right-click MODE | Mode long press (GB Camera only: switch style) |
| 3, C, or click CAM MODE | Next camera mode |
| F / O / Q | Mirror webcam / open the simulated SD card / quit |
| [ / ] | Webcam exposure −1 / +1 EV |
| A | Webcam auto-exposure on/off (for comparison) |

The webcam's own auto-exposure is **locked off**, so only the Game Boy Camera auto-exposure
in the firmware runs, as on the real camera. At startup the webcam meters once, then
exposure and gain stay fixed. Auto-exposure is handed back to the webcam when you quit.

The on-screen buttons are clickable too. Photos and settings go to `sim_data\`
(`sim_data\sdcard\GBCAM` stands in for the SD card).

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
**Space** saves to `samples/out`; **Q/Esc** quits. As in the simulator, the webcam's own
auto-exposure is locked off while it runs.

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
components/gbcam   portable image core (PC + firmware)
components/stb     PNG writer
firmware/          ESP-IDF project (app.c etc. are shared with the simulator)
tools/sim          PC simulator of the whole camera
tools/host         PC test tools (photo files, live webcam preview)
tools/fw.ps1       build/flash helper
samples/           your test photos (outputs go in samples/out)
```

Third-party code: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
