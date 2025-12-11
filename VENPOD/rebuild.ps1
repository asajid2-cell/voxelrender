# =============================================================================
# VENPOD - Quick Rebuild Script (PowerShell)
# Rebuilds without reconfiguring - much faster for iterative development
# =============================================================================

param(
    [string]$BuildConfig = "Release"
)

$ErrorActionPreference = "Stop"

# Color output functions
function Write-Step { Write-Host "`n==> $args" -ForegroundColor Cyan }
function Write-Success { Write-Host "[OK] $args" -ForegroundColor Green }
function Write-Error { Write-Host "[ERROR] $args" -ForegroundColor Red }
function Write-Info { Write-Host "  $args" -ForegroundColor Gray }

$startTime = Get-Date

Write-Host @"
===============================================================
              VENPOD - QUICK REBUILD
===============================================================
"@ -ForegroundColor Magenta

$projectRoot = $PSScriptRoot
$buildDir = Join-Path $projectRoot "build"

# Check if build directory exists
if (-not (Test-Path $buildDir)) {
    Write-Error "Build directory not found! Run setup.ps1 first."
    exit 1
}

# Import Visual Studio environment
Write-Step "Setting up Visual Studio environment..."
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -property installationPath

if (-not $vsPath) {
    Write-Error "Visual Studio not found!"
    exit 1
}

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
Write-Success "VS environment ready"

# Ensure Ninja is in PATH
$ninjaDir = "$env:ProgramFiles\Ninja"
if (Test-Path "$ninjaDir\ninja.exe") {
    if ($env:PATH -notlike "*$ninjaDir*") {
        $env:PATH = "$ninjaDir;$env:PATH"
    }
}

# Build
Write-Step "Building project..."
Push-Location $buildDir

& cmake --build . --config $BuildConfig --parallel
$buildResult = $LASTEXITCODE

Pop-Location

if ($buildResult -ne 0) {
    Write-Error "Build failed!"
    exit 1
}

# Find and display executable info
$exeDir = $buildDir
if (-not (Test-Path "$exeDir\VENPOD.exe")) {
    $exeDir = "$buildDir\bin"
}

if (Test-Path "$exeDir\VENPOD.exe") {
    $exeSize = (Get-Item "$exeDir\VENPOD.exe").Length / 1MB
    Write-Success "Executable: $exeDir\VENPOD.exe ($([math]::Round($exeSize, 2)) MB)"
}

$totalTime = (Get-Date) - $startTime
Write-Host "`n===============================================================" -ForegroundColor Magenta
Write-Host "  Rebuild completed in $($totalTime.TotalSeconds.ToString('F1')) seconds" -ForegroundColor Green
Write-Host "===============================================================" -ForegroundColor Magenta
