# =============================================================================
# VENPOD - Clean Script (PowerShell)
# =============================================================================

param(
    [switch]$All  # Also clean vcpkg_installed
)

$ErrorActionPreference = "Stop"

function Write-Step { Write-Host "`n==> $args" -ForegroundColor Cyan }
function Write-Success { Write-Host "[OK] $args" -ForegroundColor Green }
function Write-Info { Write-Host "  $args" -ForegroundColor Gray }

$projectRoot = $PSScriptRoot

Write-Host "VENPOD - Clean Script" -ForegroundColor Magenta

# Clean build directory
$buildDir = Join-Path $projectRoot "build"
if (Test-Path $buildDir) {
    Write-Step "Removing build directory..."
    Remove-Item -Recurse -Force $buildDir
    Write-Success "Build directory removed"
} else {
    Write-Info "Build directory not found (already clean)"
}

# Clean CMake cache files in root
$cacheFiles = @(
    "CMakeCache.txt",
    "CMakeFiles",
    "cmake_install.cmake",
    "Makefile",
    "compile_commands.json"
)

foreach ($file in $cacheFiles) {
    $path = Join-Path $projectRoot $file
    if (Test-Path $path) {
        Remove-Item -Recurse -Force $path
        Write-Info "Removed: $file"
    }
}

# Optionally clean vcpkg_installed
if ($All) {
    $vcpkgInstalled = Join-Path $projectRoot "vcpkg_installed"
    if (Test-Path $vcpkgInstalled) {
        Write-Step "Removing vcpkg_installed..."
        Remove-Item -Recurse -Force $vcpkgInstalled
        Write-Success "vcpkg_installed removed"
    }
}

Write-Success "Clean complete!"
Write-Info "Run .\setup.ps1 to rebuild"
