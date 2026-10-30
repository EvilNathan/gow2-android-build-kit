param(
    [string]$Apk = '',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug'
)

# Static checks on the built APK: packaging, ABI, 16 KB page alignment and the
# launcher activity. No device is needed; run it after build-android.ps1.
. (Join-Path $PSScriptRoot '_common.ps1')

if (-not $Apk) {
    $name = if ($Configuration -eq 'Release') { 'app-release.apk' } else { 'app-debug.apk' }
    $dir = if ($Configuration -eq 'Release') { 'release' } else { 'debug' }
    $Apk = Join-Path $RepoRoot "android-runner\app\build\outputs\apk\$dir\$name"
}
if (-not (Test-Path $Apk)) { throw "APK not found: $Apk. Run .\scripts\build-android.ps1 first." }
$Apk = (Resolve-Path $Apk).Path

if (-not $env:ANDROID_HOME) { $env:ANDROID_HOME = 'C:\Android\android-sdk' }
if (-not (Test-Path $env:ANDROID_HOME)) { throw "Android SDK not found. Set ANDROID_HOME." }

$failures = New-Object System.Collections.Generic.List[string]
function Check([string]$Name, [bool]$Ok, [string]$Detail) {
    if ($Ok) {
        Write-Host ("  PASS  {0}  {1}" -f $Name, $Detail) -ForegroundColor Green
    }
    else {
        Write-Host ("  FAIL  {0}  {1}" -f $Name, $Detail) -ForegroundColor Red
        $failures.Add($Name)
    }
}

Write-Step "APK: $Apk"
$size = (Get-Item $Apk).Length
Check 'apk-size' ($size -gt 20MB) "$([math]::Round($size / 1MB, 1)) MB"

# ---------------------------------------------------------------- badging
$aapt2 = Get-ChildItem (Join-Path $env:ANDROID_HOME 'build-tools') -Directory |
    Sort-Object { [version]($_.Name -replace '[^0-9.]', '') } -Descending |
    ForEach-Object { Join-Path $_.FullName 'aapt2.exe' } |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $aapt2) { throw 'aapt2.exe not found in ANDROID_HOME/build-tools.' }

$badging = & $aapt2 dump badging $Apk 2>&1
$packageLine = $badging | Where-Object { $_ -match "^package:" } | Select-Object -First 1
$launchLine = $badging | Where-Object { $_ -match "launchable-activity:" } | Select-Object -First 1
$nativeLine = $badging | Where-Object { $_ -match "native-code:" } | Select-Object -First 1

Check 'package' ([bool]($packageLine -match "name='com\.gow2\.android'")) $packageLine
Check 'version' ([bool]($packageLine -match "versionName='([^']+)'")) "versionName=$($Matches[1])"
Check 'launcher' ([bool]($launchLine -match "name='com\.gow2\.android\.MainActivity'")) $launchLine
Check 'abi' ([bool]($nativeLine -match "arm64-v8a" -and $nativeLine -notmatch 'x86|armeabi')) $nativeLine

$sdkLine = $badging | Where-Object { $_ -match '^minSdkVersion:' } | Select-Object -First 1
Check 'minSdk-28' ([bool]($sdkLine -match "minSdkVersion:'28'")) $sdkLine

# --------------------------------------------------------------- contents
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($Apk)
try {
    $native = @($zip.Entries | Where-Object { $_.FullName -match '^lib/' })
    $abis = @($native | ForEach-Object { ($_.FullName -split '/')[1] } | Sort-Object -Unique)
    Check 'only-arm64' ($abis.Count -eq 1 -and $abis[0] -eq 'arm64-v8a') ($abis -join ',')

    $runner = $native | Where-Object { $_.FullName -eq 'lib/arm64-v8a/libps2EntryRunner.so' }
    Check 'runner-so' ($null -ne $runner) $(if ($runner) { "$([math]::Round($runner.Length / 1MB, 1)) MB" } else { 'missing' })

    # Drop the library on disk for readelf (it lives inside the zip).
    $readelf = Join-Path $env:ANDROID_NDK_HOME 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe'
    if (-not (Test-Path $readelf) -and $env:ANDROID_HOME) {
        $readelf = Get-ChildItem (Join-Path $env:ANDROID_HOME 'ndk') -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe' } |
            Where-Object { Test-Path $_ } | Select-Object -First 1
    }
    if ($runner -and $readelf) {
        $temp = Join-Path $env:TEMP ('gow2-' + [guid]::NewGuid().ToString('N') + '.so')
        try {
            [IO.Compression.ZipFileExtensions]::ExtractToFile($runner, $temp, $true)
            $headers = & $readelf -lW $temp 2>&1
            $loads = @($headers | Where-Object { $_ -match '^\s*LOAD' })
            $aligned = @($loads | Where-Object { $_ -match '0x(4000|10000|20000)\s*$' })
            Check 'load-segments' ($loads.Count -gt 0 -and $aligned.Count -eq $loads.Count) "$($aligned.Count)/$($loads.Count) LOAD at >=16 KB"

            $machine = (& $readelf -hW $temp 2>&1 | Where-Object { $_ -match 'Machine:' } | Select-Object -First 1)
            Check 'machine' ($machine -match 'AArch64') $machine
        }
        finally {
            if (Test-Path $temp) { Remove-Item $temp -Force }
        }
    }
    else {
        Write-Host '  SKIP  readelf (native library or NDK toolchain unavailable)' -ForegroundColor Yellow
    }
}
finally {
    $zip.Dispose()
}

Write-Step 'Summary'
if ($failures.Count -gt 0) {
    Write-Host "FAILED: $($failures -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host "All APK checks passed: $Apk" -ForegroundColor Green
