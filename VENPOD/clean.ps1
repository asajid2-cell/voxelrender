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

$projectRoot = (Resolve-Path -LiteralPath $PSScriptRoot).Path

function Assert-ProjectCleanPath {
    param(
        [string]$Path,
        [string]$Label
    )

    $root = [System.IO.Path]::GetFullPath($projectRoot).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $full = [System.IO.Path]::GetFullPath($Path).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $separator = [System.IO.Path]::DirectorySeparatorChar
    if ($full -eq $root) {
        throw "Refusing to clean $Label because it resolves to the project root: $full"
    }
    if (-not $full.StartsWith($root + $separator, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean $Label outside the project root: $full"
    }
}

function Remove-ProjectCleanPath {
    param(
        [string]$Path,
        [string]$Label
    )

    Assert-ProjectCleanPath -Path $Path -Label $Label
    Remove-Item -LiteralPath $Path -Recurse -Force
}

Write-Host "VENPOD - Clean Script" -ForegroundColor Magenta

# Clean build directory
$buildDir = Join-Path $projectRoot "build"
if (Test-Path $buildDir) {
    Write-Step "Removing build directory..."
    Remove-ProjectCleanPath -Path $buildDir -Label "build directory"
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
        Remove-ProjectCleanPath -Path $path -Label $file
        Write-Info "Removed: $file"
    }
}

# Optionally clean vcpkg_installed
if ($All) {
    $vcpkgInstalled = Join-Path $projectRoot "vcpkg_installed"
    if (Test-Path $vcpkgInstalled) {
        Write-Step "Removing vcpkg_installed..."
        Remove-ProjectCleanPath -Path $vcpkgInstalled -Label "vcpkg_installed"
        Write-Success "vcpkg_installed removed"
    }
}

Write-Success "Clean complete!"
Write-Info "Run .\setup.ps1 to rebuild"
