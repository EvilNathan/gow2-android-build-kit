param(
    # Pinned Mesa/Turnip build published for AdrenoTools. Bump these three
    # values together after a package has been tried on a device.
    [string]$Tag = "v26.0.0-rc08",
    [string]$Asset = "Turnip_v26.0.0_R8.zip",
    [string]$Sha256 = "e634db0f929e2205e95511c769071817d0390180ec72c8e690bc76375e813715",
    [string]$Destination = "",
    [switch]$Force
)

. (Join-Path $PSScriptRoot "common.ps1")

$repo = "K11MCH1/AdrenoToolsDrivers"
$url = "https://github.com/$repo/releases/download/$Tag/$Asset"
if (-not $Destination) { $Destination = Join-Path $Root "dist\turnip" }
$Destination = [System.IO.Path]::GetFullPath($Destination)

$zip = Join-Path $Destination $Asset
$extract = Join-Path $Destination ([System.IO.Path]::GetFileNameWithoutExtension($Asset))

function Get-Sha256 {
    param([string]$Path)
    return (Get-FileHash -Algorithm SHA256 -Path $Path).Hash.ToLowerInvariant()
}

Write-Step "Turnip $Tag"
Write-Host "Source: $url"

if (-not (Test-Path $Destination)) { New-Item -ItemType Directory -Path $Destination -Force | Out-Null }

$needDownload = $true
if ((Test-Path $zip) -and -not $Force) {
    $existing = Get-Sha256 $zip
    if ($existing -eq $Sha256) {
        Write-Host "Archive already present and verified."
        $needDownload = $false
    } else {
        Write-Host "Existing archive hash $existing does not match the pin; downloading again."
    }
}

if ($needDownload) {
    $partial = "$zip.part"
    Remove-Item $partial -Force -ErrorAction SilentlyContinue
    try {
        Invoke-WebRequest -Uri $url -OutFile $partial -UseBasicParsing
    } catch {
        Remove-Item $partial -Force -ErrorAction SilentlyContinue
        throw "Download failed: $($_.Exception.Message)"
    }
    $actual = Get-Sha256 $partial
    if ($actual -ne $Sha256) {
        Remove-Item $partial -Force -ErrorAction SilentlyContinue
        throw "SHA256 mismatch for $Asset (expected $Sha256, got $actual). Refusing to unpack."
    }
    Move-Item $partial $zip -Force
    Write-Host "Downloaded and verified ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)."
}

if ($Force -or -not (Test-Path $extract)) {
    if (Test-Path $extract) { Remove-Item $extract -Recurse -Force }
    Expand-Archive -Path $zip -DestinationPath $extract -Force
}

$libraries = @(Get-ChildItem $extract -Recurse -File -Include *.so)
if ($libraries.Count -eq 0) { throw "The archive did not contain a .so driver library." }

Write-Host ""
Write-Host "Driver package: $extract"
foreach ($library in $libraries) { Write-Host "  $($library.Name)  ($([math]::Round($library.Length / 1KB)) KB)" }

Write-Host ""
Write-Host "Install on the phone:" -ForegroundColor Cyan
Write-Host "  1. adb push `"$zip`" /sdcard/Download/"
Write-Host "  2. Open God of War II -> SETTINGS -> SELECT DRIVER -> pick the zip."
Write-Host "  3. Start the game; the launcher shows the active driver. USE SYSTEM DRIVER reverts."
Write-Host ""
Write-Host "Adreno only: Mali, PowerVR and other vendors always use the stock driver." -ForegroundColor Yellow
Write-Host "No build in this kit has been validated on hardware yet; keep the system driver unless"
Write-Host "the custom one is measurably faster and stable (check adb logcat -s GoW2Stats)."
