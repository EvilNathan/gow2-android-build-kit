param(
    [switch]$ForceEngine
)

. (Join-Path $PSScriptRoot '_common.ps1')

Write-Step 'Checking prerequisites'
Require-Command git 'Install Git for Windows and restart the shell.' | Out-Null
Require-Command cmake 'Install CMake 3.24 or newer.' | Out-Null
$py = Get-Command python -ErrorAction SilentlyContinue
$py = if ($py) { $py.Source } else { (Require-Command py 'Install Python 3 (python or py launcher).') }

if (-not (Test-Path (Join-Path $RepoRoot '.git'))) {
    throw 'setup.ps1 must run inside the kit git checkout.'
}

if ($ForceEngine -and (Test-Path $RecompDir)) {
    Write-Step 'Removing the engine checkout'
    Remove-Item $RecompDir -Recurse -Force
}

Write-Step 'Engine submodule (PS2Recomp)'
if (-not (Test-Path (Join-Path $RecompDir '.git'))) {
    if (Test-Path $RecompDir) { Remove-Item $RecompDir -Recurse -Force }
    Invoke-Native git @('-C', $RepoRoot, 'submodule', 'update', '--init', '--recursive')
}
if (-not (Test-Path (Join-Path $RecompDir '.git'))) {
    throw "Could not initialize $RecompDir. Check network access to github.com."
}

$head = (& git -C $RecompDir rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot read the PS2Recomp revision.' }
if ($head -ne $EnginePin) {
    Write-Host "PS2Recomp is at $head; checking out $EnginePin."
    & git -C $RecompDir cat-file -e "$EnginePin^{commit}" 2>$null
    if ($LASTEXITCODE -ne 0) {
        & git -C $RecompDir fetch origin $EnginePin
        if ($LASTEXITCODE -ne 0) {
            Invoke-Native git @('-C', $RecompDir, 'fetch', 'origin')
        }
    }
    Invoke-Native git @('-C', $RecompDir, 'checkout', '--quiet', $EnginePin)
}

Apply-EnginePatches

Write-Step 'Done'
Write-Host "Engine:  $RecompDir ($EnginePin)"
Write-Host 'Next:    .\scripts\extract-disc.ps1 -Iso "<your God of War II (USA).iso>"'
