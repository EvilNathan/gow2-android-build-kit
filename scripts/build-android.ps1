param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [string]$BootElfName = '',
    [bool]$NullGs = $true,
    [switch]$Clean,
    [switch]$Install
)

. (Join-Path $PSScriptRoot '_common.ps1')
Ensure-Engine
Apply-EnginePatches

if (-not (Test-Path $RunnerSrcDir) -or -not (Get-ChildItem $RunnerSrcDir -Filter '*.cpp' -File -ErrorAction SilentlyContinue)) {
    throw 'PS2Recomp/ps2xRuntime/src/runner is empty. Run .\scripts\recompile.ps1 first.'
}

if (-not $BootElfName) {
    $BootElfName = if (Test-Path $ElfNameFile) { (Get-Content $ElfNameFile -TotalCount 1).Trim() } else { 'SCUS_974.81' }
}

Write-Step 'Android SDK'
if (-not $env:ANDROID_HOME) { $env:ANDROID_HOME = 'C:\Android\android-sdk' }
if (-not (Test-Path $env:ANDROID_HOME)) { throw "Android SDK not found. Set ANDROID_HOME (currently '$env:ANDROID_HOME')." }
$env:ANDROID_SDK_ROOT = $env:ANDROID_HOME
$localProps = Join-Path $RepoRoot 'android-runner\local.properties'
if (-not (Test-Path $localProps)) {
    $escaped = $env:ANDROID_HOME -replace '\\', '\\' -replace ':', '\:'
    Write-Utf8NoBom $localProps "sdk.dir=$escaped`n"
}
Write-Host "SDK: $env:ANDROID_HOME"

$gradlew = Join-Path $RepoRoot 'android-runner\gradlew.bat'
if (-not (Test-Path $gradlew)) { throw 'android-runner/gradlew.bat is missing. Restore it from the kit repository.' }
if (-not (Get-Command java -ErrorAction SilentlyContinue)) {
    throw 'Java 17 is required (AGP 8.6.1). Install a JDK 17 and put java on PATH or set JAVA_HOME.'
}

if ($Clean) {
    Write-Step 'Cleaning Android build outputs'
    foreach ($dir in 'android-runner\app\build', 'android-runner\app\.cxx', 'android-runner\build') {
        $path = Join-Path $RepoRoot $dir
        if (Test-Path $path) { Remove-Item $path -Recurse -Force }
    }
}

$nullGsValue = if ($NullGs) { 'ON' } else { 'OFF' }
$task = if ($Configuration -eq 'Release') { 'assembleRelease' } else { 'assembleDebug' }
New-Item -ItemType Directory -Force $LogDir | Out-Null

Write-Step "Building the $Configuration APK (arm64-v8a, null GS: $nullGsValue)"
$gradleArgs = @(
    '-p', (Join-Path $RepoRoot 'android-runner'),
    $task,
    '--console', 'plain',
    "-Pps2xBootElfName=$BootElfName",
    "-Pps2xNullGs=$nullGsValue"
)
Invoke-Native $gradlew $gradleArgs

$apkName = if ($Configuration -eq 'Release') { 'app-release.apk' } else { 'app-debug.apk' }
$apk = Join-Path $RepoRoot "android-runner\app\build\outputs\apk\$(if ($Configuration -eq 'Release') { 'release' } else { 'debug' })\$apkName"
if (-not (Test-Path $apk)) { throw "Gradle finished but $apk was not produced." }

$dist = Join-Path $RepoRoot 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$distApk = Join-Path $dist "gow2-android-$($Configuration.ToLower()).apk"
Copy-Item $apk $distApk -Force
Write-Host "APK: $distApk"

if ($Install) {
    $adb = Get-Adb
    if (-not $adb) { throw 'adb not found (ANDROID_HOME/platform-tools/adb.exe).' }
    Write-Step 'Installing on the device'
    Invoke-Native $adb @('install', '-r', $distApk)
    Write-Host 'Installed. Push data with .\scripts\push-game-data.ps1, then .\scripts\run-headless.ps1 -Android'
}

Write-Step 'Done'
Write-Host 'Device data: .\scripts\push-game-data.ps1'
Write-Host 'Boot on device: .\scripts\run-headless.ps1 -Android'
