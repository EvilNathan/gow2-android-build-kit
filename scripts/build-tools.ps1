param(
    [switch]$AggressiveLogs
)

. (Join-Path $PSScriptRoot '_common.ps1')
Ensure-Engine
Apply-EnginePatches

$cmake = Get-CMake
$vsArgs = Get-VsGeneratorArgs

Write-Step 'Configuring PS2Recomp tools'
$configure = @('-S', $RecompDir, '-B', $BuildDir) + $vsArgs + @(
    '-DPS2X_BUILD_TEST=OFF',
    '-DPS2X_BUILD_STUDIO=OFF'
)
if ($AggressiveLogs) { $configure += '-DPS2X_ENABLE_AGRESSIVE_LOGS=ON' }
else { $configure += '-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF' }
Invoke-Native $cmake $configure

Write-Step 'Building ps2_analyzer and ps2_recomp'
Invoke-Native $cmake @('--build', $BuildDir, '--config', 'Release', '--target', 'ps2_analyzer', 'ps2_recomp', '--parallel')

$recomp = Join-Path $BuildDir 'ps2xRecomp\Release\ps2_recomp.exe'
if (-not (Test-Path $recomp)) { throw "ps2_recomp.exe was not produced at $recomp" }

Write-Host ''
Write-Host 'Tools ready:'
Write-Host "  $(Join-Path $BuildDir 'ps2xAnalyzer\Release\ps2_analyzer.exe')"
Write-Host "  $recomp"
