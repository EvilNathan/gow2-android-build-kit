<#
.SYNOPSIS
    Reads the boot ELF (and the IOP modules) from your own ISO, or extracts the whole disc.

.DESCRIPTION
    By default only the boot ELF and the root *.IRX modules are copied to games/gow2/disc/.
    That is all the recompiler and the Android build need: the runtime reads the game data
    straight from the ISO (IoPaths.cdImage), so the 8 GB disc is never duplicated.

    Use -Full to extract every file plus the sector layout. The desktop runner only needs
    that if you want to run without GOW2_ISO pointing at the image.

.PARAMETER Iso
    Path to your own God of War II (USA, SCUS_974.81) image. Spaces are fine.

.PARAMETER Full
    Extract the whole disc with --layout (8 GB, for disc-directory mode).
#>
param(
    [Parameter(Mandatory)][string]$Iso,
    [switch]$Full
)

. (Join-Path $PSScriptRoot '_common.ps1')

$isoPath = (Resolve-Path $Iso).Path
$py = Get-Command python -ErrorAction SilentlyContinue
$pyExe = if ($py) { $py.Source } else { (Require-Command py) }

Write-Step 'Reading the disc image'
New-Item -ItemType Directory -Force $DiscDir | Out-Null

$listing = & $pyExe @('-I', (Join-Path $RepoRoot 'tools\iso_extract.py'), $isoPath, '--list')
if ($LASTEXITCODE -ne 0) { throw "iso_extract.py failed on $isoPath" }
$files = foreach ($line in $listing) {
    if ($line -match '^\s*(\d+)\s+(\d+)\s+(.+)$') {
        [pscustomobject]@{ Lba = [int]$Matches[1]; Size = [int64]$Matches[2]; Name = $Matches[3].Trim() }
    }
}
$cnfEntry = $files | Where-Object { $_.Name -eq 'SYSTEM.CNF' } | Select-Object -First 1
if (-not $cnfEntry) { throw "SYSTEM.CNF was not found in $isoPath. Is this a PS2 disc image?" }

if ($Full) {
    Write-Step 'Extracting the full disc (this copies ~8 GB)'
    Invoke-Native $pyExe @('-I', (Join-Path $RepoRoot 'tools\iso_extract.py'), $isoPath, $DiscDir, '--layout')
}
else {
    Invoke-Native $pyExe @('-I', (Join-Path $RepoRoot 'tools\iso_extract.py'), $isoPath, $DiscDir, '--only', 'SYSTEM.CNF')
}
$cnfPath = Join-Path $DiscDir 'SYSTEM.CNF'
if (-not (Test-Path $cnfPath)) { throw "SYSTEM.CNF is missing in $DiscDir. Is this a PS2 disc image?" }
$cnf = Get-Content $cnfPath -Raw
if ($cnf -notmatch 'BOOT2\s*=\s*cdrom0:\\([^;\s]+)') { throw "Cannot read BOOT2 from SYSTEM.CNF:`n$cnf" }
$elfName = $Matches[1]

if (-not $Full) {
    $want = @($elfName)
    $want += $files | Where-Object { $_.Name -match '^[^/]+\.IRX$' } | ForEach-Object Name
    Write-Step "Extracting the boot ELF and $($want.Count - 1) IOP modules"
    Invoke-Native $pyExe @('-I', (Join-Path $RepoRoot 'tools\iso_extract.py'), $isoPath, $DiscDir, '--only', ($want -join ','))
}

# The game opens IOP_MOD/NAME.IRX relative to the ELF; on the disc they live in the root.
$iopMod = Join-Path $DiscDir 'IOP_MOD'
New-Item -ItemType Directory -Force $iopMod | Out-Null
Get-ChildItem $DiscDir -Filter '*.IRX' -File | Copy-Item -Destination $iopMod -Force

Set-Content -Path $ElfNameFile -Value $elfName -Encoding ascii
Write-Utf8NoBom $IsoPathFile $isoPath

$elfPath = Join-Path $DiscDir $elfName
$sha1 = (Get-FileHash $elfPath -Algorithm SHA1).Hash

Write-Step 'Done'
Write-Host "ELF:  $elfPath"
Write-Host "SHA1: $sha1"
Write-Host "ISO:  $isoPath (read directly; no full extraction needed)"
Write-Host 'The reference ELF (SCUS_974.81 v1.01) is E6A62A0F3E781C29513F563F4625B9B3A9103C56.'
