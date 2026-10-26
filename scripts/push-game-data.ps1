param(
    [switch]$SkipIso,
    [string]$BootElfName = ''
)

. (Join-Path $PSScriptRoot '_common.ps1')

$adb = Get-Adb
if (-not $adb) { throw 'adb not found. Run .\scripts\setup.ps1 or set ANDROID_HOME.' }
$devices = & $adb devices | Select-Object -Skip 1 | Where-Object { $_ -match '\sdevice$' }
if (-not $devices) { throw 'No Android device connected (adb devices shows none).' }

$elfPath = Get-GameElfPath
if (-not $BootElfName) { $BootElfName = (Get-Content $ElfNameFile -TotalCount 1).Trim() }
$remoteDir = '/sdcard/Android/data/com.gow2.android/files'

Write-Step 'Creating the app files directory'
Invoke-Native $adb @('shell', 'mkdir', '-p', $remoteDir)

Write-Step "Pushing $BootElfName"
Invoke-Native $adb @('push', $elfPath, "$remoteDir/$BootElfName")

if (-not $SkipIso) {
    if (-not (Test-Path $IsoPathFile)) { throw 'iso-path.txt is missing. Run .\scripts\extract-disc.ps1 -Iso "<path>.iso" first.' }
    $iso = (Get-Content $IsoPathFile -TotalCount 1).Trim()
    if (-not (Test-Path $iso)) { throw "The ISO recorded in iso-path.txt no longer exists: $iso" }
    $size = (Get-Item $iso).Length
    Write-Step "Pushing the ISO ($([math]::Round($size / 1GB, 2)) GB) - this takes a while"
    Invoke-Native $adb @('push', $iso, "$remoteDir/gow2.iso")
}

Write-Step 'Done'
Write-Host "Data: $remoteDir"
Write-Host 'Next: .\scripts\run-headless.ps1 -Android'
