# Third-party notices

| Code | Where | License | Source |
|---|---|---|---|
| gb-photo (Toxa): contrast tables, dither patterns, auto-exposure logic | `components/gbcam/src/gbcam_dither.c`, `gbcam.c` | MIT, Copyright (c) 2022 Toxa. Full text in `gbcam_dither.c` | https://github.com/untoxa/gb-photo |
| stb_image, stb_image_write (Sean Barrett) | `components/stb/include` | Public domain / MIT (dual, see headers) | https://github.com/nothings/stb |
| "Early GameBoy" font (LDEJRuff, via FontStruct) | `assets/fonts/early-gameboy.ttf`, converted to `firmware/main/font8x8.h` by `tools/gen_font.py` | CC BY-SA 3.0, Copyright LDEJRuff 2012 - **share-alike**: derivatives (including this font8x8.h conversion) must stay under the same license. See http://creativecommons.org/licenses/by-sa/3.0/ | not on GitHub - FontStruct (fontstruct.com) |
| Peanut-GB (Mahyar Koshkouei) - Game Boy emulator core, vendored as a single header | `components/peanut_gb/include/peanut_gb.h` | MIT, Copyright (c) 2018-2023 Mahyar Koshkouei. Full text at the top of the file. Also includes small marked portions from the SameBoy project (MIT, Copyright (c) 2015-2019 Lior Halphon) - see the file's own header comment | https://github.com/deltabeard/Peanut-GB |

## Acknowledgments

No code copied, but [Raphael-Boichot](https://github.com/Raphael-Boichot) - whose Game Boy
Camera/Printer reverse-engineering work informed some of the concepts here (M64282FP sensor
behaviour, MAC-GBD/GameBoy Printer protocol details) - deserves a thank-you.

Also no code copied, but the GB Camera AEB/HDR feature's "Average" combine step is named
after, and confirmed against the actual source of,
[gb-printer-web](https://github.com/HerrZatacke/gb-printer-web)'s own tool of the same name
(`average.js`) - a plain per-pixel mean, which is why this project's own version instead
averages already-dithered shades (reconstructing extra apparent grey levels from independent
dither patterns) rather than raw pre-dither exposure data.

## Colour palettes

Built-in palettes (`tools/gen_palettes.py`, source `.hex` files in `PIXEL CAM/xiao-pixelcam/sd_card/palettes/`) - almost all from [Lospec](https://lospec.com/palette-list), the pixel-art palette database; credited below to whoever Lospec itself credits (a named artist, or the hardware/software a palette is drawn from, where that's how Lospec lists it). Each is free to use/redistribute per Lospec's site-wide terms, but the individual artist's name should stay attached - hence this table.

### GB Camera palettes (`palettes/gb/`, always 4 colours)

| File | Palette | Credit | Source |
|---|---|---|---|
| `2bit_demichrome` | 2bit Demichrome | Space Sandwich | https://lospec.com/palette-list/2bit-demichrome |
| `autumn_chill` | Autumn Chill | Doph | https://lospec.com/palette-list/autumn-chill |
| `coldfire_gb` | Coldfire GB | Kerrie Lake | https://lospec.com/palette-list/coldfire-gb |
| `crimson` | Crimson | WildLeoKnight | https://lospec.com/palette-list/crimson |
| `gb_green` | Nintendo Gameboy (bgb) | bgb emulator's default palette (Lospec credits no individual submitter) | https://lospec.com/palette-list/nintendo-gameboy-bgb |
| `hollow` | Hollow | Poltergasm | https://lospec.com/palette-list/hollow |
| `ice_cream_gb` | Ice Cream GB | Kerrie Lake | https://lospec.com/palette-list/ice-cream-gb |
| `kirokaze_gb` | Kirokaze Gameboy | Kirokaze | https://lospec.com/palette-list/kirokaze-gameboy |
| `links_awakening_sgb` | Link's Awakening (SGB) | Drawn from the Super Game Boy palette used by *The Legend of Zelda: Link's Awakening DX* | https://lospec.com/palette-list/links-awakening-sgb |
| `mist_gb` | Mist GB | Kerrie Lake | https://lospec.com/palette-list/mist-gb |
| `moonlight_gb` | Moonlight GB | Tofu | https://lospec.com/palette-list/moonlight-gb |
| `nostalgia` | Nostalgia | WildLeoKnight | https://lospec.com/palette-list/nostalgia |
| `rustic_gb` | Rustic GB | Kerrie Lake | https://lospec.com/palette-list/rustic-gb |
| `spacehaze` | SpaceHaze | WildLeoKnight | https://lospec.com/palette-list/spacehaze |
| `wish_gb` | Wish GB | Kerrie Lake | https://lospec.com/palette-list/wish-gb |

### Dither Cam palettes (`palettes/dithercam/`, 2-64 colours)

`2bit_demichrome` and `crimson` are shared with the GB Camera list above (same file, same credit).

| File | Palette | Credit | Source |
|---|---|---|---|
| `apollo` | Apollo | AdamCYounis | https://lospec.com/palette-list/apollo |
| `berry_nebula` | Berry Nebula | LostInIndigo | https://lospec.com/palette-list/berry-nebula |
| `blessing` | Blessing | Maruki | https://lospec.com/palette-list/blessing |
| `c64` | Commodore 64 | The Commodore 64 computer's hardware palette | https://lospec.com/palette-list/commodore64 |
| `cmyk` | CMYK | Soeryo Nugroho ("Brotho") | https://lospec.com/palette-list/cmyk |
| `curiosities` | Curiosities | sukinapan | https://lospec.com/palette-list/curiosities |
| `db32` | DawnBringer 32 | DawnBringer | https://lospec.com/palette-list/dawnbringer-32 |
| `endesga_32` | Endesga 32 | ENDESGA | https://lospec.com/palette-list/endesga-32 |
| `gb` | Nintendo Internal | Submitted by Daniel Smith, based on a palette Nintendo used internally for Game Boy screenshots, shared publicly by Kate Willaert | https://lospec.com/palette-list/nintendo-internal |
| `golden_days` | Golden Days | Chicknhawk | https://lospec.com/palette-list/golden-days |
| `hope_diamond` | Hope Diamond | patchouli | https://lospec.com/palette-list/hope-diamond |
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
| `purple_slime` | Purple Slime | TechDweeb (YouTube) | https://www.youtube.com/techdweeb |
| `resurrect_64` | Resurrect 64 | Kerrie Lake | https://lospec.com/palette-list/resurrect-64 |
| `root_beer` | Root Beer | TechDweeb (YouTube) | https://www.youtube.com/techdweeb |
| `sweetie16` | Sweetie 16 | GrafxKid | https://lospec.com/palette-list/sweetie-16 |
| `vinik24` | Vinik24 | Vinik | https://lospec.com/palette-list/vinik24 |
