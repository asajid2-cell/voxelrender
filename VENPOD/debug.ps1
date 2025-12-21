# =============================================================================
# VENPOD - Debug Setup Script (PowerShell)
# Creates VSCode launch.json for debugging with MSVC or LLDB
# =============================================================================

param(
    [switch]$Build,           # Also build Debug config before setup
    [switch]$Launch,          # Launch VSCode after setup
    [string]$Debugger = "cppvsdbg"  # "cppvsdbg" (MSVC) or "lldb" (CodeLLDB)
)

$ErrorActionPreference = "Stop"

function Write-Step { Write-Host "`n==> $args" -ForegroundColor Cyan }
function Write-Success { Write-Host "[OK] $args" -ForegroundColor Green }
function Write-Warning { Write-Host "[WARN] $args" -ForegroundColor Yellow }
function Write-Error { Write-Host "[ERROR] $args" -ForegroundColor Red }
function Write-Info { Write-Host "  $args" -ForegroundColor Gray }

$projectRoot = $PSScriptRoot
$repoRoot = Split-Path -Path $projectRoot -Parent
if (-not $repoRoot) {
    $repoRoot = $projectRoot
}
$buildDir = Join-Path $projectRoot "build"
$vscodeDir = Join-Path $repoRoot ".vscode"
$launchJson = Join-Path $vscodeDir "launch.json"

Write-Host @"
===============================================================
              VENPOD - DEBUG SETUP SCRIPT
       Creates VSCode launch.json for debugging
===============================================================
"@ -ForegroundColor Magenta

# =============================================================================
# STEP 1: Build Debug configuration if requested
# =============================================================================
if ($Build) {
    Write-Step "Building Debug configuration..."

    # Check if build directory exists
    if (-not (Test-Path $buildDir)) {
        Write-Error "Build directory not found! Run setup.ps1 first."
        exit 1
    }

    Push-Location $buildDir
    & cmake --build . --config Debug --parallel
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Debug build failed!"
        Pop-Location
        exit 1
    }
    Pop-Location
    Write-Success "Debug build complete!"
}

# =============================================================================
# STEP 2: Find executable paths
# =============================================================================
Write-Step "Locating executable..."

$debugExePath = $null
$releaseExePath = $null

# Possible Debug paths
$debugPaths = @(
    "$buildDir\bin\Debug\VENPOD.exe",
    "$buildDir\bin\VENPOD.exe",
    "$buildDir\Debug\VENPOD.exe",
    "$buildDir\VENPOD.exe"
)

# Possible Release paths
$releasePaths = @(
    "$buildDir\bin\Release\VENPOD.exe",
    "$buildDir\bin\VENPOD.exe",
    "$buildDir\Release\VENPOD.exe",
    "$buildDir\VENPOD.exe"
)

foreach ($path in $debugPaths) {
    if (Test-Path $path) {
        $debugExePath = $path
        break
    }
}

foreach ($path in $releasePaths) {
    if (Test-Path $path) {
        $releaseExePath = $path
        break
    }
}

# Use whatever we found
$exePath = if ($debugExePath) { $debugExePath } else { $releaseExePath }

if (-not $exePath) {
    Write-Warning "No executable found yet. Using default path."
    $exePath = "$buildDir\bin\VENPOD.exe"
}

$exeDir = Split-Path $exePath -Parent

Write-Info "Executable: $exePath"
Write-Info "Working Dir: $exeDir"

# =============================================================================
# STEP 3: Create .vscode directory
# =============================================================================
Write-Step "Creating .vscode directory..."

if (-not (Test-Path $vscodeDir)) {
    New-Item -ItemType Directory -Force -Path $vscodeDir | Out-Null
    Write-Success "Created $vscodeDir"
} else {
    Write-Info ".vscode directory already exists"
}

# =============================================================================
# STEP 4: Generate launch.json
# =============================================================================
Write-Step "Generating launch.json..."

# Convert paths to forward slashes for JSON (works in VSCode)
$exePathJson = $exePath.Replace('\', '/')
$exeDirJson = $exeDir.Replace('\', '/')
$projectRootJson = $projectRoot.Replace('\', '/')

# Determine debugger type
$debuggerType = switch ($Debugger) {
    "lldb" { "lldb" }
    "codelldb" { "lldb" }
    default { "cppvsdbg" }  # MSVC debugger (default for Windows)
}

# VSCode expects configs relative to workspace root (repo root)
$workspaceLaunchExe = "`${workspaceFolder}/VENPOD/build/bin/VENPOD.exe"
$workspaceLaunchCwd = "`${workspaceFolder}/VENPOD/build/bin"

$launchConfig = @"
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "VENPOD Debug",
            "type": "$debuggerType",
            "request": "launch",
            "program": "$workspaceLaunchExe",
            "args": [],
            "stopAtEntry": false,
            "cwd": "$workspaceLaunchCwd",
            "environment": [],
            "console": "integratedTerminal",
            "preLaunchTask": "Build Debug"
        },
        {
            "name": "VENPOD Release",
            "type": "$debuggerType",
            "request": "launch",
            "program": "$workspaceLaunchExe",
            "args": [],
            "stopAtEntry": false,
            "cwd": "$workspaceLaunchCwd",
            "environment": [],
            "console": "integratedTerminal"
        },
        {
            "name": "VENPOD Attach",
            "type": "$debuggerType",
            "request": "attach",
            "processId": "`${command:pickProcess}",
            "sourceFileMap": {
                "/": "`${workspaceFolder}/"
            }
        }
    ]
}
"@

# Write launch.json (without BOM)
[System.IO.File]::WriteAllText($launchJson, $launchConfig)
Write-Success "Created $launchJson"

# =============================================================================
# STEP 5: Generate tasks.json for build tasks
# =============================================================================
Write-Step "Generating tasks.json..."

$tasksJson = Join-Path $vscodeDir "tasks.json"

$tasksConfig = @"
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "Build Debug",
            "type": "shell",
            "command": "cmake",
            "args": [
                "--build",
                "`${workspaceFolder}/VENPOD/build",
                "--config",
                "Debug",
                "--parallel"
            ],
            "group": {
                "kind": "build",
                "isDefault": true
            },
            "problemMatcher": ["`$msCompile"],
            "detail": "Build VENPOD in Debug configuration"
        },
        {
            "label": "Build Release",
            "type": "shell",
            "command": "cmake",
            "args": [
                "--build",
                "`${workspaceFolder}/VENPOD/build",
                "--config",
                "Release",
                "--parallel"
            ],
            "group": "build",
            "problemMatcher": ["`$msCompile"],
            "detail": "Build VENPOD in Release configuration"
        },
        {
            "label": "Clean",
            "type": "shell",
            "command": "cmake",
            "args": [
                "--build",
                "`${workspaceFolder}/VENPOD/build",
                "--target",
                "clean"
            ],
            "problemMatcher": [],
            "detail": "Clean build artifacts"
        },
        {
            "label": "Rebuild Debug",
            "type": "shell",
            "dependsOn": ["Clean", "Build Debug"],
            "dependsOrder": "sequence",
            "problemMatcher": [],
            "detail": "Clean and rebuild Debug configuration"
        },
        {
            "label": "Sync Shaders",
            "type": "shell",
            "command": "powershell",
            "args": [
                "-Command",
                "Copy-Item -Path '`${workspaceFolder}/VENPOD/assets/shaders' -Destination '`${workspaceFolder}/VENPOD/build/bin/assets/' -Recurse -Force"
            ],
            "problemMatcher": [],
            "detail": "Copy shader files to build directory"
        }
    ]
}
"@

[System.IO.File]::WriteAllText($tasksJson, $tasksConfig)
Write-Success "Created $tasksJson"

# =============================================================================
# STEP 6: Generate c_cpp_properties.json for IntelliSense
# =============================================================================
Write-Step "Generating c_cpp_properties.json..."

$cppPropsJson = Join-Path $vscodeDir "c_cpp_properties.json"

# Find vcpkg include path
$vcpkgInclude = "C:/vcpkg/installed/x64-windows/include"
if ($env:VCPKG_ROOT) {
    $vcpkgInclude = "$($env:VCPKG_ROOT)/installed/x64-windows/include".Replace('\', '/')
}

$cppPropsConfig = @"
{
    "configurations": [
        {
            "name": "Win32",
            "includePath": [
                "`${workspaceFolder}/VENPOD/**",
                "`${workspaceFolder}/VENPOD/src",
                "`${workspaceFolder}/VENPOD/vendor/imgui",
                "$vcpkgInclude"
            ],
            "defines": [
                "_DEBUG",
                "UNICODE",
                "_UNICODE",
                "WIN32_LEAN_AND_MEAN",
                "NOMINMAX"
            ],
            "windowsSdkVersion": "10.0.22621.0",
            "compilerPath": "cl.exe",
            "cStandard": "c17",
            "cppStandard": "c++20",
            "intelliSenseMode": "windows-msvc-x64",
            "configurationProvider": "ms-vscode.cmake-tools"
        }
    ],
    "version": 4
}
"@

[System.IO.File]::WriteAllText($cppPropsJson, $cppPropsConfig)
Write-Success "Created $cppPropsJson"

# =============================================================================
# STEP 7: Summary
# =============================================================================
Write-Host "`n===============================================================" -ForegroundColor Magenta
Write-Host "  Debug setup complete!" -ForegroundColor Green
Write-Host "===============================================================" -ForegroundColor Magenta

Write-Host "`nCreated files:" -ForegroundColor Cyan
Write-Info "$launchJson"
Write-Info "$tasksJson"
Write-Info "$cppPropsJson"

Write-Host "`nDebug configurations:" -ForegroundColor Cyan
Write-Info "VENPOD Debug    - Build and launch with debugger"
Write-Info "VENPOD Release  - Launch release build with debugger"
Write-Info "VENPOD Attach   - Attach to running process"

Write-Host "`nBuild tasks (Ctrl+Shift+B):" -ForegroundColor Cyan
Write-Info "Build Debug     - Incremental debug build (default)"
Write-Info "Build Release   - Incremental release build"
Write-Info "Clean           - Remove build artifacts"
Write-Info "Rebuild Debug   - Clean + build debug"
Write-Info "Sync Shaders    - Copy shaders to build directory"

Write-Host "`nUsage:" -ForegroundColor Cyan
Write-Host "  Press F5 in VSCode to start debugging" -ForegroundColor White
Write-Host "  Press Ctrl+Shift+B to build" -ForegroundColor White
Write-Host "  Set breakpoints by clicking left of line numbers" -ForegroundColor White

if ($Launch) {
    Write-Step "Launching VSCode..."
    & code $projectRoot
}
