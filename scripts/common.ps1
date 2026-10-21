Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$script:Engine = Join-Path $script:Root "engine"
$script:Generated = Join-Path $script:Root "generated\gow2"
$script:Game = Join-Path $script:Root "games\gow2"
$script:Disc = Join-Path $script:Game "disc"
$script:Tools = Join-Path $script:Root "build\tools"

function Require-Command {
    param([Parameter(Mandatory = $true)][string]$Name, [string]$Hint = "")
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $command) {
        $message = "Required command '$Name' was not found on PATH."
        if ($Hint) { $message += " $Hint" }
        throw $message
    }
    return $command.Source
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @()
    )
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "'$FilePath' exited with code $LASTEXITCODE."
    }
}

function Get-Python {
    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) { $python = Get-Command py -ErrorAction SilentlyContinue }
    if (-not $python) { throw "Python 3 is required and was not found on PATH." }
    return $python.Source
}

function Get-Tool {
    param([Parameter(Mandatory = $true)][string]$Name)
    $suffix = if ($env:OS -eq "Windows_NT") { ".exe" } else { "" }
    $path = Join-Path $script:Tools ($Name + $suffix)
    if (-not (Test-Path $path)) {
        throw "Missing $path. Run scripts\build-tools.ps1 first."
    }
    return $path
}

function Get-IsoPath {
    param([string]$Iso)
    if (-not $Iso) {
        $pointer = Join-Path $script:Game "iso.txt"
        if (Test-Path $pointer) {
            $Iso = (Get-Content $pointer -TotalCount 1).Trim()
        }
    }
    if (-not $Iso) {
        throw "No ISO given. Pass -Iso 'D:\path\God of War II (USA).iso'."
    }
    $resolved = Resolve-Path $Iso -ErrorAction SilentlyContinue
    if (-not $resolved) { throw "ISO not found: $Iso" }
    return $resolved.Path
}

function Write-Step {
    param([string]$Text)
    Write-Host ""
    Write-Host "== $Text" -ForegroundColor Cyan
}
