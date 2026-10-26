<#
.SYNOPSIS
    Translates the game ELF to C++ and syncs generated code + overrides into the PS2Recomp runner.

.PARAMETER SyncOnly
    Do not recompile: only re-copy generated/ and overrides/ into the runner (after editing an override).

.PARAMETER AggressiveLogs
    Reconfigure CMake with AGRESSIVE_LOGS (off by default; costs ~11% of the game's time).

.NOTES
    Optional native speed-ups from the reference project (captured on a PC with a GL 4.5 GPU):
      $env:GOW_VU1_MICROCODIGO = folder with the VU1 microcode capture
      $env:GOW_IOP_MODULOS     = folder with modules.txt from PS2X_IOP_DUMP_MODULES
    Both are game-derived and stay local. VU1 native is disabled for Android for now (SSE generator).
#>
param(
    [switch]$SyncOnly,
    [switch]$AggressiveLogs
)

. (Join-Path $PSScriptRoot '_common.ps1')

$elfPath = Get-GameElfPath
function ConvertTo-ForwardSlash([string]$Path) { return $Path -replace '\\', '/' }

if (-not $SyncOnly) {
    $recomp = Join-Path $BuildDir 'ps2xRecomp\Release\ps2_recomp.exe'
    if (-not (Test-Path $recomp)) { throw 'ps2_recomp.exe is missing. Run .\scripts\build-tools.ps1 first.' }
    if ("$RepoRoot" -match '[^\x00-\x7F]') { throw "The repository path contains non-ASCII characters: $RepoRoot" }

    Write-Step 'Recompiling the ELF'
    $toml = [IO.File]::ReadAllText($TemplateToml)
    $toml = $toml.Replace('@ELF@', (ConvertTo-ForwardSlash $elfPath)).
                  Replace('@MAP@', (ConvertTo-ForwardSlash $FuncMapCsv)).
                  Replace('@OUT@', (ConvertTo-ForwardSlash $GeneratedDir) + '/')
    New-Item -ItemType Directory -Force (Split-Path $EffectiveToml) | Out-Null
    Write-Utf8NoBom $EffectiveToml $toml

    if (Test-Path $GeneratedDir) { Remove-Item $GeneratedDir -Recurse -Force }
    New-Item -ItemType Directory -Force $GeneratedDir | Out-Null

    Push-Location $RepoRoot
    try { Invoke-Native $recomp @($EffectiveToml) }
    finally { Pop-Location }

    $units = @(Get-ChildItem $GeneratedDir -File -Filter '*.cpp')
    Write-Host "Generated $($units.Count) C++ units in $GeneratedDir"
}

# ── Native IOP modules (optional) ─────────────────────────────────────────
$iopNativeDir = Join-Path $RecompDir 'ps2xIOP\src\native'
if ($env:GOW_IOP_MODULOS) {
    if (-not (Test-Path (Join-Path $env:GOW_IOP_MODULOS 'modules.txt'))) { throw "GOW_IOP_MODULOS has no modules.txt: $env:GOW_IOP_MODULOS" }
    Write-Step 'Native IOP modules'
    $iopTemp = Join-Path $BuildDir 'iopn-gen'
    if (Test-Path $iopTemp) { Remove-Item $iopTemp -Recurse -Force }
    New-Item -ItemType Directory -Force $iopTemp | Out-Null
    $py = Get-Command python -ErrorAction SilentlyContinue
    $pyExe = if ($py) { $py.Source } else { (Require-Command py) }
    Invoke-Native $pyExe @('-I', (Join-Path $RepoRoot 'tools\iop\iopn_gen.py'), $env:GOW_IOP_MODULOS, $iopTemp)
    $fresh = @(Get-ChildItem $iopTemp -Filter 'iopn_*.cpp' -File)
    $freshNames = $fresh | ForEach-Object Name
    Get-ChildItem $iopNativeDir -Filter 'iopn_*.cpp' -File -ErrorAction SilentlyContinue |
        Where-Object { $freshNames -notcontains $_.Name } | Remove-Item -Force
    $iopCopied = 0
    foreach ($file in $fresh) {
        $dest = Join-Path $iopNativeDir $file.Name
        if ((Test-Path $dest) -and (Get-FileHash $dest -Algorithm MD5).Hash -eq (Get-FileHash $file.FullName -Algorithm MD5).Hash) { continue }
        Copy-Item $file.FullName $dest -Force
        $iopCopied++
    }
    Write-Host "Native IOP: $($fresh.Count) files, $iopCopied updated."
} else {
    Get-ChildItem $iopNativeDir -Filter 'iopn_*.cpp' -File -ErrorAction SilentlyContinue | Remove-Item -Force
    Write-Host 'Native IOP modules: none (set GOW_IOP_MODULOS to enable).'
}

# ── Sync into the runner ──────────────────────────────────────────────────
if (-not (Test-Path $GeneratedDir)) { throw 'generated/ is empty. Run recompile.ps1 without -SyncOnly first.' }

Write-Step 'Syncing generated code and overrides'
New-Item -ItemType Directory -Force (Split-Path $SyncManifest) | Out-Null
New-Item -ItemType Directory -Force $RunnerSrcDir | Out-Null

$md5 = [System.Security.Cryptography.MD5]::Create()
function Test-SameContent([string]$a, [string]$b) {
    $fa = Get-Item -LiteralPath $a
    $fb = Get-Item -LiteralPath $b -ErrorAction SilentlyContinue
    if (-not $fb -or $fa.Length -ne $fb.Length) { return $false }
    $ha = $md5.ComputeHash([IO.File]::ReadAllBytes($a))
    $hb = $md5.ComputeHash([IO.File]::ReadAllBytes($b))
    return [Convert]::ToBase64String($ha) -eq [Convert]::ToBase64String($hb)
}

$copied = New-Object System.Collections.Generic.List[string]
$changed = 0
$sources = @(Get-ChildItem $GeneratedDir -File) +
           @(Get-ChildItem $OverridesDir -File | Where-Object { $_.Extension -in '.cpp', '.h' })
foreach ($file in $sources) {
    $dest = Join-Path $RunnerSrcDir $file.Name
    if (-not (Test-SameContent $file.FullName $dest)) {
        Copy-Item $file.FullName $dest -Force
        (Get-Item $dest).LastWriteTime = Get-Date
        $changed++
    }
    $copied.Add($dest)
}
$removed = 0
if (Test-Path $SyncManifest) {
    $current = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($path in $copied) { [void]$current.Add($path) }
    foreach ($old in (Get-Content $SyncManifest)) {
        if ($old -and -not $current.Contains($old) -and (Test-Path -LiteralPath $old)) {
            Remove-Item -LiteralPath $old -Force
            $removed++
        }
    }
}
Set-Content -Path $SyncManifest -Value $copied -Encoding utf8
Write-Host "Runner: $($copied.Count) files, $changed updated, $removed removed."

# ── Reconfigure CMake so new files are part of the project ────────────────
$cmake = Get-CMake
$cmakeArgs = @('-S', $RecompDir, '-B', $BuildDir) + (Get-VsGeneratorArgs)
$cmakeArgs += '-DPS2X_BUILD_TEST=OFF', '-DPS2X_BUILD_STUDIO=OFF'
if ($AggressiveLogs) { $cmakeArgs += '-DPS2X_ENABLE_AGRESSIVE_LOGS=ON' }
else { $cmakeArgs += '-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF' }
$vu1Micro = if ($env:GOW_VU1_MICROCODIGO) { ConvertTo-ForwardSlash $env:GOW_VU1_MICROCODIGO } else { '' }
if ($vu1Micro) { Write-Host "Native VU1 from $vu1Micro" }
$cmakeArgs += "-DPS2X_VU1_MICROCODE_DIR=$vu1Micro"
$cmakeArgs += "-DPS2X_VU1_GENERATOR_SOURCE=$(if ($vu1Micro) { ConvertTo-ForwardSlash (Join-Path $RepoRoot 'tools\vu1\generar_vu1.cpp') })"
$cmakeArgs += "-DPS2X_VU1N_GEN_OVERRIDE=$(if ($vu1Micro) { ConvertTo-ForwardSlash (Join-Path $RepoRoot 'tools\vu1\vu1n_gen.py') })"
$cmakeArgs += "-DPS2X_VU1N_HEADS_FILE=$(if ($vu1Micro) { ConvertTo-ForwardSlash (Join-Path $RepoRoot 'tools\vu1\vu1n_heads.txt') })"
if (Test-Path (Join-Path $BuildDir 'CMakeCache.txt')) {
    Invoke-Native $cmake $cmakeArgs | Out-Null
}

Write-Step 'Done'
Write-Host 'Desktop runner: .\scripts\run-headless.ps1'
Write-Host 'Android APK:    .\scripts\build-android.ps1'
