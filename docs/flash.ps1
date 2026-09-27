# Flashes the pre-built firmware in this folder - no ESP-IDF/Zig needed, just:
#   pip install esptool
# Usage:
#   .\flash.ps1 -Port COM5
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Port)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

python -m esptool --chip esp32p4 -b 460800 --before default_reset --after hard_reset -p $Port `
    write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m `
    0x2000 "$here\firmware\bootloader.bin" `
    0x8000 "$here\firmware\partition-table.bin" `
    0x10000 "$here\firmware\gbcam_p4eye.bin"
