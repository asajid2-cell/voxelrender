# =============================================================================
# VENPOD - Run Script (PowerShell)
# =============================================================================

param(
    [string]$Config = "Release",
    [switch]$ForceSync
)

$ErrorActionPreference = "Stop"

function Write-Step { Write-Host "`n==> $args" -ForegroundColor Cyan }
function Write-Success { Write-Host "[OK] $args" -ForegroundColor Green }
function Write-Error { Write-Host "[ERROR] $args" -ForegroundColor Red }
function Write-Info { Write-Host "  $args" -ForegroundColor Gray }

$projectRoot = $PSScriptRoot
$buildDir = Join-Path $projectRoot "build"

Write-Host "VENPOD - Run Script" -ForegroundColor Magenta

# Find executable
$exePath = $null
$possiblePaths = @(
    "$buildDir\bin\$Config\VENPOD.exe",
    "$buildDir\bin\VENPOD.exe",
    "$buildDir\$Config\VENPOD.exe",
    "$buildDir\VENPOD.exe"
)

foreach ($path in $possiblePaths) {
    if (Test-Path $path) {
        $exePath = $path
        break
    }
}

if (-not $exePath) {
    Write-Error "VENPOD.exe not found! Run setup.ps1 or build.ps1 first."
    Write-Info "Searched locations:"
    foreach ($path in $possiblePaths) {
        Write-Info "  - $path"
    }
    exit 1
}

$exeDir = Split-Path $exePath -Parent

# Sync assets if needed
$assetsSource = Join-Path $projectRoot "assets"
$assetsDest = Join-Path $exeDir "assets"

if ($ForceSync -or -not (Test-Path $assetsDest)) {
    Write-Step "Syncing assets..."
    if (Test-Path $assetsSource) {
        Copy-Item -Path $assetsSource -Destination $exeDir -Recurse -Force
        Write-Success "Assets synced"
    }
}

# Launch
Write-Step "Launching VENPOD..."
Write-Info "Executable: $exePath"
Write-Info "Working Dir: $exeDir"

Push-Location $exeDir
& $exePath
$exitCode = $LASTEXITCODE
Pop-Location

if ($exitCode -ne 0) {
    Write-Error "VENPOD exited with code $exitCode"
} else {
    Write-Success "VENPOD exited normally"
}

exit $exitCode
