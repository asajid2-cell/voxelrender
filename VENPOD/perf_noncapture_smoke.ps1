param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo")]
    [string]$Config = "Release",
    [ValidateSet("all", "fixed", "walk", "highalt")]
    [string]$Scenario = "all",
    [ValidateSet("none", "highalt-currentfirst")]
    [string]$StackPreset = "none",
    [string]$OutputDir = "build\captures\noncapture_playability_smoke_20260604",
    [switch]$NoBuild,
    [switch]$ParseOnly,
    [switch]$KillExisting,
    [int]$FixedFrame = 380,
    [int]$WalkFrame = 600,
    [int]$HighAltFrame = 400,
    [string]$BackgroundPassScale = "0.375",
    [switch]$BackgroundPassSurfaceFill,
    [switch]$BackgroundPassSurfaceFillHighAltOnly,
    [switch]$BackgroundPassSurfaceFillWaterProof,
    [switch]$BackgroundPassSurfaceFillExactRepair,
    [int]$MidInterestInterval = 1,
    [int]$WalkFixedDtMs = 0,
    [int]$ExtraFrames = 8,
    [switch]$NoHeavyDiagnostics,
    [int]$PostOpenSurfaceMaxMs = -1,
    [int]$SurfaceExtractionMaxMs = -1,
    [int]$PostOpenSurfaceBudget = -1,
    [switch]$Bounded64,
    [switch]$CriticalReuse,
    [switch]$TerrainCriticalParallelGeneration,
    [int]$TerrainCriticalParallelGenerationMaxWorkers = 4,
    [int]$TerrainCriticalParallelGenerationMinBricks = 16,
    [switch]$TerrainCriticalReuseFrameState,
    [switch]$TerrainCriticalReuseRepairOnly,
    [switch]$DirectFootprintColumns,
    [switch]$DirectExactGeneration,
    [switch]$AsyncExactGeneration,
    [switch]$AsyncExactPrefetchLane,
    [int]$AsyncExactMaxEnqueuePerFrame = 0,
    [int]$AsyncExactLowPriorityMaxApplyPerFrame = 0,
    [switch]$ParentHeldFeedback,
    [switch]$ParallelMidVoxelPump,
    [switch]$ParallelMidVoxelPumpPersistentWorkers,
    [int]$ParallelMidVoxelPumpMaxWorkers = 4,
    [switch]$ParallelMidWorkerColumnCache,
    [switch]$MidClipmapAsyncVisibleCriticalGeneration,
    [int]$MidClipmapAsyncVisibleCriticalMaxEnqueue = 16,
    [int]$MidClipmapAsyncVisibleCriticalMaxApply = 16,
    [int]$MidClipmapAsyncVisibleReservationMaxApply = -1,
    [switch]$MidClipmapSplitVisiblePump,
    [switch]$MidClipmapSplitVisiblePumpPostOpenOnly,
    [int]$MidClipmapSplitVisiblePumpBudget = 8,
    [int]$MidClipmapSplitCachePumpBudget = 0,
    [switch]$MidClipmapVisibleCriticalPrepump,
    [switch]$MidClipmapVisiblePriorityPump,
    [switch]$MidClipmapVisibleLaneGuard,
    [switch]$MidClipmapCacheOnlyDefer,
    [switch]$MidClipmapVisibleCoverageGuardV2,
    [switch]$MidClipmapStressCameraVelocity,
    [switch]$MidClipmapPredictedVisibleAdmission,
    [switch]$DisableMidClipmapPredictedVisibleAdmission,
    [int]$MidClipmapPredictedVisibleAdmissionSamples = 2,
    [int]$MidClipmapPredictedVisibleAdmissionLeadFrames = 2,
    [int]$MidClipmapPredictedVisibleAdmissionMaxCoords = 128,
    [int]$MidClipmapPredictedVisibleAdmissionSampleSide = 5,
    [int]$MidClipmapPredictedVisibleAdmissionMaxDistance = 0,
    [switch]$MidClipmapInterestDetail,
    [switch]$MidClipmapFootprintInterestSignature,
    [switch]$MidClipmapVoxelInterestSignatureReuse,
    [int]$MidClipmapVoxelInterestSignatureReuseMaxAge = 1,
    [int]$StartupPublicRenderMaxFrame = -1,
    [switch]$StartupPublicRenderMidVoxelVisibleProof,
    [switch]$StartupPublicRenderMidVoxelMovingWindowProof,
    [switch]$StartupPublicRenderMidVoxelMovingWindowAsyncReservation,
    [switch]$MidClipmapMovingWindowPriority,
    [switch]$MidClipmapMovingWindowAsyncReservation,
    [switch]$StreamingLaneDiagnostics,
    [switch]$StreamingLaneQueuePriority,
    [switch]$StreamingTicketScheduler,
    [switch]$StreamingTicketProtectedScheduling,
    [switch]$StreamingTicketStageDemandAccounting,
    [switch]$StreamingTicketGenerationOwnershipQueues,
    [switch]$StreamingTicketGenerationOwnershipReservations,
    [int]$StreamingTicketGenerationOwnershipReservationMax = 64,
    [switch]$StreamingTicketGenerationOwnershipShareScheduler,
    [int]$StreamingTicketGenerationOwnershipSharePublicMin = 48,
    [int]$StreamingTicketGenerationOwnershipShareVisibleMax = 32,
    [int]$StreamingTicketGenerationOwnershipSharePrefetchMin = 32,
    [int]$StreamingTicketGenerationOwnershipShareVisibleDebtGate = 160,
    [switch]$StreamingTicketLowPriorityDownstreamDeferral,
    [int]$StreamingTicketLowPriorityDownstreamPromoteMax = 16,
    [switch]$RequestExplicitSourceLanes,
    [switch]$PrefetchLaneSpeculativeClass,
    [switch]$PrefetchStageBudgets,
    [switch]$PrefetchStageBudgetsHighAltOnly,
    [int]$PrefetchStageUploadBudget = 8,
    [int]$PrefetchStageSurfaceBudget = 8,
    [switch]$OwnershipStageBudgets,
    [int]$OwnershipStageUploadBudget = 8,
    [int]$OwnershipStageSurfaceBudget = 8,
    [int]$OwnershipStagePublishBudget = 8,
    [switch]$OwnershipStageAdaptiveNoncritical,
    [int]$OwnershipStageAdaptiveMinVisibleCoverage = 99,
    [int]$OwnershipStageAdaptiveMaxMissingVisible = 0,
    [int]$OwnershipStageAdaptiveUploadFloor = 2,
    [int]$OwnershipStageAdaptiveSurfaceFloor = 2,
    [switch]$OwnershipStageMidVisibleDebtThrottle,
    [int]$OwnershipStageMidVisibleDebtMinCoverage = 99,
    [int]$OwnershipStageMidVisibleDebtMaxMissingVisible = 0,
    [int]$OwnershipStageMidVisibleDebtUploadFloor = 0,
    [int]$OwnershipStageMidVisibleDebtSurfaceFloor = 0,
    [switch]$NoBacklogAwarePump,
    [switch]$ParallelExactGeneration,
    [switch]$ParallelExactPersistentWorkers,
    [int]$ParallelExactGenerationMaxWorkers = 4,
    [switch]$SurfaceBuriedSolidFastPath,
    [switch]$SurfaceClassSortCache,
    [switch]$SurfaceClassPartialSort,
    [switch]$SurfaceStrictTimeBudget,
    [switch]$ParallelSurfaceExtraction,
    [int]$ParallelSurfaceExtractionMaxWorkers = 4,
    [switch]$ParallelSurfaceExtractionTimeBudgeted,
    [int]$ParallelSurfaceExtractionMaxBatch = 32,
    [switch]$SurfaceGeneralStrictBudget,
    [int]$SurfaceGeneralMinBudgetMs = 4,
    [switch]$PersistentTerrainColumnCache,
    [int]$TerrainColumnCacheMaxEntries = 131072,
    [switch]$DeferTerrainCriticalInlineSurface,
    [switch]$SurfaceReadyPublishQueue,
    [int]$SurfaceReadyPublishScanBudget = 4096,
    [switch]$SurfaceReadyPublishPressure,
    [switch]$RequestFastResidentTouch,
    [switch]$HiddenExactTrackedScanBudgeted,
    [int]$HiddenExactTrackedScanBudget = 512,
    [switch]$HiddenExactPostOpenRepairLane,
    [switch]$HiddenExactPostOpenWaterRepairLane,
    [switch]$PressureTrimFreePageGuard,
    [switch]$IncrementalPressureTrim,
    [int]$IncrementalPressureTrimScanBudget = 32768,
    [switch]$DisableVSync,
    [int]$FrameEndLogInterval = 0,
    [int]$WindowBeforeFrames = 24,
    [int]$WindowAfterFrames = 4
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:SmokeInvocationLine = $MyInvocation.Line
$script:SmokeScriptPath = $MyInvocation.MyCommand.Path
$script:SmokeBoundParameters = @{}
foreach ($key in $PSBoundParameters.Keys) {
    $script:SmokeBoundParameters[$key] = $PSBoundParameters[$key]
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$runScript = Join-Path $root "run.ps1"
$buildScript = Join-Path $root "build.ps1"
$outRoot = Join-Path $root $OutputDir

function Apply-StackPreset {
    if ($StackPreset -eq "none") {
        return
    }

    if ($StackPreset -eq "highalt-currentfirst") {
        $script:Scenario = "highalt"
        $script:HighAltFrame = 400
        $script:ExtraFrames = 4
        $script:WindowBeforeFrames = 24
        $script:WindowAfterFrames = 4
        $script:FrameEndLogInterval = 1

        $script:StartupPublicRenderMaxFrame = 480
        $script:StartupPublicRenderMidVoxelVisibleProof = $true

        $script:MidClipmapFootprintInterestSignature = $true
        $script:MidClipmapVoxelInterestSignatureReuse = $true
        $script:MidClipmapVoxelInterestSignatureReuseMaxAge = 4
        $script:MidClipmapVisibleCriticalPrepump = $true
        $script:MidClipmapVisiblePriorityPump = $true
        $script:MidClipmapCacheOnlyDefer = $true
        $script:MidClipmapStressCameraVelocity = $true
        $script:MidClipmapPredictedVisibleAdmission = $true
        $script:MidClipmapPredictedVisibleAdmissionSamples = 2
        $script:MidClipmapPredictedVisibleAdmissionLeadFrames = 2
        $script:MidClipmapPredictedVisibleAdmissionMaxCoords = 512
        $script:MidClipmapPredictedVisibleAdmissionSampleSide = 5
        $script:MidClipmapPredictedVisibleAdmissionMaxDistance = 0

        $script:MidClipmapAsyncVisibleCriticalGeneration = $true
        $script:MidClipmapAsyncVisibleCriticalMaxEnqueue = 24
        $script:MidClipmapAsyncVisibleCriticalMaxApply = 24
        $script:MidClipmapSplitVisiblePump = $true
        $script:MidClipmapSplitVisiblePumpPostOpenOnly = $true
        $script:MidClipmapSplitVisiblePumpBudget = 0
        $script:MidClipmapSplitCachePumpBudget = 0

        $script:ParallelMidVoxelPump = $true
        $script:ParallelMidVoxelPumpPersistentWorkers = $true
        $script:ParallelMidVoxelPumpMaxWorkers = 4

        $script:StreamingLaneDiagnostics = $true
        $script:StreamingLaneQueuePriority = $true
        $script:StreamingTicketScheduler = $true
        $script:StreamingTicketGenerationOwnershipQueues = $true
        $script:StreamingTicketGenerationOwnershipReservations = $true
        $script:StreamingTicketGenerationOwnershipReservationMax = 512
        $script:RequestExplicitSourceLanes = $true

        $script:OwnershipStageBudgets = $true
        $script:OwnershipStageUploadBudget = 8
        $script:OwnershipStageSurfaceBudget = 8
        $script:OwnershipStagePublishBudget = 8

        $script:HiddenExactPostOpenRepairLane = $true
        return
    }

    throw "Unknown StackPreset '$StackPreset'"
}

Apply-StackPreset

if ($DisableMidClipmapPredictedVisibleAdmission) {
    $script:MidClipmapPredictedVisibleAdmission = $false
}

function Set-EnvValue {
    param([string]$Name, [string]$Value)
    [Environment]::SetEnvironmentVariable($Name, $Value, "Process")
}

function Clear-EnvValue {
    param([string]$Name)
    [Environment]::SetEnvironmentVariable($Name, $null, "Process")
}

function Get-EnvValue {
    param([string]$Name)
    [Environment]::GetEnvironmentVariable($Name, "Process")
}

$managedEnv = @(
    "VENPOD_LOG_FILE",
    "VENPOD_CAPTURE_DIR",
    "VENPOD_CAPTURE_START_FRAME",
    "VENPOD_CAPTURE_FRAME_COUNT",
    "VENPOD_CAPTURE_OWNER_DEBUG",
    "VENPOD_EXIT_AFTER_FRAMES",
    "VENPOD_MODE",
    "VENPOD_VSYNC",
    "VENPOD_DISABLE_PHYSICS",
    "VENPOD_ENABLE_EXPERIMENTAL_SPARSE",
    "VENPOD_RENDER_BACKEND",
    "VENPOD_SPARSE_RAYMARCH",
    "VENPOD_SPARSE_ONLY",
    "VENPOD_SPARSE_SURFACE_AUTHORITATIVE",
    "VENPOD_SPARSE_MISS_FEEDBACK",
    "VENPOD_SPARSE_REQUIRE_STARTUP_READY",
    "VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_BLOCKS",
    "VENPOD_SPARSE_STARTUP_MIN_FRAMES",
    "VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MAX_FRAME",
    "VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_VISIBLE_PROOF",
    "VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_MOVING_WINDOW_PROOF",
    "VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_MOVING_WINDOW_ASYNC_RESERVATION",
    "VENPOD_SPARSE_SURFACE_EXTRACTION_MAX_MS",
    "VENPOD_SPARSE_SURFACE_PROMOTION_POLICY",
    "VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE",
    "VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_DEMOTE_BOUND",
    "VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_BOUND",
    "VENPOD_SPARSE_MAX_PAGES",
    "VENPOD_SPARSE_PAGE_TABLE",
    "VENPOD_SPARSE_STATS_SINGLE_FLUSH",
    "VENPOD_PERF_SUMMARY_LOG_INTERVAL",
    "VENPOD_PERF_FRAME_END_LOG_INTERVAL",
    "VENPOD_SPARSE_PRESSURE_TRIM_FREE_PAGE_GUARD",
    "VENPOD_SPARSE_PRESSURE_TRIM_INCREMENTAL",
    "VENPOD_SPARSE_PRESSURE_TRIM_SCAN_BUDGET",
    "VENPOD_SPARSE_POST_OPEN_PRE_PUBLISH_SURFACE_EXTRACTION_MAX_MS",
    "VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_SURFACE_BUDGET",
    "VENPOD_SPARSE_SURFACE_BURIED_SOLID_FASTPATH",
    "VENPOD_SPARSE_SURFACE_CLASS_SORT_CACHE",
    "VENPOD_SPARSE_SURFACE_CLASS_PARTIAL_SORT",
    "VENPOD_SPARSE_SURFACE_STRICT_TIME_BUDGET",
    "VENPOD_SPARSE_SURFACE_PARALLEL_EXTRACTION",
    "VENPOD_SPARSE_SURFACE_PARALLEL_MAX_WORKERS",
    "VENPOD_SPARSE_SURFACE_PARALLEL_MIN_BRICKS",
    "VENPOD_SPARSE_SURFACE_PARALLEL_TIME_BUDGETED",
    "VENPOD_SPARSE_SURFACE_PARALLEL_MAX_BATCH",
    "VENPOD_SPARSE_SURFACE_GENERAL_STRICT_BUDGET",
    "VENPOD_SPARSE_SURFACE_GENERAL_MIN_BUDGET_MS",
    "VENPOD_SPARSE_TERRAIN_COLUMN_CACHE_PERSISTENT",
    "VENPOD_SPARSE_TERRAIN_COLUMN_CACHE_MAX_ENTRIES",
    "VENPOD_SPARSE_TERRAIN_CRITICAL_DEFER_INLINE_SURFACE",
    "VENPOD_SPARSE_SURFACE_READY_PUBLISH_QUEUE",
    "VENPOD_SPARSE_SURFACE_READY_PUBLISH_PRESSURE",
    "VENPOD_SPARSE_SURFACE_READY_PUBLISH_SCAN_BUDGET",
    "VENPOD_SPARSE_HIDDEN_EXACT_TRACKED_SCAN_BUDGETED",
    "VENPOD_SPARSE_HIDDEN_EXACT_TRACKED_SCAN_BUDGET",
    "VENPOD_RENDER_QUALITY",
    "VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE",
    "VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE",
    "VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL",
    "VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_HIGH_ALT_ONLY",
    "VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_WATER_PROOF",
    "VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_EXACT_REPAIR",
    "VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_CLEAN_THROTTLE",
    "VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_REUSE_MAX_SPEED",
    "VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION",
    "VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MAX_WORKERS",
    "VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MIN_BRICKS",
    "VENPOD_SPARSE_TERRAIN_CRITICAL_REUSE_FRAME_STATE",
    "VENPOD_SPARSE_TERRAIN_CRITICAL_REUSE_REPAIR_ONLY",
    "VENPOD_SPARSE_SURFACE_INCREMENTAL_METADATA_ADDS",
    "VENPOD_SPARSE_CPU_DETAIL",
    "VENPOD_SPARSE_MID_CLIPMAP_BACKLOG_AWARE_PUMP",
    "VENPOD_SPARSE_MID_CLIPMAP_DRAIN_REUSE_DIAGNOSTICS",
    "VENPOD_SPARSE_MID_CLIPMAP_FALLBACK_VALIDITY_CLASSIFIER",
    "VENPOD_SPARSE_MID_CLIPMAP_FALLBACK_CONTRACT_DIAGNOSTICS",
    "VENPOD_SPARSE_MID_CLIPMAP_FAR_SVO_FALLBACK_PROOF",
    "VENPOD_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_FEEDBACK",
    "VENPOD_SPARSE_MID_CLIPMAP_PARENT_HELD_FEEDBACK",
    "VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP",
    "VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_PRIORITY_PUMP",
    "VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_LANE_GUARD",
    "VENPOD_SPARSE_MID_CLIPMAP_CACHE_ONLY_DEFER",
    "VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_COVERAGE_GUARD_V2",
    "VENPOD_SPARSE_MID_CLIPMAP_STRESS_CAMERA_VELOCITY",
    "VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION",
    "VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION_SAMPLES",
    "VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION_LEAD_FRAMES",
    "VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION_MAX_COORDS",
    "VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION_SAMPLE_SIDE",
    "VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION_MAX_DISTANCE",
    "VENPOD_SPARSE_MID_CLIPMAP_INTEREST_DETAIL",
    "VENPOD_SPARSE_MID_CLIPMAP_FOOTPRINT_INTEREST_SIGNATURE",
    "VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_SIGNATURE_REUSE",
    "VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_SIGNATURE_REUSE_MAX_AGE",
    "VENPOD_SPARSE_MID_CLIPMAP_MOVING_WINDOW_PRIORITY",
    "VENPOD_SPARSE_MID_CLIPMAP_MOVING_WINDOW_ASYNC_RESERVATION",
    "VENPOD_SPARSE_MID_INTEREST_INTERVAL",
    "VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS",
    "VENPOD_SPARSE_STREAMING_LANE_QUEUE_PRIORITY",
    "VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER",
    "VENPOD_SPARSE_STREAMING_TICKET_PROTECTED_SCHEDULING",
    "VENPOD_SPARSE_STREAMING_TICKET_STAGE_DEMAND_ACCOUNTING",
    "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_QUEUES",
    "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_RESERVATIONS",
    "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_RESERVATION_MAX",
    "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_SCHEDULER",
    "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_PUBLIC_MIN",
    "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_VISIBLE_MAX",
    "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_PREFETCH_MIN",
    "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_VISIBLE_DEBT_GATE",
    "VENPOD_SPARSE_STREAMING_TICKET_LOW_PRIORITY_DOWNSTREAM_DEFERRAL",
    "VENPOD_SPARSE_STREAMING_TICKET_LOW_PRIORITY_DOWNSTREAM_PROMOTE_MAX",
    "VENPOD_SPARSE_REQUEST_EXPLICIT_SOURCE_LANES",
    "VENPOD_SPARSE_PREFETCH_LANE_SPECULATIVE_CLASS",
    "VENPOD_SPARSE_PREFETCH_STAGE_BUDGETS",
    "VENPOD_SPARSE_PREFETCH_STAGE_BUDGETS_HIGH_ALT_ONLY",
    "VENPOD_SPARSE_PREFETCH_STAGE_UPLOAD_BUDGET",
    "VENPOD_SPARSE_PREFETCH_STAGE_SURFACE_BUDGET",
    "VENPOD_SPARSE_OWNERSHIP_STAGE_BUDGETS",
    "VENPOD_SPARSE_OWNERSHIP_STAGE_UPLOAD_BUDGET",
    "VENPOD_SPARSE_OWNERSHIP_STAGE_SURFACE_BUDGET",
    "VENPOD_SPARSE_OWNERSHIP_STAGE_PUBLISH_BUDGET",
    "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_NONCRITICAL",
    "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_MIN_VISIBLE_COVERAGE",
    "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_MAX_MISSING_VISIBLE",
    "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_UPLOAD_FLOOR",
    "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_SURFACE_FLOOR",
    "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_THROTTLE",
    "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_MIN_COVERAGE",
    "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_MAX_MISSING_VISIBLE",
    "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_UPLOAD_FLOOR",
    "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_SURFACE_FLOOR",
    "VENPOD_SPARSE_MID_CLIPMAP_DIRECT_FOOTPRINT_COLUMNS",
    "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_WORKER_COLUMN_CACHE",
    "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_VISIBLE_PUMP",
    "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PERSISTENT_WORKERS",
    "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PUMP_MAX_WORKERS",
    "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PUMP_MIN_BRICKS",
    "VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_GEN",
    "VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_MAX_ENQUEUE",
    "VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_MAX_APPLY",
    "VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_RESERVATION_MAX_APPLY",
    "VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP",
    "VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP_POST_OPEN_ONLY",
    "VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP_BUDGET",
    "VENPOD_SPARSE_MID_CLIPMAP_SPLIT_CACHE_PUMP_BUDGET",
    "VENPOD_SPARSE_EXACT_DIRECT_GENERATION",
    "VENPOD_SPARSE_EXACT_ASYNC_GENERATION",
    "VENPOD_SPARSE_EXACT_ASYNC_VISIBLE",
    "VENPOD_SPARSE_EXACT_ASYNC_PREFETCH_LANE",
    "VENPOD_SPARSE_EXACT_ASYNC_QUEUE_MAX",
    "VENPOD_SPARSE_EXACT_ASYNC_MAX_ENQUEUE_PER_FRAME",
    "VENPOD_SPARSE_EXACT_ASYNC_MAX_APPLY_PER_FRAME",
    "VENPOD_SPARSE_EXACT_ASYNC_LOW_PRIORITY_MAX_APPLY_PER_FRAME",
    "VENPOD_SPARSE_REQUEST_FAST_RESIDENT_TOUCH",
    "VENPOD_SPARSE_EXACT_PARALLEL_GENERATION",
    "VENPOD_SPARSE_EXACT_PERSISTENT_WORKERS",
    "VENPOD_SPARSE_EXACT_PARALLEL_GENERATION_MAX_WORKERS",
    "VENPOD_SPARSE_EXACT_PARALLEL_GENERATION_MIN_BRICKS",
    "VENPOD_SPARSE_WALK_TEST",
    "VENPOD_SPARSE_WALK_TEST_SPEED",
    "VENPOD_SPARSE_WALK_TEST_YAW_DEG",
    "VENPOD_SPARSE_WALK_TEST_PITCH_DEG",
    "VENPOD_SPARSE_WALK_TEST_FIXED_DT",
    "VENPOD_SPARSE_STRESS_REQUESTS",
    "VENPOD_SPARSE_STRESS_CAMERA",
    "VENPOD_SPARSE_STRESS_CAMERA_RADIUS",
    "VENPOD_SPARSE_STRESS_CAMERA_HEIGHT",
    "VENPOD_SPARSE_STRESS_CAMERA_BASE_HEIGHT",
    "VENPOD_SPARSE_STRESS_CAMERA_SPEED",
    "VENPOD_SPARSE_STRESS_CAMERA_ABSOLUTE_FRAME"
)

$savedEnv = @{}
foreach ($name in $managedEnv) {
    $savedEnv[$name] = Get-EnvValue $name
}

function Restore-ManagedEnv {
    foreach ($name in $managedEnv) {
        $value = $savedEnv[$name]
        if ($null -eq $value) {
            Clear-EnvValue $name
        } else {
            Set-EnvValue $name $value
        }
    }
}

function Require-NoExistingProcess {
    $existing = @(Get-Process VENPOD -ErrorAction SilentlyContinue)
    if ($existing.Count -eq 0) {
        return
    }
    if (-not $KillExisting) {
        $ids = ($existing | ForEach-Object { $_.Id }) -join ","
        throw "VENPOD.exe is already running (pid(s): $ids). Re-run with -KillExisting if these are stale test processes."
    }
    foreach ($proc in $existing) {
        Stop-Process -Id $proc.Id -Force
    }
}

function Set-CommonCandidateEnv {
    param([int]$ExitAfterFrames, [string]$LogPath)

    Clear-EnvValue "VENPOD_CAPTURE_DIR"
    Clear-EnvValue "VENPOD_CAPTURE_START_FRAME"
    Clear-EnvValue "VENPOD_CAPTURE_FRAME_COUNT"
    Clear-EnvValue "VENPOD_CAPTURE_OWNER_DEBUG"

    Set-EnvValue "VENPOD_LOG_FILE" $LogPath
    Set-EnvValue "VENPOD_EXIT_AFTER_FRAMES" ([string]$ExitAfterFrames)
    Set-EnvValue "VENPOD_MODE" "sandbox"
    Set-EnvValue "VENPOD_DISABLE_PHYSICS" "1"
    if ($DisableVSync) {
        Set-EnvValue "VENPOD_VSYNC" "0"
    } else {
        Clear-EnvValue "VENPOD_VSYNC"
    }
    Set-EnvValue "VENPOD_ENABLE_EXPERIMENTAL_SPARSE" "1"
    Set-EnvValue "VENPOD_RENDER_BACKEND" "sparse"
    Set-EnvValue "VENPOD_SPARSE_RAYMARCH" "1"
    Set-EnvValue "VENPOD_SPARSE_ONLY" "1"
    Set-EnvValue "VENPOD_SPARSE_SURFACE_AUTHORITATIVE" "1"
    Set-EnvValue "VENPOD_SPARSE_MISS_FEEDBACK" "1"
    Set-EnvValue "VENPOD_SPARSE_REQUIRE_STARTUP_READY" "1"
    Set-EnvValue "VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_BLOCKS" "1"
    Set-EnvValue "VENPOD_SPARSE_STARTUP_MIN_FRAMES" "90"
    if ($StartupPublicRenderMaxFrame -ge 0) {
        Set-EnvValue "VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MAX_FRAME" ([string][Math]::Max(0, $StartupPublicRenderMaxFrame))
    } else {
        Clear-EnvValue "VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MAX_FRAME"
    }
    if ($StartupPublicRenderMidVoxelVisibleProof) {
        Set-EnvValue "VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_VISIBLE_PROOF" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_VISIBLE_PROOF"
    }
    if ($StartupPublicRenderMidVoxelMovingWindowProof) {
        Set-EnvValue "VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_MOVING_WINDOW_PROOF" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_MOVING_WINDOW_PROOF"
    }
    if ($StartupPublicRenderMidVoxelMovingWindowAsyncReservation) {
        Set-EnvValue "VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_MOVING_WINDOW_ASYNC_RESERVATION" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_MOVING_WINDOW_ASYNC_RESERVATION"
    }
    if ($Bounded64) {
        Set-EnvValue "VENPOD_SPARSE_SURFACE_PROMOTION_POLICY" "bounded_repair"
        Set-EnvValue "VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE" "12"
        Set-EnvValue "VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_DEMOTE_BOUND" "64"
        Set-EnvValue "VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_BOUND" "64"
    } else {
        Set-EnvValue "VENPOD_SPARSE_SURFACE_PROMOTION_POLICY" "strict"
        Clear-EnvValue "VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE"
        Clear-EnvValue "VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_DEMOTE_BOUND"
        Clear-EnvValue "VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_BOUND"
    }
    Set-EnvValue "VENPOD_SPARSE_MAX_PAGES" "65536"
    Set-EnvValue "VENPOD_SPARSE_PAGE_TABLE" "131072"
    Set-EnvValue "VENPOD_SPARSE_STATS_SINGLE_FLUSH" "1"
    Set-EnvValue "VENPOD_PERF_SUMMARY_LOG_INTERVAL" "20"
    if ($FrameEndLogInterval -gt 0) {
        Set-EnvValue "VENPOD_PERF_FRAME_END_LOG_INTERVAL" ([string][Math]::Max(1, $FrameEndLogInterval))
    } else {
        Clear-EnvValue "VENPOD_PERF_FRAME_END_LOG_INTERVAL"
    }
    if ($PressureTrimFreePageGuard) {
        Set-EnvValue "VENPOD_SPARSE_PRESSURE_TRIM_FREE_PAGE_GUARD" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_PRESSURE_TRIM_FREE_PAGE_GUARD"
    }
    if ($IncrementalPressureTrim) {
        Set-EnvValue "VENPOD_SPARSE_PRESSURE_TRIM_INCREMENTAL" "1"
        Set-EnvValue "VENPOD_SPARSE_PRESSURE_TRIM_SCAN_BUDGET" ([string]$IncrementalPressureTrimScanBudget)
    } else {
        Clear-EnvValue "VENPOD_SPARSE_PRESSURE_TRIM_INCREMENTAL"
        Clear-EnvValue "VENPOD_SPARSE_PRESSURE_TRIM_SCAN_BUDGET"
    }
    if ($PostOpenSurfaceMaxMs -ge 0) {
        Set-EnvValue "VENPOD_SPARSE_POST_OPEN_PRE_PUBLISH_SURFACE_EXTRACTION_MAX_MS" ([string]$PostOpenSurfaceMaxMs)
    } else {
        Clear-EnvValue "VENPOD_SPARSE_POST_OPEN_PRE_PUBLISH_SURFACE_EXTRACTION_MAX_MS"
    }
    if ($SurfaceExtractionMaxMs -ge 0) {
        Set-EnvValue "VENPOD_SPARSE_SURFACE_EXTRACTION_MAX_MS" ([string]$SurfaceExtractionMaxMs)
    } else {
        Clear-EnvValue "VENPOD_SPARSE_SURFACE_EXTRACTION_MAX_MS"
    }
    if ($PostOpenSurfaceBudget -ge 0) {
        Set-EnvValue "VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_SURFACE_BUDGET" ([string]$PostOpenSurfaceBudget)
    } else {
        Clear-EnvValue "VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_SURFACE_BUDGET"
    }
    if ($SurfaceBuriedSolidFastPath) {
        Set-EnvValue "VENPOD_SPARSE_SURFACE_BURIED_SOLID_FASTPATH" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_SURFACE_BURIED_SOLID_FASTPATH"
    }
    if ($SurfaceClassSortCache) {
        Set-EnvValue "VENPOD_SPARSE_SURFACE_CLASS_SORT_CACHE" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_SURFACE_CLASS_SORT_CACHE"
    }
    if ($SurfaceClassPartialSort) {
        Set-EnvValue "VENPOD_SPARSE_SURFACE_CLASS_PARTIAL_SORT" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_SURFACE_CLASS_PARTIAL_SORT"
    }
    if ($SurfaceStrictTimeBudget) {
        Set-EnvValue "VENPOD_SPARSE_SURFACE_STRICT_TIME_BUDGET" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_SURFACE_STRICT_TIME_BUDGET"
    }
    if ($ParallelSurfaceExtraction) {
        Set-EnvValue "VENPOD_SPARSE_SURFACE_PARALLEL_EXTRACTION" "1"
        Set-EnvValue "VENPOD_SPARSE_SURFACE_PARALLEL_MAX_WORKERS" ([string]$ParallelSurfaceExtractionMaxWorkers)
        Set-EnvValue "VENPOD_SPARSE_SURFACE_PARALLEL_MIN_BRICKS" "4"
        if ($ParallelSurfaceExtractionTimeBudgeted) {
            Set-EnvValue "VENPOD_SPARSE_SURFACE_PARALLEL_TIME_BUDGETED" "1"
            Set-EnvValue "VENPOD_SPARSE_SURFACE_PARALLEL_MAX_BATCH" ([string]$ParallelSurfaceExtractionMaxBatch)
        } else {
            Clear-EnvValue "VENPOD_SPARSE_SURFACE_PARALLEL_TIME_BUDGETED"
            Clear-EnvValue "VENPOD_SPARSE_SURFACE_PARALLEL_MAX_BATCH"
        }
    } else {
        Clear-EnvValue "VENPOD_SPARSE_SURFACE_PARALLEL_EXTRACTION"
        Clear-EnvValue "VENPOD_SPARSE_SURFACE_PARALLEL_MAX_WORKERS"
        Clear-EnvValue "VENPOD_SPARSE_SURFACE_PARALLEL_MIN_BRICKS"
        Clear-EnvValue "VENPOD_SPARSE_SURFACE_PARALLEL_TIME_BUDGETED"
        Clear-EnvValue "VENPOD_SPARSE_SURFACE_PARALLEL_MAX_BATCH"
    }
    if ($SurfaceGeneralStrictBudget) {
        Set-EnvValue "VENPOD_SPARSE_SURFACE_GENERAL_STRICT_BUDGET" "1"
        Set-EnvValue "VENPOD_SPARSE_SURFACE_GENERAL_MIN_BUDGET_MS" ([string]$SurfaceGeneralMinBudgetMs)
    } else {
        Clear-EnvValue "VENPOD_SPARSE_SURFACE_GENERAL_STRICT_BUDGET"
        Clear-EnvValue "VENPOD_SPARSE_SURFACE_GENERAL_MIN_BUDGET_MS"
    }
    if ($PersistentTerrainColumnCache) {
        Set-EnvValue "VENPOD_SPARSE_TERRAIN_COLUMN_CACHE_PERSISTENT" "1"
        Set-EnvValue "VENPOD_SPARSE_TERRAIN_COLUMN_CACHE_MAX_ENTRIES" ([string]$TerrainColumnCacheMaxEntries)
    } else {
        Clear-EnvValue "VENPOD_SPARSE_TERRAIN_COLUMN_CACHE_PERSISTENT"
        Clear-EnvValue "VENPOD_SPARSE_TERRAIN_COLUMN_CACHE_MAX_ENTRIES"
    }
    if ($DeferTerrainCriticalInlineSurface) {
        Set-EnvValue "VENPOD_SPARSE_TERRAIN_CRITICAL_DEFER_INLINE_SURFACE" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_TERRAIN_CRITICAL_DEFER_INLINE_SURFACE"
    }
    if ($SurfaceReadyPublishQueue) {
        Set-EnvValue "VENPOD_SPARSE_SURFACE_READY_PUBLISH_QUEUE" "1"
        Set-EnvValue "VENPOD_SPARSE_SURFACE_READY_PUBLISH_SCAN_BUDGET" ([string]$SurfaceReadyPublishScanBudget)
        if ($SurfaceReadyPublishPressure) {
            Set-EnvValue "VENPOD_SPARSE_SURFACE_READY_PUBLISH_PRESSURE" "1"
        } else {
            Clear-EnvValue "VENPOD_SPARSE_SURFACE_READY_PUBLISH_PRESSURE"
        }
    } else {
        Clear-EnvValue "VENPOD_SPARSE_SURFACE_READY_PUBLISH_QUEUE"
        Clear-EnvValue "VENPOD_SPARSE_SURFACE_READY_PUBLISH_PRESSURE"
        Clear-EnvValue "VENPOD_SPARSE_SURFACE_READY_PUBLISH_SCAN_BUDGET"
    }
    if ($HiddenExactTrackedScanBudgeted) {
        Set-EnvValue "VENPOD_SPARSE_HIDDEN_EXACT_TRACKED_SCAN_BUDGETED" "1"
        Set-EnvValue "VENPOD_SPARSE_HIDDEN_EXACT_TRACKED_SCAN_BUDGET" ([string]$HiddenExactTrackedScanBudget)
    } else {
        Clear-EnvValue "VENPOD_SPARSE_HIDDEN_EXACT_TRACKED_SCAN_BUDGETED"
        Clear-EnvValue "VENPOD_SPARSE_HIDDEN_EXACT_TRACKED_SCAN_BUDGET"
    }
    if ($HiddenExactPostOpenRepairLane) {
        Set-EnvValue "VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_REPAIR_LANE" "1"
        if ($HiddenExactPostOpenWaterRepairLane) {
            Set-EnvValue "VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_WATER_REPAIR_LANE" "1"
        } else {
            Clear-EnvValue "VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_WATER_REPAIR_LANE"
        }
        Set-EnvValue "VENPOD_SPARSE_REQUEST_EXPLICIT_SOURCE_LANES" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_LANE_QUEUE_PRIORITY" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_REPAIR_LANE"
        Clear-EnvValue "VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_WATER_REPAIR_LANE"
    }

    Set-EnvValue "VENPOD_RENDER_QUALITY" "playable"
    Set-EnvValue "VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE" "1"
    Set-EnvValue "VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE" $BackgroundPassScale
    if ($BackgroundPassSurfaceFill -or (($BackgroundPassSurfaceFillWaterProof -or $BackgroundPassSurfaceFillExactRepair) -and -not $BackgroundPassSurfaceFillHighAltOnly)) {
        Set-EnvValue "VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL" "1"
    } else {
        Clear-EnvValue "VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL"
    }
    if ($BackgroundPassSurfaceFillHighAltOnly) {
        Set-EnvValue "VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_HIGH_ALT_ONLY" "1"
    } else {
        Clear-EnvValue "VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_HIGH_ALT_ONLY"
    }
    if ($BackgroundPassSurfaceFillWaterProof) {
        Set-EnvValue "VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_WATER_PROOF" "1"
    } else {
        Clear-EnvValue "VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_WATER_PROOF"
    }
    if ($BackgroundPassSurfaceFillExactRepair) {
        Set-EnvValue "VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_EXACT_REPAIR" "1"
    } else {
        Clear-EnvValue "VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_EXACT_REPAIR"
    }
    Set-EnvValue "VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_CLEAN_THROTTLE" "1"
    if ($CriticalReuse) {
        Set-EnvValue "VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_REUSE_MAX_SPEED" "64"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_REUSE_MAX_SPEED"
    }
    if ($TerrainCriticalParallelGeneration) {
        Set-EnvValue "VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION" "1"
        Set-EnvValue "VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MAX_WORKERS" ([string]$TerrainCriticalParallelGenerationMaxWorkers)
        Set-EnvValue "VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MIN_BRICKS" ([string]$TerrainCriticalParallelGenerationMinBricks)
    } else {
        Clear-EnvValue "VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION"
        Clear-EnvValue "VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MAX_WORKERS"
        Clear-EnvValue "VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MIN_BRICKS"
    }
    if ($TerrainCriticalReuseFrameState -or $TerrainCriticalReuseRepairOnly) {
        Set-EnvValue "VENPOD_SPARSE_TERRAIN_CRITICAL_REUSE_FRAME_STATE" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_TERRAIN_CRITICAL_REUSE_FRAME_STATE"
    }
    if ($TerrainCriticalReuseRepairOnly) {
        Set-EnvValue "VENPOD_SPARSE_TERRAIN_CRITICAL_REUSE_REPAIR_ONLY" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_TERRAIN_CRITICAL_REUSE_REPAIR_ONLY"
    }
    Set-EnvValue "VENPOD_SPARSE_SURFACE_INCREMENTAL_METADATA_ADDS" "1"
    Set-EnvValue "VENPOD_SPARSE_CPU_DETAIL" "1"
    if ($NoBacklogAwarePump) {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_BACKLOG_AWARE_PUMP"
    } else {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_BACKLOG_AWARE_PUMP" "1"
    }
    if ($NoHeavyDiagnostics) {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_DRAIN_REUSE_DIAGNOSTICS"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_FALLBACK_VALIDITY_CLASSIFIER"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_FALLBACK_CONTRACT_DIAGNOSTICS"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_FAR_SVO_FALLBACK_PROOF"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_FEEDBACK"
    } else {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_DRAIN_REUSE_DIAGNOSTICS" "1"
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_FALLBACK_VALIDITY_CLASSIFIER" "1"
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_FALLBACK_CONTRACT_DIAGNOSTICS" "1"
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_FAR_SVO_FALLBACK_PROOF" "1"
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_FEEDBACK" "1"
    }
    if ($ParentHeldFeedback) {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PARENT_HELD_FEEDBACK" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PARENT_HELD_FEEDBACK"
    }
    if ($MidClipmapVisibleCriticalPrepump) {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP"
    }
    if ($MidClipmapVisiblePriorityPump) {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_PRIORITY_PUMP" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_PRIORITY_PUMP"
    }
    if ($MidClipmapVisibleLaneGuard) {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_LANE_GUARD" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_LANE_GUARD"
    }
    if ($MidClipmapCacheOnlyDefer) {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_CACHE_ONLY_DEFER" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_CACHE_ONLY_DEFER"
    }
    if ($MidClipmapVisibleCoverageGuardV2) {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_COVERAGE_GUARD_V2" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_COVERAGE_GUARD_V2"
    }
    if ($MidClipmapStressCameraVelocity) {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_STRESS_CAMERA_VELOCITY" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_STRESS_CAMERA_VELOCITY"
    }
    if ($MidClipmapPredictedVisibleAdmission) {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION" "1"
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION_SAMPLES" ([string][Math]::Max(1, $MidClipmapPredictedVisibleAdmissionSamples))
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION_LEAD_FRAMES" ([string][Math]::Max(1, $MidClipmapPredictedVisibleAdmissionLeadFrames))
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION_MAX_COORDS" ([string][Math]::Max(0, $MidClipmapPredictedVisibleAdmissionMaxCoords))
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION_SAMPLE_SIDE" ([string][Math]::Max(1, $MidClipmapPredictedVisibleAdmissionSampleSide))
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION_MAX_DISTANCE" ([string][Math]::Max(0, $MidClipmapPredictedVisibleAdmissionMaxDistance))
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION_SAMPLES"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION_LEAD_FRAMES"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION_MAX_COORDS"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION_SAMPLE_SIDE"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION_MAX_DISTANCE"
    }
    if ($MidClipmapInterestDetail) {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_INTEREST_DETAIL" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_INTEREST_DETAIL"
    }
    if ($MidClipmapFootprintInterestSignature) {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_FOOTPRINT_INTEREST_SIGNATURE" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_FOOTPRINT_INTEREST_SIGNATURE"
    }
    if ($MidClipmapVoxelInterestSignatureReuse) {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_SIGNATURE_REUSE" "1"
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_SIGNATURE_REUSE_MAX_AGE" ([string][Math]::Max(0, $MidClipmapVoxelInterestSignatureReuseMaxAge))
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_SIGNATURE_REUSE"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_SIGNATURE_REUSE_MAX_AGE"
    }
    if ($MidClipmapMovingWindowPriority) {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_MOVING_WINDOW_PRIORITY" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_MOVING_WINDOW_PRIORITY"
    }
    if ($MidClipmapMovingWindowAsyncReservation) {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_MOVING_WINDOW_ASYNC_RESERVATION" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_MOVING_WINDOW_ASYNC_RESERVATION"
    }
    if ($MidInterestInterval -gt 1) {
        Set-EnvValue "VENPOD_SPARSE_MID_INTEREST_INTERVAL" ([string]$MidInterestInterval)
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_INTEREST_INTERVAL"
    }
    if ($StreamingLaneDiagnostics) {
        Set-EnvValue "VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS"
    }
    if ($StreamingLaneQueuePriority -or $HiddenExactPostOpenRepairLane) {
        Set-EnvValue "VENPOD_SPARSE_STREAMING_LANE_QUEUE_PRIORITY" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_STREAMING_LANE_QUEUE_PRIORITY"
    }
    if ($StreamingTicketScheduler) {
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER"
    }
    if ($StreamingTicketProtectedScheduling) {
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_PROTECTED_SCHEDULING" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_PROTECTED_SCHEDULING"
    }
    if ($StreamingTicketStageDemandAccounting) {
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_STAGE_DEMAND_ACCOUNTING" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_STAGE_DEMAND_ACCOUNTING"
    }
    if ($StreamingTicketGenerationOwnershipQueues) {
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_QUEUES" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_QUEUES"
    }
    if ($StreamingTicketGenerationOwnershipReservations) {
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_QUEUES" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_RESERVATIONS" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_RESERVATION_MAX" ([string][Math]::Max(0, $StreamingTicketGenerationOwnershipReservationMax))
        Set-EnvValue "VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_RESERVATIONS"
        Clear-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_RESERVATION_MAX"
    }
    if ($StreamingTicketGenerationOwnershipShareScheduler) {
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_QUEUES" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_SCHEDULER" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_PUBLIC_MIN" ([string][Math]::Max(0, $StreamingTicketGenerationOwnershipSharePublicMin))
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_VISIBLE_MAX" ([string][Math]::Max(0, $StreamingTicketGenerationOwnershipShareVisibleMax))
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_PREFETCH_MIN" ([string][Math]::Max(0, $StreamingTicketGenerationOwnershipSharePrefetchMin))
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_VISIBLE_DEBT_GATE" ([string][Math]::Max(0, $StreamingTicketGenerationOwnershipShareVisibleDebtGate))
        Set-EnvValue "VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_SCHEDULER"
        Clear-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_PUBLIC_MIN"
        Clear-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_VISIBLE_MAX"
        Clear-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_PREFETCH_MIN"
        Clear-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_VISIBLE_DEBT_GATE"
    }
    if ($StreamingTicketLowPriorityDownstreamDeferral) {
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_LOW_PRIORITY_DOWNSTREAM_DEFERRAL" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_LOW_PRIORITY_DOWNSTREAM_PROMOTE_MAX" ([string][Math]::Max(0, $StreamingTicketLowPriorityDownstreamPromoteMax))
        Set-EnvValue "VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_LOW_PRIORITY_DOWNSTREAM_DEFERRAL"
        Clear-EnvValue "VENPOD_SPARSE_STREAMING_TICKET_LOW_PRIORITY_DOWNSTREAM_PROMOTE_MAX"
    }
    if ($RequestExplicitSourceLanes -or $HiddenExactPostOpenRepairLane) {
        Set-EnvValue "VENPOD_SPARSE_REQUEST_EXPLICIT_SOURCE_LANES" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_LANE_QUEUE_PRIORITY" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_REQUEST_EXPLICIT_SOURCE_LANES"
    }
    if ($PrefetchLaneSpeculativeClass) {
        Set-EnvValue "VENPOD_SPARSE_REQUEST_EXPLICIT_SOURCE_LANES" "1"
        Set-EnvValue "VENPOD_SPARSE_PREFETCH_LANE_SPECULATIVE_CLASS" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS" "1"
        Set-EnvValue "VENPOD_SPARSE_STREAMING_LANE_QUEUE_PRIORITY" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_PREFETCH_LANE_SPECULATIVE_CLASS"
    }
    if ($PrefetchStageBudgets) {
        Set-EnvValue "VENPOD_SPARSE_PREFETCH_STAGE_BUDGETS" "1"
        if ($PrefetchStageBudgetsHighAltOnly) {
            Set-EnvValue "VENPOD_SPARSE_PREFETCH_STAGE_BUDGETS_HIGH_ALT_ONLY" "1"
        } else {
            Clear-EnvValue "VENPOD_SPARSE_PREFETCH_STAGE_BUDGETS_HIGH_ALT_ONLY"
        }
        Set-EnvValue "VENPOD_SPARSE_PREFETCH_STAGE_UPLOAD_BUDGET" ([string][Math]::Max(0, $PrefetchStageUploadBudget))
        Set-EnvValue "VENPOD_SPARSE_PREFETCH_STAGE_SURFACE_BUDGET" ([string][Math]::Max(0, $PrefetchStageSurfaceBudget))
        Set-EnvValue "VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_PREFETCH_STAGE_BUDGETS"
        Clear-EnvValue "VENPOD_SPARSE_PREFETCH_STAGE_BUDGETS_HIGH_ALT_ONLY"
        Clear-EnvValue "VENPOD_SPARSE_PREFETCH_STAGE_UPLOAD_BUDGET"
        Clear-EnvValue "VENPOD_SPARSE_PREFETCH_STAGE_SURFACE_BUDGET"
    }
    if ($OwnershipStageBudgets) {
        Set-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_BUDGETS" "1"
        Set-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_UPLOAD_BUDGET" ([string][Math]::Max(0, $OwnershipStageUploadBudget))
        Set-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_SURFACE_BUDGET" ([string][Math]::Max(0, $OwnershipStageSurfaceBudget))
        Set-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_PUBLISH_BUDGET" ([string][Math]::Max(0, $OwnershipStagePublishBudget))
        if ($OwnershipStageAdaptiveNoncritical) {
            Set-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_NONCRITICAL" "1"
            Set-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_MIN_VISIBLE_COVERAGE" ([string][Math]::Max(0, [Math]::Min(100, $OwnershipStageAdaptiveMinVisibleCoverage)))
            Set-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_MAX_MISSING_VISIBLE" ([string][Math]::Max(0, $OwnershipStageAdaptiveMaxMissingVisible))
            Set-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_UPLOAD_FLOOR" ([string][Math]::Max(0, $OwnershipStageAdaptiveUploadFloor))
            Set-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_SURFACE_FLOOR" ([string][Math]::Max(0, $OwnershipStageAdaptiveSurfaceFloor))
        } else {
            Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_NONCRITICAL"
            Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_MIN_VISIBLE_COVERAGE"
            Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_MAX_MISSING_VISIBLE"
            Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_UPLOAD_FLOOR"
            Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_SURFACE_FLOOR"
        }
        if ($OwnershipStageMidVisibleDebtThrottle) {
            Set-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_THROTTLE" "1"
            Set-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_MIN_COVERAGE" ([string][Math]::Max(0, [Math]::Min(100, $OwnershipStageMidVisibleDebtMinCoverage)))
            Set-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_MAX_MISSING_VISIBLE" ([string][Math]::Max(0, $OwnershipStageMidVisibleDebtMaxMissingVisible))
            Set-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_UPLOAD_FLOOR" ([string][Math]::Max(0, $OwnershipStageMidVisibleDebtUploadFloor))
            Set-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_SURFACE_FLOOR" ([string][Math]::Max(0, $OwnershipStageMidVisibleDebtSurfaceFloor))
        } else {
            Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_THROTTLE"
            Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_MIN_COVERAGE"
            Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_MAX_MISSING_VISIBLE"
            Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_UPLOAD_FLOOR"
            Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_SURFACE_FLOOR"
        }
        Set-EnvValue "VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_BUDGETS"
        Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_UPLOAD_BUDGET"
        Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_SURFACE_BUDGET"
        Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_PUBLISH_BUDGET"
        Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_NONCRITICAL"
        Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_MIN_VISIBLE_COVERAGE"
        Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_MAX_MISSING_VISIBLE"
        Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_UPLOAD_FLOOR"
        Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_SURFACE_FLOOR"
        Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_THROTTLE"
        Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_MIN_COVERAGE"
        Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_MAX_MISSING_VISIBLE"
        Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_UPLOAD_FLOOR"
        Clear-EnvValue "VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_SURFACE_FLOOR"
    }
    if ($DirectFootprintColumns) {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_DIRECT_FOOTPRINT_COLUMNS" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_DIRECT_FOOTPRINT_COLUMNS"
    }
    if ($ParallelMidWorkerColumnCache) {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_WORKER_COLUMN_CACHE" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_WORKER_COLUMN_CACHE"
    }
    if ($ParallelMidVoxelPump) {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_VISIBLE_PUMP" "1"
        if ($ParallelMidVoxelPumpPersistentWorkers) {
            Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PERSISTENT_WORKERS" "1"
        } else {
            Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PERSISTENT_WORKERS"
        }
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PUMP_MAX_WORKERS" ([string]$ParallelMidVoxelPumpMaxWorkers)
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PUMP_MIN_BRICKS" "8"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_VISIBLE_PUMP"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PERSISTENT_WORKERS"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PUMP_MAX_WORKERS"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PUMP_MIN_BRICKS"
    }
    if ($MidClipmapAsyncVisibleCriticalGeneration) {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_GEN" "1"
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_MAX_ENQUEUE" ([string][Math]::Max(1, $MidClipmapAsyncVisibleCriticalMaxEnqueue))
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_MAX_APPLY" ([string][Math]::Max(1, $MidClipmapAsyncVisibleCriticalMaxApply))
        if ($MidClipmapAsyncVisibleReservationMaxApply -ge 0) {
            Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_RESERVATION_MAX_APPLY" ([string][Math]::Max(0, $MidClipmapAsyncVisibleReservationMaxApply))
        } else {
            Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_RESERVATION_MAX_APPLY"
        }
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_GEN"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_MAX_ENQUEUE"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_MAX_APPLY"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_RESERVATION_MAX_APPLY"
    }
    if ($MidClipmapSplitVisiblePump) {
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP" "1"
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP_BUDGET" ([string][Math]::Max(0, $MidClipmapSplitVisiblePumpBudget))
        Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_SPLIT_CACHE_PUMP_BUDGET" ([string][Math]::Max(0, $MidClipmapSplitCachePumpBudget))
        if ($MidClipmapSplitVisiblePumpPostOpenOnly) {
            Set-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP_POST_OPEN_ONLY" "1"
        } else {
            Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP_POST_OPEN_ONLY"
        }
    } else {
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP_POST_OPEN_ONLY"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP_BUDGET"
        Clear-EnvValue "VENPOD_SPARSE_MID_CLIPMAP_SPLIT_CACHE_PUMP_BUDGET"
    }
    if ($DirectExactGeneration) {
        Set-EnvValue "VENPOD_SPARSE_EXACT_DIRECT_GENERATION" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_EXACT_DIRECT_GENERATION"
    }
    if ($AsyncExactGeneration) {
        Set-EnvValue "VENPOD_SPARSE_EXACT_ASYNC_GENERATION" "1"
        Clear-EnvValue "VENPOD_SPARSE_EXACT_ASYNC_VISIBLE"
        if ($AsyncExactPrefetchLane) {
            Set-EnvValue "VENPOD_SPARSE_EXACT_ASYNC_PREFETCH_LANE" "1"
        } else {
            Clear-EnvValue "VENPOD_SPARSE_EXACT_ASYNC_PREFETCH_LANE"
        }
        Set-EnvValue "VENPOD_SPARSE_EXACT_ASYNC_QUEUE_MAX" "512"
        if ($AsyncExactMaxEnqueuePerFrame -gt 0) {
            Set-EnvValue "VENPOD_SPARSE_EXACT_ASYNC_MAX_ENQUEUE_PER_FRAME" "$AsyncExactMaxEnqueuePerFrame"
        } else {
            Clear-EnvValue "VENPOD_SPARSE_EXACT_ASYNC_MAX_ENQUEUE_PER_FRAME"
        }
        Set-EnvValue "VENPOD_SPARSE_EXACT_ASYNC_MAX_APPLY_PER_FRAME" "64"
        if ($AsyncExactLowPriorityMaxApplyPerFrame -gt 0) {
            Set-EnvValue "VENPOD_SPARSE_EXACT_ASYNC_LOW_PRIORITY_MAX_APPLY_PER_FRAME" "$AsyncExactLowPriorityMaxApplyPerFrame"
        } else {
            Clear-EnvValue "VENPOD_SPARSE_EXACT_ASYNC_LOW_PRIORITY_MAX_APPLY_PER_FRAME"
        }
    } else {
        Clear-EnvValue "VENPOD_SPARSE_EXACT_ASYNC_GENERATION"
        Clear-EnvValue "VENPOD_SPARSE_EXACT_ASYNC_VISIBLE"
        Clear-EnvValue "VENPOD_SPARSE_EXACT_ASYNC_PREFETCH_LANE"
        Clear-EnvValue "VENPOD_SPARSE_EXACT_ASYNC_QUEUE_MAX"
        Clear-EnvValue "VENPOD_SPARSE_EXACT_ASYNC_MAX_ENQUEUE_PER_FRAME"
        Clear-EnvValue "VENPOD_SPARSE_EXACT_ASYNC_MAX_APPLY_PER_FRAME"
        Clear-EnvValue "VENPOD_SPARSE_EXACT_ASYNC_LOW_PRIORITY_MAX_APPLY_PER_FRAME"
    }
    if ($RequestFastResidentTouch) {
        Set-EnvValue "VENPOD_SPARSE_REQUEST_FAST_RESIDENT_TOUCH" "1"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_REQUEST_FAST_RESIDENT_TOUCH"
    }
    if ($ParallelExactGeneration) {
        Set-EnvValue "VENPOD_SPARSE_EXACT_PARALLEL_GENERATION" "1"
        if ($ParallelExactPersistentWorkers) {
            Set-EnvValue "VENPOD_SPARSE_EXACT_PERSISTENT_WORKERS" "1"
        } else {
            Clear-EnvValue "VENPOD_SPARSE_EXACT_PERSISTENT_WORKERS"
        }
        Set-EnvValue "VENPOD_SPARSE_EXACT_PARALLEL_GENERATION_MAX_WORKERS" ([string]$ParallelExactGenerationMaxWorkers)
        Set-EnvValue "VENPOD_SPARSE_EXACT_PARALLEL_GENERATION_MIN_BRICKS" "8"
    } else {
        Clear-EnvValue "VENPOD_SPARSE_EXACT_PARALLEL_GENERATION"
        Clear-EnvValue "VENPOD_SPARSE_EXACT_PERSISTENT_WORKERS"
        Clear-EnvValue "VENPOD_SPARSE_EXACT_PARALLEL_GENERATION_MAX_WORKERS"
        Clear-EnvValue "VENPOD_SPARSE_EXACT_PARALLEL_GENERATION_MIN_BRICKS"
    }
}

function Clear-ScenarioEnv {
    Clear-EnvValue "VENPOD_SPARSE_WALK_TEST"
    Clear-EnvValue "VENPOD_SPARSE_WALK_TEST_SPEED"
    Clear-EnvValue "VENPOD_SPARSE_WALK_TEST_YAW_DEG"
    Clear-EnvValue "VENPOD_SPARSE_WALK_TEST_PITCH_DEG"
    Clear-EnvValue "VENPOD_SPARSE_WALK_TEST_FIXED_DT"
    Clear-EnvValue "VENPOD_SPARSE_STRESS_REQUESTS"
    Clear-EnvValue "VENPOD_SPARSE_STRESS_CAMERA"
    Clear-EnvValue "VENPOD_SPARSE_STRESS_CAMERA_RADIUS"
    Clear-EnvValue "VENPOD_SPARSE_STRESS_CAMERA_HEIGHT"
    Clear-EnvValue "VENPOD_SPARSE_STRESS_CAMERA_BASE_HEIGHT"
    Clear-EnvValue "VENPOD_SPARSE_STRESS_CAMERA_SPEED"
    Clear-EnvValue "VENPOD_SPARSE_STRESS_CAMERA_ABSOLUTE_FRAME"
}

function Set-WalkScenarioEnv {
    Set-EnvValue "VENPOD_SPARSE_WALK_TEST" "1"
    Set-EnvValue "VENPOD_SPARSE_WALK_TEST_SPEED" "38"
    Set-EnvValue "VENPOD_SPARSE_WALK_TEST_YAW_DEG" "10"
    Set-EnvValue "VENPOD_SPARSE_WALK_TEST_PITCH_DEG" "-4"
    if ($WalkFixedDtMs -gt 0) {
        Set-EnvValue "VENPOD_SPARSE_WALK_TEST_FIXED_DT" ([string]$WalkFixedDtMs)
    } else {
        Clear-EnvValue "VENPOD_SPARSE_WALK_TEST_FIXED_DT"
    }
}

function Set-HighAltScenarioEnv {
    Set-EnvValue "VENPOD_SPARSE_STRESS_REQUESTS" "1"
    Set-EnvValue "VENPOD_SPARSE_STRESS_CAMERA" "1"
    Set-EnvValue "VENPOD_SPARSE_STRESS_CAMERA_RADIUS" "900"
    Set-EnvValue "VENPOD_SPARSE_STRESS_CAMERA_HEIGHT" "180"
    Set-EnvValue "VENPOD_SPARSE_STRESS_CAMERA_BASE_HEIGHT" "520"
    Set-EnvValue "VENPOD_SPARSE_STRESS_CAMERA_SPEED" "50"
    Set-EnvValue "VENPOD_SPARSE_STRESS_CAMERA_ABSOLUTE_FRAME" "1"
}

function Match-Last {
    param([string]$Text, [string]$Pattern)
    $matches = [regex]::Matches($Text, $Pattern)
    if ($matches.Count -eq 0) { return $null }
    return $matches[$matches.Count - 1]
}

function Match-First {
    param([string]$Text, [string]$Pattern)
    $match = [regex]::Match($Text, $Pattern)
    if (-not $match.Success) { return $null }
    return $match
}

function Group-Value {
    param($Match, [string]$Name, [string]$Default = "")
    if ($null -eq $Match) { return $Default }
    $group = $Match.Groups[$Name]
    if ($null -eq $group -or -not $group.Success) { return $Default }
    return $group.Value
}

function Parse-ScenarioLog {
    param([string]$ScenarioName, [int]$TargetFrame, [string]$LogPath)

    $text = Get-Content -LiteralPath $LogPath -Raw
    $framePattern = "PERF frame=$TargetFrame\b.*"
    $perf = Match-Last $text $framePattern
    $actualFrame = $TargetFrame
    if ($null -eq $perf) {
        $perfMatches = [regex]::Matches($text, "PERF frame=(?<frame>[0-9]+)\b.*")
        $best = $null
        $bestDistance = [int]::MaxValue
        foreach ($candidate in $perfMatches) {
            $candidateFrame = [int]$candidate.Groups["frame"].Value
            $distance = [Math]::Abs($candidateFrame - $TargetFrame)
            if ($distance -lt $bestDistance -or
                ($distance -eq $bestDistance -and $candidateFrame -gt $actualFrame)) {
                $best = $candidate
                $bestDistance = $distance
                $actualFrame = $candidateFrame
            }
        }
        if ($null -eq $best -or $bestDistance -gt 30) {
            $frameEndMatches = [regex]::Matches($text, "PERF_FRAME_END frame=(?<frame>[0-9]+)\b.*")
            foreach ($candidate in $frameEndMatches) {
                $candidateFrame = [int]$candidate.Groups["frame"].Value
                $distance = [Math]::Abs($candidateFrame - $TargetFrame)
                if ($distance -lt $bestDistance -or
                    ($distance -eq $bestDistance -and $candidateFrame -gt $actualFrame)) {
                    $best = $candidate
                    $bestDistance = $distance
                    $actualFrame = $candidateFrame
                }
            }
            if ($null -eq $best -or $bestDistance -gt 30) {
                throw "Missing PERF frame=$TargetFrame in $LogPath"
            }
        }
        $perf = $best
    }

    $perfLine = $perf.Value
    $frameEnd = Match-Last $text "PERF_FRAME_END frame=$actualFrame\b.*"
    $backlog = Match-Last $text "PERF_SPARSE_MID_CLIPMAP_BACKLOG frame=$actualFrame\b.*"
    $fallback = Match-Last $text "PERF_SPARSE_MID_CLIPMAP_FALLBACK frame=$actualFrame\b.*"
    $contract = Match-Last $text "PERF_SPARSE_MID_CLIPMAP_FALLBACK_CONTRACT frame=$actualFrame\b.*"
    $sample = Match-Last $text "PERF_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_FEEDBACK frame=$actualFrame\b.*"
    $parentHeldFeedback = Match-Last $text "PERF_SPARSE_MID_VOXEL_PARENT_HELD_FEEDBACK frame=$actualFrame\b.*"
    $generalGenerationBudget = Match-Last $text "PERF_SPARSE_GENERAL_GENERATION_BUDGET frame=$actualFrame\b.*"
    $visible = Match-Last $text "PERF_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP frame=$actualFrame\b.*"
    $visiblePriority = Match-Last $text "PERF_SPARSE_MID_CLIPMAP_VISIBLE_PRIORITY frame=$actualFrame\b.*"
    $visibleLaneGuard = Match-Last $text "PERF_SPARSE_MID_CLIPMAP_VISIBLE_LANE_GUARD frame=$actualFrame\b.*"
    $midLanes = Match-Last $text "PERF_SPARSE_MID_CLIPMAP_LANES frame=$actualFrame\b.*"
    $streamingLanes = Match-Last $text "PERF_SPARSE_STREAMING_LANES frame=$actualFrame\b.*"
    $generatedLanes = Match-Last $text "PERF_SPARSE_GENERATED_LANES frame=$actualFrame\b.*"
    $deferredDownstream = Match-Last $text "PERF_SPARSE_DEFERRED_DOWNSTREAM frame=$actualFrame\b.*"
    $exactAsync = Match-Last $text "PERF_SPARSE_EXACT_ASYNC frame=$actualFrame\b.*"
    $exactParallel = Match-Last $text "PERF_SPARSE_EXACT_PARALLEL frame=$actualFrame\b.*"
    $camera = Match-Last $text "PERF_CAMERA_EXPOSURE frame=$actualFrame\b.*"
    $terrainCritical = Match-Last $text "PERF_SPARSE_TERRAIN_CRITICAL frame=$actualFrame\b.*"
    $terrainSurfacePublication = Match-Last $text "PERF_SPARSE_TERRAIN_CRITICAL_SURFACE_PUBLICATION frame=$actualFrame\b.*"
    $surfaceReadyPublish = Match-Last $text "PERF_SPARSE_SURFACE_READY_PUBLISH frame=$actualFrame\b.*"
    $hiddenTrackedScan = Match-Last $text "PERF_SPARSE_HIDDEN_EXACT_TRACKED_SCAN frame=$actualFrame\b.*"
    $hiddenRepairLane = Match-Last $text "PERF_SPARSE_HIDDEN_EXACT_REPAIR_LANE frame=$actualFrame\b.*"
    $requestDetail = Match-Last $text "PERF_SPARSE_REQUEST_DETAIL frame=$actualFrame\b.*"
    $cpuDetail = Match-Last $text "PERF_SPARSE_CPU_DETAIL frame=$actualFrame\b.*"
    $ownershipStage = Match-Last $text "PERF_SPARSE_OWNERSHIP_STAGE_BUDGETS frame=$actualFrame\b.*"

    $msMatch = Match-First $perfLine "ms=(?<body>[-+0-9.]+)/(?<raw>[-+0-9.]+)"
    $gpuMatch = Match-First $perfLine "gpuRay=(?<gpu>[-+0-9.]+)ms"
    if ($null -eq $gpuMatch) {
        $gpuMatch = Match-First $perfLine "gpu=frame/upload/pre/surface/ray/overlay/ui:(?<frame>[-+0-9.]+)/(?<upload>[-+0-9.]+)/(?<pre>[-+0-9.]+)/(?<surfaceGpu>[-+0-9.]+)/(?<gpu>[-+0-9.]+)"
    }
    $waitMatch = Match-First $perfLine "\bwait=(?<wait>[-+0-9.]+)"
    $accountedMatch = Match-First $perfLine "\baccounted=(?<accounted>[-+0-9.]+)"
    $untrackedMatch = Match-First $perfLine "\buntracked=(?<untracked>[-+0-9.]+)"

    $cpuMatch = Match-First $perfLine "prepSplit=[^:]+:(?<sched>[-+0-9.]+)/(?<input>[-+0-9.]+)/(?<cpu>[-+0-9.]+)/(?<coll>[-+0-9.]+)"
    $sparseMatch = Match-First $perfLine "sparse=\{req=(?<req>[-+0-9.]+),gen=(?<gen>[-+0-9.]+),clip=(?<clip>[-+0-9.]+),surface=(?<surface>[-+0-9.]+),hidden=(?<hidden>[-+0-9.]+),upload=(?<upload>[-+0-9.]+),readback=(?<readback>[-+0-9.]+),trim=(?<trim>[-+0-9.]+)\}"
    if ($null -eq $sparseMatch) {
        $sparseMatch = Match-First $perfLine "sparseSplit=req/gen/clip/trim:(?<req>[-+0-9.]+)/(?<gen>[-+0-9.]+)/(?<clip>[-+0-9.]+)/(?<trim>[-+0-9.]+)"
    }

    $pump = ""
    $mainThreadBrickGenMs = ""
    $criticalMissing = ""
    $nonCriticalMissing = ""
    $midReservationApplyLimit = ""
    $midReservationApplied = ""
    $midReservationApplyDeferred = ""
    $midVoxelInterestReuseActive = ""
    $midVoxelInterestReuseAge = ""
    if ($null -ne $backlog) {
        $pumpMatch = Match-First $backlog.Value "pumpMs=(?<pump>[-+0-9.]+)"
        $pump = Group-Value $pumpMatch "pump"
        $mainThreadBrickGenMs = Group-Value (Match-First $backlog.Value "mainThreadBrickGenMs=(?<v>[-+0-9.]+)") "v"
        if ([string]::IsNullOrWhiteSpace($pump)) {
            $pump = $mainThreadBrickGenMs
        }
        $criticalMissing = Group-Value (Match-First $backlog.Value "criticalMissing=(?<v>[0-9]+)") "v"
        $nonCriticalMissing = Group-Value (Match-First $backlog.Value "nonCriticalMissing=(?<v>[0-9]+)") "v"
        $midReservationApply = Match-First $backlog.Value "reservationApply=limit/applied/deferred:(?<limit>[0-9]+)/(?<applied>[0-9]+)/(?<deferred>[0-9]+)"
        $midReservationApplyLimit = Group-Value $midReservationApply "limit"
        $midReservationApplied = Group-Value $midReservationApply "applied"
        $midReservationApplyDeferred = Group-Value $midReservationApply "deferred"
        $midVoxelReuse = Match-First $backlog.Value "voxelReuse=active/age:(?<active>[0-9]+)/(?<age>[0-9]+)"
        $midVoxelInterestReuseActive = Group-Value $midVoxelReuse "active"
        $midVoxelInterestReuseAge = Group-Value $midVoxelReuse "age"
    }

    $bodyEnd = ""
    $rawEnd = ""
    $surfaceExtract = ""
    $surfaceStage = ""
    if ($null -ne $frameEnd) {
        $bodyEndMatch = Match-First $frameEnd.Value "body(?:Ms)?=(?<body>[-+0-9.]+)"
        $rawEndMatch = Match-First $frameEnd.Value "rawMs=(?<raw>[-+0-9.]+)"
        $bodyEnd = Group-Value $bodyEndMatch "body"
        $rawEnd = Group-Value $rawEndMatch "raw"
        $surfacePost = Match-First $frameEnd.Value "sparsePost=feedback/cmd/begin/midSnap/plan/upload/publish/midUpload/stats/surfExtract/surfPlan/surfSnap/surfStage/surfEmit:(?<feedback>[-+0-9.]+)/(?<cmd>[-+0-9.]+)/(?<begin>[-+0-9.]+)/(?<midSnap>[-+0-9.]+)/(?<plan>[-+0-9.]+)/(?<upload>[-+0-9.]+)/(?<publish>[-+0-9.]+)/(?<midUpload>[-+0-9.]+)/(?<stats>[-+0-9.]+)/(?<surfExtract>[-+0-9.]+)/(?<surfPlan>[-+0-9.]+)/(?<surfSnap>[-+0-9.]+)/(?<surfStage>[-+0-9.]+)/(?<surfEmit>[-+0-9.]+)"
        $surfaceExtract = Group-Value $surfacePost "surfExtract"
        $surfaceStage = Group-Value $surfacePost "surfStage"
    }
    $bodyMs = Group-Value $msMatch "body"
    $rawMs = Group-Value $msMatch "raw"
    if ([string]::IsNullOrWhiteSpace($bodyMs)) {
        $bodyMs = $bodyEnd
    }
    if ([string]::IsNullOrWhiteSpace($rawMs)) {
        $rawMs = $rawEnd
    }

    $missing = ""
    $coverage = ""
    $budgetReason = ""
    $backlogVoxel = ""
    $backlogMaxAge = ""
    $parallelPumpActive = ""
    $parallelPumpBricks = ""
    $parallelPumpWorkers = ""
    $parallelPumpWallMs = ""
    $workerColumnCacheActive = ""
    $workerColumnCacheEntries = ""
    $workerColumnHeightHits = ""
    $workerColumnHeightMisses = ""
    $workerColumnReliefHits = ""
    $workerColumnReliefMisses = ""
    if ($null -ne $backlog) {
        $missing = Group-Value (Match-First $backlog.Value "missingVoxel=(?<v>[0-9]+)") "v"
        $coverage = Group-Value (Match-First $backlog.Value "coverage=(?<v>[-+0-9.]+)") "v"
        $budgetReason = Group-Value (Match-First $backlog.Value "budgetReason=(?<v>[0-9]+)") "v"
        $backlogVoxel = Group-Value (Match-First $backlog.Value "backlogVoxel=(?<v>[0-9]+)") "v"
        $backlogMaxAge = Group-Value (Match-First $backlog.Value "maxAge=(?<v>[0-9]+)") "v"
        $workerColumnMatch = Match-First $backlog.Value "workerColumnCache=active/entries/hHit/hMiss/rHit/rMiss:(?<active>[0-9]+)/(?<entries>[0-9]+)/(?<hHit>[0-9]+)/(?<hMiss>[0-9]+)/(?<rHit>[0-9]+)/(?<rMiss>[0-9]+)"
        $workerColumnCacheActive = Group-Value $workerColumnMatch "active"
        $workerColumnCacheEntries = Group-Value $workerColumnMatch "entries"
        $workerColumnHeightHits = Group-Value $workerColumnMatch "hHit"
        $workerColumnHeightMisses = Group-Value $workerColumnMatch "hMiss"
        $workerColumnReliefHits = Group-Value $workerColumnMatch "rHit"
        $workerColumnReliefMisses = Group-Value $workerColumnMatch "rMiss"
        $parallelMatch = Match-First $backlog.Value "parallelPump=active/bricks/workers/wallMs:(?<active>[0-9]+)/(?<bricks>[0-9]+)/(?<workers>[0-9]+)/(?<wall>[-+0-9.]+)"
        $parallelPumpActive = Group-Value $parallelMatch "active"
        $parallelPumpBricks = Group-Value $parallelMatch "bricks"
        $parallelPumpWorkers = Group-Value $parallelMatch "workers"
        $parallelPumpWallMs = Group-Value $parallelMatch "wall"
    }
    $fallbackValid = ""
    $fallbackInvalid = ""
    $fallbackUnknown = ""
    if ($null -ne $fallback) {
        $fallbackValid = Group-Value (Match-First $fallback.Value "missingFallbackValid=(?<v>[0-9]+)") "v"
        $fallbackInvalid = Group-Value (Match-First $fallback.Value "missingFallbackInvalid=(?<v>[0-9]+)") "v"
        $fallbackUnknown = Group-Value (Match-First $fallback.Value "missingFallbackUnknown=(?<v>[0-9]+)") "v"
    }

    $midPct = ""
    $missPct = ""
    $unsafePct = ""
    $sampled = ""
    $unsampled = ""
    $sampledPct = ""
    if ($null -ne $sample) {
        if ([string]::IsNullOrWhiteSpace($missing)) {
            $missing = Group-Value (Match-First $sample.Value "missingVoxel=(?<v>[0-9]+)") "v"
        }
        if ([string]::IsNullOrWhiteSpace($coverage)) {
            $coverage = Group-Value (Match-First $sample.Value "coverage=(?<v>[-+0-9.]+)") "v"
        }
        if ([string]::IsNullOrWhiteSpace($budgetReason)) {
            $budgetReason = Group-Value (Match-First $sample.Value "budgetReason=(?<v>[0-9]+)") "v"
        }
        $sampled = Group-Value (Match-First $sample.Value "sampledMissingBrickApprox=(?<v>[0-9]+)") "v"
        $unsampled = Group-Value (Match-First $sample.Value "unsampledMissingBrickApprox=(?<v>[0-9]+)") "v"
        $sampledPct = Group-Value (Match-First $sample.Value "sampledMissingPctOfMissing=(?<v>[-+0-9.]+)") "v"
        if ([string]::IsNullOrWhiteSpace($missPct)) {
            $missPct = Group-Value (Match-First $sample.Value "missScreenPct=(?<v>[-+0-9.]+)") "v"
        }
        if ([string]::IsNullOrWhiteSpace($unsafePct)) {
            $unsafePct = Group-Value (Match-First $sample.Value "unsafeNearMissPct=(?<v>[-+0-9.]+)") "v"
        }
    }

    $visibleCritical = ""
    $midCacheOnlyDefer = ""
    $coverageVisible = ""
    $coverageCache = ""
    $midReservationTicketsActive = ""
    $midReservationTicketsDue = ""
    $midReservationTicketsOverdue = ""
    $midReservationTicketHits = ""
    $midReservationTicketMaxAge = ""
    if ($null -ne $visible) {
        $visibleCritical = Group-Value (Match-First $visible.Value "visibleCriticalVoxel=(?<v>[0-9]+)") "v"
        $midCacheOnlyDefer = Group-Value (Match-First $visible.Value "cacheOnlyDefer=(?<v>[0-9]+)") "v"
        $coverageVisible = Group-Value (Match-First $visible.Value "coverageVisibleCritical=(?<v>[-+0-9.]+)") "v"
        $coverageCache = Group-Value (Match-First $visible.Value "coverageCache=(?<v>[-+0-9.]+)") "v"
        $midReservationTickets = Match-First $visible.Value "reservationTickets=active/due/overdue/hits/maxAge:(?<active>[0-9]+)/(?<due>[0-9]+)/(?<overdue>[0-9]+)/(?<hits>[0-9]+)/(?<maxAge>[0-9]+)"
        $midReservationTicketsActive = Group-Value $midReservationTickets "active"
        $midReservationTicketsDue = Group-Value $midReservationTickets "due"
        $midReservationTicketsOverdue = Group-Value $midReservationTickets "overdue"
        $midReservationTicketHits = Group-Value $midReservationTickets "hits"
        $midReservationTicketMaxAge = Group-Value $midReservationTickets "maxAge"
    }
    if ($null -ne $visibleLaneGuard) {
        if ([string]::IsNullOrWhiteSpace($visibleCritical)) {
            $visibleCritical =
                Group-Value (Match-First $visibleLaneGuard.Value "visibleCriticalVoxel=(?<v>[0-9]+)") "v"
        }
        if ([string]::IsNullOrWhiteSpace($coverageVisible)) {
            $coverageVisible =
                Group-Value (Match-First $visibleLaneGuard.Value "coverageVisible=(?<v>[-+0-9.]+)") "v"
        }
        if ([string]::IsNullOrWhiteSpace($coverageCache)) {
            $coverageCache =
                Group-Value (Match-First $visibleLaneGuard.Value "coverageCache=(?<v>[-+0-9.]+)") "v"
        }
    }

    $midVisiblePriorityProjected = ""
    $midVisiblePriorityPrioritized = ""
    if ($null -ne $visiblePriority) {
        $midVisiblePriorityProjected =
            Group-Value (Match-First $visiblePriority.Value "projectedVisible=(?<v>[0-9]+)") "v"
        $midVisiblePriorityPrioritized =
            Group-Value (Match-First $visiblePriority.Value "prioritizedVisible=(?<v>[0-9]+)") "v"
    }
    if ($null -ne $visibleLaneGuard) {
        if ([string]::IsNullOrWhiteSpace($midVisiblePriorityProjected)) {
            $midVisiblePriorityProjected =
                Group-Value (Match-First $visibleLaneGuard.Value "projectedVisible=(?<v>[0-9]+)") "v"
        }
        if ([string]::IsNullOrWhiteSpace($midVisiblePriorityPrioritized)) {
            $midVisiblePriorityPrioritized =
                Group-Value (Match-First $visibleLaneGuard.Value "prioritizedVisible=(?<v>[0-9]+)") "v"
        }
    }

    $ownershipMidDebtEnabled = ""
    $ownershipMidDebtActive = ""
    $ownershipMidDebtCoverageInDebt = ""
    $ownershipMidDebtUploadBudget = ""
    $ownershipMidDebtSurfaceBudget = ""
    if ($null -ne $ownershipStage) {
        $midDebtMatch = Match-First $ownershipStage.Value "midDebt=enabled/active/publicOpen/coverageInDebt/ownershipClean/coverage/missing/uploadFloor/surfaceFloor/uploadBudget/surfaceBudget:(?<enabled>[0-9]+)/(?<active>[0-9]+)/(?<public>[0-9]+)/(?<debt>[0-9]+)/(?<clean>[0-9]+)/(?<coverage>[0-9]+)/(?<missing>[0-9]+)/(?<uploadFloor>[0-9]+)/(?<surfaceFloor>[0-9]+)/(?<uploadBudget>[0-9]+)/(?<surfaceBudget>[0-9]+)"
        $ownershipMidDebtEnabled = Group-Value $midDebtMatch "enabled"
        $ownershipMidDebtActive = Group-Value $midDebtMatch "active"
        $ownershipMidDebtCoverageInDebt = Group-Value $midDebtMatch "debt"
        $ownershipMidDebtUploadBudget = Group-Value $midDebtMatch "uploadBudget"
        $ownershipMidDebtSurfaceBudget = Group-Value $midDebtMatch "surfaceBudget"
    }

    $midLaneMissingVisible = ""
    $midLaneMissingCache = ""
    $midLaneQueuedVisible = ""
    $midLaneQueuedCache = ""
    $midLaneAgeVisible = ""
    $midLaneAgeCache = ""
    if ($null -ne $midLanes) {
        $midLaneMissingVisible =
            Group-Value (Match-First $midLanes.Value "missingVisible=(?<v>[0-9]+)") "v"
        $midLaneMissingCache =
            Group-Value (Match-First $midLanes.Value "missingCache=(?<v>[0-9]+)") "v"
        $midLaneQueuedVisible =
            Group-Value (Match-First $midLanes.Value "queuedVisible=(?<v>[0-9]+)") "v"
        $midLaneQueuedCache =
            Group-Value (Match-First $midLanes.Value "queuedCache=(?<v>[0-9]+)") "v"
        $midLaneAgeVisible =
            Group-Value (Match-First $midLanes.Value "ageVisible=(?<v>[0-9]+)") "v"
        $midLaneAgeCache =
            Group-Value (Match-First $midLanes.Value "ageCache=(?<v>[0-9]+)") "v"
    }

    $parentHeldSamples = ""
    $parentHeldStored = ""
    $parentHeldAccepted = ""
    $parentHeldPending = ""
    if ($null -ne $parentHeldFeedback) {
        $samplePair = Match-First $parentHeldFeedback.Value "samples=(?<stored>[0-9]+)/(?<total>[0-9]+)"
        $parentHeldStored = Group-Value $samplePair "stored"
        $parentHeldSamples = Group-Value $samplePair "total"
        if ([string]::IsNullOrWhiteSpace($parentHeldSamples)) {
            $parentHeldSamples = Group-Value (Match-First $parentHeldFeedback.Value "parentHeldPixels=(?<v>[0-9]+)") "v"
        }
        if ([string]::IsNullOrWhiteSpace($parentHeldStored)) {
            $parentHeldStored = Group-Value (Match-First $parentHeldFeedback.Value "candidates=(?<v>[0-9]+)") "v"
        }
        $parentHeldAccepted = Group-Value (Match-First $parentHeldFeedback.Value "accepted=(?<v>[0-9]+)") "v"
        $parentHeldPending = Group-Value (Match-First $parentHeldFeedback.Value "pending=(?<v>[0-9]+)") "v"
    }

    $generalGenMax = ""
    $generalGenUncapped = ""
    $generalGenApplied = ""
    $generalGenGenerated = ""
    $generalGenElapsed = ""
    $generalGenProtected = ""
    $generalGenQueuedBefore = ""
    if ($null -ne $generalGenerationBudget) {
        $generalGenMax = Group-Value (Match-First $generalGenerationBudget.Value "max=(?<v>[0-9]+)") "v"
        $generalGenUncapped = Group-Value (Match-First $generalGenerationBudget.Value "uncapped=(?<v>[0-9]+)") "v"
        $generalGenApplied = Group-Value (Match-First $generalGenerationBudget.Value "applied=(?<v>[0-9]+)") "v"
        $generalGenGenerated = Group-Value (Match-First $generalGenerationBudget.Value "generated=(?<v>[0-9]+)") "v"
        $generalGenElapsed = Group-Value (Match-First $generalGenerationBudget.Value "elapsedMs=(?<v>[-+0-9.]+)") "v"
        $generalGenProtected = Group-Value (Match-First $generalGenerationBudget.Value "protectedGenerated=(?<v>[0-9]+)") "v"
        $generalGenQueuedBefore = Group-Value (Match-First $generalGenerationBudget.Value "queuedBefore=(?<v>[0-9]+)") "v"
    }

    $asyncQueueDepth = ""
    $asyncResultDepth = ""
    $asyncPending = ""
    $asyncEnqueued = ""
    $asyncCompleted = ""
    $asyncApplied = ""
    $asyncDiscarded = ""
    $asyncSyncFallback = ""
    $asyncOldestAge = ""
    $asyncWorkerMs = ""
    $asyncApplyMs = ""
    $asyncMaxEnqueue = ""
    $asyncLowPriorityMaxApply = ""
    $asyncDeferredLowPriority = ""
    $asyncEnqueuedLaneCache = ""
    $asyncEnqueuedLanePrefetch = ""
    $asyncEnqueuedLaneRepair = ""
    $asyncEnqueuedLaneVisible = ""
    $asyncEnqueuedLanePublic = ""
    $asyncAppliedLaneCache = ""
    $asyncAppliedLanePrefetch = ""
    $asyncAppliedLaneRepair = ""
    $asyncAppliedLaneVisible = ""
    $asyncAppliedLanePublic = ""
    $requestLaneCache = ""
    $requestLanePrefetch = ""
    $requestLaneRepair = ""
    $requestLaneVisible = ""
    $requestLanePublic = ""
    $generationLaneCache = ""
    $generationLanePrefetch = ""
    $generationLaneRepair = ""
    $generationLaneVisible = ""
    $generationLanePublic = ""
    $generatedLaneCache = ""
    $generatedLanePrefetch = ""
    $generatedLaneRepair = ""
    $generatedLaneVisible = ""
    $generatedLanePublic = ""
    $deferredDownstreamEnabled = ""
    $deferredDownstreamPending = ""
    $deferredDownstreamLaneCache = ""
    $deferredDownstreamLanePrefetch = ""
    $deferredDownstreamLaneRepair = ""
    $deferredDownstreamLaneVisible = ""
    $deferredDownstreamLanePublic = ""
    $deferredDownstreamPromoted = ""
    $deferredDownstreamStale = ""
    $deferredDownstreamGeneratedTotal = ""
    $uploadLaneCache = ""
    $uploadLanePrefetch = ""
    $uploadLaneRepair = ""
    $uploadLaneVisible = ""
    $uploadLanePublic = ""
    $surfaceLaneCache = ""
    $surfaceLanePrefetch = ""
    $surfaceLaneRepair = ""
    $surfaceLaneVisible = ""
    $surfaceLanePublic = ""
    $streamingLaneQueuePriorityActive = ""
    $prefetchSpeculativeClassActive = ""
    $prefetchSpeculativeTouches = ""
    $lanePattern = "cache/prefetch/(?:repair/)?visible/public:(?<cache>[0-9]+)/(?<prefetch>[0-9]+)/(?:(?<repair>[0-9]+)/)?(?<visible>[0-9]+)/(?<public>[0-9]+)"
    if ($null -ne $streamingLanes) {
        $streamingLaneQueuePriorityActive =
            Group-Value (Match-First $streamingLanes.Value "queuePriorityActive=(?<v>[0-9]+)") "v"
        $prefetchSpeculativeClassActive =
            Group-Value (Match-First $streamingLanes.Value "prefetchSpeculativeClassActive=(?<v>[0-9]+)") "v"
        $prefetchSpeculativeTouches =
            Group-Value (Match-First $streamingLanes.Value "prefetchSpeculativeTouches=(?<v>[0-9]+)") "v"
        $requestLaneMatch = Match-First $streamingLanes.Value "requestTouches=$lanePattern"
        $generationLaneMatch = Match-First $streamingLanes.Value "genLane=$lanePattern"
        $generatedLaneMatch = Match-First $streamingLanes.Value "generatedLane=$lanePattern"
        $uploadLaneMatch = Match-First $streamingLanes.Value "uploadLane=$lanePattern"
        $surfaceLaneMatch = Match-First $streamingLanes.Value "surfaceLane=$lanePattern"
        $requestLaneCache = Group-Value $requestLaneMatch "cache"
        $requestLanePrefetch = Group-Value $requestLaneMatch "prefetch"
        $requestLaneRepair = Group-Value $requestLaneMatch "repair"
        $requestLaneVisible = Group-Value $requestLaneMatch "visible"
        $requestLanePublic = Group-Value $requestLaneMatch "public"
        $generationLaneCache = Group-Value $generationLaneMatch "cache"
        $generationLanePrefetch = Group-Value $generationLaneMatch "prefetch"
        $generationLaneRepair = Group-Value $generationLaneMatch "repair"
        $generationLaneVisible = Group-Value $generationLaneMatch "visible"
        $generationLanePublic = Group-Value $generationLaneMatch "public"
        $generatedLaneCache = Group-Value $generatedLaneMatch "cache"
        $generatedLanePrefetch = Group-Value $generatedLaneMatch "prefetch"
        $generatedLaneRepair = Group-Value $generatedLaneMatch "repair"
        $generatedLaneVisible = Group-Value $generatedLaneMatch "visible"
        $generatedLanePublic = Group-Value $generatedLaneMatch "public"
        $uploadLaneCache = Group-Value $uploadLaneMatch "cache"
        $uploadLanePrefetch = Group-Value $uploadLaneMatch "prefetch"
        $uploadLaneRepair = Group-Value $uploadLaneMatch "repair"
        $uploadLaneVisible = Group-Value $uploadLaneMatch "visible"
        $uploadLanePublic = Group-Value $uploadLaneMatch "public"
        $surfaceLaneCache = Group-Value $surfaceLaneMatch "cache"
        $surfaceLanePrefetch = Group-Value $surfaceLaneMatch "prefetch"
        $surfaceLaneRepair = Group-Value $surfaceLaneMatch "repair"
        $surfaceLaneVisible = Group-Value $surfaceLaneMatch "visible"
        $surfaceLanePublic = Group-Value $surfaceLaneMatch "public"
    }
    if ($null -ne $generatedLanes) {
        $generatedLaneMatch = Match-First $generatedLanes.Value "generatedLane=$lanePattern"
        $generatedLaneCache = Group-Value $generatedLaneMatch "cache"
        $generatedLanePrefetch = Group-Value $generatedLaneMatch "prefetch"
        $generatedLaneRepair = Group-Value $generatedLaneMatch "repair"
        $generatedLaneVisible = Group-Value $generatedLaneMatch "visible"
        $generatedLanePublic = Group-Value $generatedLaneMatch "public"
    }
    if ($null -ne $deferredDownstream) {
        $deferredDownstreamEnabled =
            Group-Value (Match-First $deferredDownstream.Value "enabled=(?<v>[0-9]+)") "v"
        $deferredDownstreamPending =
            Group-Value (Match-First $deferredDownstream.Value "pending=(?<v>[0-9]+)") "v"
        $deferredLaneMatch = Match-First $deferredDownstream.Value "pendingLane=$lanePattern"
        $deferredDownstreamLaneCache = Group-Value $deferredLaneMatch "cache"
        $deferredDownstreamLanePrefetch = Group-Value $deferredLaneMatch "prefetch"
        $deferredDownstreamLaneRepair = Group-Value $deferredLaneMatch "repair"
        $deferredDownstreamLaneVisible = Group-Value $deferredLaneMatch "visible"
        $deferredDownstreamLanePublic = Group-Value $deferredLaneMatch "public"
        $deferredDownstreamPromoted =
            Group-Value (Match-First $deferredDownstream.Value "promoted=(?<v>[0-9]+)") "v"
        $deferredDownstreamStale =
            Group-Value (Match-First $deferredDownstream.Value "stale=(?<v>[0-9]+)") "v"
        $deferredDownstreamGeneratedTotal =
            Group-Value (Match-First $deferredDownstream.Value "generatedTotal=(?<v>[0-9]+)") "v"
    }
    if ($null -ne $exactAsync) {
        $asyncQueueDepth = Group-Value (Match-First $exactAsync.Value "queueDepth=(?<v>[0-9]+)") "v"
        $asyncResultDepth = Group-Value (Match-First $exactAsync.Value "resultDepth=(?<v>[0-9]+)") "v"
        $asyncPending = Group-Value (Match-First $exactAsync.Value "pending=(?<v>[0-9]+)") "v"
        $asyncEnqueued = Group-Value (Match-First $exactAsync.Value "enqueued=(?<v>[0-9]+)") "v"
        $asyncCompleted = Group-Value (Match-First $exactAsync.Value "completed=(?<v>[0-9]+)") "v"
        $asyncApplied = Group-Value (Match-First $exactAsync.Value "applied=(?<v>[0-9]+)") "v"
        $asyncDiscarded = Group-Value (Match-First $exactAsync.Value "discarded=(?<v>[0-9]+)") "v"
        $asyncSyncFallback = Group-Value (Match-First $exactAsync.Value "syncFallback=(?<v>[0-9]+)") "v"
        $asyncOldestAge = Group-Value (Match-First $exactAsync.Value "oldestAge=(?<v>[0-9]+)") "v"
        $asyncWorkerMs = Group-Value (Match-First $exactAsync.Value "workerMs=(?<v>[-+0-9.]+)") "v"
        $asyncApplyMs = Group-Value (Match-First $exactAsync.Value "applyMs=(?<v>[-+0-9.]+)") "v"
        $asyncMaxEnqueue = Group-Value (Match-First $exactAsync.Value "maxEnqueue=(?<v>[0-9]+)") "v"
        $asyncLowPriorityMaxApply = Group-Value (Match-First $exactAsync.Value "lowPriorityMaxApply=(?<v>[0-9]+)") "v"
        $asyncDeferredLowPriority = Group-Value (Match-First $exactAsync.Value "deferredLowPriority=(?<v>[0-9]+)") "v"
        $asyncEnqueuedLaneMatch = Match-First $exactAsync.Value "enqueuedLane=$lanePattern"
        $asyncAppliedLaneMatch = Match-First $exactAsync.Value "appliedLane=$lanePattern"
        $asyncEnqueuedLaneCache = Group-Value $asyncEnqueuedLaneMatch "cache"
        $asyncEnqueuedLanePrefetch = Group-Value $asyncEnqueuedLaneMatch "prefetch"
        $asyncEnqueuedLaneRepair = Group-Value $asyncEnqueuedLaneMatch "repair"
        $asyncEnqueuedLaneVisible = Group-Value $asyncEnqueuedLaneMatch "visible"
        $asyncEnqueuedLanePublic = Group-Value $asyncEnqueuedLaneMatch "public"
        $asyncAppliedLaneCache = Group-Value $asyncAppliedLaneMatch "cache"
        $asyncAppliedLanePrefetch = Group-Value $asyncAppliedLaneMatch "prefetch"
        $asyncAppliedLaneRepair = Group-Value $asyncAppliedLaneMatch "repair"
        $asyncAppliedLaneVisible = Group-Value $asyncAppliedLaneMatch "visible"
        $asyncAppliedLanePublic = Group-Value $asyncAppliedLaneMatch "public"
    }

    $exactParallelActive = ""
    $exactParallelBricks = ""
    $exactParallelWorkers = ""
    $exactParallelWallMs = ""
    if ($null -ne $exactParallel) {
        $exactParallelActive = Group-Value (Match-First $exactParallel.Value "active=(?<v>[0-9]+)") "v"
        $exactParallelBricks = Group-Value (Match-First $exactParallel.Value "bricks=(?<v>[0-9]+)") "v"
        $exactParallelWorkers = Group-Value (Match-First $exactParallel.Value "workers=(?<v>[0-9]+)") "v"
        $exactParallelWallMs = Group-Value (Match-First $exactParallel.Value "wallMs=(?<v>[-+0-9.]+)") "v"
    }

    $terrainCriticalReuse = ""
    $terrainCriticalRequests = ""
    $terrainCriticalNew = ""
    $terrainCriticalPostNonReady = ""
    $terrainCriticalProtectedGenerated = ""
    $terrainCriticalInlineSurfaceDeferred = ""
    $terrainCriticalInlineSurfaceExtracted = ""
    $terrainCriticalPrePublishSurfaceExtracted = ""
    $terrainCriticalSurfaceGateDefers = ""
    $terrainCriticalParallelGenerationActive = ""
    $terrainCriticalParallelGenerationGenerated = ""
    $terrainCriticalParallelGenerationWorkers = ""
    $terrainCriticalParallelGenerationWallMs = ""
    $requestFastResidentTouch = ""
    $requestFastResidentFallback = ""
    $surfaceReadyPublishPending = ""
    $surfaceReadyPublishScanned = ""
    $surfaceReadyPublishPromoted = ""
    $surfaceReadyPublishDeferred = ""
    $surfaceReadyPublishStale = ""
    $surfaceReadyPublishOldestAge = ""
    $surfaceReadyPublishReadyQueue = ""
    $surfaceReadyPublishGateDefers = ""
    $surfaceReadyPublishPressure = ""
    $surfaceReadyPublishLaneCache = ""
    $surfaceReadyPublishLanePrefetch = ""
    $surfaceReadyPublishLaneRepair = ""
    $surfaceReadyPublishLaneVisible = ""
    $surfaceReadyPublishLanePublic = ""
    if ($null -ne $terrainCritical) {
        $terrainCriticalReuse = Group-Value (Match-First $terrainCritical.Value "reuse=(?<v>[0-9]+)") "v"
        $terrainCriticalRequests = Group-Value (Match-First $terrainCritical.Value "requests=(?<v>[0-9]+)") "v"
        $terrainCriticalNew = Group-Value (Match-First $terrainCritical.Value "new=(?<v>[0-9]+)") "v"
        $terrainCriticalPostNonReady = Group-Value (Match-First $terrainCritical.Value "postMissing=(?<missing>[0-9]+) postRequested=(?<requested>[0-9]+) postGenerating=(?<generating>[0-9]+) postUploadQueued=(?<uploadQueued>[0-9]+) postUploading=(?<uploading>[0-9]+) postOptionalUploading=(?<optional>[0-9]+) postResidentMissingSurface=(?<surface>[0-9]+)") "missing"
        if (-not [string]::IsNullOrWhiteSpace($terrainCriticalPostNonReady)) {
            $postRequested = Group-Value (Match-First $terrainCritical.Value "postMissing=(?<missing>[0-9]+) postRequested=(?<requested>[0-9]+) postGenerating=(?<generating>[0-9]+) postUploadQueued=(?<uploadQueued>[0-9]+) postUploading=(?<uploading>[0-9]+) postOptionalUploading=(?<optional>[0-9]+) postResidentMissingSurface=(?<surface>[0-9]+)") "requested"
            $postGenerating = Group-Value (Match-First $terrainCritical.Value "postMissing=(?<missing>[0-9]+) postRequested=(?<requested>[0-9]+) postGenerating=(?<generating>[0-9]+) postUploadQueued=(?<uploadQueued>[0-9]+) postUploading=(?<uploading>[0-9]+) postOptionalUploading=(?<optional>[0-9]+) postResidentMissingSurface=(?<surface>[0-9]+)") "generating"
            $postUploadQueued = Group-Value (Match-First $terrainCritical.Value "postMissing=(?<missing>[0-9]+) postRequested=(?<requested>[0-9]+) postGenerating=(?<generating>[0-9]+) postUploadQueued=(?<uploadQueued>[0-9]+) postUploading=(?<uploading>[0-9]+) postOptionalUploading=(?<optional>[0-9]+) postResidentMissingSurface=(?<surface>[0-9]+)") "uploadQueued"
            $postUploading = Group-Value (Match-First $terrainCritical.Value "postMissing=(?<missing>[0-9]+) postRequested=(?<requested>[0-9]+) postGenerating=(?<generating>[0-9]+) postUploadQueued=(?<uploadQueued>[0-9]+) postUploading=(?<uploading>[0-9]+) postOptionalUploading=(?<optional>[0-9]+) postResidentMissingSurface=(?<surface>[0-9]+)") "uploading"
            $postSurface = Group-Value (Match-First $terrainCritical.Value "postMissing=(?<missing>[0-9]+) postRequested=(?<requested>[0-9]+) postGenerating=(?<generating>[0-9]+) postUploadQueued=(?<uploadQueued>[0-9]+) postUploading=(?<uploading>[0-9]+) postOptionalUploading=(?<optional>[0-9]+) postResidentMissingSurface=(?<surface>[0-9]+)") "surface"
            $terrainCriticalPostNonReady = [string](
                [int]$terrainCriticalPostNonReady +
                [int]$postRequested +
                [int]$postGenerating +
                [int]$postUploadQueued +
                [int]$postUploading +
                [int]$postSurface)
        }
        $protectedDrain = Match-First $terrainCritical.Value "protectedDrain=(?<requests>[0-9]+)/(?<queued>[0-9]+)/(?<generated>[0-9]+)"
        $terrainCriticalProtectedGenerated = Group-Value $protectedDrain "generated"
        $parallelGeneration = Match-First $terrainCritical.Value "parallelGen=active/generated/workers/wallMs:(?<active>[0-9]+)/(?<generated>[0-9]+)/(?<workers>[0-9]+)/(?<wall>[-+0-9.]+)"
        $terrainCriticalParallelGenerationActive = Group-Value $parallelGeneration "active"
        $terrainCriticalParallelGenerationGenerated = Group-Value $parallelGeneration "generated"
        $terrainCriticalParallelGenerationWorkers = Group-Value $parallelGeneration "workers"
        $terrainCriticalParallelGenerationWallMs = Group-Value $parallelGeneration "wall"
    }
    if ($null -ne $terrainSurfacePublication) {
        $terrainCriticalInlineSurfaceDeferred =
            Group-Value (Match-First $terrainSurfacePublication.Value "inlineDeferred=(?<v>[0-9]+)") "v"
        $terrainCriticalInlineSurfaceExtracted =
            Group-Value (Match-First $terrainSurfacePublication.Value "inlineExtracted=(?<v>[0-9]+)") "v"
        $terrainCriticalPrePublishSurfaceExtracted =
            Group-Value (Match-First $terrainSurfacePublication.Value "prePublishExtracted=(?<v>[0-9]+)") "v"
        $terrainCriticalSurfaceGateDefers =
            Group-Value (Match-First $terrainSurfacePublication.Value "surfaceGateDefers=(?<v>[0-9]+)") "v"
    }
    if ($null -ne $requestDetail) {
        $fastResidentTouch = Match-First $requestDetail.Value "fastResidentTouch=(?<used>[0-9]+)/(?<fallback>[0-9]+)"
        $requestFastResidentTouch = Group-Value $fastResidentTouch "used"
        $requestFastResidentFallback = Group-Value $fastResidentTouch "fallback"
    }
    if ($null -ne $surfaceReadyPublish) {
        $surfaceReadyPublishPressure =
            Group-Value (Match-First $surfaceReadyPublish.Value "pressure=(?<v>[0-9]+)") "v"
        $surfaceReadyPublishPending =
            Group-Value (Match-First $surfaceReadyPublish.Value "pending=(?<v>[0-9]+)") "v"
        $surfaceReadyPublishScanned =
            Group-Value (Match-First $surfaceReadyPublish.Value "scanned=(?<v>[0-9]+)") "v"
        $surfaceReadyPublishPromoted =
            Group-Value (Match-First $surfaceReadyPublish.Value "promoted=(?<v>[0-9]+)") "v"
        $surfaceReadyPublishDeferred =
            Group-Value (Match-First $surfaceReadyPublish.Value "deferred=(?<v>[0-9]+)") "v"
        $surfaceReadyPublishStale =
            Group-Value (Match-First $surfaceReadyPublish.Value "stale=(?<v>[0-9]+)") "v"
        $surfaceReadyPublishOldestAge =
            Group-Value (Match-First $surfaceReadyPublish.Value "oldestAge=(?<v>[0-9]+)") "v"
        $surfaceReadyPublishReadyQueue =
            Group-Value (Match-First $surfaceReadyPublish.Value "readyQueue=(?<v>[0-9]+)") "v"
        $surfaceReadyPublishGateDefers =
            Group-Value (Match-First $surfaceReadyPublish.Value "surfaceGateDefers=(?<v>[0-9]+)") "v"
        $surfaceReadyPublishLaneMatch = Match-First $surfaceReadyPublish.Value "pendingLane=$lanePattern"
        $surfaceReadyPublishLaneCache = Group-Value $surfaceReadyPublishLaneMatch "cache"
        $surfaceReadyPublishLanePrefetch = Group-Value $surfaceReadyPublishLaneMatch "prefetch"
        $surfaceReadyPublishLaneRepair = Group-Value $surfaceReadyPublishLaneMatch "repair"
        $surfaceReadyPublishLaneVisible = Group-Value $surfaceReadyPublishLaneMatch "visible"
        $surfaceReadyPublishLanePublic = Group-Value $surfaceReadyPublishLaneMatch "public"
    }

    $hiddenTrackedPrePublishScanned = ""
    $hiddenTrackedSurfaceScanned = ""
    $hiddenTrackedPruneScanned = ""
    $hiddenTrackedPruneRemoved = ""
    $hiddenTrackedBudgetHits = ""
    $hiddenTrackedCount = ""
    $hiddenRepairCriticalOnly = ""
    $hiddenRepairAccepted = ""
    $hiddenRepairCriticalAccepted = ""
    $hiddenRepairRepairAccepted = ""
    $hiddenRepairActiveSkips = ""
    $hiddenRepairLimitSkips = ""
    $hiddenRepairMax = ""
    $hiddenRepairWaterMax = ""
    $hiddenRepairCriticalCurrent = ""
    $hiddenRepairRepairCurrent = ""
    $hiddenRepairTracked = ""
    $hiddenRepairForcedGenerated = ""
    $hiddenRepairForcedUploaded = ""
    $hiddenRepairForcedSurfaced = ""
    $hiddenRepairPriorityPublished = ""
    if ($null -ne $hiddenTrackedScan) {
        $hiddenTrackedPrePublishScanned =
            Group-Value (Match-First $hiddenTrackedScan.Value "prePublishScanned=(?<v>[0-9]+)") "v"
        $hiddenTrackedSurfaceScanned =
            Group-Value (Match-First $hiddenTrackedScan.Value "surfaceScanned=(?<v>[0-9]+)") "v"
        $hiddenTrackedPruneScanned =
            Group-Value (Match-First $hiddenTrackedScan.Value "pruneScanned=(?<v>[0-9]+)") "v"
        $hiddenTrackedPruneRemoved =
            Group-Value (Match-First $hiddenTrackedScan.Value "pruneRemoved=(?<v>[0-9]+)") "v"
        $hiddenTrackedBudgetHits =
            Group-Value (Match-First $hiddenTrackedScan.Value "budgetHits=(?<v>[0-9]+)") "v"
        $hiddenTrackedCount =
            Group-Value (Match-First $hiddenTrackedScan.Value "tracked=(?<v>[0-9]+)") "v"
    }
    if ($null -ne $hiddenRepairLane) {
        $hiddenRepairCriticalOnly =
            Group-Value (Match-First $hiddenRepairLane.Value "criticalOnly=(?<v>[0-9]+)") "v"
        $hiddenRepairAccepted =
            Group-Value (Match-First $hiddenRepairLane.Value "accepted=(?<v>[0-9]+)") "v"
        $hiddenRepairCriticalAccepted =
            Group-Value (Match-First $hiddenRepairLane.Value "criticalAccepted=(?<v>[0-9]+)") "v"
        $hiddenRepairRepairAccepted =
            Group-Value (Match-First $hiddenRepairLane.Value "repairAccepted=(?<v>[0-9]+)") "v"
        $hiddenRepairActiveSkips =
            Group-Value (Match-First $hiddenRepairLane.Value "repairActiveSkips=(?<v>[0-9]+)") "v"
        $hiddenRepairLimitSkips =
            Group-Value (Match-First $hiddenRepairLane.Value "repairLimitSkips=(?<v>[0-9]+)") "v"
        $hiddenRepairMax =
            Group-Value (Match-First $hiddenRepairLane.Value "repairMax=(?<v>[0-9]+)") "v"
        $hiddenRepairWaterMax =
            Group-Value (Match-First $hiddenRepairLane.Value "repairWaterMax=(?<v>[0-9]+)") "v"
        $hiddenRepairCriticalCurrent =
            Group-Value (Match-First $hiddenRepairLane.Value "criticalCurrent=(?<v>[0-9]+)") "v"
        $hiddenRepairRepairCurrent =
            Group-Value (Match-First $hiddenRepairLane.Value "repairCurrent=(?<v>[0-9]+)") "v"
        $hiddenRepairTracked =
            Group-Value (Match-First $hiddenRepairLane.Value "tracked=(?<v>[0-9]+)") "v"
        $hiddenRepairForcedGenerated =
            Group-Value (Match-First $hiddenRepairLane.Value "forcedGenerated=(?<v>[0-9]+)") "v"
        $hiddenRepairForcedUploaded =
            Group-Value (Match-First $hiddenRepairLane.Value "forcedUploaded=(?<v>[0-9]+)") "v"
        $hiddenRepairForcedSurfaced =
            Group-Value (Match-First $hiddenRepairLane.Value "forcedSurfaced=(?<v>[0-9]+)") "v"
        $hiddenRepairPriorityPublished =
            Group-Value (Match-First $hiddenRepairLane.Value "priorityPublished=(?<v>[0-9]+)") "v"
    }

    $surfaceSortCalls = ""
    $surfaceSortHits = ""
    $surfaceStrictPops = ""
    $surfaceGeneralStrictSkipped = ""
    $surfaceGeneralRemainingMs = ""
    $surfaceParallelActive = ""
    $surfaceParallelBricks = ""
    $surfaceParallelWorkers = ""
    $surfaceParallelWallMs = ""
    if ($null -ne $cpuDetail) {
        $surfaceSort = Match-First $cpuDetail.Value "surface=extractMs/stageMs/extractQueued/extracted/buriedFast/sort/sortHit/strictPops/generalSkip/generalRemainingMs:[-+0-9.]+/[-+0-9.]+/[0-9]+/[0-9]+/[0-9]+/(?<sort>[0-9]+)/(?<hit>[0-9]+)/(?<strict>[0-9]+)/(?<generalSkip>[0-9]+)/(?<generalRemaining>[-+0-9.]+)"
        if ($null -eq $surfaceSort) {
            $surfaceSort = Match-First $cpuDetail.Value "surface=extractMs/stageMs/extractQueued/extracted/buriedFast/sort/sortHit/strictPops:[-+0-9.]+/[-+0-9.]+/[0-9]+/[0-9]+/[0-9]+/(?<sort>[0-9]+)/(?<hit>[0-9]+)/(?<strict>[0-9]+)"
        }
        if ($null -eq $surfaceSort) {
            $surfaceSort = Match-First $cpuDetail.Value "surface=extractMs/stageMs/extractQueued/extracted/buriedFast/sort/sortHit:[-+0-9.]+/[-+0-9.]+/[0-9]+/[0-9]+/[0-9]+/(?<sort>[0-9]+)/(?<hit>[0-9]+)"
        }
        $surfaceSortCalls = Group-Value $surfaceSort "sort"
        $surfaceSortHits = Group-Value $surfaceSort "hit"
        $surfaceStrictPops = Group-Value $surfaceSort "strict"
        $surfaceGeneralStrictSkipped = Group-Value $surfaceSort "generalSkip"
        $surfaceGeneralRemainingMs = Group-Value $surfaceSort "generalRemaining"
        $surfaceParallel = Match-First $cpuDetail.Value "surfaceParallel=active/bricks/workers/wallMs:(?<active>[0-9]+)/(?<bricks>[0-9]+)/(?<workers>[0-9]+)/(?<wall>[-+0-9.]+)"
        $surfaceParallelActive = Group-Value $surfaceParallel "active"
        $surfaceParallelBricks = Group-Value $surfaceParallel "bricks"
        $surfaceParallelWorkers = Group-Value $surfaceParallel "workers"
        $surfaceParallelWallMs = Group-Value $surfaceParallel "wall"
    }

    if ($null -ne $camera) {
        $cameraMidPct = Group-Value (Match-First $camera.Value "midVoxelScreenPct=(?<v>[-+0-9.]+)") "v"
        $cameraMissPct = Group-Value (Match-First $camera.Value "missScreenPct=(?<v>[-+0-9.]+)") "v"
        $cameraUnsafePct = Group-Value (Match-First $camera.Value "unsafeNearMissScreenPct=(?<v>[-+0-9.]+)") "v"
        if (-not [string]::IsNullOrWhiteSpace($cameraMidPct)) { $midPct = $cameraMidPct }
        if (-not [string]::IsNullOrWhiteSpace($cameraMissPct)) { $missPct = $cameraMissPct }
        if (-not [string]::IsNullOrWhiteSpace($cameraUnsafePct)) { $unsafePct = $cameraUnsafePct }
    }

    $farSvoDomain = ""
    $farSvoMaterialUnknown = ""
    if ($null -ne $contract) {
        $farSvoDomain = Group-Value (Match-First $contract.Value "farSvoDomainValid=(?<v>[0-9]+)") "v"
        $farSvoMaterialUnknown = Group-Value (Match-First $contract.Value "farSvoMaterialUnknown=(?<v>[0-9]+)") "v"
    }

    return [pscustomobject]@{
        scenario = $ScenarioName
        frame = $actualFrame
        targetFrame = $TargetFrame
        bodyMs = $bodyMs
        rawMs = $rawMs
        waitMs = Group-Value $waitMatch "wait"
        accountedMs = Group-Value $accountedMatch "accounted"
        untrackedMs = Group-Value $untrackedMatch "untracked"
        frameEndBodyMs = $bodyEnd
        frameEndRawMs = $rawEnd
        cpuUpdateMs = Group-Value $cpuMatch "cpu"
        requestMs = Group-Value $sparseMatch "req"
        genMs = Group-Value $sparseMatch "gen"
        clipMs = Group-Value $sparseMatch "clip"
        pumpMs = $pump
        mainThreadBrickGenMs = $mainThreadBrickGenMs
        surfaceMs = Group-Value $sparseMatch "surface"
        surfaceExtractMs = $surfaceExtract
        surfaceStageMs = $surfaceStage
        uploadMs = Group-Value $sparseMatch "upload"
        trimMs = Group-Value $sparseMatch "trim"
        gpuFrameMs = Group-Value $gpuMatch "frame"
        gpuRayMs = Group-Value $gpuMatch "gpu"
        missingVoxel = $missing
        sampledMissingApprox = $sampled
        unsampledMissingApprox = $unsampled
        sampledPct = $sampledPct
        fallbackValid = $fallbackValid
        fallbackInvalid = $fallbackInvalid
        fallbackUnknown = $fallbackUnknown
        criticalMissing = $criticalMissing
        nonCriticalMissing = $nonCriticalMissing
        parentHeldSamples = $parentHeldSamples
        parentHeldStored = $parentHeldStored
        parentHeldAccepted = $parentHeldAccepted
        parentHeldPending = $parentHeldPending
        workerColumnCacheActive = $workerColumnCacheActive
        workerColumnCacheEntries = $workerColumnCacheEntries
        workerColumnHeightHits = $workerColumnHeightHits
        workerColumnHeightMisses = $workerColumnHeightMisses
        workerColumnReliefHits = $workerColumnReliefHits
        workerColumnReliefMisses = $workerColumnReliefMisses
        parallelPumpActive = $parallelPumpActive
        parallelPumpBricks = $parallelPumpBricks
        parallelPumpWorkers = $parallelPumpWorkers
        parallelPumpWallMs = $parallelPumpWallMs
        generalGenMax = $generalGenMax
        generalGenUncapped = $generalGenUncapped
        generalGenApplied = $generalGenApplied
        generalGenGenerated = $generalGenGenerated
        generalGenElapsedMs = $generalGenElapsed
        generalGenProtected = $generalGenProtected
        generalGenQueuedBefore = $generalGenQueuedBefore
        requestLaneCache = $requestLaneCache
        requestLanePrefetch = $requestLanePrefetch
        requestLaneRepair = $requestLaneRepair
        requestLaneVisible = $requestLaneVisible
        requestLanePublic = $requestLanePublic
        prefetchSpeculativeClassActive = $prefetchSpeculativeClassActive
        prefetchSpeculativeTouches = $prefetchSpeculativeTouches
        generationLaneCache = $generationLaneCache
        generationLanePrefetch = $generationLanePrefetch
        generationLaneRepair = $generationLaneRepair
        generationLaneVisible = $generationLaneVisible
        generationLanePublic = $generationLanePublic
        generatedLaneCache = $generatedLaneCache
        generatedLanePrefetch = $generatedLanePrefetch
        generatedLaneRepair = $generatedLaneRepair
        generatedLaneVisible = $generatedLaneVisible
        generatedLanePublic = $generatedLanePublic
        deferredDownstreamEnabled = $deferredDownstreamEnabled
        deferredDownstreamPending = $deferredDownstreamPending
        deferredDownstreamLaneCache = $deferredDownstreamLaneCache
        deferredDownstreamLanePrefetch = $deferredDownstreamLanePrefetch
        deferredDownstreamLaneRepair = $deferredDownstreamLaneRepair
        deferredDownstreamLaneVisible = $deferredDownstreamLaneVisible
        deferredDownstreamLanePublic = $deferredDownstreamLanePublic
        deferredDownstreamPromoted = $deferredDownstreamPromoted
        deferredDownstreamStale = $deferredDownstreamStale
        deferredDownstreamGeneratedTotal = $deferredDownstreamGeneratedTotal
        uploadLaneCache = $uploadLaneCache
        uploadLanePrefetch = $uploadLanePrefetch
        uploadLaneRepair = $uploadLaneRepair
        uploadLaneVisible = $uploadLaneVisible
        uploadLanePublic = $uploadLanePublic
        surfaceLaneCache = $surfaceLaneCache
        surfaceLanePrefetch = $surfaceLanePrefetch
        surfaceLaneRepair = $surfaceLaneRepair
        surfaceLaneVisible = $surfaceLaneVisible
        surfaceLanePublic = $surfaceLanePublic
        streamingLaneQueuePriorityActive = $streamingLaneQueuePriorityActive
        asyncQueueDepth = $asyncQueueDepth
        asyncResultDepth = $asyncResultDepth
        asyncPending = $asyncPending
        asyncEnqueued = $asyncEnqueued
        asyncCompleted = $asyncCompleted
        asyncApplied = $asyncApplied
        asyncDiscarded = $asyncDiscarded
        asyncSyncFallback = $asyncSyncFallback
        asyncOldestAge = $asyncOldestAge
        asyncWorkerMs = $asyncWorkerMs
        asyncApplyMs = $asyncApplyMs
        asyncMaxEnqueue = $asyncMaxEnqueue
        asyncLowPriorityMaxApply = $asyncLowPriorityMaxApply
        asyncDeferredLowPriority = $asyncDeferredLowPriority
        asyncEnqueuedLaneCache = $asyncEnqueuedLaneCache
        asyncEnqueuedLanePrefetch = $asyncEnqueuedLanePrefetch
        asyncEnqueuedLaneRepair = $asyncEnqueuedLaneRepair
        asyncEnqueuedLaneVisible = $asyncEnqueuedLaneVisible
        asyncEnqueuedLanePublic = $asyncEnqueuedLanePublic
        asyncAppliedLaneCache = $asyncAppliedLaneCache
        asyncAppliedLanePrefetch = $asyncAppliedLanePrefetch
        asyncAppliedLaneRepair = $asyncAppliedLaneRepair
        asyncAppliedLaneVisible = $asyncAppliedLaneVisible
        asyncAppliedLanePublic = $asyncAppliedLanePublic
        exactParallelActive = $exactParallelActive
        exactParallelBricks = $exactParallelBricks
        exactParallelWorkers = $exactParallelWorkers
        exactParallelWallMs = $exactParallelWallMs
        terrainCriticalReuse = $terrainCriticalReuse
        terrainCriticalRequests = $terrainCriticalRequests
        terrainCriticalNew = $terrainCriticalNew
        terrainCriticalPostNonReady = $terrainCriticalPostNonReady
        terrainCriticalProtectedGenerated = $terrainCriticalProtectedGenerated
        terrainCriticalInlineSurfaceDeferred = $terrainCriticalInlineSurfaceDeferred
        terrainCriticalInlineSurfaceExtracted = $terrainCriticalInlineSurfaceExtracted
        terrainCriticalPrePublishSurfaceExtracted = $terrainCriticalPrePublishSurfaceExtracted
        terrainCriticalSurfaceGateDefers = $terrainCriticalSurfaceGateDefers
        terrainCriticalParallelGenerationActive = $terrainCriticalParallelGenerationActive
        terrainCriticalParallelGenerationGenerated = $terrainCriticalParallelGenerationGenerated
        terrainCriticalParallelGenerationWorkers = $terrainCriticalParallelGenerationWorkers
        terrainCriticalParallelGenerationWallMs = $terrainCriticalParallelGenerationWallMs
        requestFastResidentTouch = $requestFastResidentTouch
        requestFastResidentFallback = $requestFastResidentFallback
        surfaceReadyPublishPending = $surfaceReadyPublishPending
        surfaceReadyPublishScanned = $surfaceReadyPublishScanned
        surfaceReadyPublishPromoted = $surfaceReadyPublishPromoted
        surfaceReadyPublishDeferred = $surfaceReadyPublishDeferred
        surfaceReadyPublishStale = $surfaceReadyPublishStale
        surfaceReadyPublishOldestAge = $surfaceReadyPublishOldestAge
        surfaceReadyPublishReadyQueue = $surfaceReadyPublishReadyQueue
        surfaceReadyPublishGateDefers = $surfaceReadyPublishGateDefers
        surfaceReadyPublishPressure = $surfaceReadyPublishPressure
        surfaceReadyPublishLaneCache = $surfaceReadyPublishLaneCache
        surfaceReadyPublishLanePrefetch = $surfaceReadyPublishLanePrefetch
        surfaceReadyPublishLaneRepair = $surfaceReadyPublishLaneRepair
        surfaceReadyPublishLaneVisible = $surfaceReadyPublishLaneVisible
        surfaceReadyPublishLanePublic = $surfaceReadyPublishLanePublic
        hiddenTrackedPrePublishScanned = $hiddenTrackedPrePublishScanned
        hiddenTrackedSurfaceScanned = $hiddenTrackedSurfaceScanned
        hiddenTrackedPruneScanned = $hiddenTrackedPruneScanned
        hiddenTrackedPruneRemoved = $hiddenTrackedPruneRemoved
        hiddenTrackedBudgetHits = $hiddenTrackedBudgetHits
        hiddenTrackedCount = $hiddenTrackedCount
        hiddenRepairCriticalOnly = $hiddenRepairCriticalOnly
        hiddenRepairAccepted = $hiddenRepairAccepted
        hiddenRepairCriticalAccepted = $hiddenRepairCriticalAccepted
        hiddenRepairRepairAccepted = $hiddenRepairRepairAccepted
        hiddenRepairActiveSkips = $hiddenRepairActiveSkips
        hiddenRepairLimitSkips = $hiddenRepairLimitSkips
        hiddenRepairMax = $hiddenRepairMax
        hiddenRepairWaterMax = $hiddenRepairWaterMax
        hiddenRepairCriticalCurrent = $hiddenRepairCriticalCurrent
        hiddenRepairRepairCurrent = $hiddenRepairRepairCurrent
        hiddenRepairTracked = $hiddenRepairTracked
        hiddenRepairForcedGenerated = $hiddenRepairForcedGenerated
        hiddenRepairForcedUploaded = $hiddenRepairForcedUploaded
        hiddenRepairForcedSurfaced = $hiddenRepairForcedSurfaced
        hiddenRepairPriorityPublished = $hiddenRepairPriorityPublished
        surfaceSortCalls = $surfaceSortCalls
        surfaceSortHits = $surfaceSortHits
        surfaceStrictPops = $surfaceStrictPops
        surfaceGeneralStrictSkipped = $surfaceGeneralStrictSkipped
        surfaceGeneralRemainingMs = $surfaceGeneralRemainingMs
        surfaceParallelActive = $surfaceParallelActive
        surfaceParallelBricks = $surfaceParallelBricks
        surfaceParallelWorkers = $surfaceParallelWorkers
        surfaceParallelWallMs = $surfaceParallelWallMs
        farSvoDomainValid = $farSvoDomain
        farSvoMaterialUnknown = $farSvoMaterialUnknown
        visibleCriticalVoxel = $visibleCritical
        midVisiblePriorityProjected = $midVisiblePriorityProjected
        midVisiblePriorityPrioritized = $midVisiblePriorityPrioritized
        midReservationTicketsActive = $midReservationTicketsActive
        midReservationTicketsDue = $midReservationTicketsDue
        midReservationTicketsOverdue = $midReservationTicketsOverdue
        midReservationTicketHits = $midReservationTicketHits
        midReservationTicketMaxAge = $midReservationTicketMaxAge
        midReservationApplyLimit = $midReservationApplyLimit
        midReservationApplied = $midReservationApplied
        midReservationApplyDeferred = $midReservationApplyDeferred
        midVoxelInterestReuseActive = $midVoxelInterestReuseActive
        midVoxelInterestReuseAge = $midVoxelInterestReuseAge
        ownershipMidDebtEnabled = $ownershipMidDebtEnabled
        ownershipMidDebtActive = $ownershipMidDebtActive
        ownershipMidDebtCoverageInDebt = $ownershipMidDebtCoverageInDebt
        ownershipMidDebtUploadBudget = $ownershipMidDebtUploadBudget
        ownershipMidDebtSurfaceBudget = $ownershipMidDebtSurfaceBudget
        midLaneMissingVisible = $midLaneMissingVisible
        midLaneMissingCache = $midLaneMissingCache
        midLaneQueuedVisible = $midLaneQueuedVisible
        midLaneQueuedCache = $midLaneQueuedCache
        midLaneAgeVisible = $midLaneAgeVisible
        midLaneAgeCache = $midLaneAgeCache
        midCacheOnlyDefer = $midCacheOnlyDefer
        coverageVisibleCritical = $coverageVisible
        coverageCache = $coverageCache
        coverage = $coverage
        budgetReason = $budgetReason
        backlogVoxel = $backlogVoxel
        backlogMaxAge = $backlogMaxAge
        midVoxelScreenPct = $midPct
        missScreenPct = $missPct
        unsafeNearMissScreenPct = $unsafePct
        logPath = $LogPath
    }
}

function Get-WindowAverage {
    param([object[]]$Items, [string]$GroupName)
    if ($Items.Count -eq 0) { return "" }
    $measure = $Items | ForEach-Object { [double]$_.Groups[$GroupName].Value } | Measure-Object -Average
    return "{0:F2}" -f $measure.Average
}

function Get-WindowMaximum {
    param([object[]]$Items, [string]$GroupName)
    if ($Items.Count -eq 0) { return "" }
    $measure = $Items | ForEach-Object { [double]$_.Groups[$GroupName].Value } | Measure-Object -Maximum
    return "{0:F2}" -f $measure.Maximum
}

function Parse-FrameEndWindow {
    param([string]$ScenarioName, [int]$TargetFrame, [string]$LogPath)

    $text = Get-Content -LiteralPath $LogPath -Raw
    $matches = [regex]::Matches(
        $text,
        "PERF_FRAME_END frame=(?<frame>[0-9]+).*?gaps=postWait/prePhys/preRender/postRender:(?<postWait>[-+0-9.]+)/(?<prePhys>[-+0-9.]+)/(?<preRender>[-+0-9.]+)/(?<postRender>[-+0-9.]+).*?sparsePost=feedback/cmd/begin/midSnap/plan/upload/publish/midUpload/stats/surfExtract/surfPlan/surfSnap/surfStage/surfEmit:(?<feedback>[-+0-9.]+)/(?<cmd>[-+0-9.]+)/(?<begin>[-+0-9.]+)/(?<midSnap>[-+0-9.]+)/(?<plan>[-+0-9.]+)/(?<upload>[-+0-9.]+)/(?<publish>[-+0-9.]+)/(?<midUpload>[-+0-9.]+)/(?<stats>[-+0-9.]+)/(?<surfExtract>[-+0-9.]+)/(?<surfPlan>[-+0-9.]+)/(?<surfSnap>[-+0-9.]+)/(?<surfStage>[-+0-9.]+)/(?<surfEmit>[-+0-9.]+).*?body=(?<body>[-+0-9.]+).*?gapPrev=(?<gapPrev>[-+0-9.]+).*?rawMs=(?<raw>[-+0-9.]+)")

    $window = New-Object System.Collections.Generic.List[object]
    $startFrame = $TargetFrame - [Math]::Max(0, $WindowBeforeFrames)
    $endFrame = $TargetFrame + [Math]::Max(0, $WindowAfterFrames)
    foreach ($match in $matches) {
        $frame = [int]$match.Groups["frame"].Value
        if ($frame -ge $startFrame -and $frame -le $endFrame) {
            $window.Add($match)
        }
    }

    [pscustomobject]@{
        scenario = $ScenarioName
        targetFrame = $TargetFrame
        windowStartFrame = $startFrame
        windowEndFrame = $endFrame
        frames = $window.Count
        avgRawMs = Get-WindowAverage -Items $window.ToArray() -GroupName "raw"
        maxRawMs = Get-WindowMaximum -Items $window.ToArray() -GroupName "raw"
        avgBodyMs = Get-WindowAverage -Items $window.ToArray() -GroupName "body"
        maxBodyMs = Get-WindowMaximum -Items $window.ToArray() -GroupName "body"
        avgGapPrevMs = Get-WindowAverage -Items $window.ToArray() -GroupName "gapPrev"
        maxGapPrevMs = Get-WindowMaximum -Items $window.ToArray() -GroupName "gapPrev"
        avgPostWaitMs = Get-WindowAverage -Items $window.ToArray() -GroupName "postWait"
        maxPostWaitMs = Get-WindowMaximum -Items $window.ToArray() -GroupName "postWait"
        avgPrePhysMs = Get-WindowAverage -Items $window.ToArray() -GroupName "prePhys"
        maxPrePhysMs = Get-WindowMaximum -Items $window.ToArray() -GroupName "prePhys"
        avgPostRenderMs = Get-WindowAverage -Items $window.ToArray() -GroupName "postRender"
        maxPostRenderMs = Get-WindowMaximum -Items $window.ToArray() -GroupName "postRender"
        avgSparseUploadMs = Get-WindowAverage -Items $window.ToArray() -GroupName "upload"
        maxSparseUploadMs = Get-WindowMaximum -Items $window.ToArray() -GroupName "upload"
        avgSparseStatsMs = Get-WindowAverage -Items $window.ToArray() -GroupName "stats"
        maxSparseStatsMs = Get-WindowMaximum -Items $window.ToArray() -GroupName "stats"
        avgSurfaceExtractMs = Get-WindowAverage -Items $window.ToArray() -GroupName "surfExtract"
        maxSurfaceExtractMs = Get-WindowMaximum -Items $window.ToArray() -GroupName "surfExtract"
        avgSurfaceStageMs = Get-WindowAverage -Items $window.ToArray() -GroupName "surfStage"
        maxSurfaceStageMs = Get-WindowMaximum -Items $window.ToArray() -GroupName "surfStage"
    }
}

function Write-MarkdownTable {
    param([object[]]$Rows, [string]$Path)

    $headers = @(
        "scenario", "frame", "targetFrame", "bodyMs", "rawMs", "waitMs", "accountedMs", "untrackedMs",
        "frameEndBodyMs", "frameEndRawMs",
        "cpuUpdateMs", "requestMs", "genMs", "clipMs", "pumpMs", "mainThreadBrickGenMs", "surfaceMs",
        "surfaceExtractMs", "surfaceStageMs", "uploadMs", "trimMs", "gpuFrameMs", "gpuRayMs", "missingVoxel", "sampledMissingApprox",
        "unsampledMissingApprox", "sampledPct", "fallbackValid", "fallbackInvalid",
        "fallbackUnknown", "criticalMissing", "nonCriticalMissing", "visibleCriticalVoxel", "coverageVisibleCritical",
        "midVisiblePriorityProjected", "midVisiblePriorityPrioritized",
        "midReservationTicketsActive", "midReservationTicketsDue", "midReservationTicketsOverdue",
        "midReservationTicketHits", "midReservationTicketMaxAge",
        "midReservationApplyLimit", "midReservationApplied", "midReservationApplyDeferred",
        "midVoxelInterestReuseActive", "midVoxelInterestReuseAge",
        "ownershipMidDebtEnabled", "ownershipMidDebtActive", "ownershipMidDebtCoverageInDebt",
        "ownershipMidDebtUploadBudget", "ownershipMidDebtSurfaceBudget",
        "midLaneMissingVisible", "midLaneMissingCache", "midLaneQueuedVisible", "midLaneQueuedCache",
        "midLaneAgeVisible", "midLaneAgeCache",
        "midCacheOnlyDefer", "coverageCache", "parentHeldSamples", "parentHeldStored", "parentHeldAccepted", "parentHeldPending",
        "workerColumnCacheActive", "workerColumnCacheEntries", "workerColumnHeightHits",
        "workerColumnHeightMisses", "workerColumnReliefHits", "workerColumnReliefMisses",
        "parallelPumpActive", "parallelPumpBricks", "parallelPumpWorkers", "parallelPumpWallMs",
        "generalGenMax", "generalGenUncapped", "generalGenApplied", "generalGenGenerated", "generalGenElapsedMs",
        "generalGenProtected", "generalGenQueuedBefore",
        "requestLaneCache", "requestLanePrefetch", "requestLaneRepair", "requestLaneVisible", "requestLanePublic",
        "prefetchSpeculativeClassActive", "prefetchSpeculativeTouches",
        "generationLaneCache", "generationLanePrefetch", "generationLaneRepair", "generationLaneVisible", "generationLanePublic",
        "generatedLaneCache", "generatedLanePrefetch", "generatedLaneRepair", "generatedLaneVisible", "generatedLanePublic",
        "deferredDownstreamEnabled", "deferredDownstreamPending",
        "deferredDownstreamLaneCache", "deferredDownstreamLanePrefetch", "deferredDownstreamLaneRepair",
        "deferredDownstreamLaneVisible", "deferredDownstreamLanePublic",
        "deferredDownstreamPromoted", "deferredDownstreamStale", "deferredDownstreamGeneratedTotal",
        "uploadLaneCache", "uploadLanePrefetch", "uploadLaneRepair", "uploadLaneVisible", "uploadLanePublic",
        "surfaceLaneCache", "surfaceLanePrefetch", "surfaceLaneRepair", "surfaceLaneVisible", "surfaceLanePublic",
        "asyncQueueDepth", "asyncResultDepth", "asyncPending", "asyncEnqueued", "asyncCompleted", "asyncApplied",
        "asyncDiscarded", "asyncSyncFallback", "asyncOldestAge", "asyncWorkerMs", "asyncApplyMs", "asyncMaxEnqueue",
        "asyncLowPriorityMaxApply", "asyncDeferredLowPriority",
        "asyncEnqueuedLaneCache", "asyncEnqueuedLanePrefetch", "asyncEnqueuedLaneRepair", "asyncEnqueuedLaneVisible", "asyncEnqueuedLanePublic",
        "asyncAppliedLaneCache", "asyncAppliedLanePrefetch", "asyncAppliedLaneRepair", "asyncAppliedLaneVisible", "asyncAppliedLanePublic",
        "exactParallelActive", "exactParallelBricks", "exactParallelWorkers", "exactParallelWallMs",
        "terrainCriticalReuse", "terrainCriticalRequests", "terrainCriticalNew", "terrainCriticalPostNonReady",
        "terrainCriticalProtectedGenerated",
        "terrainCriticalInlineSurfaceDeferred", "terrainCriticalInlineSurfaceExtracted",
        "terrainCriticalPrePublishSurfaceExtracted", "terrainCriticalSurfaceGateDefers",
        "terrainCriticalParallelGenerationActive", "terrainCriticalParallelGenerationGenerated",
        "terrainCriticalParallelGenerationWorkers", "terrainCriticalParallelGenerationWallMs",
        "requestFastResidentTouch", "requestFastResidentFallback",
        "surfaceReadyPublishPending", "surfaceReadyPublishScanned", "surfaceReadyPublishPromoted",
        "surfaceReadyPublishDeferred", "surfaceReadyPublishStale", "surfaceReadyPublishOldestAge",
        "surfaceReadyPublishReadyQueue", "surfaceReadyPublishGateDefers", "surfaceReadyPublishPressure",
        "surfaceReadyPublishLaneCache", "surfaceReadyPublishLanePrefetch", "surfaceReadyPublishLaneRepair",
        "surfaceReadyPublishLaneVisible", "surfaceReadyPublishLanePublic",
        "hiddenTrackedPrePublishScanned", "hiddenTrackedSurfaceScanned", "hiddenTrackedPruneScanned",
        "hiddenTrackedPruneRemoved", "hiddenTrackedBudgetHits", "hiddenTrackedCount",
        "hiddenRepairCriticalOnly", "hiddenRepairAccepted", "hiddenRepairCriticalAccepted",
        "hiddenRepairRepairAccepted", "hiddenRepairActiveSkips", "hiddenRepairLimitSkips",
        "hiddenRepairMax", "hiddenRepairWaterMax", "hiddenRepairCriticalCurrent",
        "hiddenRepairRepairCurrent", "hiddenRepairTracked", "hiddenRepairForcedGenerated",
        "hiddenRepairForcedUploaded", "hiddenRepairForcedSurfaced", "hiddenRepairPriorityPublished",
        "surfaceSortCalls", "surfaceSortHits", "surfaceStrictPops",
        "surfaceGeneralStrictSkipped", "surfaceGeneralRemainingMs",
        "surfaceParallelActive", "surfaceParallelBricks", "surfaceParallelWorkers", "surfaceParallelWallMs",
        "coverage", "budgetReason", "backlogVoxel", "backlogMaxAge",
        "missScreenPct", "unsafeNearMissScreenPct"
    )

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("| " + ($headers -join " | ") + " |")
    $lines.Add("| " + (($headers | ForEach-Object { "---" }) -join " | ") + " |")
    foreach ($row in $Rows) {
        $values = foreach ($h in $headers) {
            $value = [string]$row.$h
            if ([string]::IsNullOrWhiteSpace($value)) { $value = "n/a" }
            $value -replace "\|", "/"
        }
        $lines.Add("| " + ($values -join " | ") + " |")
    }
    Set-Content -LiteralPath $Path -Value $lines -Encoding utf8
}

function Write-WindowMarkdownTable {
    param([object[]]$Rows, [string]$Path)

    $headers = @(
        "scenario", "targetFrame", "windowStartFrame", "windowEndFrame", "frames",
        "avgRawMs", "maxRawMs", "avgBodyMs", "maxBodyMs", "avgGapPrevMs", "maxGapPrevMs",
        "avgPostWaitMs", "maxPostWaitMs", "avgPrePhysMs", "maxPrePhysMs",
        "avgPostRenderMs", "maxPostRenderMs", "avgSparseUploadMs", "maxSparseUploadMs",
        "avgSparseStatsMs", "maxSparseStatsMs",
        "avgSurfaceExtractMs", "maxSurfaceExtractMs", "avgSurfaceStageMs", "maxSurfaceStageMs"
    )

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("| " + ($headers -join " | ") + " |")
    $lines.Add("| " + (($headers | ForEach-Object { "---" }) -join " | ") + " |")
    foreach ($row in $Rows) {
        $values = foreach ($h in $headers) {
            $value = [string]$row.$h
            if ([string]::IsNullOrWhiteSpace($value)) { $value = "n/a" }
            $value -replace "\|", "/"
        }
        $lines.Add("| " + ($values -join " | ") + " |")
    }
    Set-Content -LiteralPath $Path -Value $lines -Encoding utf8
}

function Write-RunManifest {
    param([string]$Path)

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("commandLine=$script:SmokeInvocationLine")
    $lines.Add("script=$script:SmokeScriptPath")
    $lines.Add("timestamp=$((Get-Date).ToString('o'))")
    $lines.Add("workingDirectory=$root")
    $lines.Add("parameters:")
    foreach ($key in ($script:SmokeBoundParameters.Keys | Sort-Object)) {
        $lines.Add("  $key=$($script:SmokeBoundParameters[$key])")
    }
    $lines.Add("effectiveParameters:")
    $effective = [ordered]@{
        StackPreset = $StackPreset
        Scenario = $Scenario
        HighAltFrame = $HighAltFrame
        ExtraFrames = $ExtraFrames
        StartupPublicRenderMaxFrame = $StartupPublicRenderMaxFrame
        StartupPublicRenderMidVoxelVisibleProof = [bool]$StartupPublicRenderMidVoxelVisibleProof
        StartupPublicRenderMidVoxelMovingWindowProof = [bool]$StartupPublicRenderMidVoxelMovingWindowProof
        StartupPublicRenderMidVoxelMovingWindowAsyncReservation = [bool]$StartupPublicRenderMidVoxelMovingWindowAsyncReservation
        MidClipmapFootprintInterestSignature = [bool]$MidClipmapFootprintInterestSignature
        MidClipmapVoxelInterestSignatureReuse = [bool]$MidClipmapVoxelInterestSignatureReuse
        MidClipmapVoxelInterestSignatureReuseMaxAge = $MidClipmapVoxelInterestSignatureReuseMaxAge
        MidClipmapVisibleCriticalPrepump = [bool]$MidClipmapVisibleCriticalPrepump
        MidClipmapVisiblePriorityPump = [bool]$MidClipmapVisiblePriorityPump
        MidClipmapCacheOnlyDefer = [bool]$MidClipmapCacheOnlyDefer
        MidClipmapStressCameraVelocity = [bool]$MidClipmapStressCameraVelocity
        MidClipmapPredictedVisibleAdmission = [bool]$MidClipmapPredictedVisibleAdmission
        MidClipmapPredictedVisibleAdmissionMaxCoords = $MidClipmapPredictedVisibleAdmissionMaxCoords
        MidClipmapAsyncVisibleCriticalGeneration = [bool]$MidClipmapAsyncVisibleCriticalGeneration
        MidClipmapAsyncVisibleCriticalMaxEnqueue = $MidClipmapAsyncVisibleCriticalMaxEnqueue
        MidClipmapAsyncVisibleCriticalMaxApply = $MidClipmapAsyncVisibleCriticalMaxApply
        MidClipmapAsyncVisibleReservationMaxApply = $MidClipmapAsyncVisibleReservationMaxApply
        MidClipmapSplitVisiblePump = [bool]$MidClipmapSplitVisiblePump
        MidClipmapSplitVisiblePumpPostOpenOnly = [bool]$MidClipmapSplitVisiblePumpPostOpenOnly
        MidClipmapSplitVisiblePumpBudget = $MidClipmapSplitVisiblePumpBudget
        MidClipmapSplitCachePumpBudget = $MidClipmapSplitCachePumpBudget
        ParallelMidVoxelPump = [bool]$ParallelMidVoxelPump
        ParallelMidVoxelPumpPersistentWorkers = [bool]$ParallelMidVoxelPumpPersistentWorkers
        ParallelMidVoxelPumpMaxWorkers = $ParallelMidVoxelPumpMaxWorkers
        StreamingLaneDiagnostics = [bool]$StreamingLaneDiagnostics
        StreamingLaneQueuePriority = [bool]$StreamingLaneQueuePriority
        StreamingTicketScheduler = [bool]$StreamingTicketScheduler
        StreamingTicketGenerationOwnershipQueues = [bool]$StreamingTicketGenerationOwnershipQueues
        StreamingTicketGenerationOwnershipReservations = [bool]$StreamingTicketGenerationOwnershipReservations
        StreamingTicketGenerationOwnershipReservationMax = $StreamingTicketGenerationOwnershipReservationMax
        RequestExplicitSourceLanes = [bool]$RequestExplicitSourceLanes
        OwnershipStageBudgets = [bool]$OwnershipStageBudgets
        OwnershipStageUploadBudget = $OwnershipStageUploadBudget
        OwnershipStageSurfaceBudget = $OwnershipStageSurfaceBudget
        OwnershipStagePublishBudget = $OwnershipStagePublishBudget
        OwnershipStageMidVisibleDebtThrottle = [bool]$OwnershipStageMidVisibleDebtThrottle
        OwnershipStageMidVisibleDebtMinCoverage = $OwnershipStageMidVisibleDebtMinCoverage
        OwnershipStageMidVisibleDebtMaxMissingVisible = $OwnershipStageMidVisibleDebtMaxMissingVisible
        OwnershipStageMidVisibleDebtUploadFloor = $OwnershipStageMidVisibleDebtUploadFloor
        OwnershipStageMidVisibleDebtSurfaceFloor = $OwnershipStageMidVisibleDebtSurfaceFloor
        HiddenExactPostOpenRepairLane = [bool]$HiddenExactPostOpenRepairLane
    }
    foreach ($key in $effective.Keys) {
        $lines.Add("  $key=$($effective[$key])")
    }
    Set-Content -LiteralPath $Path -Value $lines -Encoding utf8
}

function Invoke-NonCaptureScenario {
    param([string]$Name, [int]$TargetFrame)

    $scenarioDir = Join-Path $outRoot $Name
    New-Item -ItemType Directory -Path $scenarioDir -Force | Out-Null
    $logPath = Join-Path $scenarioDir "venpod_runtime.log"
    Remove-Item -LiteralPath $logPath -Force -ErrorAction SilentlyContinue

    Clear-ScenarioEnv
    $exitAfter = $TargetFrame + $ExtraFrames
    Set-CommonCandidateEnv -ExitAfterFrames $exitAfter -LogPath $logPath
    if ($Name -eq "walk_realtime") {
        Set-WalkScenarioEnv
    } elseif ($Name -eq "highalt") {
        Set-HighAltScenarioEnv
    }

    Write-Host "Running noncapture $Name to frame $TargetFrame (exit $exitAfter)..."
    Push-Location $root
    try {
        & $runScript -Config $Config -ForceSync
        if ($LASTEXITCODE -ne 0) {
            throw "run.ps1 failed for $Name with exit code $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }

    if (-not (Test-Path -LiteralPath $logPath)) {
        $fallbackLog = Join-Path $root "build\bin\venpod_runtime.log"
        if (Test-Path -LiteralPath $fallbackLog) {
            Copy-Item -LiteralPath $fallbackLog -Destination $logPath -Force
        } else {
            throw "No runtime log was produced for $Name at $logPath"
        }
    }

    Parse-ScenarioLog -ScenarioName $Name -TargetFrame $TargetFrame -LogPath $logPath
}

try {
    New-Item -ItemType Directory -Path $outRoot -Force | Out-Null
    Write-RunManifest -Path (Join-Path $outRoot "run_manifest.txt")

    if (-not $ParseOnly) {
        Require-NoExistingProcess
    }

    if (-not $NoBuild -and -not $ParseOnly) {
        Write-Host "Building $Config before noncapture smoke..."
        Push-Location $root
        try {
            & $buildScript -Config $Config
            if ($LASTEXITCODE -ne 0) {
                throw "build.ps1 failed with exit code $LASTEXITCODE"
            }
        } finally {
            Pop-Location
        }
    }

    $scenarioSpecs = @()
    if ($Scenario -eq "all" -or $Scenario -eq "fixed") {
        $scenarioSpecs += [pscustomobject]@{ Name = "fixed"; Frame = $FixedFrame }
    }
    if ($Scenario -eq "all" -or $Scenario -eq "walk") {
        $scenarioSpecs += [pscustomobject]@{ Name = "walk_realtime"; Frame = $WalkFrame }
    }
    if ($Scenario -eq "all" -or $Scenario -eq "highalt") {
        $scenarioSpecs += [pscustomobject]@{ Name = "highalt"; Frame = $HighAltFrame }
    }

    $rows = New-Object System.Collections.Generic.List[object]
    foreach ($spec in $scenarioSpecs) {
        if ($ParseOnly) {
            $logPath = Join-Path (Join-Path $outRoot $spec.Name) "venpod_runtime.log"
            if (-not (Test-Path -LiteralPath $logPath)) {
                throw "ParseOnly requested, but missing log for $($spec.Name): $logPath"
            }
            $rows.Add((Parse-ScenarioLog -ScenarioName $spec.Name -TargetFrame $spec.Frame -LogPath $logPath))
        } else {
            $rows.Add((Invoke-NonCaptureScenario -Name $spec.Name -TargetFrame $spec.Frame))
        }
    }

    $csvPath = Join-Path $outRoot "summary.csv"
    $mdPath = Join-Path $outRoot "table.md"
    $rows | Export-Csv -NoTypeInformation -Path $csvPath
    Write-MarkdownTable -Rows $rows.ToArray() -Path $mdPath
    Write-Host "Wrote $csvPath"
    Write-Host "Wrote $mdPath"

    $windowRows = New-Object System.Collections.Generic.List[object]
    foreach ($spec in $scenarioSpecs) {
        $logPath = Join-Path (Join-Path $outRoot $spec.Name) "venpod_runtime.log"
        if (Test-Path -LiteralPath $logPath) {
            $windowRows.Add((Parse-FrameEndWindow -ScenarioName $spec.Name -TargetFrame $spec.Frame -LogPath $logPath))
        }
    }
    if ($windowRows.Count -gt 0) {
        $windowCsvPath = Join-Path $outRoot "window_summary.csv"
        $windowMdPath = Join-Path $outRoot "window_table.md"
        $windowRows | Export-Csv -NoTypeInformation -Path $windowCsvPath
        Write-WindowMarkdownTable -Rows $windowRows.ToArray() -Path $windowMdPath
        Write-Host "Wrote $windowCsvPath"
        Write-Host "Wrote $windowMdPath"
    }
} finally {
    Restore-ManagedEnv
}
