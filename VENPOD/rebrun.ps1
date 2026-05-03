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
    [switch]$ForceSync,
    [switch]$HighDensity,
    [switch]$LowMemoryDense,
    [switch]$Sparse,
    [switch]$SparseOnly,
    [switch]$SparseDebug
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
    VENPOD_HIGH_DENSITY = $env:VENPOD_HIGH_DENSITY
    VENPOD_LOW_MEMORY_DENSE = $env:VENPOD_LOW_MEMORY_DENSE
    VENPOD_ENABLE_TEST_MODES = $env:VENPOD_ENABLE_TEST_MODES
    VENPOD_ENABLE_EXPERIMENTAL_SPARSE = $env:VENPOD_ENABLE_EXPERIMENTAL_SPARSE
    VENPOD_RENDER_BACKEND = $env:VENPOD_RENDER_BACKEND
    VENPOD_SPARSE_RAYMARCH = $env:VENPOD_SPARSE_RAYMARCH
    VENPOD_SPARSE_ONLY = $env:VENPOD_SPARSE_ONLY
    VENPOD_SPARSE_DEBUG_MODE = $env:VENPOD_SPARSE_DEBUG_MODE
    VENPOD_SPARSE_FULL_RAYMARCH = $env:VENPOD_SPARSE_FULL_RAYMARCH
    VENPOD_SPARSE_LEGACY_RUNTIME = $env:VENPOD_SPARSE_LEGACY_RUNTIME
    VENPOD_SPARSE_RAY_PREFETCH_DISTANCE = $env:VENPOD_SPARSE_RAY_PREFETCH_DISTANCE
    VENPOD_SPARSE_RAY_PREFETCH_MAX_REQUESTS = $env:VENPOD_SPARSE_RAY_PREFETCH_MAX_REQUESTS
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

    # Sparse rendering is still experimental. Do not let old terminal
    # environment variables leak into the normal one-command test loop.
    Remove-Item env:VENPOD_HIGH_DENSITY -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_LOW_MEMORY_DENSE -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_ENABLE_EXPERIMENTAL_SPARSE -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_ENABLE_TEST_MODES -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_RENDER_BACKEND -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_RAYMARCH -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_ONLY -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_DEBUG_MODE -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_FULL_RAYMARCH -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_LEGACY_RUNTIME -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_RAY_PREFETCH_DISTANCE -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_RAY_PREFETCH_MAX_REQUESTS -ErrorAction SilentlyContinue

    if ($HighDensity) { $env:VENPOD_HIGH_DENSITY = "1" }
    if ($LowMemoryDense) { $env:VENPOD_LOW_MEMORY_DENSE = "1" }
    if ($Sparse -or $SparseOnly -or $SparseDebug) {
        $env:VENPOD_ENABLE_EXPERIMENTAL_SPARSE = "1"
        $env:VENPOD_RENDER_BACKEND = "sparse"
        $env:VENPOD_SPARSE_RAYMARCH = "1"
    }
    if ($SparseOnly) { $env:VENPOD_SPARSE_ONLY = "1" }
    if ($SparseDebug) { $env:VENPOD_SPARSE_DEBUG_MODE = "7" }
    if ($BoundaryTest) { $env:VENPOD_ENABLE_TEST_MODES = "1" }

    Write-Host "VENPOD - Rebuild + Run" -ForegroundColor Magenta
    Write-Info "Config: $Config"
    if ($HighDensity) {
        Write-Info "High-density dense render window: enabled"
    }
    if ($LowMemoryDense) {
        Write-Info "Low-memory dense render window: enabled (partial coverage debug mode)"
    }
    if ($Sparse -or $SparseOnly -or $SparseDebug) {
        Write-Info "Sparse test: enabled (only=$([int]$SparseOnly), debug=$([int]$SparseDebug))"
    }

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
