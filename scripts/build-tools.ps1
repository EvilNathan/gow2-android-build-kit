. (Join-Path $PSScriptRoot "common.ps1")

Write-Step "Building eerecomp and vurecomp"
$cmake = Require-Command cmake "Install CMake 3.24 or newer."
Require-Command ninja "Install Ninja and add it to PATH." | Out-Null

$build = Join-Path $Root "build\tools"
New-Item -ItemType Directory $build -Force | Out-Null

$configure = @(
    "-S", $Engine,
    "-B", $build,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release"
)
$clang = Get-Command clang++ -ErrorAction SilentlyContinue
if ($clang) {
    $configure += "-DCMAKE_C_COMPILER=clang"
    $configure += "-DCMAKE_CXX_COMPILER=clang++"
}
Invoke-Checked $cmake $configure
Invoke-Checked $cmake @("--build", $build, "--target", "eerecomp", "vurecomp", "--parallel")

$eerecomp = Get-Tool "eerecomp"
$vurecomp = Get-Tool "vurecomp"
Write-Host "eerecomp: $eerecomp"
Write-Host "vurecomp: $vurecomp"
