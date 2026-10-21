param(
    [string]$Iso = "",
    [int]$Frames = 2400,
    [int]$ExploreRounds = 3,
    [switch]$SkipExplore
)

. (Join-Path $PSScriptRoot "common.ps1")

$isoPath = Get-IsoPath $Iso
$eerecomp = Get-Tool "eerecomp"
$vurecomp = Get-Tool "vurecomp"
$elf = Join-Path $Disc "SCUS_974.81"
if (-not (Test-Path $elf)) {
    throw "Missing $elf. Run scripts\extract-disc.ps1 -Iso '$isoPath' first."
}

New-Item -ItemType Directory $Generated -Force | Out-Null
New-Item -ItemType Directory $Game -Force | Out-Null
$entries = Join-Path $Game "entries.txt"
if (-not (Test-Path $entries)) { New-Item -ItemType File $entries | Out-Null }

function Invoke-Translate {
    Write-Step "Translating Emotion Engine code"
    Invoke-Checked $eerecomp @($elf, "--entries", $entries, "--out", $Generated)
    $units = @(Get-ChildItem $Generated -Filter "unit_*.cpp" -ErrorAction SilentlyContinue)
    if ($units.Count -eq 0) { throw "eerecomp produced no generated units in $Generated." }
    Write-Host "Generated $($units.Count) C++ units."
}

function Add-MissedEntries {
    param([string]$Misses)
    if (-not (Test-Path $Misses)) { return 0 }
    $known = @{}
    foreach ($line in Get-Content $entries -ErrorAction SilentlyContinue) {
        foreach ($token in ($line -split "\s+")) {
            if ($token) { $known[$token.ToLowerInvariant()] = $true }
        }
    }
    $added = 0
    $newLines = New-Object System.Collections.Generic.List[string]
    foreach ($line in Get-Content $Misses -ErrorAction SilentlyContinue) {
        foreach ($token in ($line -split "\s+")) {
            if (-not $token) { continue }
            $key = $token.ToLowerInvariant()
            if (-not $known.ContainsKey($key)) {
                $known[$key] = $true
                $newLines.Add($token)
                $added++
            }
        }
    }
    if ($newLines.Count -gt 0) {
        Add-Content -Path $entries -Value $newLines -Encoding ASCII
    }
    return $added
}

function Write-EmptyVuTable {
    $table = Join-Path $Generated "vu1_table.cpp"
    Set-Content -Path $table -Value "// No VU programs were recorded; the runtime will interpret them.`nvoid register_generated_vu() {}" -Encoding ASCII
}

function Build-Native {
    Write-Step "Building the headless native runner"
    $cmake = Require-Command cmake
    $build = Join-Path $Root "build\native-gow2"
    New-Item -ItemType Directory $build -Force | Out-Null
    $configure = @(
        "-S", $Engine,
        "-B", $build,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DPS2_GENERATED=$Generated"
    )
    $clang = Get-Command clang++ -ErrorAction SilentlyContinue
    if ($clang) {
        $configure += "-DCMAKE_C_COMPILER=clang"
        $configure += "-DCMAKE_CXX_COMPILER=clang++"
    }
    $log = Join-Path $build "build.log"
    Invoke-Checked $cmake $configure *> $log
    Invoke-Checked $cmake @("--build", $build, "--target", "ps2run", "--parallel") *>> $log
    $suffix = if ($env:OS -eq "Windows_NT") { ".exe" } else { "" }
    $runner = Join-Path $build ("ps2run" + $suffix)
    if (-not (Test-Path $runner)) {
        Get-Content $log -Tail 40 -ErrorAction SilentlyContinue
        throw "ps2run was not produced in $build."
    }
    return $runner
}

Invoke-Translate

if ($SkipExplore) {
    Write-Step "Skipping VU exploration"
    Write-EmptyVuTable
    Write-Host "The runtime will interpret VU programs that were not compiled."
    return
}

$runner = Build-Native
$vuDir = Join-Path $Game "vu1"
$shots = Join-Path $Game "shots"
$missesPath = Join-Path $Game "misses.txt"
$memcard = Join-Path $Game "explore.ps2"
Remove-Item $vuDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $vuDir | Out-Null
New-Item -ItemType Directory $shots -Force | Out-Null

$press = @()
for ($f = 300; $f -lt $Frames; $f += 300) {
    $button = if ((($f / 300) % 2) -eq 1) { "start" } else { "cross" }
    $press += @("--press", "${f}:${button}:8")
}

for ($round = 1; $round -le $ExploreRounds; $round++) {
    Write-Step "Exploring for VU programs (round $round/$ExploreRounds)"
    Remove-Item $missesPath -Force -ErrorAction SilentlyContinue
    Remove-Item $memcard -Force -ErrorAction SilentlyContinue
    $env:PS2_VU_RECORD = $vuDir
    $env:PS2_GPU_LOW_POWER = "1"
    try {
        $runArgs = @(
            $elf,
            "--disc", $isoPath,
            "--misses", $missesPath,
            "--memcard", $memcard,
            "--frames", "$Frames",
            "--frames-dir", $shots,
            "--frame-every", "300"
        ) + $press
        & $runner @runArgs
        $runCode = $LASTEXITCODE
    } finally {
        Remove-Item Env:PS2_VU_RECORD -ErrorAction SilentlyContinue
        Remove-Item Env:PS2_GPU_LOW_POWER -ErrorAction SilentlyContinue
    }
    if ($runCode -ne 0) {
        Write-Warning "Exploration exited with code $runCode. Check the log output above."
    }
    $added = Add-MissedEntries $missesPath
    Write-Host "New entry points: $added"
    if ($added -le 0) { break }
    Invoke-Translate
    $runner = Build-Native
}

$recorded = @(Get-ChildItem $vuDir -Filter "*.bin" -ErrorAction SilentlyContinue)
if ($recorded.Count -gt 0) {
    Write-Step "Compiling $($recorded.Count) recorded VU programs"
    Remove-Item (Join-Path $Generated "vu1_*.cpp") -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $Generated "vu1_table.cpp") -Force -ErrorAction SilentlyContinue
    Invoke-Checked $vurecomp @($vuDir, $Generated)
} else {
    Write-Step "No VU programs were recorded"
    Write-EmptyVuTable
    Write-Host "The game will still run, but VU microcode will be interpreted."
}

# Rebuild once so the final native runner uses the generated VU code too.
Build-Native | Out-Null

Write-Step "Recompilation complete"
Write-Host "Generated code: $Generated"
Write-Host "Next: scripts\build-android.ps1"
