# Windows build helper (MSYS2 / UCRT64 g++).
#
# The Makefile is the canonical build entry (used inside Docker). On Windows we
# invoke g++ directly so a missing `make` cannot block local iteration.
#
#   .\build.ps1            -> build\minivectordb.exe
#   .\build.ps1 -Clean
param(
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = Join-Path $root "build"

if ($Clean -and (Test-Path $build)) {
    Remove-Item -Recurse -Force $build
}
New-Item -ItemType Directory -Force -Path $build | Out-Null

$sources = Get-ChildItem -Path (Join-Path $root "src") -Recurse -Filter *.cpp |
    ForEach-Object { $_.FullName }
if (-not $sources) {
    throw "no sources found under src/"
}

$output = Join-Path $build "minivectordb.exe"
$arguments = @("-std=c++17", "-O2", "-Wall", "-Wextra", "-Isrc", "-o", $output) + $sources

Push-Location $root
try {
    & g++ @arguments
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}

Write-Host "built $output"
