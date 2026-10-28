<#
.SYNOPSIS
    Generates the native VU1 + IOP code that makes God of War II playable on Android.

.DESCRIPTION
    Android builds cannot run the host-only VU1 microcode compiler (the engine
    defines ps2x_vu1_generator only when NOT PS2X_IS_ANDROID), so this script
    runs the host generator and the Python generators once on the PC and leaves
    plain C++ in generated/. The Android build then compiles those files:

      generated\vu1n-parts\programa*.cpp  13 parts, host generator (host-only, MSVC)
      generated\vu1n\vu1n_part*.cpp       6 parts, tools\vu1\vu1n_gen.py
      generated\iop\iopn_*.cpp            IRX modules, tools\iop\iopn_gen.py

    The IOP files are also copied into PS2Recomp\ps2xIOP\src\native\ because the
    engine CMake globs them there (they are gitignored inside the submodule).

    The inputs are game-derived and stay local (generated/ and games/ are
    gitignored); only code is committed to the kit.

    Capture folders (defaults):
      generated\capture\vu1  16 KB VU1 microcode .bin files (GOW_VU1_CAPTURA)
      generated\capture\iop  modules.txt + <module>.bin (PS2X_IOP_DUMP_MODULES)

    The script is idempotent: the generators only rewrite changed files, copies
    are MD5-compared and the CMake target builds incrementally.

.PARAMETER Vu1MicrocodeDir
    Folder with the captured VU1 microcode (*.bin, 16384 bytes each).
    Default: generated\capture\vu1

.PARAMETER IopModulesDir
    Folder with the captured IOP modules (modules.txt + *.bin).
    Default: generated\capture\iop

.PARAMETER SkipGeneratorBuild
    Reuse the ps2x_vu1_generator.exe already built under build\ (no CMake).

.EXAMPLE
    .\scripts\gen-native.ps1
    .\scripts\gen-native.ps1 -Vu1MicrocodeDir D:\caps\vu1 -IopModulesDir D:\caps\iop

.NOTES
    The generated C++ is consumed by the Android build through
        -DPS2X_ANDROID_NATIVE_VU1_DIR=<repo>/generated/vu1n
        -DPS2X_ANDROID_NATIVE_VU1_PARTS_DIR=<repo>/generated/vu1n-parts
    and the engine's ps2xIOP glob. The last section of the output prints the
    exact -D arguments.
#>
param(
    [string]$Vu1MicrocodeDir = '',
    [string]$IopModulesDir = '',
    [switch]$SkipGeneratorBuild
)

. (Join-Path $PSScriptRoot '_common.ps1')

function ConvertTo-ForwardSlash([string]$Path) { return $Path -replace '\\', '/' }

function Resolve-RequiredDirectory([string]$Path, [string]$Label, [string]$Hint) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Label not found: $Path`n$Hint"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Assert-FileExists([string]$Path, [string]$Hint) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing file: $Path`n$Hint"
    }
}

function Get-Python {
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) { return $python.Source }
    return Require-Command py
}

# Only rewrite a file when its bytes change (same approach as recompile.ps1).
$md5 = [System.Security.Cryptography.MD5]::Create()
function Test-SameContent([string]$First, [string]$Second) {
    $a = Get-Item -LiteralPath $First
    $b = Get-Item -LiteralPath $Second -ErrorAction SilentlyContinue
    if (-not $b -or $a.Length -ne $b.Length) { return $false }
    $hashA = $md5.ComputeHash([IO.File]::ReadAllBytes($First))
    $hashB = $md5.ComputeHash([IO.File]::ReadAllBytes($Second))
    return [Convert]::ToBase64String($hashA) -eq [Convert]::ToBase64String($hashB)
}

# ── Inputs ────────────────────────────────────────────────────────────────
if (-not $Vu1MicrocodeDir) { $Vu1MicrocodeDir = Join-Path $RepoRoot 'generated\capture\vu1' }
if (-not $IopModulesDir) { $IopModulesDir = Join-Path $RepoRoot 'generated\capture\iop' }

Write-Step 'Checking the prerequisites'
if (-not (Test-Path $ElfNameFile)) {
    throw "games\gow2\elf-name.txt is missing. Extract your own disc first:`n  .\scripts\extract-disc.ps1 -Iso `"<your God of War II (USA) disc image>`""
}
$elfName = (Get-Content $ElfNameFile -TotalCount 1).Trim()
if (-not $elfName) { throw "games\gow2\elf-name.txt is empty." }

$Vu1MicrocodeDir = Resolve-RequiredDirectory $Vu1MicrocodeDir 'VU1 microcode capture folder' `
    'Capture the VU1 microcode on the PC first (GOW_VU1_CAPTURA, see docs\NOTES.md) or pass -Vu1MicrocodeDir.'
$IopModulesDir = Resolve-RequiredDirectory $IopModulesDir 'IOP modules capture folder' `
    'Capture the IOP modules on the PC first (PS2X_IOP_DUMP_MODULES) or pass -IopModulesDir.'

$vu1Bins = @(Get-ChildItem -LiteralPath $Vu1MicrocodeDir -Filter '*.bin' -File | Sort-Object Name)
if ($vu1Bins.Count -eq 0) {
    throw "No VU1 microcode captures (*.bin) in $Vu1MicrocodeDir. The engine only creates ps2x_vu1_generator when there is at least one .bin capture."
}
$wrongSize = @($vu1Bins | Where-Object { $_.Length -ne 16384 })
if ($wrongSize.Count -gt 0) {
    throw "A VU1 microcode capture must be exactly 16384 bytes: $($wrongSize[0].FullName) has $($wrongSize[0].Length) bytes."
}

$modulesTxt = Join-Path $IopModulesDir 'modules.txt'
Assert-FileExists $modulesTxt "modules.txt is written by PS2X_IOP_DUMP_MODULES."
$iopBins = @(Get-ChildItem -LiteralPath $IopModulesDir -Filter '*.bin' -File)
if ($iopBins.Count -eq 0) {
    throw "No IOP module dumps (*.bin) in $IopModulesDir (modules.txt exists but the module binaries are missing)."
}

$Vu1PartsDir = Join-Path $RepoRoot 'generated\vu1n-parts'
$Vu1GenDir = Join-Path $RepoRoot 'generated\vu1n'
$IopGenDir = Join-Path $RepoRoot 'generated\iop'
$IopNativeDir = Join-Path $RecompDir 'ps2xIOP\src\native'
$GeneratorSource = Join-Path $RepoRoot 'tools\vu1\generar_vu1.cpp'
$Vu1nGen = Join-Path $RepoRoot 'tools\vu1\vu1n_gen.py'
$Vu1nHeads = Join-Path $RepoRoot 'tools\vu1\vu1n_heads.txt'
$IopnGen = Join-Path $RepoRoot 'tools\iop\iopn_gen.py'

Assert-FileExists $GeneratorSource 'The kit checkout is incomplete: tools\vu1\generar_vu1.cpp is missing.'
Assert-FileExists $Vu1nGen 'The kit checkout is incomplete: tools\vu1\vu1n_gen.py is missing.'
Assert-FileExists $Vu1nHeads 'The kit checkout is incomplete: tools\vu1\vu1n_heads.txt is missing.'
Assert-FileExists $IopnGen 'The kit checkout is incomplete: tools\iop\iopn_gen.py is missing.'
if (-not $SkipGeneratorBuild -and -not (Test-EngineReady)) {
    throw "PS2Recomp\ is missing or empty. Run .\scripts\setup.ps1 first (engine pin $EnginePin)."
}

# The host generators receive paths on the command line; MSVC tooling and the
# Windows code page make non-ASCII paths unreliable.
if ("$RepoRoot" -match '[^\x00-\x7F]') {
    throw "The repository path contains non-ASCII characters: $RepoRoot`nMove the kit to an ASCII-only path before generating native code."
}

$pyExe = Get-Python
$cmake = $null
if (-not $SkipGeneratorBuild) { $cmake = Get-CMake }

Write-Host "Boot ELF:      $elfName"
Write-Host "VU1 captures:  $($vu1Bins.Count) *.bin in $Vu1MicrocodeDir"
Write-Host "IOP dumps:     $($iopBins.Count) *.bin in $IopModulesDir"

# ── 1. Host VU1 microcode generator ───────────────────────────────────────
$generatorExe = $null
if ($SkipGeneratorBuild) {
    Write-Step 'Reusing the host generator (skipping CMake)'
}
else {
    Write-Step 'Configuring PS2Recomp for the host VU1 generator'
    $configureArgs = @('-S', $RecompDir, '-B', $BuildDir) + (Get-VsGeneratorArgs)
    $configureArgs += "-DPS2X_VU1_MICROCODE_DIR=$(ConvertTo-ForwardSlash $Vu1MicrocodeDir)"
    $configureArgs += "-DPS2X_VU1_GENERATOR_SOURCE=$(ConvertTo-ForwardSlash $GeneratorSource)"
    $configureArgs += "-DPS2X_VU1N_GEN_OVERRIDE=$(ConvertTo-ForwardSlash $Vu1nGen)"
    $configureArgs += "-DPS2X_VU1N_HEADS_FILE=$(ConvertTo-ForwardSlash $Vu1nHeads)"
    Invoke-Native $cmake $configureArgs

    $cachePath = Join-Path $BuildDir 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cachePath)) {
        throw "CMake did not write $cachePath. Delete build\ and rerun .\scripts\setup.ps1."
    }
    $cacheText = Get-Content -LiteralPath $cachePath -Raw
    $expectedMicrocode = 'PS2X_VU1_MICROCODE_DIR:PATH=' + (ConvertTo-ForwardSlash $Vu1MicrocodeDir)
    if ($cacheText -notmatch [regex]::Escape($expectedMicrocode)) {
        throw "CMake ignored $expectedMicrocode. Delete build\CMakeCache.txt and rerun, and check that PS2Recomp is at pin $EnginePin."
    }

    # The engine only creates the target when the microcode folder has *.bin
    # captures and the configure is a host (non-Android) one.
    $projectFile = Join-Path $BuildDir 'ps2xRuntime\ps2x_vu1_generator.vcxproj'
    if (-not (Test-Path -LiteralPath $projectFile)) {
        $matches = @(Get-ChildItem -LiteralPath $BuildDir -Recurse -Filter 'ps2x_vu1_generator.*proj' -File -ErrorAction SilentlyContinue)
        if ($matches.Count -eq 0) {
            throw @"
The ps2x_vu1_generator target was not created by the configure.
The engine adds it only when all of these hold:
  - PS2X_VU1_MICROCODE_DIR contains at least one *.bin (currently $($vu1Bins.Count))
  - PS2X_VU1_GENERATOR_SOURCE points at tools\vu1\generar_vu1.cpp
  - the configure is NOT Android and NOT Vita (this is a host configure)
Delete build\ and rerun .\scripts\setup.ps1, then run this script again.
"@
        }
        $projectFile = $matches[0].FullName
    }

    Write-Step 'Building ps2x_vu1_generator (host only, no runner/engine build)'
    Invoke-Native $cmake @('--build', $BuildDir, '--config', 'Release', '--target', 'ps2x_vu1_generator')

    $generatorExe = Join-Path $BuildDir 'ps2xRuntime\Release\ps2x_vu1_generator.exe'
    if (-not (Test-Path -LiteralPath $generatorExe)) {
        $matches = @(Get-ChildItem -LiteralPath $BuildDir -Recurse -Filter 'ps2x_vu1_generator.exe' -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending)
        if ($matches.Count -eq 0) {
            throw "ps2x_vu1_generator.exe was not produced under $BuildDir. Delete build\ and rerun .\scripts\setup.ps1."
        }
        $generatorExe = $matches[0].FullName
    }
}

if (-not $generatorExe -or -not (Test-Path -LiteralPath $generatorExe)) {
    throw "ps2x_vu1_generator.exe not found. Run this script without -SkipGeneratorBuild first."
}

# ── 2. VU1 native parts (host generator, 13 parts) ────────────────────────
Write-Step 'Generating VU1 parts (host generator, 13 parts)'
New-Item -ItemType Directory -Force $Vu1PartsDir | Out-Null
$generatorSw = [Diagnostics.Stopwatch]::StartNew()
Invoke-Native $generatorExe @($Vu1PartsDir, $Vu1MicrocodeDir)
$generatorSw.Stop()

$expectedVu1Parts = @()
foreach ($part in 0..12) { $expectedVu1Parts += (Join-Path $Vu1PartsDir "programa$part.cpp") }
$missingVu1Parts = @($expectedVu1Parts | Where-Object { -not (Test-Path -LiteralPath $_) })
if ($missingVu1Parts.Count -gt 0) {
    throw "The VU1 generator did not produce all 13 parts. Missing: $($missingVu1Parts -join ', ')"
}
$staleVu1Parts = @(Get-ChildItem -LiteralPath $Vu1PartsDir -Filter 'programa*.cpp' -File |
    Where-Object { $expectedVu1Parts -notcontains $_.FullName })
if ($staleVu1Parts.Count -gt 0) {
    $staleVu1Parts | Remove-Item -Force
    Write-Host "Removed $($staleVu1Parts.Count) stale VU1 part(s)."
}

# ── 3. VU1 lean parts (vu1n_gen.py, 6 parts) ──────────────────────────────
Write-Step 'Generating VU1 lean parts (tools/vu1/vu1n_gen.py, 6 parts)'
New-Item -ItemType Directory -Force $Vu1GenDir | Out-Null
Invoke-Native $pyExe @('-I', $Vu1nGen, $Vu1GenDir, $Vu1MicrocodeDir, '--heads', $Vu1nHeads, '--parts', '6')

$expectedVu1nParts = @()
foreach ($part in 0..5) { $expectedVu1nParts += (Join-Path $Vu1GenDir "vu1n_part$part.cpp") }
$missingVu1nParts = @($expectedVu1nParts | Where-Object { -not (Test-Path -LiteralPath $_) })
if ($missingVu1nParts.Count -gt 0) {
    throw "vu1n_gen.py did not produce all 6 parts. Missing: $($missingVu1nParts -join ', ')"
}
$staleVu1nParts = @(Get-ChildItem -LiteralPath $Vu1GenDir -Filter 'vu1n_part*.cpp' -File |
    Where-Object { $expectedVu1nParts -notcontains $_.FullName })
if ($staleVu1nParts.Count -gt 0) {
    $staleVu1nParts | Remove-Item -Force
    Write-Host "Removed $($staleVu1nParts.Count) stale VU1 lean part(s)."
}

# ── 4. IOP modules (iopn_gen.py) ──────────────────────────────────────────
Write-Step 'Generating native IOP modules (tools/iop/iopn_gen.py)'
New-Item -ItemType Directory -Force $IopGenDir | Out-Null
Invoke-Native $pyExe @('-I', $IopnGen, $IopModulesDir, $IopGenDir)

$generatedIop = @(Get-ChildItem -LiteralPath $IopGenDir -Filter 'iopn_*.cpp' -File | Sort-Object Name)
$registryPath = Join-Path $IopGenDir 'iopn_registry.cpp'
if (-not (Test-Path -LiteralPath $registryPath)) {
    throw "iopn_gen.py did not write iopn_registry.cpp. Check modules.txt ($modulesTxt) and the dump folder."
}
if ($generatedIop.Count -le 1) {
    throw "iopn_gen.py produced no module files; every module in modules.txt was skipped (size mismatch?)."
}

# The engine CMake globs ps2xIOP\src\native\iopn_*.cpp (CONFIGURE_DEPENDS), so
# the files must live inside the engine checkout; they are gitignored there.
if (-not (Test-Path -LiteralPath $IopNativeDir -PathType Container)) {
    throw "PS2Recomp is not checked out at $RecompDir (missing ps2xIOP\src\native). Run .\scripts\setup.ps1."
}
Write-Step "Copying IOP modules into PS2Recomp\ps2xIOP\src\native"
$generatedNames = @($generatedIop | ForEach-Object Name)
$iopUpdated = 0
foreach ($file in $generatedIop) {
    $destination = Join-Path $IopNativeDir $file.Name
    if ((Test-Path -LiteralPath $destination) -and (Test-SameContent $file.FullName $destination)) { continue }
    Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
    (Get-Item -LiteralPath $destination).LastWriteTime = Get-Date
    $iopUpdated++
}
$staleIop = @(Get-ChildItem -LiteralPath $IopNativeDir -Filter 'iopn_*.cpp' -File |
    Where-Object { $generatedNames -notcontains $_.Name })
if ($staleIop.Count -gt 0) {
    $staleIop | Remove-Item -Force
    Write-Host "Removed $($staleIop.Count) stale IOP module file(s)."
}
$iopModuleCount = @($generatedIop | Where-Object { $_.Name -ne 'iopn_registry.cpp' }).Count

# ── 5. Summary ────────────────────────────────────────────────────────────
Write-Step 'Summary'
$vu1nParts = @(Get-ChildItem -LiteralPath $Vu1GenDir -Filter 'vu1n_part*.cpp' -File)
$vu1nBytes = ($vu1nParts | Measure-Object -Property Length -Sum).Sum
if ($null -eq $vu1nBytes) { $vu1nBytes = 0 }
$vu1Parts = @(Get-ChildItem -LiteralPath $Vu1PartsDir -Filter 'programa*.cpp' -File)
$vu1PartsBytes = ($vu1Parts | Measure-Object -Property Length -Sum).Sum
if ($null -eq $vu1PartsBytes) { $vu1PartsBytes = 0 }
Write-Host ("VU1 parts (host generator): {0} files, {1:N1} MB in generated\vu1n-parts ({2:N0} s)" -f `
    $vu1Parts.Count, ($vu1PartsBytes / 1MB), $generatorSw.Elapsed.TotalSeconds)
Write-Host ("VU1 lean parts (vu1n_gen):  {0} files, {1:N1} MB in generated\vu1n" -f $vu1nParts.Count, ($vu1nBytes / 1MB))
Write-Host ("IOP native modules:         {0} modules + registry, {1} file(s) updated in PS2Recomp\ps2xIOP\src\native" -f $iopModuleCount, $iopUpdated)
Write-Host ''
Write-Host 'Android build must receive these CMake variables:' -ForegroundColor Cyan
Write-Host ("  -DPS2X_ANDROID_NATIVE_VU1_DIR={0}" -f (ConvertTo-ForwardSlash $Vu1GenDir))
Write-Host ("  -DPS2X_ANDROID_NATIVE_VU1_PARTS_DIR={0}" -f (ConvertTo-ForwardSlash $Vu1PartsDir))
Write-Host '  (the IOP modules are compiled by the engine from PS2Recomp\ps2xIOP\src\native)'
Write-Host ''
Write-Host 'Nothing game-derived was written to the repository: generated\ and the'
Write-Host 'iopn_*.cpp files inside PS2Recomp\ are gitignored. Only code is committed.'
