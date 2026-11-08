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

function Set-EngineLfCheckout {
    # The kit patches are LF. A clone made with core.autocrlf=true checks the
    # engine out as CRLF, which makes `git apply` fail on hunks with accented
    # comments. Force LF for this checkout; existing files keep their bytes
    # until they are refreshed from the index (see Apply-EnginePatches).
    if (-not (Test-Path (Join-Path $RecompDir '.git'))) { return }
    Invoke-GitQuiet @('-C', $RecompDir, 'config', 'core.autocrlf', 'false') | Out-Null
}

# Runs git and returns only the exit code: PowerShell 5.1 turns native stderr
# into a terminating error under $ErrorActionPreference = 'Stop', and `git apply
# --check` uses stderr for the "does not apply" case.
function Invoke-GitQuiet([string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & git @Arguments 2>&1 | Out-Null
        return $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
    }
}

function Apply-EnginePatches {
    if (-not (Test-Path $PatchesDir)) { return }
    $patches = Get-ChildItem $PatchesDir -Filter '*.patch' -File | Sort-Object Name
    if (-not $patches) { return }

    Set-EngineLfCheckout

    # The patches are ordered and each one applies on top of the previous, so a
    # partially patched tree cannot be inspected patch by patch. Instead, drop
    # any tracked engine modification (the kit patches are the source of truth;
    # untracked files such as generated/ survive) and rebuild the stack from the
    # pinned revision. checkout-index also rewrites a CRLF clone as LF.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $tracked = & git -C $RecompDir status --porcelain --untracked-files=no 2>$null
        if ($tracked) {
            Write-Host '  resetting tracked engine changes'
            & git -C $RecompDir checkout -- . 2>$null | Out-Null
        }
        & git -C $RecompDir checkout-index -f -a 2>$null | Out-Null

        # Untracked new files introduced by patches must be removed so that
        # re-applying the patch stack from the pinned commit does not fail.
        foreach ($patch in $patches) {
            $lines = Get-Content $patch.FullName
            for ($i = 0; $i -lt $lines.Count; $i++) {
                if ($lines[$i] -match '^diff --git a/.*? b/(.+)$') {
                    $rel = $Matches[1]
                    if ($i + 1 -lt $lines.Count -and $lines[$i + 1] -match '^new file mode') {
                        $target = Join-Path $RecompDir $rel
                        if (Test-Path $target) {
                            Remove-Item -Force $target 2>$null
                        }
                    }
                }
            }
        }
    }
    finally { $ErrorActionPreference = $previous }

    Write-Step 'Kit patches'
    foreach ($patch in $patches) {
        $check = Invoke-GitQuiet @('-C', $RecompDir, 'apply', '--check', '--ignore-whitespace', $patch.FullName)
        if ($check -ne 0) {
            throw "Patch $($patch.Name) does not apply to PS2Recomp at $EnginePin. Delete PS2Recomp\ and rerun setup.ps1."
        }
        Invoke-Native git @('-C', $RecompDir, 'apply', '--ignore-whitespace', $patch.FullName)
        Write-Host "  applied $($patch.Name)"
    }

    if (Test-Path (Join-Path $GeneratedDir 'register_functions.cpp')) {
        Sync-RunnerSources
    }
}

function Sync-RunnerSources {
    if (-not (Test-Path $GeneratedDir) -or -not (Get-ChildItem $GeneratedDir -Filter '*.cpp' -File -ErrorAction SilentlyContinue)) {
        Write-Warning "generated/ is empty ($GeneratedDir). Run .\scripts\recompile.ps1 first."
        return
    }

    Write-Step 'Syncing generated code and overrides'
    New-Item -ItemType Directory -Force (Split-Path $SyncManifest) | Out-Null
    New-Item -ItemType Directory -Force $RunnerSrcDir | Out-Null

    $md5 = [System.Security.Cryptography.MD5]::Create()
    $testSameContent = {
        param([string]$a, [string]$b)
        $fa = Get-Item -LiteralPath $a
        $fb = Get-Item -LiteralPath $b -ErrorAction SilentlyContinue
        if (-not $fb -or $fa.Length -ne $fb.Length) { return $false }
        $ha = $md5.ComputeHash([IO.File]::ReadAllBytes($a))
        $hb = $md5.ComputeHash([IO.File]::ReadAllBytes($b))
        return [Convert]::ToBase64String($ha) -eq [Convert]::ToBase64String($hb)
    }

    $copied = New-Object System.Collections.Generic.List[string]
    $changed = 0
    $sources = @(Get-ChildItem $GeneratedDir -File) +
               @(Get-ChildItem $OverridesDir -File | Where-Object { $_.Extension -in '.cpp', '.h' })
    foreach ($file in $sources) {
        $dest = Join-Path $RunnerSrcDir $file.Name
        if (-not (& $testSameContent $file.FullName $dest)) {
            Copy-Item $file.FullName $dest -Force
            (Get-Item $dest).LastWriteTime = Get-Date
            $changed++
        }
        $copied.Add($dest)
    }
    $removed = 0
    if (Test-Path $SyncManifest) {
        $current = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        foreach ($path in $copied) { [void]$current.Add($path) }
        foreach ($old in (Get-Content $SyncManifest)) {
            if ($old -and -not $current.Contains($old) -and (Test-Path -LiteralPath $old)) {
                Remove-Item -LiteralPath $old -Force
                $removed++
            }
        }
    }
    Set-Content -Path $SyncManifest -Value $copied -Encoding utf8
    Write-Host "Runner: $($copied.Count) files, $changed updated, $removed removed."
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
