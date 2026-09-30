@echo off
REM PixelBoy - flash the whole chip from one merged image.
REM Usage:  flash-single.bin.bat [COMx]      (default COM23)
setlocal
set PORT=%1
if "%PORT%"=="" set PORT=COM23
echo Flashing %PORT% from pixelboy-full-4MB-flash-at-0x0.bin ...
python -m esptool --chip esp32p4 -p %PORT% -b 460800 --before default_reset --after hard_reset ^
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m ^
  0x0 pixelboy-full-4MB-flash-at-0x0.bin
pause
