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
    [switch]$SparseBrushSmokeUserPath,
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
    [int]$ExitAfterFrames = 0,
    # Generation Overhaul V2 performance modes (see generation-overhaul-v2.md):
    #   quality -> ~100 FPS median, FULL-RES near+far, NO quality reduction. THE DEFAULT
    #     no-dip ~100fps build (early-Z prepass + interest signature-reuse/interval=2 + lazy
    #     stats; GPU ~94% idle headroom; only ~1.5% grazing-horizon frames dip on far raymarch).
    #   60fps -> ~60 FPS, stable, no holes; terrain COARSER (bounded generation)
    #   30fps -> ~30 FPS, stable, no holes; terrain DETAILED (high coverage)
    # All use best-available-LOD render (no freezes/holes). "none" = legacy raw path.
    [ValidateSet("none", "60fps", "30fps", "detail", "quality")]
    [string]$PerfMode = "quality",
    # Raymarch render scale (the dominant FPS lever, since the frame is GPU-raymarch
    # bound). <1.0 renders the voxel raymarch at lower res then upscales: lower =
    # faster but softer. 0 = use the perf-mode default. e.g. -RenderScale 0.4
    [double]$RenderScale = 0,
    # Isolated profiling knob: force the far/background split pass inside quality
    # mode without changing the foreground raymarch render scale. This is not a
    # default quality policy; use it to measure/visual-audit far-field products.
    [double]$ExperimentalBackgroundPassScale = 0,
    # Isolated profiling knob for the experimental background split: skip the
    # near/surface fill path inside the low-res background pass. Defaults keep
    # the existing perf-mode behavior unchanged.
    [switch]$ExperimentalBackgroundPassNoSurfaceFill,
    # Isolated profiling knob for the experimental background split: draw the
    # foreground surface depth/stencil into the low-res background target so the
    # background raymarch skips pixels already owned by raster surfaces.
    [switch]$ExperimentalBackgroundPassForegroundMask,
    # Isolated profiling knob for surface-tail work: cap only the broad/general
    # pre-publish surface extraction pass. Terrain-critical and hidden-critical
    # repair coordinates still run first under their existing budgets.
    [int]$ExperimentalPrePublishGeneralSurfaceBudget = -1,
    # Isolated profiling knob for edit/brush surface-tail work: while edits are
    # active, cap only the broad/general pre-publish surface extraction pass.
    [int]$ExperimentalPrePublishEditGeneralSurfaceBudget = -1,
    # Isolated profiling knob for post-edit surface-tail work: after the edit
    # active window, cap non-critical pre-publish surface extraction only when
    # the frame-pressure threshold is exceeded. Ownership-critical visible/edit/
    # collision surface work remains protected.
    [int]$ExperimentalPrePublishPostEditGeneralSurfaceBudget = -1,
    [int]$ExperimentalPrePublishPostEditGeneralSpillFrames = -1,
    [double]$ExperimentalPrePublishPostEditGeneralSpillPressureMs = -1,
    # Isolated profiling knob for edit/brush hidden-exact repair: cap the
    # post-open screen probe time without changing startup proof.
    [int]$ExperimentalHiddenExactPostOpenProbeMaxMsTenths = -1,
    # Isolated profiling knob for edit/brush hidden-exact repair: cap post-open
    # hidden-exact forced generation without changing startup safety.
    [int]$ExperimentalHiddenExactPostOpenGenerationBudget = -1,
    # Isolated profiling knob for edit/brush hidden-exact repair: cap post-open
    # hidden-critical pre-publish surface work without changing startup safety.
    [int]$ExperimentalHiddenExactPostOpenSurfaceBudget = -1,
    # Isolated profiling knob for edit/brush hidden-exact tracked catchup: cap
    # non-critical tracked pre-publish surface work after critical repair.
    [int]$ExperimentalPrePublishHiddenTrackedSurfaceBudget = -1,
    # Isolated profiling knob for mid-clipmap generation: override the
    # perf-mode hard pump budget without changing the rest of the quality preset.
    [double]$ExperimentalMidClipmapPumpHardBudgetMs = -1,
    # Isolated profiling knob: allow async surface meshing during edits for coords
    # whose local edit dependency neighborhood has no overlay.
    [switch]$ExperimentalSurfaceAsyncPerCoordEditGate,
    # Loop 86 surface work route: general (Visible/Speculative) surface catch-up
    # prefers per-coord async enqueue over the blocking fork-join batch, gated per
    # frame on async backlog + surface-ready publish backlog + page publish backlog.
    # -1 = leave default (off), 0 = force off, 1 = force on.
    [int]$ExperimentalSurfaceRouteGeneralAsync = -1,
    [int]$ExperimentalSurfaceRouteAsyncBacklogLimit = -1,
    [int]$ExperimentalSurfaceRoutePublishPendingLimit = -1,
    [int]$ExperimentalSurfaceRoutePublishAgeLimit = -1,
    [int]$ExperimentalSurfaceRoutePagePublishLimit = -1
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
    VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_PROBE_MAX_MS_TENTHS = $env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_PROBE_MAX_MS_TENTHS
    VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_GENERATION_BUDGET = $env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_GENERATION_BUDGET
    VENPOD_SPARSE_REQUEST_EXPLICIT_SOURCE_LANES = $env:VENPOD_SPARSE_REQUEST_EXPLICIT_SOURCE_LANES
    VENPOD_SPARSE_HIDDEN_EXACT_DEFER_PROACTIVE = $env:VENPOD_SPARSE_HIDDEN_EXACT_DEFER_PROACTIVE
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
    VENPOD_STREAMING_V2 = $env:VENPOD_STREAMING_V2
    VENPOD_SPARSE_MID_CLIPMAP_PUMP_HARD_BUDGET_MS = $env:VENPOD_SPARSE_MID_CLIPMAP_PUMP_HARD_BUDGET_MS
    VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_REBUILD_RINGS_PER_FRAME = $env:VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_REBUILD_RINGS_PER_FRAME
    VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH = $env:VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH
    VENPOD_RAYMARCH_RENDER_SCALE = $env:VENPOD_RAYMARCH_RENDER_SCALE
    VENPOD_SPARSE_SURFACE_STRICT_TIME_BUDGET = $env:VENPOD_SPARSE_SURFACE_STRICT_TIME_BUDGET
    VENPOD_SPARSE_SURFACE_EXTRACTION_MAX_MS = $env:VENPOD_SPARSE_SURFACE_EXTRACTION_MAX_MS
    VENPOD_SPARSE_PRE_PUBLISH_SURFACE_EXTRACTION_MAX_MS = $env:VENPOD_SPARSE_PRE_PUBLISH_SURFACE_EXTRACTION_MAX_MS
    VENPOD_SPARSE_POST_OPEN_PRE_PUBLISH_SURFACE_EXTRACTION_MAX_MS = $env:VENPOD_SPARSE_POST_OPEN_PRE_PUBLISH_SURFACE_EXTRACTION_MAX_MS
    VENPOD_SPARSE_PRE_PUBLISH_GENERAL_SURFACE_BUDGET = $env:VENPOD_SPARSE_PRE_PUBLISH_GENERAL_SURFACE_BUDGET
    VENPOD_SPARSE_PRE_PUBLISH_EDIT_GENERAL_SURFACE_BUDGET = $env:VENPOD_SPARSE_PRE_PUBLISH_EDIT_GENERAL_SURFACE_BUDGET
    VENPOD_SPARSE_PRE_PUBLISH_POST_EDIT_GENERAL_SURFACE_BUDGET = $env:VENPOD_SPARSE_PRE_PUBLISH_POST_EDIT_GENERAL_SURFACE_BUDGET
    VENPOD_SPARSE_PRE_PUBLISH_POST_EDIT_GENERAL_SPILL_FRAMES = $env:VENPOD_SPARSE_PRE_PUBLISH_POST_EDIT_GENERAL_SPILL_FRAMES
    VENPOD_SPARSE_PRE_PUBLISH_POST_EDIT_GENERAL_SPILL_PRESSURE_MS = $env:VENPOD_SPARSE_PRE_PUBLISH_POST_EDIT_GENERAL_SPILL_PRESSURE_MS
    VENPOD_SPARSE_PRE_PUBLISH_STACKED_CLIPMAP_PREP_THRESHOLD_MS = $env:VENPOD_SPARSE_PRE_PUBLISH_STACKED_CLIPMAP_PREP_THRESHOLD_MS
    VENPOD_SPARSE_PRE_PUBLISH_STACKED_GENERAL_SURFACE_BUDGET = $env:VENPOD_SPARSE_PRE_PUBLISH_STACKED_GENERAL_SURFACE_BUDGET
    VENPOD_SPARSE_PRE_PUBLISH_HIDDEN_TRACKED_SURFACE_BUDGET = $env:VENPOD_SPARSE_PRE_PUBLISH_HIDDEN_TRACKED_SURFACE_BUDGET
    VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_SURFACE_BUDGET = $env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_SURFACE_BUDGET
    VENPOD_SPARSE_SURFACE_ASYNC_PERCOORD_EDIT_GATE = $env:VENPOD_SPARSE_SURFACE_ASYNC_PERCOORD_EDIT_GATE
    VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE = $env:VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE
    VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE = $env:VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE
    VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL = $env:VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL
    VENPOD_RAYMARCH_BACKGROUND_PASS_FOREGROUND_MASK = $env:VENPOD_RAYMARCH_BACKGROUND_PASS_FOREGROUND_MASK
    VENPOD_SPARSE_SURFACE_PARALLEL_EXTRACTION = $env:VENPOD_SPARSE_SURFACE_PARALLEL_EXTRACTION
    VENPOD_SPARSE_SURFACE_PARALLEL_TIME_BUDGETED = $env:VENPOD_SPARSE_SURFACE_PARALLEL_TIME_BUDGETED
    VENPOD_SPARSE_SURFACE_PARALLEL_MAX_WORKERS = $env:VENPOD_SPARSE_SURFACE_PARALLEL_MAX_WORKERS
    VENPOD_SPARSE_SURFACE_ASYNC_EXTRACTION = $env:VENPOD_SPARSE_SURFACE_ASYNC_EXTRACTION
    VENPOD_SPARSE_SURFACE_ASYNC_MAX_WORKERS = $env:VENPOD_SPARSE_SURFACE_ASYNC_MAX_WORKERS
    VENPOD_SPARSE_SURFACE_ROUTE_GENERAL_ASYNC = $env:VENPOD_SPARSE_SURFACE_ROUTE_GENERAL_ASYNC
    VENPOD_SPARSE_SURFACE_ROUTE_ASYNC_BACKLOG_LIMIT = $env:VENPOD_SPARSE_SURFACE_ROUTE_ASYNC_BACKLOG_LIMIT
    VENPOD_SPARSE_SURFACE_ROUTE_PUBLISH_PENDING_LIMIT = $env:VENPOD_SPARSE_SURFACE_ROUTE_PUBLISH_PENDING_LIMIT
    VENPOD_SPARSE_SURFACE_ROUTE_PUBLISH_AGE_LIMIT = $env:VENPOD_SPARSE_SURFACE_ROUTE_PUBLISH_AGE_LIMIT
    VENPOD_SPARSE_SURFACE_ROUTE_PAGE_PUBLISH_LIMIT = $env:VENPOD_SPARSE_SURFACE_ROUTE_PAGE_PUBLISH_LIMIT
    VENPOD_SPARSE_EXACT_ASYNC_GENERATION = $env:VENPOD_SPARSE_EXACT_ASYNC_GENERATION
    VENPOD_SPARSE_EXACT_ASYNC_VISIBLE = $env:VENPOD_SPARSE_EXACT_ASYNC_VISIBLE
    VENPOD_SPARSE_EXACT_ASYNC_PREFETCH_LANE = $env:VENPOD_SPARSE_EXACT_ASYNC_PREFETCH_LANE
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
    Remove-Item env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_PROBE_MAX_MS_TENTHS -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_GENERATION_BUDGET -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_REQUEST_EXPLICIT_SOURCE_LANES -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_HIDDEN_EXACT_DEFER_PROACTIVE -ErrorAction SilentlyContinue
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
    # User-path smoke: ONLY enables the scripted brush. No feedback pipeline, no
    # debug colors, no physics disable - the exact config interactive play runs.
    if ($SparseBrushSmokeUserPath) { $env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE = "1" }
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

    # --- Generation Overhaul V2 performance modes (architecture-aligned) ---
    # The framerate is GPU-RAYMARCH bound: the fullscreen raymarch fills every pixel
    # the cheap rasterized surfaces don't cover (mostly the far horizon). The real GPU
    # lever is the LOW-RES BACKGROUND PASS (render the far/horizon raymarch at reduced
    # resolution and composite; the near stays full-res sharp). V2 keeps it stable
    # (best-available render, no force-gen freezes). We do NOT bound generation here --
    # bounding lowers surface coverage which pushes MORE pixels onto the raymarch.
    # Applied after the sparse env setup so it overrides the startup render gate.
    foreach ($pvName in @(
        "VENPOD_STREAMING_V2",
        "VENPOD_SPARSE_MID_CLIPMAP_PUMP_HARD_BUDGET_MS",
        "VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_REBUILD_RINGS_PER_FRAME",
        "VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH",
        "VENPOD_SPARSE_REQUEST_EXPLICIT_SOURCE_LANES",
        "VENPOD_SPARSE_HIDDEN_EXACT_DEFER_PROACTIVE",
        "VENPOD_SPARSE_SURFACE_STRICT_TIME_BUDGET",
        "VENPOD_SPARSE_SURFACE_EXTRACTION_MAX_MS",
        "VENPOD_SPARSE_PRE_PUBLISH_SURFACE_EXTRACTION_MAX_MS",
        "VENPOD_SPARSE_POST_OPEN_PRE_PUBLISH_SURFACE_EXTRACTION_MAX_MS",
        "VENPOD_SPARSE_PRE_PUBLISH_GENERAL_SURFACE_BUDGET",
        "VENPOD_SPARSE_PRE_PUBLISH_EDIT_GENERAL_SURFACE_BUDGET",
        "VENPOD_SPARSE_PRE_PUBLISH_POST_EDIT_GENERAL_SURFACE_BUDGET",
        "VENPOD_SPARSE_PRE_PUBLISH_POST_EDIT_GENERAL_SPILL_FRAMES",
        "VENPOD_SPARSE_PRE_PUBLISH_POST_EDIT_GENERAL_SPILL_PRESSURE_MS",
        "VENPOD_SPARSE_PRE_PUBLISH_STACKED_CLIPMAP_PREP_THRESHOLD_MS",
        "VENPOD_SPARSE_PRE_PUBLISH_STACKED_GENERAL_SURFACE_BUDGET",
        "VENPOD_SPARSE_PRE_PUBLISH_HIDDEN_TRACKED_SURFACE_BUDGET",
        "VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_SURFACE_BUDGET",
        "VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_PROBE_MAX_MS_TENTHS",
        "VENPOD_SPARSE_SURFACE_ASYNC_PERCOORD_EDIT_GATE",
        "VENPOD_RAYMARCH_RENDER_SCALE",
        "VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE",
        "VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE",
        "VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL",
        "VENPOD_RAYMARCH_BACKGROUND_PASS_FOREGROUND_MASK",
        "VENPOD_SPARSE_SURFACE_PARALLEL_EXTRACTION",
        "VENPOD_SPARSE_SURFACE_PARALLEL_TIME_BUDGETED",
        "VENPOD_SPARSE_SURFACE_PARALLEL_MAX_WORKERS",
        "VENPOD_SPARSE_SURFACE_ASYNC_EXTRACTION",
        "VENPOD_SPARSE_SURFACE_ASYNC_MAX_WORKERS",
        "VENPOD_SPARSE_SURFACE_ASYNC_MAX_APPLY_PER_FRAME",
        "VENPOD_SPARSE_SURFACE_ROUTE_GENERAL_ASYNC",
        "VENPOD_SPARSE_SURFACE_ROUTE_ASYNC_BACKLOG_LIMIT",
        "VENPOD_SPARSE_SURFACE_ROUTE_PUBLISH_PENDING_LIMIT",
        "VENPOD_SPARSE_SURFACE_ROUTE_PUBLISH_AGE_LIMIT",
        "VENPOD_SPARSE_SURFACE_ROUTE_PAGE_PUBLISH_LIMIT",
        "VENPOD_SPARSE_SURFACE_EXTRACTION_BUDGET",
        "VENPOD_SPARSE_SURFACE_UPLOAD_MIN_INTERVAL_FRAMES",
        "VENPOD_SPARSE_SURFACE_COPY_FACE_BUDGET",
        "VENPOD_SPARSE_SURFACE_COPY_REGION_BUDGET",
        "VENPOD_SPARSE_STATS_SINGLE_FLUSH",
        "VENPOD_SPARSE_EXACT_ASYNC_GENERATION",
        "VENPOD_SPARSE_EXACT_ASYNC_VISIBLE",
        "VENPOD_SPARSE_EXACT_ASYNC_PREFETCH_LANE",
        "VENPOD_SPARSE_EXACT_ASYNC_MAX_APPLY_PER_FRAME",
        "VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_GEN",
        "VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_GEN",
        "VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_MAX_ENQUEUE",
        "VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_MAX_APPLY",
        "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_VISIBLE_PUMP",
        "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PERSISTENT_WORKERS",
        "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PUMP_MAX_WORKERS",
        "VENPOD_SPARSE_MID_VOXEL_COVERAGE_CATCHUP_BUDGET",
        "VENPOD_SPARSE_MID_VOXEL_RADIUS_XZ",
        "VENPOD_SPARSE_MID_CLIPMAP_GPU_GENERATION",
        "VENPOD_RAYMARCH_MID_PASS_ENABLE",
        "VENPOD_SPARSE_MID_INTEREST_INTERVAL",
        "VENPOD_SPARSE_MID_CLIPMAP_FOOTPRINT_INTEREST_SIGNATURE",
        "VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_SIGNATURE_REUSE",
        "VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_SIGNATURE_REUSE_MAX_AGE",
        "VENPOD_SPARSE_HIDDEN_EXACT_MISS_PROBE_INTERVAL")) {
        Remove-Item "env:$pvName" -ErrorAction SilentlyContinue
    }
    if ($PerfMode -ne "none") {
        $quality = $PerfMode -eq "quality"
        # quality mode = visual correctness first. Exact foreground surfaces stay
        # full-res; the far/background product is foreground-masked so it does
        # not raymarch pixels already owned by raster surfaces.
        $bgScale = if ($PerfMode -eq "60fps") { "0.3" } else { "0.5" }  # far raymarch res
        $pumpBudget =
            if ($ExperimentalMidClipmapPumpHardBudgetMs -ge 0) {
                "{0:0.###}" -f $ExperimentalMidClipmapPumpHardBudgetMs
            } elseif ($quality) {
                "4"
            } elseif ($PerfMode -eq "60fps") {
                "16"
            } else {
                "12"
            }
        $surfBudget = if ($quality) { "24" } elseif ($PerfMode -eq "60fps") { "4" } else { "8" }
        $qualityDefaultBackgroundMask = $quality -and $ExperimentalBackgroundPassScale -le 0
        $useBgPass = (-not $quality) -or $qualityDefaultBackgroundMask
        if ($ExperimentalBackgroundPassScale -gt 0) {
            $bgScale = "{0:0.###}" -f ([Math]::Min(1.0, [Math]::Max(0.25, $ExperimentalBackgroundPassScale)))
            $useBgPass = $true
        }
        $env:VENPOD_STREAMING_V2 = "1"
        $env:VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH = "0"   # speculative; not needed under best-available
        # After public render opens, keep forced hidden-exact generation/upload focused on
        # current critical feedback instead of replaying the whole tracked history.
        # Explicit source-lane request admission remains off until surface readiness is
        # proven stable with it.
        $env:VENPOD_SPARSE_HIDDEN_EXACT_DEFER_PROACTIVE = "1"
        # CPU caps: when MOVING, the mid-clipmap pump + surface meshing explode
        # (real play: clip hit ~99ms unbounded). Bound them; V2 best-available renders
        # the not-yet-generated terrain coarser instead of stalling. The old downside
        # (less coverage -> more raymarch) no longer matters: the background pass +
        # render scale handle the GPU regardless of coverage.
        $env:VENPOD_SPARSE_MID_CLIPMAP_PUMP_HARD_BUDGET_MS = $pumpBudget
        $env:VENPOD_SPARSE_SURFACE_STRICT_TIME_BUDGET = "1"
        $env:VENPOD_SPARSE_SURFACE_EXTRACTION_MAX_MS = $surfBudget
        $env:VENPOD_SPARSE_PRE_PUBLISH_SURFACE_EXTRACTION_MAX_MS = $surfBudget
        $env:VENPOD_SPARSE_POST_OPEN_PRE_PUBLISH_SURFACE_EXTRACTION_MAX_MS = $surfBudget
        if ($quality) {
            # Loop 68: masked-background quality removes the raymarch floor; the
            # remaining edit hitches came from hidden/general surface catch-up
            # releasing in the same frame. Keep critical repair bounded and let
            # broad surface debt spill instead of hitching.
            if ($ExperimentalHiddenExactPostOpenSurfaceBudget -lt 0) {
                $env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_SURFACE_BUDGET = "8"
            }
            if ($ExperimentalPrePublishHiddenTrackedSurfaceBudget -lt 0) {
                $env:VENPOD_SPARSE_PRE_PUBLISH_HIDDEN_TRACKED_SURFACE_BUDGET = "0"
            }
            if ($ExperimentalPrePublishEditGeneralSurfaceBudget -lt 0) {
                # Loop 88: was "0" (protect edit frames when extraction was all
                # inline). Zero starved the stroke's OWN bricks: their surfaces
                # never meshed, the surface-ready gate blocked their publishes,
                # and edits only rendered after release. With the per-coord edit
                # gate + async routing, edit-window general extraction is mostly
                # cheap async ENQUEUE, so a real budget is frame-safe.
                $env:VENPOD_SPARSE_PRE_PUBLISH_EDIT_GENERAL_SURFACE_BUDGET = "48"
            }
            if ($ExperimentalPrePublishPostEditGeneralSurfaceBudget -lt 0) {
                $env:VENPOD_SPARSE_PRE_PUBLISH_POST_EDIT_GENERAL_SURFACE_BUDGET = "0"
            }
            if ($ExperimentalPrePublishPostEditGeneralSpillFrames -lt 0) {
                $env:VENPOD_SPARSE_PRE_PUBLISH_POST_EDIT_GENERAL_SPILL_FRAMES = "180"
            }
            if ($ExperimentalPrePublishPostEditGeneralSpillPressureMs -lt 0) {
                $env:VENPOD_SPARSE_PRE_PUBLISH_POST_EDIT_GENERAL_SPILL_PRESSURE_MS = "15"
            }
            if ($ExperimentalHiddenExactPostOpenProbeMaxMsTenths -lt 0) {
                $env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_PROBE_MAX_MS_TENTHS = "8"
            }
            if ($ExperimentalPrePublishGeneralSurfaceBudget -lt 0) {
                $env:VENPOD_SPARSE_PRE_PUBLISH_GENERAL_SURFACE_BUDGET = "24"
            }
            $env:VENPOD_SPARSE_PRE_PUBLISH_STACKED_CLIPMAP_PREP_THRESHOLD_MS = "3"
            $env:VENPOD_SPARSE_PRE_PUBLISH_STACKED_GENERAL_SURFACE_BUDGET = "8"
            $env:VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_REBUILD_RINGS_PER_FRAME = "1"
            # Loop 86 promoted default: route general (Visible/Speculative) surface
            # catch-up to the async mesher instead of the blocking fork-join batch,
            # gated per frame on async backlog + surface-ready publish backlog +
            # page publish backlog (limits 512/192/8/256 are code defaults).
            # Measured: walk 11.02/15.77/17.71 -> 7.55/11.42/14.54; edit p95
            # 13.89 -> 12.05; idle/yaw improved; correctness clean; repeats held.
            if ($ExperimentalSurfaceRouteGeneralAsync -lt 0) {
                $env:VENPOD_SPARSE_SURFACE_ROUTE_GENERAL_ASYNC = "1"
            }
        }
        if ($ExperimentalPrePublishGeneralSurfaceBudget -ge 0) {
            $env:VENPOD_SPARSE_PRE_PUBLISH_GENERAL_SURFACE_BUDGET = "$ExperimentalPrePublishGeneralSurfaceBudget"
        }
        if ($ExperimentalPrePublishEditGeneralSurfaceBudget -ge 0) {
            $env:VENPOD_SPARSE_PRE_PUBLISH_EDIT_GENERAL_SURFACE_BUDGET = "$ExperimentalPrePublishEditGeneralSurfaceBudget"
        }
        if ($ExperimentalPrePublishPostEditGeneralSurfaceBudget -ge 0) {
            $env:VENPOD_SPARSE_PRE_PUBLISH_POST_EDIT_GENERAL_SURFACE_BUDGET = "$ExperimentalPrePublishPostEditGeneralSurfaceBudget"
        }
        if ($ExperimentalPrePublishPostEditGeneralSpillFrames -ge 0) {
            $env:VENPOD_SPARSE_PRE_PUBLISH_POST_EDIT_GENERAL_SPILL_FRAMES = "$ExperimentalPrePublishPostEditGeneralSpillFrames"
        }
        if ($ExperimentalPrePublishPostEditGeneralSpillPressureMs -ge 0) {
            $env:VENPOD_SPARSE_PRE_PUBLISH_POST_EDIT_GENERAL_SPILL_PRESSURE_MS =
                "{0:0.###}" -f $ExperimentalPrePublishPostEditGeneralSpillPressureMs
        }
        if ($ExperimentalHiddenExactPostOpenProbeMaxMsTenths -ge 0) {
            $env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_PROBE_MAX_MS_TENTHS = "$ExperimentalHiddenExactPostOpenProbeMaxMsTenths"
        }
        if ($ExperimentalHiddenExactPostOpenGenerationBudget -ge 0) {
            $env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_GENERATION_BUDGET = "$ExperimentalHiddenExactPostOpenGenerationBudget"
        }
        if ($ExperimentalHiddenExactPostOpenSurfaceBudget -ge 0) {
            $env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_SURFACE_BUDGET = "$ExperimentalHiddenExactPostOpenSurfaceBudget"
        }
        if ($ExperimentalPrePublishHiddenTrackedSurfaceBudget -ge 0) {
            $env:VENPOD_SPARSE_PRE_PUBLISH_HIDDEN_TRACKED_SURFACE_BUDGET = "$ExperimentalPrePublishHiddenTrackedSurfaceBudget"
        }
        if ($ExperimentalSurfaceAsyncPerCoordEditGate) {
            $env:VENPOD_SPARSE_SURFACE_ASYNC_PERCOORD_EDIT_GATE = "1"
        }
        # Loop 91: cap per-frame surface payload copy staging. The default (1M faces)
        # let a recenter wave stage a 222k-face upload in ONE frame (= the measured
        # 39ms spike class at exploration speed). 96k spreads such waves over ~3
        # frames; the copy path is budget-aware/retry-safe by design.
        if (-not $env:VENPOD_SPARSE_SURFACE_COPY_FACE_BUDGET) {
            $env:VENPOD_SPARSE_SURFACE_COPY_FACE_BUDGET = "98304"
        }
        # Loop 88 promoted default: per-coord edit gate ON in quality. During a brush
        # stroke the global edit gate rejected ALL async meshing, so the stroke's
        # flood of ordinary visible bricks fell to ~0-8/frame inline extraction,
        # their publishes starved behind the surface-ready gate, and edits rendered
        # only after release ("all appears at once"). The per-coord gate keeps only
        # edit-overlapping bricks inline; everything else meshes on the worker pool.
        if (-not $env:VENPOD_SPARSE_SURFACE_ASYNC_PERCOORD_EDIT_GATE) {
            $env:VENPOD_SPARSE_SURFACE_ASYNC_PERCOORD_EDIT_GATE = "1"
        }
        # Loop 86 surface work route (opt-in probe; code default is OFF):
        if ($ExperimentalSurfaceRouteGeneralAsync -ge 0) {
            $env:VENPOD_SPARSE_SURFACE_ROUTE_GENERAL_ASYNC = "$ExperimentalSurfaceRouteGeneralAsync"
        }
        if ($ExperimentalSurfaceRouteAsyncBacklogLimit -ge 0) {
            $env:VENPOD_SPARSE_SURFACE_ROUTE_ASYNC_BACKLOG_LIMIT = "$ExperimentalSurfaceRouteAsyncBacklogLimit"
        }
        if ($ExperimentalSurfaceRoutePublishPendingLimit -ge 0) {
            $env:VENPOD_SPARSE_SURFACE_ROUTE_PUBLISH_PENDING_LIMIT = "$ExperimentalSurfaceRoutePublishPendingLimit"
        }
        if ($ExperimentalSurfaceRoutePublishAgeLimit -ge 0) {
            $env:VENPOD_SPARSE_SURFACE_ROUTE_PUBLISH_AGE_LIMIT = "$ExperimentalSurfaceRoutePublishAgeLimit"
        }
        if ($ExperimentalSurfaceRoutePagePublishLimit -ge 0) {
            $env:VENPOD_SPARSE_SURFACE_ROUTE_PAGE_PUBLISH_LIMIT = "$ExperimentalSurfaceRoutePagePublishLimit"
        }
        # Fire-and-forget ASYNC surface meshing: surface extraction (~20ms median while
        # moving) runs on a worker pool OFF the main thread, results applied within a
        # per-frame budget. This is the clean async producer (the old fork-join parallel
        # path was contended). Best-available render shows coarser terrain until a mesh
        # lands. The synchronous surface budgets above still bound any inline fallback.
        $env:VENPOD_SPARSE_SURFACE_ASYNC_EXTRACTION = "1"
        # Worker count: 8 of 16 logical cores (was 4). The exact-surface mesher is the
        # near-detail throughput producer; doubling the pool lets more bricks mesh in
        # parallel while moving into fresh terrain (mirrors the mid-voxel parallel pump's
        # 10-worker scaling). Meshing runs OFF the render thread, so this adds throughput
        # without adding main-thread cost (verified: converged fps unregressed). quality=8 too.
        $env:VENPOD_SPARSE_SURFACE_ASYNC_MAX_WORKERS = "8"
        # General surface-extraction enqueue budget (bricks/frame fed to the worker pool).
        # With async meshing the per-frame pump only ENQUEUES (cheap), so the runtime
        # pressure scaler that collapsed the effective budget to ~14-17/frame while moving
        # was needlessly starving the producers. Raise the base 48->128 so the pump keeps
        # the 8 workers fed during the convergence transient. Inline (non-async) fallback is
        # still bounded by VENPOD_SPARSE_SURFACE_EXTRACTION_MAX_MS, so this can't stall render.
        if (-not $env:VENPOD_SPARSE_SURFACE_EXTRACTION_BUDGET) {
            $env:VENPOD_SPARSE_SURFACE_EXTRACTION_BUDGET = if ($quality) { "128" } elseif ($PerfMode -eq "60fps") { "128" } else { "96" }
        }
        # Surface mesh APPLY (UpdateBrickWithExtractedFaces, default 256/frame) dominates
        # the post-fence cost (untracked ~12-23ms). Throttle it: workers still produce,
        # results apply over more frames (best-available shows coarse briefly). quality=high.
        $surfApply = if ($quality) { "256" } elseif ($PerfMode -eq "60fps") { "192" } else { "128" }
        $env:VENPOD_SPARSE_SURFACE_ASYNC_MAX_APPLY_PER_FRAME = $surfApply
        # Surface GPU upload (snapshot/stage/emit) every frame is a big main-thread post-fence
        # cost. Upload every Nth frame instead (best-available shows 1-frame-older surface).
        # Verified: interval 2 -> fps 46->54, steady (max 27ms). quality uploads every frame.
        # Upload every 3rd frame: amortizes the per-upload FIXED cost (snapshot build/begin).
        # (Tested interval 1 + bounded copy to spread it -> WORSE ~47fps: the fixed per-call
        # cost x every-frame exceeds the spike x 1/3. The spike needs incremental-snapshot
        # rework to remove, not spreading.)
        $surfUploadInterval = if ($quality) { "1" } elseif ($PerfMode -eq "60fps") { "3" } else { "2" }
        $env:VENPOD_SPARSE_SURFACE_UPLOAD_MIN_INTERVAL_FRAMES = $surfUploadInterval
        # Skip the per-frame stats FlushStats (~2.25ms pure telemetry overhead). Single-flush
        # mode keeps the metrics overlay slightly staler but is invisible to gameplay.
        if (-not $quality) { $env:VENPOD_SPARSE_STATS_SINGLE_FLUSH = "1" }
        # Async EXACT generation too: move brick generation (gen ~6-10ms while moving)
        # off the main thread. VISIBLE+PREFETCH lanes must be async or moving-play bricks
        # (visible lane) bail to synchronous. Generated bricks apply a frame later, then
        # feed the async surface mesher -> two-stage off-thread pipeline.
        $env:VENPOD_SPARSE_EXACT_ASYNC_GENERATION = "1"
        $env:VENPOD_SPARSE_EXACT_ASYNC_VISIBLE = "1"
        $env:VENPOD_SPARSE_EXACT_ASYNC_PREFETCH_LANE = "1"
        # Cap async exact-gen apply per frame: consistent per-frame work tames the apply-
        # bunching that caused run-to-run fps variance. Verified: tightened to avg ~60-62,
        # min ~56-57 (crosses 60). 60fps mode only; quality/30fps keep the default 32.
        if ($PerfMode -eq "60fps") { $env:VENPOD_SPARSE_EXACT_ASYNC_MAX_APPLY_PER_FRAME = "96" }
        # Async MID-CLIPMAP voxel generation: move the mid-voxel pump (the 'clip' cost,
        # the last big synchronous CPU item ~9ms) off the main thread -> path to steady 60.
        $env:VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_GEN = "1"
        $env:VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_GEN = "1"
        $env:VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_MAX_ENQUEUE = "256"
        $env:VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_MAX_APPLY = "256"
        # FRONTIER STREAMING (P2) -- investigated, NOT applied. Measured the
        # mid-voxel frontier under a sustained Speed-44 walk (PERF_SPARSE
        # midVoxInterest resident/wanted at 10-frame granularity): in steady-state
        # motion the resident set already tracks wanted at ~99.3% (e.g.
        # 12189-12288 / 12288) with the GPU-gen path freeing the CPU voxel fill --
        # the streaming already keeps up. Raising the VISIBLE-CRITICAL apply/
        # enqueue budget (16 -> 256), lowering motionLookaheadMinSpeed (64 -> 16),
        # and enabling predicted-visible GPU-gen admission did NOT improve steady-
        # state residency (still ~99.3%) but dropped fps from ~61-70 to ~37-44 (the
        # extra per-frame interest rebuilds / applies are pure CPU cost here). So
        # the frontier bottleneck the report describes is the interest-set RAMP
        # when entering virgin terrain (inherent to growing the wanted set), not a
        # residency/budget cap -- and the aggressive prefetch is a net regression.
        # Left at defaults; see report for numbers.
        # Parallel VISIBLE mid-voxel pump across worker threads (default is single-threaded ->
        # terrain you are looking at generates one brick at a time = 'streams in slowly'). 16
        # cores available; 10 persistent workers fills the visible terrain much faster.
        $env:VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_VISIBLE_PUMP = "1"
        $env:VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PERSISTENT_WORKERS = "1"
        $env:VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PUMP_MAX_WORKERS = "10"
        # Frontier fill rate into virgin terrain: GPU generation freed the per-brick voxel
        # fill, so the coverage-catchup budget can be raised 48->192 -> ~4x faster streaming
        # when moving into new terrain, at no fps cost (verified ~59fps @ speed 44, no TDR).
        $env:VENPOD_SPARSE_MID_VOXEL_COVERAGE_CATCHUP_BUDGET = "192"
        # Mid-voxel interest radius is the dominant per-frame CPU driver (clip/req/upload
        # scale ~quadratically with it; default 8 -> ~9200 bricks). Shrink it for fps; the
        # far LOD + background pass fill beyond the mid-detail radius. quality keeps 8.
        # GPU mid-voxel terrain generation is the DEFAULT: the compute shader fills
        # the sample pool (byte-identical to CPU), freeing the CPU per-voxel fill ->
        # streaming keeps up at speed (no holes) + a larger render distance. Set
        # VENPOD_SPARSE_MID_CLIPMAP_GPU_GENERATION=0 to force the legacy CPU path.
        if (-not $env:VENPOD_SPARSE_MID_CLIPMAP_GPU_GENERATION) {
            $env:VENPOD_SPARSE_MID_CLIPMAP_GPU_GENERATION = "1"
        }
        # Hidden-exact probe cadence: interval 3 was promoted on a real-aim A/B
        # (Loop 102) then DEMOTED on the deterministic lane (Loop 105 — interval
        # 1 equal-or-better in both pairs; the original win was workload luck).
        # Engine default (1) applies; env still overridable for experiments.
        # Mid overlay pass: no longer forced here. The launcher decides the default
        # (OFF when the mesh-mid raster owns the band -- its default -- ON for the
        # legacy raymarch-mid path). An explicit VENPOD_RAYMARCH_MID_PASS_ENABLE in
        # the environment still wins everywhere.
        # Phase 2: when GPU mid-voxel generation is ON, the CPU no longer pays the
        # per-voxel fill, so the grown render-distance bubble (radius default 12,
        # set in main_launcher when the GPU-gen flag is on) is affordable. Do NOT
        # override the radius here in that case -- let the grown C++ default win.
        if ($env:VENPOD_SPARSE_MID_CLIPMAP_GPU_GENERATION -ne "1") {
            $midVoxRadius = if ($quality) { "8" } elseif ($PerfMode -eq "60fps") { "6" } else { "7" }
            $env:VENPOD_SPARSE_MID_VOXEL_RADIUS_XZ = $midVoxRadius
        }
        # Mid interest set is rebuilt EVERY frame (interestInterval=1) -- a big chunk of
        # 'clip'. Amortize it across frames (camera moves smoothly, the set barely changes
        # frame-to-frame). Loop 54: quality=2 too -- the per-frame rebuild was a redundant CPU cost
        # against a 94%-idle GPU. A/B: interest 6.26->1.68ms, median 18.9->16.4ms (-13%), missing=0 +
        # residentMissingSurface=0 + MORE ready bricks across the full mtns.rec fast-yaw replay.
        $midInterestInterval = if ($quality) { "2" } elseif ($PerfMode -eq "60fps") { "2" } else { "2" }
        $env:VENPOD_SPARSE_MID_INTEREST_INTERVAL = $midInterestInterval
        # Interest SIGNATURE REUSE: skip the mid interest rebuild when the camera footprint
        # is unchanged (a big chunk of 'clip'). Verified +8fps, no recenter bursts. Loop 54: enabled
        # for quality too (A/B above: no hole, no under-coverage on the fast-yaw replay).
        $env:VENPOD_SPARSE_MID_CLIPMAP_FOOTPRINT_INTEREST_SIGNATURE = "1"
        $env:VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_SIGNATURE_REUSE = "1"
        # (Extending REUSE_MAX_AGE was tried + reverted: longer reuse caused bigger
        # catch-up bursts -> MORE fps variance. Default age is steadier.)
        # Low-res far/background raymarch (the GPU win); near terrain stays full-res.
        # quality mode disables it for a sharp full-res horizon.
        if ($useBgPass) {
            $env:VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE = "1"
            $env:VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE = $bgScale
            $env:VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL =
                if ($env:VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL) { $env:VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL }
                elseif ($qualityDefaultBackgroundMask -or
                    ($ExperimentalBackgroundPassNoSurfaceFill -and $ExperimentalBackgroundPassScale -gt 0)) { "0" } else { "1" }
            if ($qualityDefaultBackgroundMask -or
                ($ExperimentalBackgroundPassForegroundMask -and $ExperimentalBackgroundPassScale -gt 0)) {
                $env:VENPOD_RAYMARCH_BACKGROUND_PASS_FOREGROUND_MASK = "1"
            }
        }
        # Foreground raymarch (uncovered near/mid terrain) is the other half of the GPU
        # cost; render scale lowers it. -RenderScale overrides the per-mode default.
        $rayScale = if ($RenderScale -gt 0) { "$RenderScale" } elseif ($quality) { "1.0" } elseif ($PerfMode -eq "60fps") { "0.5" } else { "0.85" }
        $env:VENPOD_RAYMARCH_RENDER_SCALE = $rayScale
        if ($useSparse) {
            # Open the world promptly at best-available LOD (V2); don't hold for proofs.
            $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_PROOF = "0"
            $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_SURFACE_PROOF = "0"
            $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_READY_BRICKS = "128"
            $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MAX_FRAME = "120"
            $env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_FAIL_OPEN_AT_MAX_FRAME = "1"
        }
        if ($quality -and ($qualityDefaultBackgroundMask -or $ExperimentalBackgroundPassScale -gt 0)) {
            $surfaceFillInfo =
                if ($qualityDefaultBackgroundMask -or $ExperimentalBackgroundPassNoSurfaceFill) { "off" } else { "on" }
            $foregroundMaskInfo =
                if ($qualityDefaultBackgroundMask -or $ExperimentalBackgroundPassForegroundMask) { "on" } else { "off" }
            $bgModeInfo = if ($qualityDefaultBackgroundMask) { "foreground-masked far/background pass" } else { "experimental far/background pass" }
            Write-Info "Perf mode: quality + $bgModeInfo scale $bgScale (foreground scale $rayScale, surface fill $surfaceFillInfo, foreground mask $foregroundMaskInfo)"
        } elseif ($quality) {
            Write-Info "Perf mode: quality (visual-first: full coverage pump ${pumpBudget}ms, async producers, full-res near+far; fps secondary)"
        } else {
            Write-Info "Perf mode: $PerfMode (V2 best-available + low-res far background pass $bgScale + foreground render scale $rayScale)"
        }
    }

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
