# Firmware helper for the ESP32-P4X-EYE.
#
# ESP-IDF cannot build from a path containing spaces, so this mirrors the
# project into C:\esp\gbcam_build and runs idf.py there. Edit files in the real
# project folder as usual; the mirror is refreshed on every run.
#
# Usage:
#   .\tools\fw.ps1 build
#   .\tools\fw.ps1 flash  [-Port COM5]
#   .\tools\fw.ps1 monitor [-Port COM5]
#   .\tools\fw.ps1 flash monitor [-Port COM5]
#   .\tools\fw.ps1 menuconfig | fullclean | <any idf.py command>
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$Commands = @('build'),
    [string]$Port
)
$ErrorActionPreference = 'Stop'

$src = Resolve-Path "$PSScriptRoot\.."
$dst = 'C:\esp\gbcam_build'
$env:IDF_TOOLS_PATH = 'C:\esp\.espressif'
$idf = 'C:\esp\esp-idf'

if (-not (Test-Path "$idf\export.ps1")) { throw "ESP-IDF not found at $idf" }

# Mirror sources; keep the build cache and downloaded components in the mirror.
robocopy "$src" "$dst" /MIR /XD build managed_components .git samples tools\host /XF sdkconfig sdkconfig.old dependencies.lock *.exe /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }

# export.ps1 changes PATH etc. for the whole terminal session (e.g. `python` would
# become ESP-IDF's private Python). Snapshot the environment and restore it after.
$savedEnv = @{}
Get-ChildItem env: | ForEach-Object { $savedEnv[$_.Name] = $_.Value }
$savedLocation = Get-Location
$code = 1
try {
    . "$idf\export.ps1" | Out-Null
    Set-Location "$dst\firmware"
    $idfArgs = @()
    if ($Port) { $idfArgs += @('-p', $Port) }
    idf.py @idfArgs @Commands
    $code = $LASTEXITCODE
}
finally {
    Get-ChildItem env: | Where-Object { -not $savedEnv.ContainsKey($_.Name) } |
        ForEach-Object { Remove-Item "env:$($_.Name)" }
    foreach ($k in $savedEnv.Keys) { Set-Item "env:$k" $savedEnv[$k] }
    Set-Location $savedLocation
}
exit $code
