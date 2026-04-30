# =============================================================================
# VENPOD - Rebuild and Run Script
# One-command local loop for testing the latest Sandbox build.
# =============================================================================

param(
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$Diagnostics,
    [switch]$BoundaryTest,
    [switch]$InfinitePhysics,
    [switch]$DisablePhysics,
    [switch]$D3DDebug,
    [switch]$ForceSync
)

$ErrorActionPreference = "Stop"

function Write-Step { Write-Host "`n==> $args" -ForegroundColor Cyan }
function Write-Success { Write-Host "[OK] $args" -ForegroundColor Green }
function Write-Info { Write-Host "  $args" -ForegroundColor Gray }

$projectRoot = $PSScriptRoot
$buildScript = Join-Path $projectRoot "build.ps1"
$runScript = Join-Path $projectRoot "run.ps1"

if (-not (Test-Path $buildScript)) {
    throw "build.ps1 not found at $buildScript"
}
if (-not (Test-Path $runScript)) {
    throw "run.ps1 not found at $runScript"
}

$savedEnv = @{
    VENPOD_DIAGNOSTICS = $env:VENPOD_DIAGNOSTICS
    VENPOD_BOUNDARY_TEST = $env:VENPOD_BOUNDARY_TEST
    VENPOD_ENABLE_INFINITE_PHYSICS = $env:VENPOD_ENABLE_INFINITE_PHYSICS
    VENPOD_DISABLE_PHYSICS = $env:VENPOD_DISABLE_PHYSICS
    VENPOD_D3D_DEBUG = $env:VENPOD_D3D_DEBUG
    VENPOD_MODE = $env:VENPOD_MODE
}

function Restore-Env {
    foreach ($name in $savedEnv.Keys) {
        if ($null -eq $savedEnv[$name]) {
            Remove-Item "env:$name" -ErrorAction SilentlyContinue
        } else {
            Set-Item "env:$name" $savedEnv[$name]
        }
    }
}

try {
    if ($Diagnostics) { $env:VENPOD_DIAGNOSTICS = "1" }
    if ($BoundaryTest) { $env:VENPOD_BOUNDARY_TEST = "1" }
    if ($InfinitePhysics) { $env:VENPOD_ENABLE_INFINITE_PHYSICS = "1" }
    if ($DisablePhysics) { $env:VENPOD_DISABLE_PHYSICS = "1" }
    if ($D3DDebug) { $env:VENPOD_D3D_DEBUG = "1" }
    $env:VENPOD_MODE = "sandbox"

    Write-Host "VENPOD - Rebuild + Run" -ForegroundColor Magenta
    Write-Info "Config: $Config"

    Write-Step "Building latest code..."
    if ($Clean) {
        & $buildScript -Config $Config -Clean
    } else {
        & $buildScript -Config $Config
    }
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    Write-Step "Launching VENPOD..."
    if ($ForceSync) {
        & $runScript -Config $Config -ForceSync
    } else {
        & $runScript -Config $Config
    }
    exit $LASTEXITCODE
}
finally {
    Restore-Env
}
