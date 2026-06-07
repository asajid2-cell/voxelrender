# =============================================================================
# VENPOD - Rebuild and Run Script
# One-command local loop for testing the latest Sandbox build.
# =============================================================================

param(
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$NoBuild,
    [switch]$Diagnostics,
    [switch]$BoundaryTest,
    [switch]$InfinitePhysics,
    [switch]$DisablePhysics,
    [switch]$D3DDebug,
    [switch]$ForceSync,
    [switch]$HighDensity,
    [switch]$LowMemoryDense,
    [switch]$DisableFarSVO,
    [switch]$DenseLegacy,
    [switch]$Sparse,
    [switch]$SparseOnly,
    [switch]$SparseDebug,
    [switch]$SparseSurfaceAuthoritative,
    [switch]$SparseLegacyFullscreen,
    [switch]$SparseGpuRaycast,
    [switch]$SparseMissFeedback,
    [switch]$SparseBrushFeedback,
    [switch]$SparseBrushFeedbackApply,
    [switch]$SparseBrushFeedbackAuthoritative,
    [switch]$SparseBrushFeedbackStrictResidentOnly,
    [switch]$SparseBrushFeedbackMovingDiagnostic,
    [switch]$SparseBrushPaintSmoke,
    [switch]$SparseBrushPaintMovingSmoke,
    [switch]$SparseBrushPaintNonresidentSmoke,
    [string]$SparseEditFile = "",
    [switch]$SparsePhysics,
    [switch]$SparseGpuPhysics,
    [switch]$SparseGpuPhysicsApply,
    [switch]$SparseGpuPhysicsStrict,
    [switch]$SparsePhysicsDiagnosticSeed,
    [switch]$SparsePhysicsDiagnosticFluidSeed,
    [switch]$SparseValidatePool,
    [switch]$SparseStressRequests,
    [switch]$SparseStressCamera,
    [switch]$SparseSmoke,
    [switch]$SparsePhysicsSmoke,
    [switch]$SparseFlickerSmoke,
    [switch]$SparseSurfaceSmoke,
    [switch]$SparseGpuRaycastSmoke,
    [switch]$SparseMissFeedbackSmoke,
    [switch]$SparseBrushFeedbackSmoke,
    [int]$SparseDebugMode = -1,
    [int]$SparseOwnershipInterval = -1,
    [switch]$DisableSparseOwnership,
    [switch]$RequireSparsePipeReady,
    [int]$SparsePipeReadyFrame = 180,
    [switch]$RequireSparseOwnershipQuality,
    [int]$SparseOwnershipQualityReadyFrame = 180,
    [int]$SparseMinTerrainPixelsPct = 35,
    [int]$SparseMaxMissPixelsPct = 15,
    [switch]$RequireSparseOwnershipStability,
    [int]$SparseOwnershipStabilityReadyFrame = 180,
    [int]$SparseMaxTerrainDeltaPct = 25,
    [int]$SparseMaxMissDeltaPct = 12,
    [int]$SparseMinSurfaceFragments = 512,
    [int]$SparseSurfaceFragmentsReadyFrame = 150,
    [int]$SparseGpuRaycastHealthReadyFrame = 120,
    [int]$SparseGpuRaycastMaxFallbackPct = 95,
    [int]$SparseGpuRaycastMinAccepted = 1,
    [switch]$SparseGpuRaycastStrict,
    [int]$ExitAfterFrames = 0
)

$ErrorActionPreference = "Stop"

function Write-Step { Write-Host "`n==> $args" -ForegroundColor Cyan }
function Write-Success { Write-Host "[OK] $args" -ForegroundColor Green }
function Write-Info { Write-Host "  $args" -ForegroundColor Gray }

$projectRoot = $PSScriptRoot
$buildScript = Join-Path $projectRoot "build.ps1"
$runScript = Join-Path $projectRoot "run.ps1"

function Refresh-RuntimeAssets {
    $sourceAssets = Join-Path $projectRoot "assets"
    $targetAssets = Join-Path $projectRoot "build\bin\assets"
    if (-not (Test-Path $sourceAssets)) {
        throw "Source assets not found at $sourceAssets"
    }
    $binDir = Split-Path $targetAssets -Parent
    if (-not (Test-Path $binDir)) {
        throw "Runtime bin directory not found at $binDir. Run build.ps1 first."
    }
    if (-not (Test-Path $targetAssets)) {
        New-Item -ItemType Directory -Path $targetAssets | Out-Null
    }
    Copy-Item -Path (Join-Path $sourceAssets "*") -Destination $targetAssets -Recurse -Force
    $sourceRaymarch = Join-Path $sourceAssets "shaders\Graphics\PS_Raymarch.hlsl"
    $runtimeRaymarch = Join-Path $targetAssets "shaders\Graphics\PS_Raymarch.hlsl"
    if ((Test-Path $sourceRaymarch) -and (Test-Path $runtimeRaymarch)) {
        $sourceHash = (Get-FileHash -Algorithm SHA256 $sourceRaymarch).Hash
        $runtimeHash = (Get-FileHash -Algorithm SHA256 $runtimeRaymarch).Hash
        if ($sourceHash -ne $runtimeHash) {
            throw "Runtime PS_Raymarch.hlsl differs from source after asset refresh."
        }
    }
}

if (-not (Test-Path $buildScript)) {
    throw "build.ps1 not found at $buildScript"
}
if (-not (Test-Path $runScript)) {
    throw "run.ps1 not found at $runScript"
}

$savedEnv = @{
    VENPOD_LOG_FILE = $env:VENPOD_LOG_FILE
    VENPOD_DIAGNOSTICS = $env:VENPOD_DIAGNOSTICS
    VENPOD_BOUNDARY_TEST = $env:VENPOD_BOUNDARY_TEST
    VENPOD_ENABLE_INFINITE_PHYSICS = $env:VENPOD_ENABLE_INFINITE_PHYSICS
    VENPOD_DISABLE_PHYSICS = $env:VENPOD_DISABLE_PHYSICS
    VENPOD_D3D_DEBUG = $env:VENPOD_D3D_DEBUG
    VENPOD_MODE = $env:VENPOD_MODE
    VENPOD_HIGH_DENSITY = $env:VENPOD_HIGH_DENSITY
    VENPOD_LOW_MEMORY_DENSE = $env:VENPOD_LOW_MEMORY_DENSE
    VENPOD_DISABLE_FAR_SVO = $env:VENPOD_DISABLE_FAR_SVO
    VENPOD_ENABLE_TEST_MODES = $env:VENPOD_ENABLE_TEST_MODES
    VENPOD_ENABLE_EXPERIMENTAL_SPARSE = $env:VENPOD_ENABLE_EXPERIMENTAL_SPARSE
    VENPOD_RENDER_BACKEND = $env:VENPOD_RENDER_BACKEND
    VENPOD_SPARSE_RAYMARCH = $env:VENPOD_SPARSE_RAYMARCH
    VENPOD_SPARSE_ONLY = $env:VENPOD_SPARSE_ONLY
    VENPOD_SPARSE_SURFACE_AUTHORITATIVE = $env:VENPOD_SPARSE_SURFACE_AUTHORITATIVE
    VENPOD_SPARSE_VOXEL_TERRAIN_ONLY = $env:VENPOD_SPARSE_VOXEL_TERRAIN_ONLY
    VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_GATE = $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_GATE
    VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_FRAME = $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_FRAME
    VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MAX_FRAME = $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MAX_FRAME
    VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_FAIL_OPEN_AT_MAX_FRAME = $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_FAIL_OPEN_AT_MAX_FRAME
    VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_READY_BRICKS = $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_READY_BRICKS
    VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_TERRAIN_CRITICAL_BLOCKS = $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_TERRAIN_CRITICAL_BLOCKS
    VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_SURFACE_PROOF = $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_SURFACE_PROOF
    VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_SURFACE_FRAGMENTS = $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_SURFACE_FRAGMENTS
    VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_FAR_SVO_PROOF = $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_FAR_SVO_PROOF
    VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_PROOF = $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_PROOF
    VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_MID_VOXEL_COVERAGE_PCT = $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_MID_VOXEL_COVERAGE_PCT
    VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_MID_VOXEL_WORST_RING_PCT = $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_MID_VOXEL_WORST_RING_PCT
    VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_HIDDEN_EXACT_PROOF = $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_HIDDEN_EXACT_PROOF
    VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_HIDDEN_EXACT_BLOCKS = $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_HIDDEN_EXACT_BLOCKS
    VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_HIDDEN_EXACT_CLEAN_FRAMES = $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_HIDDEN_EXACT_CLEAN_FRAMES
    VENPOD_SPARSE_STARTUP_HIDDEN_EXACT_REPAIR_BLOCKS = $env:VENPOD_SPARSE_STARTUP_HIDDEN_EXACT_REPAIR_BLOCKS
    VENPOD_SPARSE_HIDDEN_EXACT_MISS_DURING_STARTUP = $env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_DURING_STARTUP
    VENPOD_SPARSE_GPU_RAYCAST = $env:VENPOD_SPARSE_GPU_RAYCAST
    VENPOD_SPARSE_MISS_FEEDBACK = $env:VENPOD_SPARSE_MISS_FEEDBACK
    VENPOD_SPARSE_BRUSH_FEEDBACK = $env:VENPOD_SPARSE_BRUSH_FEEDBACK
    VENPOD_SPARSE_BRUSH_FEEDBACK_APPLY = $env:VENPOD_SPARSE_BRUSH_FEEDBACK_APPLY
    VENPOD_SPARSE_BRUSH_FEEDBACK_AUTHORITATIVE = $env:VENPOD_SPARSE_BRUSH_FEEDBACK_AUTHORITATIVE
    VENPOD_SPARSE_BRUSH_FEEDBACK_STRICT_RESIDENT_ONLY = $env:VENPOD_SPARSE_BRUSH_FEEDBACK_STRICT_RESIDENT_ONLY
    VENPOD_SPARSE_BRUSH_FEEDBACK_MOVING_DIAGNOSTIC = $env:VENPOD_SPARSE_BRUSH_FEEDBACK_MOVING_DIAGNOSTIC
    VENPOD_SPARSE_BRUSH_PAINT_SMOKE = $env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE
    VENPOD_SPARSE_BRUSH_PAINT_MOVING_SMOKE = $env:VENPOD_SPARSE_BRUSH_PAINT_MOVING_SMOKE
    VENPOD_SPARSE_BRUSH_PAINT_NONRESIDENT_SMOKE = $env:VENPOD_SPARSE_BRUSH_PAINT_NONRESIDENT_SMOKE
    VENPOD_SPARSE_EDIT_FILE = $env:VENPOD_SPARSE_EDIT_FILE
    VENPOD_ENABLE_SPARSE_PHYSICS = $env:VENPOD_ENABLE_SPARSE_PHYSICS
    VENPOD_SPARSE_PHYSICS_GPU = $env:VENPOD_SPARSE_PHYSICS_GPU
    VENPOD_SPARSE_PHYSICS_GPU_APPLY = $env:VENPOD_SPARSE_PHYSICS_GPU_APPLY
    VENPOD_SPARSE_PHYSICS_GPU_STRICT = $env:VENPOD_SPARSE_PHYSICS_GPU_STRICT
    VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_SEED = $env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_SEED
    VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_MATERIAL_SEED = $env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_MATERIAL_SEED
    VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_FLUID_SEED = $env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_FLUID_SEED
    VENPOD_SPARSE_VALIDATE_POOL = $env:VENPOD_SPARSE_VALIDATE_POOL
    VENPOD_SPARSE_STRESS_REQUESTS = $env:VENPOD_SPARSE_STRESS_REQUESTS
    VENPOD_SPARSE_STRESS_CAMERA = $env:VENPOD_SPARSE_STRESS_CAMERA
    VENPOD_SPARSE_DEBUG_MODE = $env:VENPOD_SPARSE_DEBUG_MODE
    VENPOD_SPARSE_FULL_RAYMARCH = $env:VENPOD_SPARSE_FULL_RAYMARCH
    VENPOD_SPARSE_LEGACY_RUNTIME = $env:VENPOD_SPARSE_LEGACY_RUNTIME
    VENPOD_SPARSE_RAY_PREFETCH_DISTANCE = $env:VENPOD_SPARSE_RAY_PREFETCH_DISTANCE
    VENPOD_SPARSE_RAY_PREFETCH_MAX_REQUESTS = $env:VENPOD_SPARSE_RAY_PREFETCH_MAX_REQUESTS
    VENPOD_SPARSE_RENDER_OWNERSHIP = $env:VENPOD_SPARSE_RENDER_OWNERSHIP
    VENPOD_SPARSE_RENDER_OWNERSHIP_INTERVAL = $env:VENPOD_SPARSE_RENDER_OWNERSHIP_INTERVAL
    VENPOD_SPARSE_REQUIRE_PIPE_READY = $env:VENPOD_SPARSE_REQUIRE_PIPE_READY
    VENPOD_SPARSE_PIPE_READY_FRAME = $env:VENPOD_SPARSE_PIPE_READY_FRAME
    VENPOD_SPARSE_REQUIRE_OWNERSHIP_QUALITY = $env:VENPOD_SPARSE_REQUIRE_OWNERSHIP_QUALITY
    VENPOD_SPARSE_OWNERSHIP_QUALITY_READY_FRAME = $env:VENPOD_SPARSE_OWNERSHIP_QUALITY_READY_FRAME
    VENPOD_SPARSE_MIN_TERRAIN_PIXELS_PCT = $env:VENPOD_SPARSE_MIN_TERRAIN_PIXELS_PCT
    VENPOD_SPARSE_MAX_MISS_PIXELS_PCT = $env:VENPOD_SPARSE_MAX_MISS_PIXELS_PCT
    VENPOD_SPARSE_REQUIRE_OWNERSHIP_STABILITY = $env:VENPOD_SPARSE_REQUIRE_OWNERSHIP_STABILITY
    VENPOD_SPARSE_OWNERSHIP_STABILITY_READY_FRAME = $env:VENPOD_SPARSE_OWNERSHIP_STABILITY_READY_FRAME
    VENPOD_SPARSE_MAX_TERRAIN_DELTA_PCT = $env:VENPOD_SPARSE_MAX_TERRAIN_DELTA_PCT
    VENPOD_SPARSE_MAX_MISS_DELTA_PCT = $env:VENPOD_SPARSE_MAX_MISS_DELTA_PCT
    VENPOD_SPARSE_SURFACE_DIAGNOSTIC_SEED = $env:VENPOD_SPARSE_SURFACE_DIAGNOSTIC_SEED
    VENPOD_SPARSE_GPU_RAYCAST_DIAGNOSTIC_SEED = $env:VENPOD_SPARSE_GPU_RAYCAST_DIAGNOSTIC_SEED
    VENPOD_SPARSE_BRUSH_FEEDBACK_DIAGNOSTIC_SEED = $env:VENPOD_SPARSE_BRUSH_FEEDBACK_DIAGNOSTIC_SEED
    VENPOD_SPARSE_REQUIRE_SURFACE_FRAGMENTS = $env:VENPOD_SPARSE_REQUIRE_SURFACE_FRAGMENTS
    VENPOD_SPARSE_MIN_SURFACE_FRAGMENTS = $env:VENPOD_SPARSE_MIN_SURFACE_FRAGMENTS
    VENPOD_SPARSE_SURFACE_FRAGMENTS_READY_FRAME = $env:VENPOD_SPARSE_SURFACE_FRAGMENTS_READY_FRAME
    VENPOD_SPARSE_REQUIRE_GPU_RAYCAST_HEALTH = $env:VENPOD_SPARSE_REQUIRE_GPU_RAYCAST_HEALTH
    VENPOD_SPARSE_GPU_RAYCAST_STRICT = $env:VENPOD_SPARSE_GPU_RAYCAST_STRICT
    VENPOD_SPARSE_GPU_RAYCAST_HEALTH_READY_FRAME = $env:VENPOD_SPARSE_GPU_RAYCAST_HEALTH_READY_FRAME
    VENPOD_SPARSE_GPU_RAYCAST_MAX_FALLBACK_PCT = $env:VENPOD_SPARSE_GPU_RAYCAST_MAX_FALLBACK_PCT
    VENPOD_SPARSE_GPU_RAYCAST_MIN_ACCEPTED = $env:VENPOD_SPARSE_GPU_RAYCAST_MIN_ACCEPTED
    VENPOD_EXIT_AFTER_FRAMES = $env:VENPOD_EXIT_AFTER_FRAMES
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

$explicitSparsePhysicsDiagnosticSeed = $SparsePhysicsDiagnosticSeed.IsPresent

try {
    if ($SparseSmoke) {
        $DisablePhysics = $true
        $SparseMissFeedback = $true
        $SparseValidatePool = $true
        $SparseStressRequests = $true
        $SparseStressCamera = $true
        $RequireSparsePipeReady = $true
        $RequireSparseOwnershipQuality = $true
        $RequireSparseOwnershipStability = $true
        if ($SparseDebugMode -lt 0) { $SparseDebugMode = 50 }
        if ($SparseOwnershipInterval -lt 0) { $SparseOwnershipInterval = 30 }
        if ($ExitAfterFrames -le 0) { $ExitAfterFrames = 360 }
    }
    if ($SparsePhysicsSmoke) {
        $SparsePhysicsDiagnosticSeed = $true
        $SparseGpuPhysics = $true
        $SparseGpuPhysicsApply = $true
        $SparseValidatePool = $true
        $RequireSparsePipeReady = $true
        $RequireSparseOwnershipQuality = $true
        $RequireSparseOwnershipStability = $true
        if ($SparseDebugMode -lt 0) { $SparseDebugMode = 50 }
        if ($SparseOwnershipInterval -lt 0) { $SparseOwnershipInterval = 30 }
        if ($ExitAfterFrames -le 0) { $ExitAfterFrames = 240 }
    }
    if ($SparseFlickerSmoke) {
        $DisablePhysics = $true
        $SparseValidatePool = $true
        $RequireSparsePipeReady = $true
        $RequireSparseOwnershipQuality = $true
        $RequireSparseOwnershipStability = $true
        if ($SparseDebugMode -lt 0) { $SparseDebugMode = 50 }
        if ($SparseOwnershipInterval -lt 0) { $SparseOwnershipInterval = 1 }
        if ($SparsePipeReadyFrame -eq 180) { $SparsePipeReadyFrame = 120 }
        if ($SparseOwnershipQualityReadyFrame -eq 180) { $SparseOwnershipQualityReadyFrame = 120 }
        if ($SparseOwnershipStabilityReadyFrame -eq 180) { $SparseOwnershipStabilityReadyFrame = 120 }
        if ($SparseMaxTerrainDeltaPct -eq 25) { $SparseMaxTerrainDeltaPct = 8 }
        if ($SparseMaxMissDeltaPct -eq 12) { $SparseMaxMissDeltaPct = 4 }
        if ($ExitAfterFrames -le 0) { $ExitAfterFrames = 180 }
    }
    if ($SparseSurfaceSmoke) {
        $DisablePhysics = $true
        $SparseValidatePool = $true
        $RequireSparsePipeReady = $true
        $RequireSparseOwnershipQuality = $true
        $RequireSparseOwnershipStability = $true
        if ($SparseDebugMode -lt 0) { $SparseDebugMode = 50 }
        if ($SparseOwnershipInterval -lt 0) { $SparseOwnershipInterval = 1 }
        if ($SparsePipeReadyFrame -eq 180) { $SparsePipeReadyFrame = 120 }
        if ($SparseOwnershipQualityReadyFrame -eq 180) { $SparseOwnershipQualityReadyFrame = 120 }
        if ($SparseOwnershipStabilityReadyFrame -eq 180) { $SparseOwnershipStabilityReadyFrame = 120 }
        if ($SparseSurfaceFragmentsReadyFrame -eq 150) { $SparseSurfaceFragmentsReadyFrame = 150 }
        if ($SparseMaxTerrainDeltaPct -eq 25) { $SparseMaxTerrainDeltaPct = 10 }
        if ($SparseMaxMissDeltaPct -eq 12) { $SparseMaxMissDeltaPct = 5 }
        if ($ExitAfterFrames -le 0) { $ExitAfterFrames = 240 }
    }
    if ($SparseGpuRaycastSmoke) {
        $DisablePhysics = $true
        $SparseGpuRaycast = $true
        $SparseValidatePool = $true
        $RequireSparsePipeReady = $true
        $RequireSparseOwnershipQuality = $true
        $RequireSparseOwnershipStability = $true
        if ($SparseDebugMode -lt 0) { $SparseDebugMode = 50 }
        if ($SparseOwnershipInterval -lt 0) { $SparseOwnershipInterval = 30 }
        if ($SparsePipeReadyFrame -eq 180) { $SparsePipeReadyFrame = 180 }
        if ($SparseOwnershipQualityReadyFrame -eq 180) { $SparseOwnershipQualityReadyFrame = 180 }
        if ($SparseOwnershipStabilityReadyFrame -eq 180) { $SparseOwnershipStabilityReadyFrame = 180 }
        if ($ExitAfterFrames -le 0) { $ExitAfterFrames = 300 }
    }
    if ($SparseBrushFeedbackSmoke) {
        $DisablePhysics = $true
        $SparseBrushFeedback = $true
        $SparseValidatePool = $true
        $RequireSparsePipeReady = $true
        $RequireSparseOwnershipQuality = $true
        $RequireSparseOwnershipStability = $true
        if ($SparseDebugMode -lt 0) { $SparseDebugMode = 50 }
        if ($SparseOwnershipInterval -lt 0) { $SparseOwnershipInterval = 30 }
        if ($ExitAfterFrames -le 0) { $ExitAfterFrames = 240 }
    }
    if ($SparseBrushPaintMovingSmoke -or $SparseBrushPaintNonresidentSmoke) {
        $SparseBrushPaintSmoke = $true
    }
    if ($SparseBrushPaintSmoke) {
        if (-not ($SparsePhysics -or $SparseGpuPhysics -or $SparseGpuPhysicsApply -or $SparseGpuPhysicsStrict)) {
            $DisablePhysics = $true
        }
        $SparseBrushFeedback = $true
        $SparseValidatePool = $true
        $RequireSparsePipeReady = $true
        $RequireSparseOwnershipQuality = $true
        $RequireSparseOwnershipStability = $true
        if ($SparseDebugMode -lt 0) { $SparseDebugMode = 50 }
        if ($SparseOwnershipInterval -lt 0) { $SparseOwnershipInterval = 30 }
        if ($ExitAfterFrames -le 0) { $ExitAfterFrames = 600 }
    }
    if ($SparseMissFeedbackSmoke) {
        $DisablePhysics = $true
        $SparseMissFeedback = $true
        $SparseValidatePool = $true
        $SparseStressRequests = $true
        $SparseStressCamera = $true
        $RequireSparsePipeReady = $true
        $RequireSparseOwnershipQuality = $true
        $RequireSparseOwnershipStability = $true
        if ($SparseDebugMode -lt 0) { $SparseDebugMode = 50 }
        if ($SparseOwnershipInterval -lt 0) { $SparseOwnershipInterval = 30 }
        if ($ExitAfterFrames -le 0) { $ExitAfterFrames = 240 }
    }

    if ($Diagnostics) { $env:VENPOD_DIAGNOSTICS = "1" }
    $env:VENPOD_LOG_FILE = "1"
    if ($BoundaryTest) { $env:VENPOD_BOUNDARY_TEST = "1" }
    if ($InfinitePhysics) { $env:VENPOD_ENABLE_INFINITE_PHYSICS = "1" }
    if ($DisablePhysics) { $env:VENPOD_DISABLE_PHYSICS = "1" }
    if ($D3DDebug) { $env:VENPOD_D3D_DEBUG = "1" }
    $env:VENPOD_MODE = "sandbox"

    # Sparse rendering is still experimental. Do not let old terminal
    # environment variables leak into the normal one-command test loop.
    Remove-Item env:VENPOD_HIGH_DENSITY -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_LOW_MEMORY_DENSE -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_DISABLE_FAR_SVO -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_ENABLE_EXPERIMENTAL_SPARSE -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_ENABLE_TEST_MODES -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_RENDER_BACKEND -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_RAYMARCH -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_ONLY -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_SURFACE_AUTHORITATIVE -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_VOXEL_TERRAIN_ONLY -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_GATE -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_FRAME -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MAX_FRAME -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_FAIL_OPEN_AT_MAX_FRAME -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_READY_BRICKS -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_TERRAIN_CRITICAL_BLOCKS -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_SURFACE_PROOF -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_SURFACE_FRAGMENTS -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_FAR_SVO_PROOF -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_PROOF -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_MID_VOXEL_COVERAGE_PCT -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_MID_VOXEL_WORST_RING_PCT -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_HIDDEN_EXACT_PROOF -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_HIDDEN_EXACT_BLOCKS -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_HIDDEN_EXACT_CLEAN_FRAMES -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_DURING_STARTUP -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_GPU_RAYCAST -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_MISS_FEEDBACK -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_BRUSH_FEEDBACK -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_BRUSH_FEEDBACK_APPLY -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_BRUSH_FEEDBACK_AUTHORITATIVE -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_BRUSH_FEEDBACK_STRICT_RESIDENT_ONLY -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_BRUSH_FEEDBACK_MOVING_DIAGNOSTIC -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_BRUSH_PAINT_MOVING_SMOKE -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_BRUSH_PAINT_NONRESIDENT_SMOKE -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_EDIT_FILE -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_ENABLE_SPARSE_PHYSICS -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_PHYSICS_GPU -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_PHYSICS_GPU_APPLY -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_PHYSICS_GPU_STRICT -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_SEED -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_MATERIAL_SEED -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_FLUID_SEED -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_VALIDATE_POOL -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_STRESS_REQUESTS -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_STRESS_CAMERA -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_DEBUG_MODE -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_FULL_RAYMARCH -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_LEGACY_RUNTIME -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_RAY_PREFETCH_DISTANCE -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_RAY_PREFETCH_MAX_REQUESTS -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_RENDER_OWNERSHIP -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_RENDER_OWNERSHIP_INTERVAL -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_REQUIRE_PIPE_READY -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_PIPE_READY_FRAME -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_REQUIRE_OWNERSHIP_QUALITY -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_OWNERSHIP_QUALITY_READY_FRAME -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_MIN_TERRAIN_PIXELS_PCT -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_MAX_MISS_PIXELS_PCT -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_REQUIRE_OWNERSHIP_STABILITY -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_OWNERSHIP_STABILITY_READY_FRAME -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_MAX_TERRAIN_DELTA_PCT -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_MAX_MISS_DELTA_PCT -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_SURFACE_DIAGNOSTIC_SEED -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_GPU_RAYCAST_DIAGNOSTIC_SEED -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_BRUSH_FEEDBACK_DIAGNOSTIC_SEED -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_REQUIRE_SURFACE_FRAGMENTS -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_MIN_SURFACE_FRAGMENTS -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_SURFACE_FRAGMENTS_READY_FRAME -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_REQUIRE_GPU_RAYCAST_HEALTH -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_GPU_RAYCAST_STRICT -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_GPU_RAYCAST_HEALTH_READY_FRAME -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_GPU_RAYCAST_MAX_FALLBACK_PCT -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_GPU_RAYCAST_MIN_ACCEPTED -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_EXIT_AFTER_FRAMES -ErrorAction SilentlyContinue

    if ($HighDensity) { $env:VENPOD_HIGH_DENSITY = "1" }
    if ($LowMemoryDense) { $env:VENPOD_LOW_MEMORY_DENSE = "1" }
    if ($DisableFarSVO) { $env:VENPOD_DISABLE_FAR_SVO = "1" }
    if ($SparseGpuRaycastStrict) { $SparseGpuRaycast = $true }
    $useSparseDefault = -not $DenseLegacy -and -not $HighDensity -and -not $LowMemoryDense
    $useSparse = $useSparseDefault -or $Sparse -or $SparseOnly -or $SparseDebug -or $SparseSurfaceAuthoritative -or $SparseLegacyFullscreen -or $SparseGpuRaycast -or $SparseMissFeedback -or $SparseBrushFeedback -or $SparseBrushFeedbackApply -or $SparseBrushFeedbackMovingDiagnostic -or $SparseBrushPaintSmoke -or $SparseBrushPaintMovingSmoke -or ($SparseEditFile -ne "") -or $SparsePhysics -or $SparseGpuPhysics -or $SparseGpuPhysicsApply -or $SparseGpuPhysicsStrict -or $SparsePhysicsDiagnosticSeed -or $SparsePhysicsDiagnosticFluidSeed -or $SparseValidatePool -or $SparseStressRequests -or $SparseStressCamera -or $SparseSmoke -or $SparsePhysicsSmoke -or $SparseFlickerSmoke -or $SparseSurfaceSmoke -or $SparseGpuRaycastSmoke -or $SparseMissFeedbackSmoke -or $SparseBrushFeedbackSmoke -or ($SparseDebugMode -ge 0)
    if ($useSparse) {
        $env:VENPOD_ENABLE_EXPERIMENTAL_SPARSE = "1"
        $env:VENPOD_RENDER_BACKEND = "sparse"
        $env:VENPOD_SPARSE_RAYMARCH = "1"
        $env:VENPOD_SPARSE_MISS_FEEDBACK = "1"
        # Public sparse runs should not expose the world until coherent LOD
        # owners are ready. Startup hidden-exact repair prewarms the first
        # public view so the renderer does not open on a partial exact/mid/far
        # mix and then visibly assemble terrain.
        $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_GATE = "1"
        $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_FRAME = "24"
        $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MAX_FRAME = "360"
        $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_FAIL_OPEN_AT_MAX_FRAME = "0"
        $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_READY_BRICKS = "512"
        $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_TERRAIN_CRITICAL_BLOCKS = "1"
        $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_SURFACE_PROOF = "1"
        $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_FAR_SVO_PROOF = "1"
        $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_PROOF = "1"
        $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_MID_VOXEL_COVERAGE_PCT = "95"
        $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_MID_VOXEL_WORST_RING_PCT = "90"
        $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_HIDDEN_EXACT_PROOF = "0"
        $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_HIDDEN_EXACT_BLOCKS = "0"
        if ([string]::IsNullOrWhiteSpace($savedEnv.VENPOD_SPARSE_HIDDEN_EXACT_MISS_DURING_STARTUP)) {
            $env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_DURING_STARTUP = "1"
        } else {
            $env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_DURING_STARTUP =
                $savedEnv.VENPOD_SPARSE_HIDDEN_EXACT_MISS_DURING_STARTUP
        }
        if ([string]::IsNullOrWhiteSpace($savedEnv.VENPOD_SPARSE_STARTUP_HIDDEN_EXACT_REPAIR_BLOCKS)) {
            $env:VENPOD_SPARSE_STARTUP_HIDDEN_EXACT_REPAIR_BLOCKS = "0"
        } else {
            $env:VENPOD_SPARSE_STARTUP_HIDDEN_EXACT_REPAIR_BLOCKS =
                $savedEnv.VENPOD_SPARSE_STARTUP_HIDDEN_EXACT_REPAIR_BLOCKS
        }
        $env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_WARMUP_FRAME = "240"
        $env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_CLEAN_IDLE_FRAMES = "8"
    }
    if ($SparseOnly -or $useSparseDefault) { $env:VENPOD_SPARSE_ONLY = "1" }
    if (($SparseOnly -or $SparseSurfaceAuthoritative -or $useSparseDefault) -and -not $SparseLegacyFullscreen) {
        $env:VENPOD_SPARSE_SURFACE_AUTHORITATIVE = "1"
    }
    if ($SparseGpuRaycast) { $env:VENPOD_SPARSE_GPU_RAYCAST = "1" }
    if ($SparseGpuRaycastStrict) { $env:VENPOD_SPARSE_GPU_RAYCAST_STRICT = "1" }
    if ($SparseMissFeedback) { $env:VENPOD_SPARSE_MISS_FEEDBACK = "1" }
    if ($SparseBrushFeedback) { $env:VENPOD_SPARSE_BRUSH_FEEDBACK = "1" }
    if ($SparseBrushFeedbackApply) {
        $env:VENPOD_SPARSE_BRUSH_FEEDBACK = "1"
        $env:VENPOD_SPARSE_BRUSH_FEEDBACK_APPLY = "1"
    }
    if ($SparseBrushFeedbackAuthoritative) {
        $env:VENPOD_SPARSE_BRUSH_FEEDBACK = "1"
        $env:VENPOD_SPARSE_BRUSH_FEEDBACK_APPLY = "1"
        $env:VENPOD_SPARSE_BRUSH_FEEDBACK_AUTHORITATIVE = "1"
    }
    if ($SparseBrushFeedbackStrictResidentOnly) {
        $env:VENPOD_SPARSE_BRUSH_FEEDBACK = "1"
        $env:VENPOD_SPARSE_BRUSH_FEEDBACK_APPLY = "1"
        $env:VENPOD_SPARSE_BRUSH_FEEDBACK_AUTHORITATIVE = "1"
        $env:VENPOD_SPARSE_BRUSH_FEEDBACK_STRICT_RESIDENT_ONLY = "1"
    }
    if ($SparseBrushFeedbackMovingDiagnostic) { $env:VENPOD_SPARSE_BRUSH_FEEDBACK_MOVING_DIAGNOSTIC = "1" }
    if ($SparseBrushPaintSmoke) {
        $env:VENPOD_SPARSE_BRUSH_FEEDBACK = "1"
        $env:VENPOD_SPARSE_BRUSH_FEEDBACK_APPLY = "1"
        $env:VENPOD_SPARSE_BRUSH_FEEDBACK_AUTHORITATIVE = "1"
        $env:VENPOD_SPARSE_BRUSH_FEEDBACK_STRICT_RESIDENT_ONLY = "1"
        $env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE = "1"
    }
    if ($SparseBrushPaintMovingSmoke) { $env:VENPOD_SPARSE_BRUSH_PAINT_MOVING_SMOKE = "1" }
    if ($SparseBrushPaintNonresidentSmoke) { $env:VENPOD_SPARSE_BRUSH_PAINT_NONRESIDENT_SMOKE = "1" }
    if ($SparseEditFile -ne "") { $env:VENPOD_SPARSE_EDIT_FILE = $SparseEditFile }
    if ($SparseBrushFeedbackSmoke) { $env:VENPOD_SPARSE_BRUSH_FEEDBACK_DIAGNOSTIC_SEED = "1" }
    if ($SparsePhysics) { $env:VENPOD_ENABLE_SPARSE_PHYSICS = "1" }
    if ($SparseGpuPhysics) {
        $env:VENPOD_ENABLE_SPARSE_PHYSICS = "1"
        $env:VENPOD_SPARSE_PHYSICS_GPU = "1"
    }
    if ($SparseGpuPhysicsApply) {
        $env:VENPOD_ENABLE_SPARSE_PHYSICS = "1"
        $env:VENPOD_SPARSE_PHYSICS_GPU = "1"
        $env:VENPOD_SPARSE_PHYSICS_GPU_APPLY = "1"
    }
    if ($SparseGpuPhysicsStrict) {
        $env:VENPOD_ENABLE_SPARSE_PHYSICS = "1"
        $env:VENPOD_SPARSE_PHYSICS_GPU = "1"
        $env:VENPOD_SPARSE_PHYSICS_GPU_APPLY = "1"
        $env:VENPOD_SPARSE_PHYSICS_GPU_STRICT = "1"
    }
    if ($SparsePhysicsDiagnosticSeed -or $SparsePhysicsDiagnosticFluidSeed) {
        $env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_SEED = "1"
    }
    if ($explicitSparsePhysicsDiagnosticSeed -or ($SparsePhysicsDiagnosticSeed -and -not $SparsePhysicsDiagnosticFluidSeed)) {
        $env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_MATERIAL_SEED = "1"
    }
    if ($SparsePhysicsDiagnosticFluidSeed) { $env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_FLUID_SEED = "1" }
    if ($SparseValidatePool) { $env:VENPOD_SPARSE_VALIDATE_POOL = "1" }
    if ($SparseStressRequests) { $env:VENPOD_SPARSE_STRESS_REQUESTS = "1" }
    if ($SparseStressCamera) { $env:VENPOD_SPARSE_STRESS_CAMERA = "1" }
    if ($SparseDebug) { $env:VENPOD_SPARSE_DEBUG_MODE = "7" }
    if ($SparseDebugMode -ge 0) { $env:VENPOD_SPARSE_DEBUG_MODE = "$SparseDebugMode" }
    if ($DisableSparseOwnership) { $env:VENPOD_SPARSE_RENDER_OWNERSHIP = "0" }
    if ($SparseOwnershipInterval -gt 0) { $env:VENPOD_SPARSE_RENDER_OWNERSHIP_INTERVAL = "$SparseOwnershipInterval" }
    if ($RequireSparsePipeReady) {
        $SparsePipeReadyFrame = [Math]::Max(1, $SparsePipeReadyFrame)
        $env:VENPOD_SPARSE_REQUIRE_PIPE_READY = "1"
        $env:VENPOD_SPARSE_PIPE_READY_FRAME = "$SparsePipeReadyFrame"
    }
    if ($RequireSparseOwnershipQuality) {
        $SparseMinTerrainPixelsPct = [Math]::Min(100, [Math]::Max(0, $SparseMinTerrainPixelsPct))
        $SparseMaxMissPixelsPct = [Math]::Min(100, [Math]::Max(0, $SparseMaxMissPixelsPct))
        $SparseOwnershipQualityReadyFrame = [Math]::Max(1, $SparseOwnershipQualityReadyFrame)
        $env:VENPOD_SPARSE_REQUIRE_OWNERSHIP_QUALITY = "1"
        $env:VENPOD_SPARSE_OWNERSHIP_QUALITY_READY_FRAME = "$SparseOwnershipQualityReadyFrame"
        $env:VENPOD_SPARSE_MIN_TERRAIN_PIXELS_PCT = "$SparseMinTerrainPixelsPct"
        $env:VENPOD_SPARSE_MAX_MISS_PIXELS_PCT = "$SparseMaxMissPixelsPct"
    }
    if ($RequireSparseOwnershipStability) {
        $SparseMaxTerrainDeltaPct = [Math]::Min(100, [Math]::Max(0, $SparseMaxTerrainDeltaPct))
        $SparseMaxMissDeltaPct = [Math]::Min(100, [Math]::Max(0, $SparseMaxMissDeltaPct))
        $SparseOwnershipStabilityReadyFrame = [Math]::Max(1, $SparseOwnershipStabilityReadyFrame)
        $env:VENPOD_SPARSE_REQUIRE_OWNERSHIP_STABILITY = "1"
        $env:VENPOD_SPARSE_OWNERSHIP_STABILITY_READY_FRAME = "$SparseOwnershipStabilityReadyFrame"
        $env:VENPOD_SPARSE_MAX_TERRAIN_DELTA_PCT = "$SparseMaxTerrainDeltaPct"
        $env:VENPOD_SPARSE_MAX_MISS_DELTA_PCT = "$SparseMaxMissDeltaPct"
    }
    if ($SparseSurfaceSmoke) {
        $SparseMinSurfaceFragments = [Math]::Max(1, $SparseMinSurfaceFragments)
        $SparseSurfaceFragmentsReadyFrame = [Math]::Max(1, $SparseSurfaceFragmentsReadyFrame)
        $env:VENPOD_SPARSE_SURFACE_DIAGNOSTIC_SEED = "1"
        $env:VENPOD_SPARSE_REQUIRE_SURFACE_FRAGMENTS = "1"
        $env:VENPOD_SPARSE_MIN_SURFACE_FRAGMENTS = "$SparseMinSurfaceFragments"
        $env:VENPOD_SPARSE_SURFACE_FRAGMENTS_READY_FRAME = "$SparseSurfaceFragmentsReadyFrame"
    }
    if ($SparseGpuRaycastSmoke) {
        $SparseGpuRaycastHealthReadyFrame = [Math]::Max(1, $SparseGpuRaycastHealthReadyFrame)
        if ($SparseGpuRaycastMaxFallbackPct -eq 95) { $SparseGpuRaycastMaxFallbackPct = 0 }
        $SparseGpuRaycastMaxFallbackPct = [Math]::Min(100, [Math]::Max(0, $SparseGpuRaycastMaxFallbackPct))
        $SparseGpuRaycastMinAccepted = [Math]::Max(1, $SparseGpuRaycastMinAccepted)
        $SparseGpuRaycastStrict = $true
        $env:VENPOD_SPARSE_GPU_RAYCAST_DIAGNOSTIC_SEED = "1"
        $env:VENPOD_SPARSE_REQUIRE_GPU_RAYCAST_HEALTH = "1"
        $env:VENPOD_SPARSE_GPU_RAYCAST_STRICT = "1"
        $env:VENPOD_SPARSE_GPU_RAYCAST_HEALTH_READY_FRAME = "$SparseGpuRaycastHealthReadyFrame"
        $env:VENPOD_SPARSE_GPU_RAYCAST_MAX_FALLBACK_PCT = "$SparseGpuRaycastMaxFallbackPct"
        $env:VENPOD_SPARSE_GPU_RAYCAST_MIN_ACCEPTED = "$SparseGpuRaycastMinAccepted"
    }
    if ($BoundaryTest) { $env:VENPOD_ENABLE_TEST_MODES = "1" }
    if ($ExitAfterFrames -gt 0) { $env:VENPOD_EXIT_AFTER_FRAMES = "$ExitAfterFrames" }

    Write-Host "VENPOD - Rebuild + Run" -ForegroundColor Magenta
    Write-Info "Config: $Config"
    if ($HighDensity) {
        Write-Info "High-density dense render window: enabled"
    }
    if ($LowMemoryDense) {
        Write-Info "Low-memory dense render window: enabled (partial coverage debug mode)"
    }
    if ($DisableFarSVO) {
        Write-Info "Far SVO: disabled"
    }
    if ($DenseLegacy) {
        Write-Info "Dense legacy renderer: enabled"
    }
    if ($useSparse) {
        $sparsePhysicsExpected = ($env:VENPOD_ENABLE_SPARSE_PHYSICS -ne '0') -and ($env:VENPOD_DISABLE_PHYSICS -ne '1')
        Write-Info "Sparse test: enabled (only=$([int]($env:VENPOD_SPARSE_ONLY -eq '1')), debug=$($env:VENPOD_SPARSE_DEBUG_MODE), surface-auth=$([int]($env:VENPOD_SPARSE_SURFACE_AUTHORITATIVE -eq '1')), gpu-raycast=$([int]($env:VENPOD_SPARSE_GPU_RAYCAST -eq '1')), miss-feedback=$([int]($env:VENPOD_SPARSE_MISS_FEEDBACK -eq '1')), brush-feedback=$([int]($env:VENPOD_SPARSE_BRUSH_FEEDBACK -eq '1')), brush-apply=$([int]($env:VENPOD_SPARSE_BRUSH_FEEDBACK_APPLY -eq '1')), brush-auth=$([int]($env:VENPOD_SPARSE_BRUSH_FEEDBACK_AUTHORITATIVE -eq '1')), physics=$([int]$sparsePhysicsExpected), gpu-physics=$([int]($env:VENPOD_SPARSE_PHYSICS_GPU -eq '1')), gpu-apply=$([int]($env:VENPOD_SPARSE_PHYSICS_GPU_APPLY -eq '1')), physics-seed=$([int]($env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_SEED -eq '1')), fluid-seed=$([int]($env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_FLUID_SEED -eq '1')), validate-pool=$([int]($env:VENPOD_SPARSE_VALIDATE_POOL -eq '1')), stress=$([int]($env:VENPOD_SPARSE_STRESS_REQUESTS -eq '1')), stress-cam=$([int]($env:VENPOD_SPARSE_STRESS_CAMERA -eq '1')))"
        if ($SparseSmoke) {
            Write-Info "Sparse smoke preset: pipe readiness + ownership quality + pool validation + stress camera/requests"
        }
        if ($SparsePhysicsSmoke) {
            Write-Info "Sparse physics smoke preset: GPU physics diagnostics + proposal apply + pipe readiness + ownership quality + pool validation"
        }
        if ($SparseFlickerSmoke) {
            Write-Info "Sparse flicker smoke preset: stationary every-frame ownership stability + pipe/quality checks + pool validation"
        }
        if ($SparseSurfaceSmoke) {
            Write-Info "Sparse surface smoke preset: seeded editable near-surface + every-frame surface-fragment gate"
        }
        if ($SparseGpuRaycastSmoke) {
            Write-Info "Sparse GPU raycast smoke preset: GPU brush raycast health + ownership quality + pool validation"
        }
        if ($SparseMissFeedbackSmoke) {
            Write-Info "Sparse miss-feedback smoke preset: async miss readback + request feedback + pool validation"
        }
        if ($useSparseDefault) {
            Write-Info "Sparse surface-authoritative renderer is the default rebrun target; use -DenseLegacy for the old renderer."
            Write-Info "Startup contract: hold public render for coherent surface/mid/Far-SVO readiness; hidden exact streams after open."
        }
        if ($SparseEditFile -ne "") {
            Write-Info "Sparse edit persistence file: $SparseEditFile"
        }
    }
    if ($ExitAfterFrames -gt 0) {
        Write-Info "Exit-after-frames smoke mode: $ExitAfterFrames"
    }
    if ($RequireSparsePipeReady) {
        Write-Info "Sparse backend pipe readiness required by frame: $SparsePipeReadyFrame"
    }
    if ($RequireSparseOwnershipQuality) {
        Write-Info "Sparse render ownership quality required by frame: $SparseOwnershipQualityReadyFrame (terrain >= $SparseMinTerrainPixelsPct%, miss <= $SparseMaxMissPixelsPct%)"
    }
    if ($RequireSparseOwnershipStability) {
        Write-Info "Sparse render ownership stability required by frame: $SparseOwnershipStabilityReadyFrame (terrain delta <= $SparseMaxTerrainDeltaPct%, miss delta <= $SparseMaxMissDeltaPct%)"
    }
    if ($SparseSurfaceSmoke) {
        Write-Info "Sparse surface fragments required by frame: $SparseSurfaceFragmentsReadyFrame (fragments >= $SparseMinSurfaceFragments)"
    }
    if ($SparseGpuRaycastSmoke) {
        Write-Info "Sparse GPU raycast health required by frame: $SparseGpuRaycastHealthReadyFrame (accepted >= $SparseGpuRaycastMinAccepted, fallback <= $SparseGpuRaycastMaxFallbackPct%)"
    }

    if ($NoBuild) {
        Write-Info "Build step: skipped (-NoBuild)"
        Write-Step "Refreshing runtime assets for -NoBuild..."
        Refresh-RuntimeAssets
        Write-Success "Runtime assets refreshed"
    } else {
        Write-Step "Building latest code..."
        if ($Clean) {
            & $buildScript -Config $Config -Clean
        } else {
            & $buildScript -Config $Config
        }
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }

    Write-Step "Launching VENPOD..."
    if ($ForceSync) {
        & $runScript -Config $Config -ForceSync
    } else {
        & $runScript -Config $Config
    }
    $runExitCode = $LASTEXITCODE
    if (($SparseSmoke -or $SparsePhysicsSmoke -or $SparseFlickerSmoke -or $SparseSurfaceSmoke -or $SparseGpuRaycastSmoke -or $SparseMissFeedbackSmoke -or $SparseBrushFeedbackSmoke) -and $runExitCode -eq 0) {
        $runtimeLog = Join-Path $projectRoot "build\bin\venpod_runtime.log"
        if (Test-Path $runtimeLog) {
            $badLogLines = Select-String `
                -Path $runtimeLog `
                -Pattern "\] \[(critical|error)\]|device removed|device-removed|timeout|SPARSE_BACKEND_PIPE readiness failed|SPARSE_RENDER_OWNERSHIP quality failed|SPARSE_RENDER_OWNERSHIP stability failed|SPARSE_SURFACE_FRAGMENTS failed|SPARSE_GPU_RAYCAST health failed" `
                -CaseSensitive:$false
            if ($badLogLines) {
                Write-Host "[ERROR] Sparse smoke found runtime failure markers:" -ForegroundColor Red
                $badLogLines | Select-Object -First 20 | ForEach-Object {
                    Write-Host "  $($_.Line)" -ForegroundColor Red
                }
                exit 10
            }
            if ($SparsePhysicsSmoke) {
                $physicsResultLines = Select-String `
                    -Path $runtimeLog `
                    -Pattern "PERF_SPARSE_PHYSICS_GPU_RESULT" `
                    -CaseSensitive:$false
                if (-not $physicsResultLines) {
                    Write-Host "[ERROR] Sparse physics smoke did not observe GPU physics result readback lines." -ForegroundColor Red
                    exit 11
                }
            }
            if ($SparseGpuRaycastSmoke) {
                $raycastResultLines = Select-String `
                    -Path $runtimeLog `
                    -Pattern "SPARSE_GPU_RAYCAST health observed|PERF_SPARSE_GPU_RAYCAST.*health=1" `
                    -CaseSensitive:$false
                if (-not $raycastResultLines) {
                    Write-Host "[ERROR] Sparse GPU raycast smoke did not observe healthy GPU raycast diagnostics." -ForegroundColor Red
                    exit 12
                }
            }
            if ($SparseMissFeedbackSmoke) {
                $missFeedbackPipelineLines = Select-String `
                    -Path $runtimeLog `
                    -Pattern "Sparse miss feedback pipeline created successfully" `
                    -CaseSensitive:$false
                $missFeedbackPendingLines = Select-String `
                    -Path $runtimeLog `
                    -Pattern "missPending=[1-9][0-9]*" `
                    -CaseSensitive:$false
                $missFeedbackCleanOwnershipLines = Select-String `
                    -Path $runtimeLog `
                    -Pattern "PERF_RENDER_OWNERSHIP .*miss=0 unsafeNearMiss=0" `
                    -CaseSensitive:$false
                if (-not $missFeedbackPipelineLines -or (-not $missFeedbackPendingLines -and -not $missFeedbackCleanOwnershipLines)) {
                    Write-Host "[ERROR] Sparse miss feedback smoke did not observe pipeline creation with either nonzero pending miss requests or clean zero-miss ownership." -ForegroundColor Red
                    exit 15
                }
            }
            if ($SparseBrushFeedbackSmoke) {
                $brushFeedbackDiagnosticLines = Select-String `
                    -Path $runtimeLog `
                    -Pattern "Sparse brush feedback diagnostic queued" `
                    -CaseSensitive:$false
                $brushFeedbackRetireLines = Select-String `
                    -Path $runtimeLog `
                    -Pattern "SPARSE_BRUSH_FEEDBACK parity observed .* expected=[1-9][0-9]* gpu=[1-9][0-9]* matched=[1-9][0-9]* missingResident=0" `
                    -CaseSensitive:$false
                $brushFeedbackResidentLines = Select-String `
                    -Path $runtimeLog `
                    -Pattern "brushGpuFbMiss=0" `
                    -CaseSensitive:$false
                $brushFeedbackParityLines = Select-String `
                    -Path $runtimeLog `
                    -Pattern "SPARSE_BRUSH_FEEDBACK parity observed" `
                    -CaseSensitive:$false
                $brushFeedbackSuitePattern = if ($env:VENPOD_SPARSE_BRUSH_FEEDBACK_STRICT_RESIDENT_ONLY -eq "1") {
                    "SPARSE_BRUSH_FEEDBACK diagnostic suite passed cases=6 strictResidentOnly=1"
                } else {
                    "SPARSE_BRUSH_FEEDBACK diagnostic suite passed cases=7"
                }
                $brushFeedbackSuiteLines = Select-String `
                    -Path $runtimeLog `
                    -Pattern $brushFeedbackSuitePattern `
                    -CaseSensitive:$false
                if (-not $brushFeedbackDiagnosticLines -or -not $brushFeedbackRetireLines -or -not $brushFeedbackResidentLines -or -not $brushFeedbackParityLines -or -not $brushFeedbackSuiteLines) {
                    Write-Host "[ERROR] Sparse brush feedback smoke did not observe queued diagnostic, retired resident feedback records, zero missing resident pages, and full parity-suite success." -ForegroundColor Red
                    exit 13
                }
                if ($SparseBrushFeedbackApply) {
                    $brushFeedbackApplyLines = Select-String `
                        -Path $runtimeLog `
                        -Pattern "SPARSE_BRUSH_FEEDBACK GPU apply .* records=[1-9][0-9]*" `
                        -CaseSensitive:$false
                    $brushFeedbackFallbackLines = Select-String `
                        -Path $runtimeLog `
                        -Pattern "SPARSE_BRUSH_FEEDBACK CPU fallback .* missingResident=[1-9][0-9]*.*hints=[1-9][0-9]*.*queuedBricks=[1-9][0-9]*" `
                        -CaseSensitive:$false
                    if (-not $brushFeedbackApplyLines) {
                        Write-Host "[ERROR] Sparse brush feedback apply smoke did not observe applied feedback records." -ForegroundColor Red
                        exit 14
                    }
                    if (-not $brushFeedbackFallbackLines) {
                        Write-Host "[ERROR] Sparse brush feedback apply smoke did not observe CPU fallback for nonresident GPU feedback." -ForegroundColor Red
                        exit 15
                    }
                }
            }
        }
    }
    exit $runExitCode
}
finally {
    Restore-Env
}
