# Game Boy Camera device: research archive

> Research notes collected before the ESP32-P4X-EYE was chosen. The current plan is in [PLAN.md](PLAN.md).

---

## 0. Direction update (2026-09-24): native camera app, no emulator

The goal is now a **dedicated camera**, not the full Game Boy Camera software:
- Boot straight into the viewfinder.
- **Shutter button:** take a photo.
- **Encoder:** adjust brightness or contrast. A **mode button** switches between the two.
- **Gallery button:** open or close the gallery. **Encoder** scrolls through photos.

**That makes the Game Boy emulator unnecessary.** Scripting the original ROM's menus with button macros would be fragile. Instead, write a small native app that reproduces the Game Boy Camera *look* directly. The image pipeline in §2.3 is the core of it. Raphael Boichot's DashBoy Camera (§3) already does this on an RP2040.

What goes away:
- Peanut-GB
- the camera mapper
- the ROM legal issue
- the rule that the display must fit a 160×144 Game Boy screen

What's left:
- camera capture
- the image pipeline
- a small user interface
- saving photos to SD

What you give up: the original software's extras (games, stamps, frames, the album interface). An "emulator mode" could be added back later.

**Camera behaviour to match the original:**
- **Brightness** controls exposure: sensor exposure/gain, or digital gain.
- **Contrast** selects between dither threshold sets, the way the original camera does. Raphael's repos and gb-photo document the real camera's matrices and contrast levels.
- Output is 128×112, 4 shades, with Bayer dithering.

**Saving:**
- a 128×112 PNG, plus an enlarged copy for sharing
- optionally, Game Boy Camera-format tiles or save files, for compatibility with existing tools (see Raphael's "Inject pictures" repo)

**Display implication (ESP32-P4X-EYE, 240×240):**
- 2× of 128×112 is 256×224, which is **16 px too wide**.
- Options:
  1. 2× viewfinder cropped 8 px on each side (the saved photo stays full 128×112).
  2. 1× (sharp but tiny).
  3. Change the output format to 120×120, which scales to exactly 240×240, at the cost of the authentic 128×112.
- A larger screen (e.g. MSP3525 at 2× or 3×) avoids the problem.

### 0.1 Reusing gb-photo code (MIT licence; keep the copyright notice)

[untoxa/gb-photo](https://github.com/untoxa/gb-photo) is C for the Game Boy's own CPU (via GBDK-2020). Its **algorithms and tables port directly**. Its rendering and user interface draw with Game Boy tiles and video RAM, so use those as reference only.

| File | What to take |
|---|---|
| `src/dither_patterns.c` | **Take as-is.** 16 contrast levels × 2 tables (`dither_high_light_values`, `dither_low_light_values`). 10 dither patterns: Default (the original 4×4 Bayer), 2x2, Grid, Maze, Nest, Fuzz, Vertical, Horizontal, Diagonal, Mix. `dither_pattern_apply()` builds the 48-byte threshold matrix. Make it write to our own array instead of `CAM_DITHERPATTERN`. |
| `src/state_camera.c` | **Auto-exposure loop.** Histogram error against the brightness target, with step sizes chosen by thresholds 5/10/20/95, "like the real camera". **Brightness is the auto-exposure target**, not raw exposure. Also `RENDER_REGS_FROM_EXPOSURE()`: gain, edge mode and vRef switch in tiers as exposure changes (no edge enhancement in the dark, 2-D in mid light, horizontal in bright light). Emulate those tiers in software, because they're part of the look. |
| `src/histogram.c` | Histogram and metering areas (full frame, top, right, bottom, left) |
| `include/gbcamera.h` | **Real `.sav` layout:** 30 image slots, "Magic" checksums (seeds `0x2F`/`0x15`), per-image metadata, 16×14-tile images and 4×4-tile thumbnails. Use it to export saves that emulators, flash carts and existing tools can read. |
| `src/gbprinter.c` | Game Boy Printer protocol. Could print to a **real Game Boy Printer** over a link-port connector, or feed printer-emulator tools. |
| Auto-exposure bracketing code in `state_camera.c` | Exposure-bracketing mode, if wanted later |
| `doc/` | M64282FP and M64283FP datasheets, `exposure_time.xlsx` |
| `assets/` | Fonts, frames, cursors and sounds. **Check each asset's credits before reusing**; e.g. the sounds credit Tronimal/FX Hammer. |

**Calibration note:** the MAC-GBD compares the sensor's *voltage* against the thresholds, which is why the threshold values sit in roughly the `0x80–0xFF` range. A modern sensor's 0–255 brightness values must be mapped into that range, or the thresholds rescaled. That mapping is the main tuning step for matching the look.

The platform choice is now open again. The P4X-EYE fits this simpler app well: battery, SD, buttons and encoder are all built in.

---

## 1. Summary of decisions

| Topic | Decision | Why (details in the sections below) |
|---|---|---|
| Form factor | **Standalone handheld**, not a cartridge | No Game Boy bus timing, no pin shortage, and saves are safe because the device controls its own power. See §4. |
| Image sensor (final) | **Himax HM0360, monochrome** | Low power, greyscale output, and drivers exist. The original Mitsubishi M64282FP is hard to get. See §5. |
| Image sensor (prototype) | OV7725 or OV2640 in greyscale mode | Plugs straight into the dev board's camera connector. The image pipeline is the same for any sensor. |
| MCU | **STM32H743** (Cortex-M7, 480 MHz) | Game Boy emulation and HM0360 capture have both been done on the STM32H7. It has a hardware camera interface and a hardware display bus. See §6. |
| Dev board | **WeAct MiniSTM32H743** (~$14–20) | Has a camera connector, microSD, 2MB internal flash and 1MB RAM. See §7. |
| Emulator | **Peanut-GB** (MIT) | Single C header, and it already runs at full speed on much slower chips. See §8. |
| Camera software | **gb-photo** (open-source homebrew) as the default. The original ROM also works if the user loads a dump of their own cart. | gb-photo can legally be shipped with the device. |
| Display | **3.5" 480×320 ST7796S IPS, 8-bit 8080 parallel** | Exact 2× integer scaling, and the picture is almost the same physical size as the original DMG screen. See §9. |
| GBC-shell build | Possible later using an aftermarket GBC IPS kit | Needs a signal-compatibility test first. See §9.4. |

---

## 2. How the Game Boy Camera works

The Game Boy Camera cartridge contains:
- a **1MB ROM** (64 × 16KB banks)
- **128KB of battery-backed save RAM** (16 × 8KB banks)
- the **MAC-GBD** mapper chip, which handles bank switching, the camera registers and the analog-to-digital conversion
- the **Mitsubishi M64282FP** sensor (128×128 analog "artificial retina"), sitting in the "eye" on a 9-pin ribbon cable

### 2.1 Mapper (cartridge type `0xFC`)

Check every detail below against Pan Docs before implementing.

| Address | Function |
|---|---|
| `0000–1FFF` | RAM enable (`0x0A`) |
| `2000–3FFF` | ROM bank select (6 bits, 0–63) |
| `4000–5FFF` | RAM bank select (0–15). **Bit 4 set (`0x10`) maps the camera registers into `A000–BFFF`**, mirrored. |
| `A000` (register mode) | Capture control. Bit 0 = start capture / busy. The game polls this until it clears. |
| `A001` | Sensor settings: N and VH edge-mode bits, gain |
| `A002–A003` | Exposure time (16-bit) |
| `A004` | Edge ratio, invert, output reference voltage |
| `A005` | Zero point / output reference |
| `A006–A035` | Dither matrix: 4×4 positions × 3 thresholds = 48 bytes |

Notes:
- Only `A000` is readable in register mode; the other registers read back as `0x00`.
- The captured picture is written into **RAM bank 0 at `A100–AEFF`** as 128×112 pixels at 2 bits per pixel: 16×14 tiles × 16 bytes = 3,584 bytes.
- Capture time depends on the exposure setting. Pan Docs gives a cycle-count formula. The ROM simply polls the busy bit, so timing is forgiving, but matching the real duration makes the viewfinder frame rate feel right.

### 2.2 The ROM's auto-exposure loop

The camera ROM (and gb-photo) run their own auto-exposure: they look at each captured picture, then rewrite the exposure register. **An emulated sensor must get brighter and darker in proportion to that exposure value, or the loop will keep pushing until the picture is fully black or white.**

Planned approach: leave the real sensor's auto-exposure on, and treat the ROM's exposure value as a relative gain around a middle value. Raphael Boichot's logs from a real camera show the values the ROM expects to settle on.

### 2.3 Image pipeline (capture to tiles)

1. Take the latest greyscale frame from the sensor, which streams continuously into a double buffer.
2. Crop and scale to 128×112 by averaging several sensor pixels per output pixel, which also reduces noise.
3. Apply the exposure (§2.2), then gain and offset (`A001`, `A004`, `A005`).
4. Apply edge enhancement according to the N/VH/E bits.
5. Dither: compare each pixel against the 3 thresholds for its position in the 4×4 matrix (`A006–A035`), giving a 2-bit value.
6. Pack the result into Game Boy tile format at bank 0 `A100`, then clear the busy bit.

Reference code for this processing:
- **SameBoy** (MIT) implements Game Boy Camera emulation from a webcam.
- **mGBA** (MPL-2.0) has webcam camera support.
- **GiiBiiAdvance** by AntonioND. His documentation is the basis for the Pan Docs camera section.
- **Raphael Boichot's DashBoy Camera** reproduces the real camera's register handling, auto-exposure and dithering on an RP2040 (GPL-3).

---

## 3. Background resources (Raphael Boichot)

| Repo | Relevance |
|---|---|
| [Mitsubishi-M64282FP-dashcam](https://github.com/Raphael-Boichot/Mitsubishi-M64282FP-dashcam) | RP2040 driving the real M64282FP. Reads VOUT with the 12-bit ADC, reproduces the real camera's register handling and auto-exposure, and uses the same dithering. Also has an HDR mode. Runs at about 4 fps in raw mode. GPL-3. |
| [Play-with-the-Game-Boy-Camera-Mitsubishi-M64282FP-sensor](https://github.com/Raphael-Boichot/Play-with-the-Game-Boy-Camera-Mitsubishi-M64282FP-sensor) | Arduino prototype; sensor register experiments |
| [Play-with-the-Mitsubishi-M64283FP-sensor](https://github.com/Raphael-Boichot/Play-with-the-Mitsubishi-M64283FP-sensor) | Sister sensor (cropping, projections) |
| [Game-Boy-chips-decapping-project](https://github.com/Raphael-Boichot/Game-Boy-chips-decapping-project) | Die analysis of the sensor and MAC-GBD |
| [Inject-pictures-in-your-Game-Boy-Camera-saves](https://github.com/Raphael-Boichot/Inject-pictures-in-your-Game-Boy-Camera-saves) | Save-RAM layout and checksums |
| [Game-Boy-camera-sniffer](https://github.com/Raphael-Boichot/Game-Boy-camera-sniffer) | Logging a real camera's behaviour (MAX153 ADC) |
| [gb-photo](https://github.com/untoxa/gb-photo) (untoxa; Raphael contributed) | **Open-source replacement for the Game Boy Camera software**, targeting the MAC-GBD mapper. Can be shipped with the device. |

The M64282FP itself is out of production, and the only supply is donor Game Boy Cameras. That's why this project uses a modern sensor (§5).

---

## 4. Approaches explored

### 4.1 Croco Cartridge V1 (RP2040) plus a camera

- Hardware: [shilga/rp-gameboy-cartridge-hw](https://github.com/shilga/rp-gameboy-cartridge-hw) (CERN-OHL-P). Firmware: [rp2040-gameboy-cartridge-firmware](https://github.com/shilga/rp2040-gameboy-cartridge-firmware) (GPL-3, C).
- Pin map: GPIO0 CLK, 1 /RD, 2–17 A0–A15, 18–25 D0–D7, 26 GB reset, 27 WS2812 LED, **28 debug UART (the only spare pin; it's also ADC2)**, 29 bus enable.
- All 8 PIO state machines are in use. Core 1 is idle. 128KB of save RAM is already supported (`GB_MAX_RAM_BANKS 16`). Mappers supported: none, MBC1, MBC2, MBC3 and MBC5.
- **Verdict:** there are no pins for a sensor. The only workable version is a companion MCU on a camera daughterboard, talking over a single-wire UART on GPIO28.

### 4.2 Croco Cartridge V2 (RP2350B)

- Hardware: `KiCad/V2/GameboyCartridgeV2.1` in the same repo, also sold assembled on Tindie. Firmware: [rp2350-gameboy-cartridge-firmware](https://github.com/shilga/rp2350-gameboy-cartridge-firmware) (GPL-3, Rust/Embassy, runs at 266 MHz).
- Main parts from the BOM:
  - RP2350B (QFN-80)
  - W956A8 HyperRAM: ROMs are loaded here from SD and served by DMA
  - 2MB W25Q16 flash
  - microSD slot
  - MCP79510 RTC with coin cell
  - TXB0108 level shifters
  - USB-C and a WS2812 LED
- Pin map:
  - 0–3: SD card (SPI0)
  - 4: button
  - 5: RTC chip select
  - 6–16: HyperRAM (PIO2)
  - 17: CLK
  - 18: /RD
  - 20–35: A0–A15
  - 36–43: D0–D7
  - 44: bus enable
  - 45: GB reset
  - 46: debug UART
  - 47: WS2812 LED
- Mappers supported: none, MBC1, MBC3, MBC5 and HuC3. **HuC3's `enable_huc3_io_access(read_ptr, write_ptr)` already maps I/O registers into the `A000` window, which is the same approach the camera mapper needs.**
- For a camera-only cart, removing the SD slot, RTC and button frees GPIO0–5. The HM0360 in 1-bit mode (D0, PCLK, HSYNC, VSYNC, SDA, SCL, using its internal oscillator) would fit exactly on those pins.
- Open risks:
  - PIO2 has only about 4 instructions left after the HyperRAM program.
  - HM0360 1-bit output mode needs confirming in the datasheet.
  - Need to check that flash writes don't stall the bus.
- **Verdict:** feasible, and the best cart option. Set aside in favour of the standalone device, but worth coming back to. The project Discord is linked in the V2 firmware README.

### 4.3 Standalone emulated device (chosen)

Emulate the whole Game Boy plus the camera cart on one MCU with a screen.

Advantages:
- no bus timing constraints and no pin shortage
- safe saves, because the device controls power-off
- direct PNG export, and Game Boy Printer emulation through Peanut-GB's serial-port hooks
- can ship with gb-photo

---

## 5. Image sensor research

| Sensor | Pros | Cons | Verdict |
|---|---|---|---|
| Mitsubishi M64282FP | Authentic look, and the dashcam code exists | Out of production; donor carts only | Not practical |
| **Himax HM0360** | Monochrome 656×496 array (VGA). Very low power. 1/4/8-bit output. Internal oscillator (OpenMV has an `OSC_ENABLE` option). Supported by **OpenMV (MIT)** and **esp32-camera** (PR #707). Used on the **Portenta Vision Shield Rev 2**. | Datasheet access; fewer hobby examples than the OV parts | **Final choice** |
| Himax HM01B0 | Monochrome 320×320, very low power, Arducam RP2040 drivers | Noisier and lower resolution | Fallback |
| Arducam Mega (SPI) | SPI only, with exposure handled on the module | Higher power, large board | Prototype only |
| OV2640 / OV7725 / OV7670 / GC0308 | Cheap; plugs straight into the WeAct camera connector | 8-bit parallel bus, higher power, colour sensors | **Prototype on the dev board** |
| MIPI modules (OV5647, IMX…) | High quality | Need a MIPI interface | Not suitable |

**The lens matters more than the sensor.** Choose a module with an M12 or replaceable lens so the field of view can be tuned to feel like the original. Most machine-learning modules ship with lenses that are too wide.

The pipeline only needs a greyscale frame, so moving from the prototype sensor to the HM0360 doesn't change the processing code.

---

## 6. MCU / platform research

| Platform | Emulator track record | Camera | Notes |
|---|---|---|---|
| **STM32H7** (H743/H750/H7B0) | The hacked Game & Watch (H7B0) runs Game Boy through retro-go. STM32Boy runs Peanut-GB on an STM32F429 at 180 MHz. | Hardware camera interface (DCMI); the HM0360 is proven on the H7 (OpenMV, Portenta) | 480 MHz M7. The H743 has 2MB flash and 1MB RAM. Has hardware display buses (FMC and LTDC). **Chosen.** |
| ESP32-S3 | retro-go runs Game Boy on ESP32 handhelds; RETRO-BOY-COLOR is an ESP32-S3 in a GBC case | LCD_CAM camera interface; esp32-camera supports the HM0360 | Wi-Fi/Bluetooth for photo sharing; higher power. **Alternative if wireless matters.** |
| RP2350 | Peanut-GB runs at full speed on the RP2040 (RP2040-GB) | No camera interface; needs PIO capture | Cheapest and lowest power, but the most custom low-level code |
| i.MX RT1062 (Teensy 4.1) | Possible | Camera interface (CSI) | Fewer examples |
| Pi Zero 2 W + Linux | mGBA supports the camera through a webcam | Pi camera or USB webcam | Fastest demo, but slow boot, high power and SD-corruption risk. Not for the final device. |

---

## 7. Dev board: WeAct MiniSTM32H743

- STM32H743VIT6 at 480 MHz, with 2MB flash and 1MB RAM on-chip.
- 8MB QSPI flash, 8MB SPI flash, microSD, USB-C.
- Onboard 0.96" ST7735 LCD, **160×80**, so it can't show a Game Boy screen. It's only useful for bring-up.
- 24-pin camera connector (DCMI) for OV2640, OV5640-AF, OV7670 and OV7725. About $14, or $16–20 bundled with a camera.
- 2×22-pin headers. Supported in Zephyr, and advertised as OpenMV-compatible.

### 7.0 All-in-one kits with a screen and camera included (researched 2026-09-24)

| Kit | Screen | Camera | Integer scale | Price | Notes |
|---|---|---|---|---|---|
| **STM32H747I-DISCO + B-CAMS-OMV** | 4" 800×480, MIPI DSI, touch | OV5640 (MB1379); the adapter also takes OpenMV and Waveshare camera modules | **3×** (480×432) | ~$100 + ~$40 (check) | **Same STM32H7 family as the plan**, so code carries over to an H743 board. Needs the separate camera bundle. |
| **ESP32-P4-Function-EV-Board** | 7" 1024×600, touch | 2MP, MIPI CSI | **4×** (640×576) | ~$55 (Espressif AliExpress store) | Best value. 400 MHz dual RISC-V, 32MB PSRAM. Too big to be a handheld. The retro-go ESP32-P4 port is experimental. Changes platform. |
| **ESP32-P4X-EYE** | 1.54" 240×240 ST7789VW, SPI | 2MP MIPI CSI (HDF2710-47), **manual-focus lens** | 1× only (1.5× is non-integer) | ~$33–35; DigiKey out of stock until 2026-11-30 | Camera-shaped. Battery connector with charging (**battery not included**; the guide lists it as optional: 1S LiPo, max 4×25×45 mm, i.e. a "402545" cell, with a 1.25 mm-pitch 2-pin connector; check polarity against the PCB markings), microSD, 3 user buttons plus a rotary encoder, fill light, ESP32-C6 Wi-Fi, 2×10 header, no speaker. Good cheap platform for pipeline work; could add an MSP3525 3.5" SPI screen through the header. |
| STM32N6570-DK | 5" LCD, touch | 5MP, MIPI CSI, included | 3× if 800×480 (check) | ~$224 | Cortex-M55 with hardware image processing. Overkill and expensive. |
| ESP32-S3-EYE | 1.3" 240×240 | OV2640 | 1× only | ~$45 | Small screen |
| M5Stack CoreS3 | 2" 320×240 IPS, touch | GC0308 | 1× only | — | No physical buttons |
| M5StickV | 1.14" 135×240 | OV7740 | Doesn't fit 144 rows | EOL | Discontinued |

None of these have Game Boy-style buttons, so wire buttons to the expansion header on any of them.

### 7.1 Pin usage (from Zephyr's `mini_stm32h743.dts`)

| Function | Pins |
|---|---|
| Camera interface | HSYNC PA4, PCLK PA6, VSYNC PB7, D0 PC6, D1 PC7, D2 PE0, D3 PE1, D4 PE4, D5 PD3, D6 PE5, D7 PE6 |
| Camera control | SDA PB9 / SCL PB8 (I2C1), PWDN PA7, XCLK PA8 (clock output MCO1) |
| microSD (SDMMC1) | D0–D3 PC8–PC11, CK PC12, CMD PD2, card detect PD4 |
| QSPI flash | CLK PB2, NCS PB6, IO0 PD11, IO1 PD12, IO2 PE2, IO3 PD13 |
| SPI flash (SPI1) | SCK PB3, MISO PB4, MOSI PD7, CS PD6 |
| Onboard LCD (SPI4) | SCK PE12, MOSI PE14, CS PE11, DC PE13 |
| LED / key | LED PE3, key K1 PC13 |
| USB | PA11 / PA12 |

---

## 8. Emulator: Peanut-GB

- [deltabeard/Peanut-GB](https://github.com/deltabeard/peanut-gb) is MIT-licensed, single-header C99, and supports the original Game Boy (DMG) only. The Game Boy Camera is a DMG cart, so that's fine.
- Supports MBC1, MBC2, MBC3 (with RTC) and MBC5. **It has no camera mapper**, so we'll add type `0xFC` to its cart-type check and its memory read/write handlers (§2.1).
- Runs at full speed on the RP2040. Listed ports include the RP2350 and PIC32MZ. STM32Boy uses it on an STM32F429 (180 MHz). The H743 is several times faster than any of those, so **it will run comfortably**. Measure headroom with the DWT cycle counter.
- Rendering is line by line through an `lcd_draw_line` callback. Effects that change mid-line won't show correctly, which shouldn't matter for the camera software.
- Sound is off by default (`ENABLE_SOUND`) and uses an external APU library (minigb_apu).
- Memory placement on the H743:
  - ROM (1MB) in internal flash, read through a pointer with the caches on
  - emulator state (`struct gb_s`, a few tens of KB) in DTCM
  - 128KB save RAM in AXI SRAM, saved to microSD

---

## 9. Display research

### 9.1 Requirement: integer scaling only

The screen content is 160×144, and **camera photos are dither patterns**. Non-integer scaling (e.g. 1.5×) makes pixel columns uneven, which shows up as shimmer and moiré in the dithering. So use only 1×, 2× or 3×.

### 9.2 Choice: 3.5" 480×320 ST7796S IPS over an 8-bit 8080 parallel bus

- 2× gives 320×288, which fits in 480×320. In portrait (320×480) it fills the width with 192 pixels left below for a status bar.
- The pixel pitch is about 0.154 mm, so the 2× picture is about **49×44 mm**. The DMG's visible screen is about 47×43 mm.
- ST7796S rather than ILI9488: 16-bit colour on any interface, plus a tearing-effect (TE) pin to sync updates.
- **Buy the IPS version.** Many cheap 3.5" modules are TN.
- Bandwidth: 2× at 60 fps is about 88 Mbit/s, too much for SPI. The STM32's external memory controller (FMC) in 8080 mode handles it easily. Even at 8 bits, around 10 Mpixels/s is achievable against roughly 5.5 Mpixels/s needed.

### 9.3 FMC 8-bit pin map on the WeAct board

| Display signal | Pin | Conflict and resolution |
|---|---|---|
| D0–D3 | PD14, PD15, PD0, PD1 | none |
| D4–D7 | PE7–PE10 | none |
| WR (NWE) | PD5 | none |
| RS / DC (A16) | PD11 | QSPI IO0: **don't use the QSPI flash**; hold NCS PB6 high |
| CS (NE1) | PD7 | SPI1 MOSI: don't use the SPI flash (hold CS PD6 high), or tie the display's CS low |
| RD | tie high | the display is write-only |
| RST, backlight PWM, TE | free GPIO | — |

A 16-bit bus would collide with the onboard LCD's pins (PE11–PE14), so stay with 8-bit. **To check:** that all these pins reach the 2×22 headers. The Zephyr file shows pin assignments, not what's on the headers, so check the WeAct schematic.

**Bring-up fallback:** any 2.0–2.8" 320×240 ST7789 or ILI9341 over SPI, at 1× (160×144 with a border).

**3.5" 480×320 ILI9488 SPI-only modules (e.g. MSP3520-style with resistive touch): workable but limited.** The size fits 2× perfectly, but over SPI the ILI9488 accepts only **18-bit colour (3 bytes per pixel)**, so a full 2× frame is about 276KB. At a realistic 40 MHz SPI that's about 18 fps for full-screen updates. It becomes usable if only changed lines are sent: most of the camera interface is static, and the 2× viewfinder area (256×224) can update at about 25–30 fps. Scrolling games would struggle. Two more things to check: the resistive touch layer slightly reduces contrast, and the ILI9488's SPI data-out pin often doesn't release the bus, so give it a SPI bus of its own. **If buying SPI, prefer an ST7796S 3.5" SPI module**: it takes 16-bit colour (2 bytes per pixel, one-third less data) and should reach roughly 40 fps full-frame at around 60 MHz, which needs checking.

**Not suitable: 1.8" 128×160 ST7735 modules.** In landscape they're 160×128, which is **16 rows short** of 160×144. You'd have to crop screen rows or scale down by a non-integer amount (bad for dithering). A 128×112 photo does fit at 1×, so one could still be used as a scratch display for testing the pipeline. The driver code is shared with the WeAct's onboard ST7735.

### 9.4 Game Boy Color screens

- **Original GBC screen:** not recommended. It's reflective with no backlight, needs bias voltages the GBC mainboard generates, and is poorly documented.
- **Aftermarket GBC IPS kits** (FunnyPlaying Retro Pixel Q5 2.0, Hispeedido V3, etc.):
  - 4× integer scaling with pixel-grid display modes, a laminated lens and a backlight.
  - They plug into the GBC's 40-pin 0.5 mm LCD ribbon cable.
  - Signals: 15-bit RGB (5/5/5), plus Sharp-style timing on DCK, SPL, SPS, CLS and LP. The GBA version of this interface is well documented (insideGadgets, squk/GBA-LCD); the GBC version only partially (gbdev forum).
  - Problems:
    - How much timing variation each kit tolerates isn't documented, and may differ between kit versions.
    - It needs about 20 pins that change in step with the pixel clock. On the H743 that means a timer triggering DMA writes to one GPIO port, and the WeAct board's ports are crowded.
    - It needs an adapter board with the 40-pin ribbon connector, plus a check of the kit's logic voltage and power needs.
- **Plan:** a later phase for a GBC-shell build on a custom board. **Test first:** capture a real GBC's LCD signals with a logic analyser, play them back from the H743 into a kit, and confirm the picture is stable before designing the board.

---

## 10. Firmware architecture (STM32H743)

- **Main loop:** Peanut-GB runs one frame. The `lcd_draw_line` callback converts each line to RGB565 at 2× into an 8080 DMA line buffer, synced to the display's TE pin.
- **Camera:** the camera interface plus DMA streams frames continuously into a double buffer in AXI SRAM. When the ROM writes the trigger to `A000`, the pipeline (§2.3) runs on the latest frame and writes the tiles into save-RAM bank 0.
- **Input:** 8 GPIO buttons. On boards with fewer controls (e.g. ESP32-P4X-EYE: 3 buttons plus a rotary encoder), **turn the physical inputs into the 8 Game Boy buttons in the emulator's input code rather than modifying ROMs**:
  - Encoder turn → Up/Down. Hold a modifier button + turn → Left/Right.
  - Buttons → A, B, Start. Select via a chord (A+B) or a long press.
  - Each encoder step becomes a press lasting about 2–3 frames, because games read the buttons once per frame and act on a press when they see it change.
  - If a custom layout is ever needed, gb-photo is open source (C, GBDK-2020). Patching the original Nintendo ROM would mean disassembling it and couldn't be distributed, so avoid it.
- **Audio (later):** minigb_apu → SAI/I²S → a MAX98357A amplifier.
- **Storage:** save RAM to microSD (FatFs) after writes stop and on power-off. Later: PNG export and Game Boy Printer emulation.

### 10.1 STM32H7 traps

1. **Data cache versus DMA.** After DMA writes a camera frame, the CPU may read stale cached data. Invalidate the cache for the frame buffer after each frame, or use the MPU to mark it non-cacheable. This is the most likely bug to cost a day.
2. **Memory regions.** RAM is split: 128KB DTCM, 512KB AXI SRAM, SRAM1–3 and SRAM4. **DMA can't reach DTCM.** Put frame and DMA buffers in AXI SRAM, and emulator state in DTCM.
3. Set up the linker script and MPU regions deliberately.

---

## 11. Difficulty and effort

| Part | Difficulty |
|---|---|
| Board setup (clocks, caches, MPU) | Low–medium |
| Peanut-GB port | Low |
| Camera mapper | Low–medium |
| Camera capture (OV7725 over the camera interface) | Medium |
| Display (FMC 8080, 2×, TE sync) | Medium |
| Image pipeline | Medium to build; **long tail of tuning** (look, auto-exposure loop) |
| Buttons / saves | Low |
| Audio | Low–medium |

Estimate at weekend-hobby pace: **3–6 weekends** to take photos in gb-photo on the dev board, if you're comfortable with STM32 and embedded C. Roughly double that if you're new to STM32. Tuning comes after.

---

## 12. Phased plan

Each phase removes one risk before the next.

**Phase 0: buy parts** (§13)

**Phase 1: emulator and display** (proves CPU speed and display bandwidth)
- Project skeleton: CubeMX-compatible CMake, 480 MHz clock, caches, MPU and memory regions.
- Start with an SPI 320×240 display at 1×, then the ST7796S over 8-bit FMC at 2× with TE sync.
- Peanut-GB running a homebrew ROM at full speed. Measure CPU headroom.

**Phase 2: camera mapper** (proves the mapper)
- Add type `0xFC` to Peanut-GB and serve a test-pattern "photo".
- Boot gb-photo (and the original ROM, from your own cart), take and save photos, and compare behaviour against SameBoy.

**Phase 3: live camera** (proves capture and the pipeline)
- OV7725 over the camera interface with DMA, handling the cache correctly.
- The §2.3 pipeline, with the exposure mapping from §2.2.

**Phase 4: completeness**
- Buttons, saves to microSD, audio, PNG export, printer emulation.

**Phase 5: HM0360**
- Connect an HM0360 module to the camera interface, using OpenMV's register setup tables. Retune the pipeline and choose the lens.

**Phase 6: custom hardware**
- STM32H743 board, HM0360 on a short flex cable or daughterboard, 3.5" ST7796S, LiPo, charger and soft power switch, enclosure.
- Optional GBC-shell variant (§9.4), after the kit signal test.

---

## 13. Shopping list (prototype)

- [ ] WeAct MiniSTM32H743 with the **OV7725** camera bundle (~$20)
- [ ] 3.5" 480×320 **ST7796S IPS** module with 8/16-bit parallel ("MCU 8080") mode
  - **Confirmed IPS ST7796S options (search by these exact part numbers):**
    - **LCDwiki MSP3525**: IPS, SPI, no touch. 14-pin 2.54 mm header plus 0.5 mm ribbon connector. 5V supply. MSP3526 is the capacitive-touch version. Best for the SPI prototype.
    - **LCDwiki MRB3512**: IPS, **16-bit 8080 parallel only** (no 8-bit mode), 34-pin header, 3.3/5V. Resistive (XPT2046) or capacitive (GT911) touch; choose capacitive. Full 60 fps on the WeAct board, using FMC 16-bit: D8–D12 on PE11–PE15 and D13–D15 on PD8–PD10. Those collide with the onboard 0.96" LCD's pins, so leave the onboard LCD unused or disconnected.
    - **Waveshare "3.5inch Capacitive Touch LCD"**: IPS, ST7796S, SPI, FT6336U touch. Well documented alternative to the MSP3526.
    - **DM-TFT35-431** (displaymodule.com, $39.90): IPS panel with a ribbon connector that supports SPI, 8/9/16/18-bit 8080 and RGB. **2.8V supply.** Candidate for the custom PCB, not the dev board.
  - Earlier candidate (~$19, "no touch" version): a blue adapter board with a 9-pin SPI header plus **"8080-8P" and "8080-16P" ribbon connectors**, and an interface-mode resistor table printed on the back. It supports SPI out of the box and 8-bit parallel by moving resistors. **It's a standard TFT panel (TN), not IPS.** Fine for the prototype. For the final device, look for an IPS ST7796S panel, since TN shifts shades and contrast with viewing angle.
- [ ] Cheap 2.0–2.8" 320×240 ST7789 SPI display for early bring-up (optional)
- [ ] 8 tactile buttons, breadboard and jumper wires
- [ ] MAX98357A I²S amplifier board and a small speaker (later)
- [ ] ST-Link or another SWD debugger
- [ ] microSD card
- [ ] Cheap logic analyser (useful for display debugging, and needed for the GBC kit test)
- [ ] Optional: HM0360 module (Arducam, or a Portenta Vision Shield Rev 2)
- [ ] Optional: ESP32-S3-EYE (~$45) to compare the ESP32 route

---

## 14. Open questions / to verify

- [ ] Every mapper and register detail in §2.1 against Pan Docs (ROM bank 0 handling, RAM write-protect during capture, register mirroring, capture-time formula).
- [ ] That the WeAct schematic brings all the §9.3 display pins out to the headers.
- [ ] That the chosen 3.5" module actually offers 8-bit 8080 mode and is IPS.
- [ ] HM0360 datasheet access, output modes and supply voltages.
- [ ] HM0360 module availability, and lens / field-of-view options.
- [ ] Rough power budget (the backlight will likely dominate), to size the battery.
- [ ] GBC IPS kit signal tolerance (only if doing the GBC-shell variant).

---

## 15. Licensing notes

- **Peanut-GB:** MIT. **OpenMV drivers:** MIT. **SameBoy:** MIT. **mGBA:** MPL-2.0.
- **Croco firmware (V1 and V2)** and **Raphael Boichot's repos:** GPL-3. Copying their code makes this project GPL-3.
- **Croco hardware:** CERN-OHL-P (permissive).
- **The Nintendo Game Boy Camera ROM can't be distributed.** Users must load a dump of their own cart. Ship gb-photo by default (check its licence).

---

## 16. References

**Game Boy Camera hardware and emulation**
- Pan Docs, Game Boy Camera section (gbdev.io/pandocs)
- AntonioND: Game Boy Camera documentation and GiiBiiAdvance
- [SameBoy](https://github.com/LIJI32/SameBoy), [mGBA](https://github.com/mgba-emu/mgba)
- Raphael Boichot's repos (§3), [gb-photo](https://github.com/untoxa/gb-photo)

**Croco Cartridge**
- [rp-gameboy-cartridge-hw](https://github.com/shilga/rp-gameboy-cartridge-hw)
- [rp2040-gameboy-cartridge-firmware](https://github.com/shilga/rp2040-gameboy-cartridge-firmware)
- [rp2350-gameboy-cartridge-firmware](https://github.com/shilga/rp2350-gameboy-cartridge-firmware)

**Sensors**
- [OpenMV HM0360 driver](https://github.com/openmv/openmv/blob/master/drivers/sensors/hm0360.c)
- [esp32-camera PR #707 (HM0360 / HM1055)](https://github.com/espressif/esp32-camera/pull/707)
- [Arduino Portenta Vision Shield](https://docs.arduino.cc/hardware/portenta-vision-shield), [HM01B0 or HM0360? (Arduino forum)](https://forum.arduino.cc/t/hm01b0-or-hm0360/1402798)

**Boards and platforms**
- [WeActStudio/MiniSTM32H7xx](https://github.com/WeActStudio/MiniSTM32H7xx)
- [CNX Software: WeAct STM32H743 review](https://www.cnx-software.com/2024/03/12/weact-stm32h743-arm-cortex-m7-board-ships-with-a-0-96-inch-lcd-and-a-choice-of-camera-sensors/)
- [Zephyr: MiniSTM32H743](https://docs.zephyrproject.org/latest/boards/weact/mini_stm32h743/doc/index.html)
- [ESP32-S3-EYE (Adafruit)](https://www.adafruit.com/product/5955), [retro-go](https://github.com/ducalex/retro-go), [RETRO-BOY-COLOR](https://github.com/206003723/RETRO-BOY-COLOR)
- [ST STM32H7B3I-DK](https://www.st.com/en/evaluation-tools/stm32h7b3i-dk.html)

**Emulator**
- [Peanut-GB](https://github.com/deltabeard/peanut-gb)
- [Hackaday: STM32Boy](https://hackaday.com/2024/10/08/running-game-boy-games-on-stm32-mcus-is-peanuts/)
- [mrkct/stm32-gameboy](https://github.com/mrkct/stm32-gameboy)

**Game Boy Color screens**
- [ConsoleMods: FunnyPlaying IPS](https://consolemods.org/wiki/Game_Boy:FunnyPlaying_IPS_Screen)
- [FunnyPlaying GBC Retro Pixel kit](https://funnyplaying.com/products/gbc-retro-pixel-ips-lcd-kit-1)
- [Hispeedido GBC IPS V3](https://handheldlegend.com/products/game-boy-color-ips-backlight-lcd)
- [gbdev forum: LCD ribbon pinout](https://gbdev.gg8.se/forums/viewtopic.php?pid=331)
- [insideGadgets: GBA LCD](https://www.insidegadgets.com/2019/08/08/playing-around-with-the-gba-lcd/), [squk/GBA-LCD](https://github.com/squk/GBA-LCD)
