# Third-party notices

| Code | Where | License |
|---|---|---|
| gb-photo (Toxa): contrast tables, dither patterns, auto-exposure logic | `components/gbcam/src/gbcam_dither.c`, `gbcam.c` | MIT, Copyright (c) 2022 Toxa. Full text in `gbcam_dither.c` |
| stb_image, stb_image_write (Sean Barrett) | `components/stb/include` | Public domain / MIT (dual, see headers) |
| Espressif ESP-IDF, esp32_p4_eye BSP and most managed components (button, knob, led_indicator, esp_cam_sensor, ...) | downloaded at build time, `firmware/managed_components/` | Apache-2.0 (see each component's own `LICENSE`) |
| Espressif esp_video (camera driver) and esp_ipa (ISP image processing algorithms) | `firmware/managed_components/espressif__esp_video`, `espressif__esp_ipa` - the former used directly by `app_camera.c`, the latter a transitive dependency of the camera/ISP pipeline | "ESPRESSIF MIT License" - an MIT variant restricted to use *on Espressif Systems products* (satisfied here: this firmware only targets the ESP32-P4-EYE). Not the same as plain MIT - don't reuse either component off-Espressif-hardware without checking its own `LICENSE` file. Copyright (c) 2024 Espressif Systems |
| "Early GameBoy" font (LDEJRuff, via FontStruct) | `early-gameboy.ttf`, converted to `firmware/main/font8x8.h` by `tools/gen_font.py` | CC BY-SA 3.0, Copyright LDEJRuff 2012 - **share-alike**: derivatives (including this font8x8.h conversion) must stay under the same license. See http://creativecommons.org/licenses/by-sa/3.0/ |
| 5×7 font (fallback glyphs only: `#$%&+/=@\^_`, missing from the font above) | `firmware/main/font5x7.h` | Classic public-domain 5×7 glyph set |

## Colour palettes

Built-in palettes (`tools/gen_palettes.py`, source `.hex` files in `PIXEL CAM/xiao-pixelcam/sd_card/palettes/`) - almost all from [Lospec](https://lospec.com/palette-list), the pixel-art palette database; credited below to whoever Lospec itself credits (a named artist, or the hardware/software a palette is drawn from, where that's how Lospec lists it). Each is free to use/redistribute per Lospec's site-wide terms, but the individual artist's name should stay attached - hence this table.

### GB Camera palettes (`palettes/gb/`, always 4 colours)

| File | Palette | Credit | Source |
|---|---|---|---|
| `2bit_demichrome` | 2bit Demichrome | Space Sandwich | https://lospec.com/palette-list/2bit-demichrome |
| `autumn_chill` | Autumn Chill | Doph | https://lospec.com/palette-list/autumn-chill |
| `bw` | - | **Not from Lospec** - a plain black/white pair (each duplicated to fill 4 slots); looks hand-made for this project. Confirm and credit yourself if so. | - |
| `coldfire_gb` | Coldfire GB | Kerrie Lake | https://lospec.com/palette-list/coldfire-gb |
| `crimson` | Crimson | WildLeoKnight | https://lospec.com/palette-list/crimson |
| `gb_green` | Nintendo Gameboy (bgb) | bgb emulator's default palette (Lospec credits no individual submitter) | https://lospec.com/palette-list/nintendo-gameboy-bgb |
| `grayscale` | - | **Not from Lospec** - an even mathematical 4-step ramp (0/85/170/255), not a specific named palette. Likely generated for this project as the default. | - |
| `hollow` | Hollow | Poltergasm | https://lospec.com/palette-list/hollow |
| `ice_cream_gb` | Ice Cream GB | Kerrie Lake | https://lospec.com/palette-list/ice-cream-gb |
| `kirokaze_gb` | Kirokaze Gameboy | Kirokaze | https://lospec.com/palette-list/kirokaze-gameboy |
| `links_awakening_sgb` | Link's Awakening (SGB) | Drawn from the Super Game Boy palette used by *The Legend of Zelda: Link's Awakening DX*; Lospec credits no individual submitter | https://lospec.com/palette-list/links-awakening-sgb |
| `mist_gb` | Mist GB | Kerrie Lake | https://lospec.com/palette-list/mist-gb |
| `moonlight_gb` | Moonlight GB | Tofu | https://lospec.com/palette-list/moonlight-gb |
| `nostalgia` | Nostalgia | WildLeoKnight | https://lospec.com/palette-list/nostalgia |
| `rustic_gb` | Rustic GB | Kerrie Lake | https://lospec.com/palette-list/rustic-gb |
| `sepia` | - | **Not from Lospec** - no matching palette found (searched by exact colour). Likely hand-made for this project. Confirm and credit yourself if so. | - |
| `spacehaze` | SpaceHaze | WildLeoKnight | https://lospec.com/palette-list/spacehaze |
| `wish_gb` | Wish GB | Kerrie Lake | https://lospec.com/palette-list/wish-gb |

### Dither Cam palettes (`palettes/dithercam/`, 2-64 colours)

`2bit_demichrome` and `crimson` are shared with the GB Camera list above (same file, same credit).

| File | Palette | Credit | Source |
|---|---|---|---|
| `8ancient` | - | **Not from Lospec** - no matching palette found. Likely hand-made for this project. Confirm and credit yourself if so. | - |
| `8autumn` | - | **Not from Lospec** - no matching palette found. Likely hand-made for this project. Confirm and credit yourself if so. | - |
| `apollo` | Apollo | AdamCYounis | https://lospec.com/palette-list/apollo |
| `berry_nebula` | Berry Nebula | LostInIndigo | https://lospec.com/palette-list/berry-nebula |
| `blessing` | Blessing | Maruki | https://lospec.com/palette-list/blessing |
| `c64` | Commodore 64 | The Commodore 64 computer's hardware palette; Lospec credits no individual submitter | https://lospec.com/palette-list/commodore64 |
| `cmyk` | CMYK | Soeryo Nugroho ("Brotho") | https://lospec.com/palette-list/cmyk |
| `curiosities` | Curiosities | sukinapan | https://lospec.com/palette-list/curiosities |
| `db32` | DawnBringer 32 | DawnBringer | https://lospec.com/palette-list/dawnbringer-32 |
| `endesga_32` | Endesga 32 | ENDESGA | https://lospec.com/palette-list/endesga-32 |
| `gb` | Nintendo Internal | Submitted by Daniel Smith, based on a palette Nintendo used internally for Game Boy screenshots, shared publicly by Kate Willaert | https://lospec.com/palette-list/nintendo-internal |
| `golden_days` | Golden Days | Chicknhawk | https://lospec.com/palette-list/golden-days |
| `harvest_dusk` | - | **Not from Lospec** - no matching palette found. Likely hand-made for this project. Confirm and credit yourself if so. | - |
| `hope_diamond` | Hope Diamond | patchouli | https://lospec.com/palette-list/hope-diamond |
| `ice_cream` | - | **Not from Lospec** - no exact-name/colour match found (Lospec has other, differently-coloured "ice cream"-themed palettes, e.g. Ice Cream 16, Ice Cream Shoppe). Likely hand-made for this project. Confirm and credit yourself if so. | - |
| `journey` | Journey | PineappleOnPizza | https://lospec.com/palette-list/journey |
| `lost_century` | Lost Century | SurrealEmber | https://lospec.com/palette-list/lost-century |
| `memory_block_36` | memory block 36 | Vsigos | https://lospec.com/palette-list/memory-block-36 |
| `midnight_ablaze` | Midnight Ablaze | Inkpendude | https://lospec.com/palette-list/midnight-ablaze |
| `night_16` | Night 16 | Night (3rd place, PixelJoint 16-colour palette competition, 2015) | https://lospec.com/palette-list/night-16 |
| `oil6` | Oil 6 | GrafxKid | https://lospec.com/palette-list/oil-6 |
| `paperback_2` | Paperback-2 | Doph | https://lospec.com/palette-list/paperback-2 |
| `pastel_qt` | pastel qt | polyphrog | https://lospec.com/palette-list/pastel-qt |
| `pico8` | PICO-8 | Lexaloffle Games (built into the PICO-8 fantasy console) | https://lospec.com/palette-list/pico-8 |
| `pixelwave` | pixelwave | DogesArePros | https://lospec.com/palette-list/pixelwave |
| `purple_slime` | - | **Not from Lospec** - no matching palette found. Likely hand-made for this project. Confirm and credit yourself if so. | - |
| `resurrect_64` | Resurrect 64 | Kerrie Lake | https://lospec.com/palette-list/resurrect-64 |
| `root_beer` | - | **Not from Lospec** - no matching palette found (closest relative found, "horehound-4," is differently coloured and not a real match). Likely hand-made for this project. Confirm and credit yourself if so. | - |
| `sweetie16` | Sweetie 16 | GrafxKid | https://lospec.com/palette-list/sweetie-16 |
| `vinik24` | Vinik24 | Vinik | https://lospec.com/palette-list/vinik24 |
