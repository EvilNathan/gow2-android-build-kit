param(
    [int]$Seconds = 60,
    [switch]$Android,
    [switch]$Build,
    [IntPtr]$WindowHandle = [IntPtr]::Zero
)

. (Join-Path $PSScriptRoot '_common.ps1')
New-Item -ItemType Directory -Force $LogDir | Out-Null

if ($Android) {
    $adb = Get-Adb
    if (-not $adb) { throw 'adb not found. Run .\scripts\setup.ps1 or set ANDROID_HOME.' }
    $log = Join-Path $LogDir 'android-headless.log'

    Write-Step "Launching on the device, capturing logcat for $Seconds s"
    Invoke-Native $adb @('shell', 'am', 'force-stop', 'com.gow2.android')
    Invoke-Native $adb @('logcat', '-c')
    Invoke-Native $adb @('shell', 'am', 'start', '-n', 'com.gow2.android/android.app.NativeActivity')

    $job = Start-Job -ScriptBlock {
        param($Adb, $Seconds)
        & $Adb logcat -s ps2x:V AndroidRuntime:E DEBUG:E libc:F
        Start-Sleep -Seconds $Seconds
    } -ArgumentList $adb, $Seconds
    Start-Sleep -Seconds ($Seconds + 5)
    $out = Receive-Job $job
    Remove-Job $job -Force
    $out | Set-Content -Path $log -Encoding utf8
    $out | Select-Object -Last 40
    Write-Host "Log: $log"
    return
}

$exe = Join-Path $BuildDir 'ps2xRuntime\Release\ps2EntryRunner.exe'
if ($Build -or -not (Test-Path $exe)) {
    $cmake = Get-CMake
    Write-Step 'Configuring and building the desktop runner'
    $configure = @('-S', $RecompDir, '-B', $BuildDir) + (Get-VsGeneratorArgs) + @(
        '-DPS2X_BUILD_TEST=OFF', '-DPS2X_BUILD_STUDIO=OFF'
    )
    Invoke-Native $cmake $configure
    Invoke-Native $cmake @('--build', $BuildDir, '--config', 'Release', '--target', 'ps2EntryRunner', '--parallel')
    if (-not (Test-Path $exe)) { throw "ps2EntryRunner.exe was not produced at $exe" }
}

$env:GOW2_ISO = if (Test-Path $IsoPathFile) { (Get-Content $IsoPathFile -TotalCount 1).Trim() } else { '' }
if (-not $env:GOW2_ISO -or -not (Test-Path $env:GOW2_ISO)) { throw 'No ISO recorded. Run .\scripts\extract-disc.ps1 -Iso "<path>.iso".' }

$log = Join-Path $LogDir 'desktop-headless.log'
$outLog = Join-Path $LogDir 'desktop-headless.out.log'
$errLog = Join-Path $LogDir 'desktop-headless.err.log'

Write-Step "Running the desktop runner for $Seconds s (ISO: $env:GOW2_ISO)"
$proc = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru -NoNewWindow `
    -RedirectStandardOutput $outLog -RedirectStandardError $errLog
if (-not $proc.WaitForExit($Seconds * 1000)) {
    & taskkill /PID $proc.Id /T /F | Out-Null
    Write-Host 'Stopped (timeout).'
}
else {
    Write-Host "Exited early with code $($proc.ExitCode)."
}
Get-Content $outLog -Tail 40 -ErrorAction SilentlyContinue
Get-Content $errLog -Tail 20 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ -ForegroundColor DarkYellow }
Write-Host "Logs: $outLog / $errLog"
