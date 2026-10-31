# Builds tests/gs_probe (offscreen Vulkan GS backend tests) with clang and runs
# them against the SwiftShader ICD. No Visual Studio, no Gradle:
#
#   .\scripts\build-gs-probe.ps1            build + run
#   .\scripts\build-gs-probe.ps1 -NoRun     build only
#
# The engine sources are compiled from the patched submodule (the same files the
# Android build uses), so a probe failure is a backend/engine regression, not a
# test artifact.
param([switch]$NoRun)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '_common.ps1')
Ensure-Engine
Apply-EnginePatches

$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
if (-not (Test-Path $clang)) { throw "clang++ not found at $clang (install LLVM or point the script at it)." }
$swiftShader = 'C:\Program Files\Unity Hub\vk_swiftshader.dll'
if (-not (Test-Path $swiftShader))
{
    $chrome = Get-ChildItem 'C:\Program Files\Google\Chrome\Application\*\vk_swiftshader.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
    $edge = Get-ChildItem 'C:\Program Files (x86)\Microsoft\Edge\Application\*\vk_swiftshader.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($chrome) { $swiftShader = $chrome.FullName }
    elseif ($edge) { $swiftShader = $edge.FullName }
    else { throw "no SwiftShader ICD found (Unity Hub / Chrome / Edge)." }
}

# Scratch dirs: embedded-SPIR-V C++ (from the same shaders the APK embeds) and
# a copy of the NDK's Vulkan headers (the desktop clang has none).
$vkRoot = (Join-Path $RepoRoot 'android-runner\native\gs_vk\_check')
$genRoot = (Join-Path $RepoRoot 'android-runner\native\gs_vk\_gen')
New-Item -ItemType Directory -Force (Join-Path $vkRoot 'vulkan') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $vkRoot 'vk_video') | Out-Null
New-Item -ItemType Directory -Force $genRoot | Out-Null
$ndkInc = 'C:\Android\android-sdk\ndk\27.3.13750724\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\include'
Copy-Item (Join-Path $ndkInc 'vulkan\*.h') (Join-Path $vkRoot 'vulkan') -Force
Copy-Item (Join-Path $ndkInc 'vk_video\*.h') (Join-Path $vkRoot 'vk_video') -Force
$spvDir = Join-Path $BuildDir 'gs-probe-spv'
New-Item -ItemType Directory -Force $spvDir | Out-Null
$glslc = 'C:\Android\android-sdk\ndk\27.3.13750724\shader-tools\windows-x86_64\glslc.exe'
Get-ChildItem (Join-Path $RepoRoot 'android-runner\native\gs_vk\shaders') -Include '*.vert', '*.frag' -File -Recurse | ForEach-Object {
    & $glslc --target-env=vulkan1.1 -o (Join-Path $spvDir ($_.Name + '.spv')) $_.FullName
    if ($LASTEXITCODE -ne 0) { throw "shader failed: $($_.Name)" }
}
& cmake -DSPV_DIR=$spvDir -DOUT_CPP=(Join-Path $genRoot 'gs_vk_shaders_generated.cpp') -DOUT_H=(Join-Path $genRoot 'gs_vk_shaders_generated.h') -P (Join-Path $RepoRoot 'cmake\embed_spirv.cmake')
if ($LASTEXITCODE -ne 0) { throw 'SPIR-V embedding failed' }

$inc = @(
    '-I', (Join-Path $RepoRoot 'android-runner\native'),
    '-I', (Join-Path $RepoRoot 'android-runner\native\gs_vk'),
    '-I', (Join-Path $RepoRoot 'android-runner\native\gs_vk\_gen'),
    '-I', (Join-Path $RepoRoot 'android-runner\native\gs_vk\_check'),
    '-I', (Join-Path $RecompDir 'ps2xRuntime\include'),
    '-I', (Join-Path $RecompDir 'ps2xRuntime\src\lib'),
    '-I', (Join-Path $RecompDir 'ps2xRuntime\src\lib\gs')
)
$defines = @('-DVK_USE_PLATFORM_ANDROID_KHR=0', '-DPS2X_GS_PROBE=1', '-D_CRT_SECURE_NO_WARNINGS')
# The engine's GL backend and host stuff are not part of the probe; the GS
# sources the backend really needs plus their dependencies are listed here.
$engineSources = @(
    (Join-Path $RecompDir 'ps2xRuntime\src\lib\gs\gs_swizzle.cpp'),
    (Join-Path $RecompDir 'ps2xRuntime\src\lib\gs\ps2_gs_memory.cpp'),
    (Join-Path $RecompDir 'ps2xRuntime\src\lib\gs\gpu\gs_texture_store.cpp'),
    (Join-Path $RepoRoot 'android-runner\native\gs_vk\gs_vk_loader.cpp'),
    (Join-Path $RepoRoot 'android-runner\native\gs_vk\gs_vk_backend.cpp'),
    (Join-Path $RepoRoot 'android-runner\native\gs_vk\_gen\gs_vk_shaders_generated.cpp')
)

$outDir = Join-Path $BuildDir 'gs-probe'
New-Item -ItemType Directory -Force $outDir | Out-Null

Write-Step 'Compiling the probe'
$objects = @()
foreach ($src in (@((Join-Path $RepoRoot 'tests\gs_probe\gs_probe.cpp')) + $engineSources))
{
    $obj = Join-Path $outDir (([IO.Path]::GetFileNameWithoutExtension($src)) + '.o')
    $log = & cmd /c "`"$clang`" -std=c++20 -O1 -g0 -c `"$src`" -o `"$obj`" $($inc -join ' ') $($defines -join ' ') 2>&1"
    $log | Where-Object { $_ -match 'error' } | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $src" }
    $objects += $obj
}

$exe = Join-Path $outDir 'gs_probe.exe'
Write-Step 'Linking'
& cmd /c "`"$clang`" $($objects -join ' ') -o `"$exe`" 2>&1" | Where-Object { $_ -match 'error|undefined' } | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { throw 'link failed' }

if ($NoRun) { Write-Host "probe: $exe"; return }

Write-Step 'Running against SwiftShader'
# Software rasterizer on this machine: Subzero's JIT emits AVX that crashes on
# some CI VMs, so force the interpreter and accept slower runs.
$env:REACTOR_JIT = '0'
$env:PS2X_VK_ICD = $swiftShader
& $exe
exit $LASTEXITCODE
