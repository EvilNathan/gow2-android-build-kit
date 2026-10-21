param(
    [switch]$Release
)

. (Join-Path $PSScriptRoot "common.ps1")

$units = @(Get-ChildItem $Generated -Filter "unit_*.cpp" -ErrorAction SilentlyContinue)
if ($units.Count -eq 0) {
    throw "No generated game code in $Generated. Run scripts\recompile.ps1 first."
}
if (-not (Test-Path (Join-Path $Engine "third_party\SDL3-3.4.18\CMakeLists.txt"))) {
    throw "SDL3 sources are missing. Run scripts\setup.ps1 first."
}
if (-not (Test-Path (Join-Path $Engine "third_party\wgpu-native-android\lib\libwgpu_native.a"))) {
    throw "wgpu-native Android is missing. Run scripts\setup.ps1 first."
}

$sdk = $env:ANDROID_HOME
if (-not $sdk) { $sdk = $env:ANDROID_SDK_ROOT }
if (-not $sdk) {
    $candidate = Join-Path $env:LOCALAPPDATA "Android\Sdk"
    if (Test-Path $candidate) { $sdk = $candidate }
}
if (-not $sdk) {
    throw "Set ANDROID_HOME (or ANDROID_SDK_ROOT) to the Android SDK folder."
}
if (-not (Test-Path (Join-Path $sdk "platforms\android-35"))) {
    throw "Android SDK platform 35 is missing from $sdk."
}
if (-not (Test-Path (Join-Path $sdk "ndk\27.3.13750724"))) {
    throw "NDK 27.3.13750724 is missing from $sdk. Install it with sdkmanager."
}

Require-Command java "Install JDK 17 and set JAVA_HOME." | Out-Null

$android = Join-Path $Root "android"
$localProperties = Join-Path $android "local.properties"
$sdkGradle = $sdk.Replace("\", "/")
Set-Content -Path $localProperties -Value "sdk.dir=$sdkGradle" -Encoding ASCII

Write-Step "Building the Android APK"
Push-Location $android
try {
    $task = if ($Release) { ":app:assembleRelease" } else { ":app:assembleDebug" }
    & .\gradlew.bat --no-daemon $task
    if ($LASTEXITCODE -ne 0) { throw "Gradle failed with code $LASTEXITCODE." }
} finally {
    Pop-Location
}

$variant = if ($Release) { "release" } else { "debug" }
$apk = Join-Path $android "app\build\outputs\apk\$variant\app-$variant.apk"
if (-not (Test-Path $apk) -and $Release) {
    $apk = Join-Path $android "app\build\outputs\apk\release\app-release-unsigned.apk"
    Write-Warning "Release APK is unsigned; configure signing before installing or distributing it."
}
if (-not (Test-Path $apk)) { throw "Gradle finished but $apk is missing." }

$dist = Join-Path $Root "dist"
New-Item -ItemType Directory $dist -Force | Out-Null
$output = Join-Path $dist "gow2-$variant.apk"
Copy-Item $apk $output -Force
Write-Host ""
Write-Host "APK: $output" -ForegroundColor Green
Write-Host "Install with: adb install -r `"$output`""
Write-Host "The APK contains generated code for your disc; do not redistribute it."
