# PixelBoy flash files

Built 2026-09-30 from `feature/battery-life` @ `ae41261` + deep-sleep resume and
state-save verification work (see `firmware/main/app.c`, `firmware/main/app_gbemu.c`).

Two ways to flash, same result:

- `flash-single.bin.bat COM23` — one merged image, `pixelboy-full-4MB-flash-at-0x0.bin`
  written at offset 0x0.
- `flash-separate-files.bat COM23` — the four images, each at its own offset.

Both use `python -m esptool --chip esp32p4`; a `.bat` takes the COM port as its
first argument and defaults to COM23. Board must not be held open by a monitor.

| File | Offset | What it is |
|------|--------|------------|
| `pixelboy-full-4MB-flash-at-0x0.bin` | 0x0 | Everything below, merged (5.6 MB) |
| `bootloader.bin` | 0x2000 | 2nd-stage bootloader |
| `partition-table.bin` | 0x8000 | Partition table |
| `gbcam_p4eye.bin` | 0x10000 | The app |
| `c6fw.bin` | 0x420000 | ESP32-C6 co-processor firmware (WiFi hosted) |

Flashing the app alone (`gbcam_p4eye.bin` at 0x10000) is enough for firmware-only
changes — the bootloader, partition table and C6 firmware are unchanged in this build.

After flashing: set SLEEP to 1 MIN and STANDBY to NEVER, launch a ROM, and let it
sleep. The sleep screen now prints `GAME SAVED` or `GAME NOT SAVED` under
`SLEEPING`; on wake the game should come back on the frame it was left on with a
`RESUMED` box on screen.
