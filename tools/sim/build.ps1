# Builds the firmware simulator (tools\sim\gbcam_sim.dll) with zig's C compiler.
# It compiles the real app code from firmware\main with PC stand-ins for the hardware.
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\.."
$zig = (Get-Command zig -ErrorAction SilentlyContinue).Source
if (-not $zig) {
    $zig = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter zig.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $zig) { throw 'zig not found. Install with: winget install zig.zig' }

$fw = "$root\firmware\main"
# -mcpu=baseline pins codegen to a plain x86_64 target instead of whatever CPU
# happens to build this DLL - without it, zig cc defaults to the build
# machine's native CPU, and gbcam_sim.dll can hit an illegal instruction on
# any machine whose CPU is missing features (AVX2/FMA/etc.) the build box had.
& $zig cc -std=gnu99 -O2 -mcpu=baseline -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers -Wno-unused-parameter `
    -I "$PSScriptRoot\include" -I $fw -I "$root\components\gbcam\include" -I "$root\components\stb\include" `
    -shared `
    "$PSScriptRoot\sim.c" `
    "$fw\app.c" "$fw\app_display.c" "$fw\app_frames.c" "$fw\app_frames_sd.c" "$fw\app_palettes_sd.c" "$fw\app_storage.c" "$fw\app_settings_common.c" `
    "$root\components\gbcam\src\gbcam.c" "$root\components\gbcam\src\gbcam_dither.c" "$root\components\gbcam\src\gbcam_palette.c" "$root\components\gbcam\src\gbcam_pixelcam.c" "$root\components\gbcam\src\dithercam.c" `
    -o "$PSScriptRoot\gbcam_sim.new.dll"
if ($LASTEXITCODE -ne 0) { throw 'simulator build failed' }
& "$PSScriptRoot\..\replace_dll.ps1" -New "$PSScriptRoot\gbcam_sim.new.dll" -Target "$PSScriptRoot\gbcam_sim.dll"
Write-Host "built $PSScriptRoot\gbcam_sim.dll"
