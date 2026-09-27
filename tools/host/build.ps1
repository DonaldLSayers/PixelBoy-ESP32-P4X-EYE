# Builds the PC tools with zig's C compiler (winget install zig.zig):
#   tools\host\gbcam_cli.exe   - process image files
#   tools\host\gbcam.dll       - image core for gbcam_live.py (webcam viewer)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\.."
$zig = (Get-Command zig -ErrorAction SilentlyContinue).Source
if (-not $zig) {
    $zig = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter zig.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $zig) { throw 'zig not found. Install with: winget install zig.zig' }

$flags = @('-std=c99', '-O2', '-Wall', '-Wextra', '-Wno-unused-function', '-Wno-missing-field-initializers',
           '-I', "$root\components\gbcam\include", '-I', "$root\components\stb\include")
$core = @("$root\components\gbcam\src\gbcam.c",
          "$root\components\gbcam\src\gbcam_dither.c",
          "$root\components\gbcam\src\gbcam_palette.c",
          "$root\components\gbcam\src\gbcam_pixelcam.c",
          "$root\components\gbcam\src\dithercam.c")

& $zig cc @flags "$PSScriptRoot\gbcam_cli.c" @core -o "$PSScriptRoot\gbcam_cli.exe"
if ($LASTEXITCODE -ne 0) { throw 'gbcam_cli build failed' }
Write-Host "built $PSScriptRoot\gbcam_cli.exe"

& $zig cc @flags -shared "$PSScriptRoot\gbcam_api.c" @core -o "$PSScriptRoot\gbcam.new.dll"
if ($LASTEXITCODE -ne 0) { throw 'gbcam.dll build failed' }
& "$PSScriptRoot\..\replace_dll.ps1" -New "$PSScriptRoot\gbcam.new.dll" -Target "$PSScriptRoot\gbcam.dll"
Write-Host "built $PSScriptRoot\gbcam.dll"
