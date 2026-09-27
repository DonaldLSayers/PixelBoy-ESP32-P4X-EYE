# Put a freshly built DLL in place even if the old one is loaded by a running
# viewer: Windows won't overwrite a DLL that's in use, but it
# does allow renaming it. The running program keeps using the old copy; the
# next start picks up the new one. Leftover *.old-*.dll files are cleaned up
# once nothing has them open any more.
param([Parameter(Mandatory)][string]$New, [Parameter(Mandatory)][string]$Target)

# -LeafBase needs PowerShell 7+; this script also runs under Windows
# PowerShell 5.1, so the base name is trimmed by hand instead.
$leafBase = [System.IO.Path]::GetFileNameWithoutExtension($Target)

Get-ChildItem (Split-Path $Target) -Filter ($leafBase + '.old-*.dll') -ErrorAction SilentlyContinue |
    ForEach-Object { try { Remove-Item $_.FullName -ErrorAction Stop } catch {} }

if (Test-Path $Target) {
    try {
        Remove-Item $Target -ErrorAction Stop
    } catch {
        $old = Join-Path (Split-Path $Target) ($leafBase + '.old-' + (Get-Date -Format 'HHmmss') + '.dll')
        Rename-Item $Target $old
        Write-Host "note: $(Split-Path $Target -Leaf) was in use (viewer open?) - restart it to load the new build"
    }
}
Move-Item $New $Target
