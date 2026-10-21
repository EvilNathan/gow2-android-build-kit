param(
    [Parameter(Mandatory = $true)][string]$Iso,
    [int]$Frames = 2400,
    [switch]$SkipExplore,
    [switch]$ForceDeps
)

. (Join-Path $PSScriptRoot "common.ps1")

$isoPath = Get-IsoPath $Iso

Write-Step "1/5 Engine and dependencies"
& (Join-Path $PSScriptRoot "setup.ps1") -ForceDeps:$ForceDeps


Write-Step "2/5 Disc extraction"
& (Join-Path $PSScriptRoot "extract-disc.ps1") -Iso $isoPath


Write-Step "3/5 Recompiler tools"
& (Join-Path $PSScriptRoot "build-tools.ps1")


Write-Step "4/5 Game code and VU programs"
& (Join-Path $PSScriptRoot "recompile.ps1") -Iso $isoPath -Frames $Frames -SkipExplore:$SkipExplore


Write-Step "5/5 Android APK"
& (Join-Path $PSScriptRoot "build-android.ps1")


Write-Host ""
Write-Host "Build complete." -ForegroundColor Green
