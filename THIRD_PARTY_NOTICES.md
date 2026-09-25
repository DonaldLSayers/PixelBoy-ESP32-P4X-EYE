# Third-party notices

| Code | Where | License |
|---|---|---|
| gb-photo (Toxa): contrast tables, dither patterns, auto-exposure logic | `components/gbcam/src/gbcam_dither.c`, `gbcam.c` | MIT, Copyright (c) 2022 Toxa. Full text in `gbcam_dither.c` |
| stb_image, stb_image_write (Sean Barrett) | `components/stb/include` | Public domain / MIT (dual, see headers) |
| Espressif ESP-IDF, esp32_p4_eye BSP, esp_video and dependencies | downloaded at build time | Apache-2.0 (see each component) |
| "Early GameBoy" font (LDEJRuff, via FontStruct) | `early-gameboy.ttf`, converted to `firmware/main/font8x8.h` by `tools/gen_font.py` | CC BY-SA 3.0, Copyright LDEJRuff 2012 - **share-alike**: derivatives (including this font8x8.h conversion) must stay under the same license. See http://creativecommons.org/licenses/by-sa/3.0/ |
| 5×7 font (fallback glyphs only: `#$%&+/=@\^_`, missing from the font above) | `firmware/main/font5x7.h` | Classic public-domain 5×7 glyph set |
