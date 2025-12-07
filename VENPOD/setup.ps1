# =============================================================================
# VENPOD - Automated Setup Script (PowerShell)
# High-Performance Voxel Physics Engine
# =============================================================================

param(
    [switch]$SkipVcpkg,
    [switch]$SkipBuild,
    [string]$BuildConfig = "Release"
)

$ErrorActionPreference = "Stop"

# Color output functions
function Write-Step { Write-Host "`n==> $args" -ForegroundColor Cyan }
function Write-Success { Write-Host "[OK] $args" -ForegroundColor Green }
function Write-Warning { Write-Host "[WARN] $args" -ForegroundColor Yellow }
function Write-Error { Write-Host "[ERROR] $args" -ForegroundColor Red }
function Write-Info { Write-Host "  $args" -ForegroundColor Gray }

$startTime = Get-Date

Write-Host @"
===============================================================
              VENPOD - SETUP SCRIPT
     High-Performance Voxel Physics Engine v0.1.0
          Target: 100M+ Active Voxels @ 60 FPS
===============================================================
"@ -ForegroundColor Magenta

# Check if running as Administrator
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Info "Note: Not running as Administrator. Some operations may require elevation."
}

$projectRoot = $PSScriptRoot

# =============================================================================
# STEP 1: Check Prerequisites & Tools
# =============================================================================
Write-Step "Checking prerequisites..."

# Check for CMake
try {
    $cmakeVersion = & cmake --version 2>&1 | Select-String -Pattern "version (\d+\.\d+)" | ForEach-Object { $_.Matches.Groups[1].Value }
    Write-Success "CMake found: version $cmakeVersion"
} catch {
    Write-Error "CMake not found! Install from https://cmake.org/download/"
    exit 1
}

# Check for Git
try {
    $gitVersion = & git --version 2>&1
    Write-Success "Git found: $gitVersion"
} catch {
    Write-Error "Git not found!"
    exit 1
}

# Check for Visual Studio / MSBuild
try {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath = & $vswhere -latest -property installationPath

    if (-not $vsPath) { throw "Visual Studio not found" }
    Write-Success "Visual Studio found at: $vsPath"

    $vsDevCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"

    Write-Info "Importing VS environment..."
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
    Write-Success "Visual Studio environment imported"

} catch {
    Write-Error "Could not detect Visual Studio! Ensure C++ workload is installed."
    exit 1
}

# =============================================================================
# STEP 2: Ensure Ninja Build Tool
# =============================================================================
function Ensure-Ninja {
    $ninjaDir = "$env:ProgramFiles\Ninja"
    $ninjaExe = "$ninjaDir\ninja.exe"

    if (-not (Test-Path $ninjaExe)) {
        Write-Info "Installing Ninja build tool..."
        mkdir $ninjaDir -ErrorAction SilentlyContinue | Out-Null
        Invoke-WebRequest -Uri "https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip" -OutFile "$env:TEMP\ninja.zip"
        Expand-Archive "$env:TEMP\ninja.zip" -DestinationPath $ninjaDir -Force
    }

    # Add to PATH if missing
    if ($env:PATH -notlike "*$ninjaDir*") {
        $env:PATH = "$ninjaDir;$env:PATH"
    }

    Write-Success "Ninja build tool ready at $ninjaExe"
    return $ninjaExe
}

$ninjaExe = Ensure-Ninja

# =============================================================================
# STEP 3: Setup vcpkg
# =============================================================================
if (-not $SkipVcpkg) {
    Write-Step "Setting up vcpkg..."

    # Use standalone vcpkg at C:\vcpkg
    if (Test-Path "C:\vcpkg\vcpkg.exe") {
        Write-Info "Using existing vcpkg at C:\vcpkg"
        $vcpkgRoot = "C:\vcpkg"
        $env:VCPKG_ROOT = "C:\vcpkg"
    } else {
        Write-Info "Installing vcpkg to C:\vcpkg..."
        Push-Location C:\
        git clone https://github.com/Microsoft/vcpkg.git
        Set-Location vcpkg
        .\bootstrap-vcpkg.bat
        Pop-Location
        $vcpkgRoot = "C:\vcpkg"
        $env:VCPKG_ROOT = "C:\vcpkg"
    }
    Write-Success "vcpkg found at: $vcpkgRoot"

    Write-Step "Installing dependencies..."
    $packages = @(
        "sdl3:x64-windows",
        "nlohmann-json:x64-windows",
        "spdlog:x64-windows",
        "directx-headers:x64-windows",
        "glm:x64-windows"
    )

    Push-Location $vcpkgRoot
    foreach ($package in $packages) {
        Write-Info "Installing $package..."
        & .\vcpkg install $package
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Failed to install $package"
            Pop-Location
            exit 1
        }
    }
    Pop-Location
    Write-Success "All dependencies installed!"
}
else {
    # When skipping vcpkg installation, infer vcpkg root
    if (-not $vcpkgRoot) {
        if ($env:VCPKG_ROOT) {
            $vcpkgRoot = $env:VCPKG_ROOT
        } else {
            $vcpkgRoot = "C:\vcpkg"
        }
    }
}

# =============================================================================
# STEP 4: Download ImGui (if not present)
# =============================================================================
Write-Step "Checking ImGui..."
$imguiDir = Join-Path $projectRoot "vendor\imgui"
$imguiMain = Join-Path $imguiDir "imgui.h"

if (-not (Test-Path $imguiMain)) {
    Write-Info "Downloading Dear ImGui..."
    $imguiZip = "$env:TEMP\imgui.zip"
    $imguiVersion = "v1.91.6"
    Invoke-WebRequest -Uri "https://github.com/ocornut/imgui/archive/refs/tags/$imguiVersion.zip" -OutFile $imguiZip

    # Extract to temp first
    $tempExtract = "$env:TEMP\imgui-extract"
    if (Test-Path $tempExtract) { Remove-Item -Recurse -Force $tempExtract }
    Expand-Archive $imguiZip -DestinationPath $tempExtract -Force

    # Move contents to vendor/imgui
    $extractedFolder = Get-ChildItem $tempExtract | Select-Object -First 1
    if (Test-Path $imguiDir) { Remove-Item -Recurse -Force $imguiDir }
    Move-Item $extractedFolder.FullName $imguiDir

    Remove-Item $imguiZip -Force
    Remove-Item -Recurse -Force $tempExtract -ErrorAction SilentlyContinue
    Write-Success "ImGui downloaded to vendor/imgui"
} else {
    Write-Success "ImGui already present"
}

# =============================================================================
# STEP 5: Configure & Build
# =============================================================================
Write-Step "Configuring CMake build..."
$buildDir = Join-Path $projectRoot "build"
$toolchainFile = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"

Write-Info "Ninja Path: $ninjaExe"
Write-Info "Toolchain:  $toolchainFile"

# Clean build directory if it exists
if (Test-Path $buildDir) {
    try {
        Remove-Item -Recurse -Force $buildDir -ErrorAction Stop
    } catch {
        Write-Info "Build directory is in use; reusing existing folder."
    }
}
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
Push-Location $buildDir

Write-Info "Running CMake configure..."

$cmakeArgs = @(
    "..",
    "-G", "Ninja",
    "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile",
    "-DCMAKE_BUILD_TYPE=$BuildConfig",
    "-DCMAKE_MAKE_PROGRAM=$ninjaExe"
)

& cmake $cmakeArgs

if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configuration failed!"
    Pop-Location
    exit 1
}
Write-Success "CMake configuration complete!"

if (-not $SkipBuild) {
    Write-Step "Building project..."
    & cmake --build . --config $BuildConfig --parallel
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed!"
        Pop-Location
        exit 1
    }
    Write-Success "Build complete!"

    # Find executable location
    $exeDir = $buildDir
    if (-not (Test-Path "$exeDir\VENPOD.exe")) {
        $exeDir = "$buildDir\bin"
    }

    if (Test-Path "$exeDir\VENPOD.exe") {
        $exeSize = (Get-Item "$exeDir\VENPOD.exe").Length / 1MB
        Write-Success "Executable: $exeDir\VENPOD.exe ($([math]::Round($exeSize, 2)) MB)"
    }
}

Pop-Location

$totalTime = (Get-Date) - $startTime
Write-Host "`n===============================================================" -ForegroundColor Magenta
Write-Host "  Setup completed in $($totalTime.TotalSeconds.ToString('F1')) seconds" -ForegroundColor Green
Write-Host "===============================================================" -ForegroundColor Magenta
Write-Host "`nNext steps:"
Write-Host "  .\build.ps1        # Incremental build"
Write-Host "  .\run.ps1          # Run the engine"
Write-Host "  .\clean.ps1        # Clean build artifacts"
