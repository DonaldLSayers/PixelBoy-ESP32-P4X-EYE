@echo off
REM PixelBoy - flash the individual images (the same files the merged image is
REM built from, just written to their own offsets - use this one to reflash only
REM the app and leave the bootloader/partition table/C6 firmware alone).
REM Usage:  flash-separate-files.bat [COMx]      (default COM23)
setlocal
set PORT=%1
if "%PORT%"=="" set PORT=COM23
echo Flashing %PORT% from bootloader.bin / partition-table.bin / gbcam_p4eye.bin / c6fw.bin ...
python -m esptool --chip esp32p4 -p %PORT% -b 460800 --before default_reset --after hard_reset ^
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m ^
  0x2000 bootloader.bin ^
  0x8000 partition-table.bin ^
  0x10000 gbcam_p4eye.bin ^
  0x420000 c6fw.bin
pause
