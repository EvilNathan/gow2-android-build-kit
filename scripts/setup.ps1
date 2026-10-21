param(
    [switch]$ForceDeps
)

. (Join-Path $PSScriptRoot "common.ps1")

$pin = "6526f4803d98e10de50bea3af104a4d26e9df033"
$engineRepo = "https://github.com/snuri00/ps2-web-recomp.git"
$patch = Join-Path $Root "engine.patch"

Write-Step "Checking prerequisites"
Require-Command git "Install Git for Windows and restart the shell." | Out-Null
Require-Command cmake "Install CMake 3.24 or newer." | Out-Null
Require-Command ninja "Install Ninja, or use a Visual Studio Developer PowerShell." | Out-Null
Get-Python | Out-Null

Write-Step "Engine"
if (-not (Test-Path (Join-Path $Engine ".git"))) {
    if (Test-Path $Engine) {
        throw "$Engine exists but is not a git checkout. Move it aside and rerun."
    }
    Invoke-Checked git @("clone", "--depth", "1", $engineRepo, $Engine)
}
$head = (& git -C $Engine rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw "Cannot read the engine revision." }
if ($head -ne $pin) {
    Write-Host "Engine is at $head; checking out $pin."
    & git -C $Engine cat-file -e "$pin^{commit}" 2>$null
    if ($LASTEXITCODE -ne 0) {
        & git -C $Engine fetch --depth 1 origin $pin
        if ($LASTEXITCODE -ne 0) {
            Write-Host "Fetching the full engine history for the pinned revision."
            Invoke-Checked git @("-C", $Engine, "fetch", "origin")
        }
    }
    Invoke-Checked git @("-C", $Engine, "checkout", "--quiet", $pin)
}

$alreadyPatched = (Test-Path (Join-Path $Engine "host\android_jni.cpp")) -and
                  (Select-String -Path (Join-Path $Engine "runtime\src\iop\io.cpp") -Pattern "GODOFWAR.TOC" -Quiet)
if (-not $alreadyPatched) {
    & git -C $Engine apply --check --ignore-whitespace $patch
    if ($LASTEXITCODE -ne 0) {
        throw "engine.patch does not apply to this engine checkout. Delete engine\ and rerun."
    }
    Invoke-Checked git @("-C", $Engine, "apply", "--ignore-whitespace", $patch)
} else {
    Write-Host "GoW2 Android patch already applied."
}

$thirdParty = Join-Path $Engine "third_party"
New-Item -ItemType Directory $thirdParty -Force | Out-Null

function Get-Zip {
    param([string]$Url, [string]$Destination)
    Write-Host "Downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $Destination -UseBasicParsing
}

Write-Step "SDL3 3.4.18"
$sdl = Join-Path $thirdParty "SDL3-3.4.18"
if ($ForceDeps) { Remove-Item $sdl -Recurse -Force -ErrorAction SilentlyContinue }
if (-not (Test-Path (Join-Path $sdl "CMakeLists.txt"))) {
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("gow2-sdl-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory $tmp | Out-Null
    try {
        $zip = Join-Path $tmp "SDL3.zip"
        Get-Zip "https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.zip" $zip
        Expand-Archive -Path $zip -DestinationPath $tmp -Force
        $source = Get-ChildItem $tmp -Directory |
            Where-Object { Test-Path (Join-Path $_.FullName "CMakeLists.txt") } |
            Select-Object -First 1
        if (-not $source) { throw "The SDL3 archive did not contain a source tree." }
        Move-Item $source.FullName $sdl
    } finally {
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
} else {
    Write-Host "SDL3 sources present."
}

Write-Step "wgpu-native v29.0.1.1 (Android arm64)"
$wgpu = Join-Path $thirdParty "wgpu-native-android"
if ($ForceDeps) { Remove-Item $wgpu -Recurse -Force -ErrorAction SilentlyContinue }
if (-not (Test-Path (Join-Path $wgpu "lib\libwgpu_native.a"))) {
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("gow2-wgpu-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory $tmp | Out-Null
    try {
        $zip = Join-Path $tmp "wgpu.zip"
        Get-Zip "https://github.com/gfx-rs/wgpu-native/releases/download/v29.0.1.1/wgpu-android-aarch64-release.zip" $zip
        Expand-Archive -Path $zip -DestinationPath $tmp -Force
        $library = Get-ChildItem $tmp -Recurse -File -Filter "libwgpu_native.a" | Select-Object -First 1
        if (-not $library) { throw "The wgpu-native archive did not contain libwgpu_native.a." }
        # The archive normally has include/, lib/, and wgpu-native-meta/ below one folder.
        $root = $library.Directory.Parent
        Move-Item $root.FullName $wgpu
    } finally {
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
} else {
    Write-Host "wgpu-native Android library present."
}

Write-Step "Done"
Write-Host "Engine:  $Engine"
Write-Host "SDL3:    $sdl"
Write-Host "wgpu:    $wgpu"
