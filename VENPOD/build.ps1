# =============================================================================
# VENPOD - Quick Build Script (PowerShell)
# =============================================================================

param(
    [string]$Config = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

function Write-Step { Write-Host "`n==> $args" -ForegroundColor Cyan }
function Write-Success { Write-Host "[OK] $args" -ForegroundColor Green }
function Write-Error { Write-Host "[ERROR] $args" -ForegroundColor Red }
function Write-Info { Write-Host "  $args" -ForegroundColor Gray }

$projectRoot = $PSScriptRoot
$buildDir = Join-Path $projectRoot "build"

Write-Host "VENPOD - Build Script" -ForegroundColor Magenta

# Check if build directory exists
if (-not (Test-Path $buildDir)) {
    Write-Error "Build directory not found! Run setup.ps1 first."
    exit 1
}

# Import VS environment
Write-Step "Importing Visual Studio environment..."
try {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath = & $vswhere -latest -property installationPath
    $vsDevCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"

    $tempFile = [System.IO.Path]::GetTempFileName()
    cmd /c " `"$vsDevCmd`" -arch=amd64 -host_arch=amd64 > NUL && set > `"$tempFile`" "

    Get-Content $tempFile | ForEach-Object {
        if ($_ -match "^(.*?)=(.*)$") {
            $name = $matches[1]; $value = $matches[2]
            if ($name -match "^(PATH|INCLUDE|LIB|LIBPATH|VC|WindowsSDK)") {
                Set-Item -Path "env:$name" -Value $value
            }
        }
    }
    Remove-Item $tempFile -Force
    Write-Success "VS environment loaded"
} catch {
    Write-Error "Failed to load VS environment"
    exit 1
}

# Ensure Ninja is in PATH
$ninjaDir = "$env:ProgramFiles\Ninja"
if (Test-Path "$ninjaDir\ninja.exe") {
    if ($env:PATH -notlike "*$ninjaDir*") {
        $env:PATH = "$ninjaDir;$env:PATH"
    }
}

# Optional clean
if ($Clean) {
    Write-Step "Cleaning build..."
    Push-Location $buildDir
    & ninja clean
    Pop-Location
}

# Build
Write-Step "Building ($Config)..."
$startTime = Get-Date

Push-Location $buildDir
& cmake --build . --config $Config --parallel
$buildResult = $LASTEXITCODE
Pop-Location

if ($buildResult -ne 0) {
    Write-Error "Build failed!"
    exit 1
}

$buildTime = (Get-Date) - $startTime

# Report
$exePath = "$buildDir\bin\VENPOD.exe"
if (-not (Test-Path $exePath)) {
    $exePath = "$buildDir\VENPOD.exe"
}

if (Test-Path $exePath) {
    $exeSize = (Get-Item $exePath).Length / 1MB
    Write-Success "Build completed in $($buildTime.TotalSeconds.ToString('F1'))s"
    Write-Info "Executable: $exePath"
    Write-Info "Size: $([math]::Round($exeSize, 2)) MB"
} else {
    Write-Success "Build completed in $($buildTime.TotalSeconds.ToString('F1'))s"
}
