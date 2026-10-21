param(
    [string]$Iso = "",
    [int]$Frames = 900,
    [string]$FramesDir = "",
    [int]$FrameEvery = 300
)

. (Join-Path $PSScriptRoot "common.ps1")

$isoPath = Get-IsoPath $Iso
$elf = Join-Path $Disc "SCUS_974.81"
if (-not (Test-Path $elf)) { throw "Missing $elf. Run extract-disc.ps1 first." }
if (@(Get-ChildItem $Generated -Filter "unit_*.cpp" -ErrorAction SilentlyContinue).Count -eq 0) {
    throw "Generated game code is missing. Run recompile.ps1 first."
}

$suffix = if ($env:OS -eq "Windows_NT") { ".exe" } else { "" }
$runner = Join-Path $Root ("build\native-gow2\ps2run" + $suffix)
if (-not (Test-Path $runner)) { throw "Missing $runner. Run recompile.ps1 first." }

if (-not $FramesDir) { $FramesDir = Join-Path $Game "shots" }
New-Item -ItemType Directory $FramesDir -Force | Out-Null
$misses = Join-Path $Game "misses.txt"
$memcard = Join-Path $Game "explore.ps2"

Write-Step "Headless run"
& $runner $elf --disc $isoPath --misses $misses --memcard $memcard --frames $Frames --frames-dir $FramesDir --frame-every $FrameEvery
if ($LASTEXITCODE -ne 0) { throw "ps2run failed with code $LASTEXITCODE." }
Write-Host "Screenshots: $FramesDir"
