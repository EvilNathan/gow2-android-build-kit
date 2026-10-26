# Shared paths and helpers for every script in the kit. Load it with ". _common.ps1".
$ErrorActionPreference = 'Stop'

$RepoRoot      = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$RecompDir     = Join-Path $RepoRoot 'PS2Recomp'
$EnginePin     = '379fa48b1a292eb7f688ccdc202d97f5ffc09c6d'
$PatchesDir    = Join-Path $RepoRoot 'patches'
$CompatDir     = Join-Path $RepoRoot 'compat'
$BuildDir      = Join-Path $RepoRoot 'build'
$LogDir        = Join-Path $BuildDir 'logs'
$GameDir       = Join-Path $RepoRoot 'games\gow2'
$DiscDir       = Join-Path $GameDir 'disc'
$ElfNameFile   = Join-Path $GameDir 'elf-name.txt'
$IsoPathFile   = Join-Path $GameDir 'iso-path.txt'
$TemplateToml  = Join-Path $RepoRoot 'config\recomp.template.toml'
$FuncMapCsv    = Join-Path $RepoRoot 'config\funcmap.csv'
$EffectiveToml = Join-Path $BuildDir 'gow.effective.toml'
$GeneratedDir  = Join-Path $RepoRoot 'generated\gow2'
$OverridesDir  = Join-Path $RepoRoot 'overrides'
$RunnerSrcDir  = Join-Path $RecompDir 'ps2xRuntime\src\runner'
$SyncManifest  = Join-Path $BuildDir 'synced-files.txt'

function Write-Step([string]$Text) {
    Write-Host ''
    Write-Host "== $Text" -ForegroundColor Cyan
}

function Require-Command([string]$Name, [string]$Hint = '') {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $cmd) {
        $message = "Missing '$Name'."
        if ($Hint) { $message += " $Hint" }
        throw $message
    }
    return $cmd.Source
}

function Get-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installs = & $vswhere -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        foreach ($path in $installs) {
            $exe = Join-Path $path 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $exe) { return $exe }
        }
    }
    throw 'CMake not found. Install CMake 3.24+ or the Visual Studio C++ workload.'
}

# Returns the CMake generator arguments for the newest installed Visual Studio.
function Get-VsGeneratorArgs {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $line = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property catalog_productLineVersion
        if ($line) {
            $major = [int]($line.Trim())
            switch ($major) {
                15 { return @('-G', 'Visual Studio 15 2017', '-A', 'x64') }
                16 { return @('-G', 'Visual Studio 16 2019', '-A', 'x64') }
                17 { return @('-G', 'Visual Studio 17 2022', '-A', 'x64') }
                18 { return @('-G', 'Visual Studio 18 2026', '-A', 'x64') }
            }
        }
    }
    throw 'No Visual Studio installation with the C++ workload was found.'
}

function Get-GameElfPath {
    if (-not (Test-Path $ElfNameFile)) {
        throw 'The disc has not been extracted. Run: .\scripts\extract-disc.ps1 -Iso "<path>.iso"'
    }
    $name = (Get-Content $ElfNameFile -TotalCount 1).Trim()
    return Join-Path $DiscDir $name
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding $false))
}

function Invoke-Native([string]$Exe, [string[]]$Arguments) {
    # Only the exit code matters: PowerShell 5.1 aborts on stderr with 'Stop'.
    $ErrorActionPreference = 'Continue'
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Failed: $Exe $($Arguments -join ' ') (exit $LASTEXITCODE)" }
}

function Test-EngineReady {
    return (Test-Path (Join-Path $RecompDir 'CMakeLists.txt'))
}

function Ensure-Engine {
    if (Test-EngineReady) { return }

    if (-not (Test-Path (Join-Path $RepoRoot '.git'))) {
        throw "PS2Recomp/ is missing and this is not a git checkout. Clone the kit or put PS2Recomp/ at $EnginePin."
    }

    Write-Step 'Engine submodule (PS2Recomp)'
    Invoke-Native git @('-C', $RepoRoot, 'submodule', 'update', '--init', '--recursive')
    if (-not (Test-EngineReady)) {
        throw 'PS2Recomp/ is still empty after submodule update. Check network access to github.com.'
    }
}

function Apply-EnginePatches {
    if (-not (Test-Path $PatchesDir)) { return }
    $patches = Get-ChildItem $PatchesDir -Filter '*.patch' -File | Sort-Object Name
    if (-not $patches) { return }

    Write-Step 'Kit patches'
    foreach ($patch in $patches) {
        & git -C $RecompDir apply --check --ignore-whitespace $patch.FullName 2>$null
        if ($LASTEXITCODE -eq 0) {
            Invoke-Native git @('-C', $RecompDir, 'apply', '--ignore-whitespace', $patch.FullName)
            Write-Host "  applied $($patch.Name)"
        }
        else {
            & git -C $RecompDir apply --reverse --check --ignore-whitespace $patch.FullName 2>$null
            if ($LASTEXITCODE -eq 0) {
                Write-Host "  already applied $($patch.Name)"
            }
            else {
                throw "Patch $($patch.Name) does not apply to PS2Recomp at $EnginePin. Delete PS2Recomp\ and rerun setup.ps1."
            }
        }
    }
}

function Get-Adb {
    $cmd = Get-Command adb -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if ($env:ANDROID_HOME) {
        $exe = Join-Path $env:ANDROID_HOME 'platform-tools\adb.exe'
        if (Test-Path $exe) { return $exe }
    }
    return $null
}
