param(
    [Parameter(Mandatory)][string]$Iso,
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [switch]$SkipTools,
    [switch]$SkipDesktopRun,
    [switch]$SkipAndroid,
    [switch]$Install,
    [switch]$PushData
)

. (Join-Path $PSScriptRoot '_common.ps1')

& (Join-Path $PSScriptRoot 'setup.ps1')
if (-not $SkipTools) { & (Join-Path $PSScriptRoot 'build-tools.ps1') }
& (Join-Path $PSScriptRoot 'extract-disc.ps1') -Iso $Iso
& (Join-Path $PSScriptRoot 'recompile.ps1')

if (-not $SkipDesktopRun) {
    & (Join-Path $PSScriptRoot 'run-headless.ps1') -Seconds 20 -Build
}

if (-not $SkipAndroid) {
    & (Join-Path $PSScriptRoot 'build-android.ps1') -Configuration $Configuration -Install:$Install
    if ($PushData) { & (Join-Path $PSScriptRoot 'push-game-data.ps1') }
}
