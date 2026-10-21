param(
    [Parameter(Mandatory = $true)][string]$Iso
)

. (Join-Path $PSScriptRoot "common.ps1")

$isoPath = Get-IsoPath $Iso
Write-Step "Extracting the boot files from the disc"
Write-Host $isoPath
New-Item -ItemType Directory $Disc -Force | Out-Null
New-Item -ItemType Directory $Game -Force | Out-Null

$python = Get-Python
$discScript = Join-Path $Engine "scripts\disc.py"
if (-not (Test-Path $discScript)) { throw "Missing $discScript. Run setup.ps1 first." }

Invoke-Checked $python @("-I", $discScript, "extract", $isoPath, $Disc)

$elf = Join-Path $Disc "SCUS_974.81"
$toc = Join-Path $Disc "GODOFWAR.TOC"
if (-not (Test-Path $elf)) { throw "SCUS_974.81 was not extracted. Is this the USA DVD9 ISO?" }
if (-not (Test-Path $toc)) { throw "GODOFWAR.TOC was not extracted. The disc image may be incomplete." }

# Remember the path locally so the later scripts can be run without repeating it.
Set-Content -Path (Join-Path $Game "iso.txt") -Value $isoPath -Encoding ASCII
Write-Step "Disc files"
Get-ChildItem $Disc -File | Select-Object Name, Length | Format-Table -AutoSize
Write-Host "The disc image stays where it is; only the boot files were copied."
