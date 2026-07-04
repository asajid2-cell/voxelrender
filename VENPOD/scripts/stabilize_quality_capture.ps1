param(
    [ValidateSet("idle", "walk", "yaw", "edit")]
    [string]$Scenario = "idle",
    [string]$OutputDir = "build\captures\stabilize_quality",
    [int]$Frames = 900,
    [string]$Label = "",
    [int]$SummaryLogInterval = 30,
    [double]$RenderScale = 0,
    [double]$ExperimentalBackgroundPassScale = 0,
    [int]$UploadBudget = -1,
    [int]$MidPumpBudget = -1,
    [int]$CleanCatchupBudget = -1,
    [switch]$DisableMidFeedback,
    [switch]$RestTrimOld,
    [switch]$ViewFollowTrimOff,
    [switch]$ViewFollowTrimWhileStationary,
    [int]$FrameLatencyWaitable = -1,
    [int]$OwnershipInterval = -1,
    [int]$CaptureStartFrame = -1,
    [int]$CaptureIntervalFrames = 30,
    [int]$CaptureCount = 0,
    [switch]$AbsoluteWalkFrame,
    [switch]$ExperimentalBackgroundPassNoSurfaceFill,
    [switch]$ExperimentalBackgroundPassForegroundMask,
    [int]$ExperimentalPrePublishGeneralSurfaceBudget = -1,
    [int]$ExperimentalPrePublishEditGeneralSurfaceBudget = -1,
    [int]$ExperimentalPrePublishPostEditGeneralSurfaceBudget = -1,
    [int]$ExperimentalPrePublishPostEditGeneralSpillFrames = -1,
    [double]$ExperimentalPrePublishPostEditGeneralSpillPressureMs = -1,
    [int]$ExperimentalHiddenExactPostOpenProbeMaxMsTenths = -1,
    [int]$ExperimentalHiddenExactPostOpenGenerationBudget = -1,
    [int]$ExperimentalHiddenExactPostOpenSurfaceBudget = -1,
    [int]$ExperimentalPrePublishHiddenTrackedSurfaceBudget = -1,
    [switch]$ExperimentalSurfaceAsyncPerCoordEditGate,
    [switch]$ExperimentalSparseRequestExplicitSourceLanes,
    [switch]$ExperimentalHiddenExactPostOpenRepairLane,
    [switch]$ExperimentalHiddenExactDeferProactive,
    [int]$ExperimentalHiddenExactPostOpenRepairLaneMaxRequests = -1,
    [switch]$ClipInterestProfile,
    [switch]$ClipInterestDetail,
    [int]$ExperimentalVoxelInterestRebuildRingsPerFrame = -1,
    [double]$ExperimentalMidClipmapPumpHardBudgetMs = -1,
    [int]$ExperimentalSurfaceRouteGeneralAsync = -1,
    [int]$ExperimentalSurfaceRouteAsyncBacklogLimit = -1,
    [int]$ExperimentalSurfaceRoutePublishPendingLimit = -1,
    [int]$ExperimentalSurfaceRoutePublishAgeLimit = -1,
    [int]$ExperimentalSurfaceRoutePagePublishLimit = -1,
    # Edit scenario: hold the ERASE button instead of paint (real interactive erase
    # path: IsErasing -> carve-front ray-follow). Diagnosis harness for erase bugs.
    [switch]$BrushSmokeErase,
    # Edit scenario: use the USER-PATH smoke (rebrun -SparseBrushSmokeUserPath): the
    # exact interactive config -- CPU brush apply, NO GPU feedback pipeline. The
    # default moving smoke forces the gpu-authoritative strict-resident pipeline,
    # which is NOT what interactive play runs.
    [switch]$BrushSmokeUserPath,
    # Edit scenario: pin a single brush smoke case (0=paint,1=replace,2=erase,3=replace)
    # instead of the 45-frame case cycle, for per-mode diagnosis. -1 = cycle (default).
    [int]$BrushSmokeCase = -1,
    # Edit scenario: REAL-AIM smoke -- do NOT override brush placement; let the actual
    # raycast hit drive where the edit lands (the true interactive placement path,
    # which the scripted target override hides). Logs SPARSE_BRUSH_REALAIM lines.
    [switch]$BrushSmokeRealAim,
    # Edit scenario: brush radius in tenths (e.g. 120 = 12u) — big-brush "fast
    # editing" stress. -1 = engine default.
    [int]$BrushSmokeRadiusTenths = -1,
    # Hidden-exact probe cadence A/B (VENPOD_SPARSE_HIDDEN_EXACT_MISS_PROBE_INTERVAL,
    # engine default 1 = every frame). -1 = engine default.
    [int]$HiddenExactProbeInterval = -1,
    # Brush apply cadence A/B (VENPOD_SPARSE_BRUSH_APPLY_CADENCE, engine default 1
    # = off; bimodal at 2, see Loop 103). -1 = default.
    [int]$BrushApplyCadence = -1,
    # Large-radius stamp spacing percent A/B (VENPOD_SPARSE_BRUSH_STAMP_SPACING_
    # LARGE_PCT, engine default 65; old behavior = 45). -1 = default.
    [int]$BrushStampSpacingLargePct = -1,
    # Edit scenario: extend the stroke window end frame (default engine 360) so the
    # smoke edits continuously through long runs ("fast editing" perf repro).
    [int]$BrushSmokeEndFrame = -1,
    # Walk scenario: override the bot speed (default 25). High values approximate
    # fast interactive exploration (streaming/trim race, residency growth).
    [int]$WalkSpeed = -1,
    # Edit scenario: override the yaw rate (default 10 deg/s). 0 = static camera
    # for pixel-curve measurements. -1 = scenario default.
    [int]$WalkYawDegPerSec = -1,
    # Edit scenario: EDIT_TELEM log interval (default 60). 1 = per-frame telemetry
    # for pixel-curve stage alignment (Loop 108 staircase attribution).
    [int]$EditTelemetryInterval = -1,
    # Height-serial oscillator trace (Loop 110): logs which site/tile bumps the
    # height dirty serial (HEIGHT_SERIAL_TRACE lines).
    [switch]$HeightSerialTrace,
    # Background temporal accumulation lane (TAA increments, VENPOD_BG_TEMPORAL).
    [switch]$BgTemporal,
    # Walk scenario: FLY mode -- the scripted camera holds its elevated altitude
    # (vista view). Reproduces high-altitude exploration residency growth.
    [switch]$WalkFly,
    # Walk scenario: lift the spawn into the air by N units (vista altitude).
    [int]$WalkEyeOffsetY = 0,
    # Camera pitch override in degrees (e.g. -70 for near-top-down aerials).
    [int]$WalkPitchDeg = -1000,
    # Sparse debug visualization mode (58 = render-ownership map).
    [int]$SparseDebugMode = -1
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location $projectRoot
[Environment]::CurrentDirectory = $projectRoot

if (-not $Label) {
    $Label = $Scenario
}

$requestedFrames = $Frames
if ($Scenario -eq "edit" -and $Frames -lt 600) {
    # Brush paint smoke defaults to frames 180..359, then waits 180 settle frames.
    # Shorter edit captures exit before the smoke can ever mark itself passed.
    $Frames = 600
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$stdoutPath = Join-Path $OutputDir "$Label.stdout.txt"
$statusPath = Join-Path $OutputDir "$Label.status.txt"
$runtimeLog = Join-Path $projectRoot "build\bin\venpod_runtime.log"
$savedLog = Join-Path $OutputDir "$Label.log"

$managedEnv = @(
    "VENPOD_VSYNC",
    "VENPOD_PERF_FRAME_END_LOG_INTERVAL",
    "VENPOD_PERF_SUMMARY_LOG_INTERVAL",
    "VENPOD_FRAME_LATENCY_WAITABLE",
    "VENPOD_SPARSE_RENDER_OWNERSHIP_INTERVAL",
    "VENPOD_CAPTURE_DIR",
    "VENPOD_CAPTURE_START_FRAME",
    "VENPOD_CAPTURE_INTERVAL_FRAMES",
    "VENPOD_CAPTURE_COUNT",
    "VENPOD_CAPTURE_HIDE_UI",
    "VENPOD_EDIT_TELEMETRY",
    "VENPOD_SPARSE_BRUSH_SMOKE_ERASE",
    "VENPOD_SPARSE_BRUSH_SMOKE_CASE",
    "VENPOD_SPARSE_BRUSH_SMOKE_REAL_AIM",
    "VENPOD_SPARSE_WALK_TEST_FLY",
    "VENPOD_SPARSE_WALK_TEST_EYE_OFFSET_Y",
    "VENPOD_SPARSE_WALK_TEST",
    "VENPOD_SPARSE_WALK_TEST_SPEED",
    "VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC",
    "VENPOD_SPARSE_WALK_TEST_PITCH_DEG",
    "VENPOD_SPARSE_UPLOAD_BUDGET",
    "VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP_BUDGET",
    "VENPOD_SPARSE_MID_CLIPMAP_SPLIT_CACHE_PUMP_BUDGET",
    "VENPOD_SPARSE_STARTUP_MID_VOXEL_BUDGET",
    "VENPOD_SPARSE_MID_VOXEL_COVERAGE_CATCHUP_BUDGET",
    "VENPOD_SPARSE_MID_VOXEL_CLEAN_CATCHUP_BUDGET",
    "VENPOD_SPARSE_MID_VOXEL_CLEAN_BACKLOG_BUDGET",
    "VENPOD_SPARSE_MID_CLIPMAP_PARENT_HELD_FEEDBACK",
    "VENPOD_SPARSE_TRIM_STATIONARY_ON_MISS_FEEDBACK",
    "VENPOD_SPARSE_STATIONARY_MIN_FREE_PAGES",
    "VENPOD_SPARSE_TRIM_STATIONARY_VISIBLE",
    "VENPOD_SPARSE_VIEW_FOLLOW_TRIM",
    "VENPOD_SPARSE_VIEW_FOLLOW_TRIM_WHILE_STATIONARY",
    "VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS",
    "VENPOD_SPARSE_WALK_TEST_ABSOLUTE_YAW_FRAME",
    "VENPOD_SPARSE_REQUEST_EXPLICIT_SOURCE_LANES",
    "VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_REPAIR_LANE",
    "VENPOD_SPARSE_HIDDEN_EXACT_DEFER_PROACTIVE",
    "VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_REPAIR_LANE_MAX_REQUESTS",
    "VENPOD_CLIPINTEREST_PROFILE",
    "VENPOD_SPARSE_MID_CLIPMAP_INTEREST_DETAIL",
    "VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_REBUILD_RINGS_PER_FRAME",
    "VENPOD_SPARSE_MID_CLIPMAP_PUMP_HARD_BUDGET_MS"
)
$savedEnv = @{}
foreach ($name in $managedEnv) {
    $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}

function Set-ProcessEnv([string]$Name, [string]$Value) {
    [Environment]::SetEnvironmentVariable($Name, $Value, "Process")
}

function Clear-ProcessEnv([string]$Name) {
    [Environment]::SetEnvironmentVariable($Name, $null, "Process")
}

"START scenario=$Scenario label=$Label frames=$Frames requestedFrames=$requestedFrames time=$(Get-Date -Format o)" | Set-Content -LiteralPath $statusPath
$exitCode = 1

try {
    Get-Process VENPOD -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

    Set-ProcessEnv "VENPOD_VSYNC" "0"
    Set-ProcessEnv "VENPOD_PERF_FRAME_END_LOG_INTERVAL" "1"
    Set-ProcessEnv "VENPOD_PERF_SUMMARY_LOG_INTERVAL" "$SummaryLogInterval"
    if ($FrameLatencyWaitable -ge 0) {
        Set-ProcessEnv "VENPOD_FRAME_LATENCY_WAITABLE" "$FrameLatencyWaitable"
    }
    if ($ExperimentalSparseRequestExplicitSourceLanes) {
        Set-ProcessEnv "VENPOD_SPARSE_REQUEST_EXPLICIT_SOURCE_LANES" "1"
    }
    if ($ExperimentalHiddenExactPostOpenRepairLane) {
        Set-ProcessEnv "VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_REPAIR_LANE" "1"
    }
    if ($ExperimentalHiddenExactDeferProactive) {
        Set-ProcessEnv "VENPOD_SPARSE_HIDDEN_EXACT_DEFER_PROACTIVE" "1"
    }
    if ($ExperimentalHiddenExactPostOpenRepairLaneMaxRequests -ge 0) {
        Set-ProcessEnv `
            "VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_REPAIR_LANE_MAX_REQUESTS" `
            "$ExperimentalHiddenExactPostOpenRepairLaneMaxRequests"
    }
    if ($ClipInterestProfile) {
        Set-ProcessEnv "VENPOD_CLIPINTEREST_PROFILE" "1"
    }
    if ($ClipInterestDetail) {
        Set-ProcessEnv "VENPOD_SPARSE_MID_CLIPMAP_INTEREST_DETAIL" "1"
    }
    if ($ExperimentalVoxelInterestRebuildRingsPerFrame -ge 0) {
        Set-ProcessEnv `
            "VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_REBUILD_RINGS_PER_FRAME" `
            "$ExperimentalVoxelInterestRebuildRingsPerFrame"
    }
    if ($ExperimentalMidClipmapPumpHardBudgetMs -ge 0) {
        Set-ProcessEnv `
            "VENPOD_SPARSE_MID_CLIPMAP_PUMP_HARD_BUDGET_MS" `
            ("{0:0.###}" -f $ExperimentalMidClipmapPumpHardBudgetMs)
    }
    if ($CaptureCount -gt 0) {
        if ($CaptureStartFrame -lt 0) {
            $CaptureStartFrame = 120
        }
        $captureDir = Join-Path $OutputDir "$Label.frames"
        New-Item -ItemType Directory -Force -Path $captureDir | Out-Null
        $captureDir = (Resolve-Path -LiteralPath $captureDir).ProviderPath
        Set-ProcessEnv "VENPOD_CAPTURE_DIR" $captureDir
        Set-ProcessEnv "VENPOD_CAPTURE_START_FRAME" "$CaptureStartFrame"
        Set-ProcessEnv "VENPOD_CAPTURE_INTERVAL_FRAMES" "$CaptureIntervalFrames"
        Set-ProcessEnv "VENPOD_CAPTURE_COUNT" "$CaptureCount"
        Set-ProcessEnv "VENPOD_CAPTURE_HIDE_UI" "1"
        "CAPTURE_DIR $captureDir" | Add-Content -LiteralPath $statusPath
    }

    $runParams = @{
        PerfMode = "quality"
        NoBuild = $true
        ExitAfterFrames = $Frames
    }
    if ($BgTemporal) {
        Set-ProcessEnv "VENPOD_BG_TEMPORAL" "1"
    }
    if ($RenderScale -gt 0) {
        $runParams["RenderScale"] = $RenderScale
    }
    if ($UploadBudget -ge 0) {
        Set-ProcessEnv "VENPOD_SPARSE_UPLOAD_BUDGET" "$UploadBudget"
    }
    if ($MidPumpBudget -ge 0) {
        # Probe the mid-voxel GENERATION throttle: visible + cache split pump + startup catchup.
        Set-ProcessEnv "VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP_BUDGET" "$MidPumpBudget"
        Set-ProcessEnv "VENPOD_SPARSE_MID_CLIPMAP_SPLIT_CACHE_PUMP_BUDGET" "$MidPumpBudget"
        Set-ProcessEnv "VENPOD_SPARSE_STARTUP_MID_VOXEL_BUDGET" "$MidPumpBudget"
    }
    if ($RestTrimOld) {
        # A/B: restore ALL pre-fix rest-time trim behavior (miss-feedback pressure
        # trims when stationary + 256-page free reserve) to compare vs the fix.
        Set-ProcessEnv "VENPOD_SPARSE_TRIM_STATIONARY_ON_MISS_FEEDBACK" "1"
        Set-ProcessEnv "VENPOD_SPARSE_STATIONARY_MIN_FREE_PAGES" "256"
        Set-ProcessEnv "VENPOD_SPARSE_TRIM_STATIONARY_VISIBLE" "1"
    }
    if ($ViewFollowTrimOff) {
        # CAUSAL PROBE (siege3): disable the recency-only view-follow stale trim
        # (TrimStaleResidentBricks, default-on). Hypothesis: at rest it sheds resident
        # terrain bricks not touched within the 120-frame window despite a 93%-free pool
        # -> re-request -> regen -> surface re-extract -> the visible regeneration.
        # If cpuSerial stops climbing + bursts drop with this off, the cause is proven.
        Set-ProcessEnv "VENPOD_SPARSE_VIEW_FOLLOW_TRIM" "0"
    }
    if ($ViewFollowTrimWhileStationary) {
        # DISCONFIRMATION A/B (siege3): restore the OLD behavior on the FIXED binary --
        # keep running the stale trim even while stationary. Proves the stationary-gate fix
        # is what removed the rest-time regeneration (bursts should RETURN) and that we are
        # running the new binary (this env only exists in fixed code).
        Set-ProcessEnv "VENPOD_SPARSE_VIEW_FOLLOW_TRIM_WHILE_STATIONARY" "1"
    }
    if ($DisableMidFeedback) {
        # Render-vs-fill discriminator: kill the mid-voxel LOD parent-held feedback loop + miss
        # feedback so the ring SELECTION can't be re-driven per-frame. If churn stops with residency
        # static, the feedback loop (not the fill) is the churn driver.
        Set-ProcessEnv "VENPOD_SPARSE_MID_CLIPMAP_PARENT_HELD_FEEDBACK" "0"
    }
    if ($CleanCatchupBudget -ge 0) {
        # THE fill-speed throttle (codex+claude converged): the whole mid-voxel catchup budget chain
        # caps at coverageCatchupBudget(48), then clean catchup/backlog clamp it to 24/32
        # (main_launcher:14054/14070) -> ~40/frame -> ~300-frame visible assembly. Lift the WHOLE
        # chain (coverage + clean catchup + clean backlog) to fill the visible set fast (~1s).
        Set-ProcessEnv "VENPOD_SPARSE_MID_VOXEL_COVERAGE_CATCHUP_BUDGET" "$CleanCatchupBudget"
        Set-ProcessEnv "VENPOD_SPARSE_MID_VOXEL_CLEAN_CATCHUP_BUDGET" "$CleanCatchupBudget"
        Set-ProcessEnv "VENPOD_SPARSE_MID_VOXEL_CLEAN_BACKLOG_BUDGET" "$CleanCatchupBudget"
    }
    if ($ExperimentalBackgroundPassScale -gt 0) {
        $runParams["ExperimentalBackgroundPassScale"] = $ExperimentalBackgroundPassScale
        if ($ExperimentalBackgroundPassNoSurfaceFill) {
            $runParams["ExperimentalBackgroundPassNoSurfaceFill"] = $true
        }
        if ($ExperimentalBackgroundPassForegroundMask) {
            $runParams["ExperimentalBackgroundPassForegroundMask"] = $true
        }
    }
    if ($ExperimentalPrePublishGeneralSurfaceBudget -ge 0) {
        $runParams["ExperimentalPrePublishGeneralSurfaceBudget"] =
            $ExperimentalPrePublishGeneralSurfaceBudget
    }
    if ($ExperimentalPrePublishEditGeneralSurfaceBudget -ge 0) {
        $runParams["ExperimentalPrePublishEditGeneralSurfaceBudget"] =
            $ExperimentalPrePublishEditGeneralSurfaceBudget
    }
    if ($ExperimentalPrePublishPostEditGeneralSurfaceBudget -ge 0) {
        $runParams["ExperimentalPrePublishPostEditGeneralSurfaceBudget"] =
            $ExperimentalPrePublishPostEditGeneralSurfaceBudget
    }
    if ($ExperimentalPrePublishPostEditGeneralSpillFrames -ge 0) {
        $runParams["ExperimentalPrePublishPostEditGeneralSpillFrames"] =
            $ExperimentalPrePublishPostEditGeneralSpillFrames
    }
    if ($ExperimentalPrePublishPostEditGeneralSpillPressureMs -ge 0) {
        $runParams["ExperimentalPrePublishPostEditGeneralSpillPressureMs"] =
            $ExperimentalPrePublishPostEditGeneralSpillPressureMs
    }
    if ($ExperimentalHiddenExactPostOpenProbeMaxMsTenths -ge 0) {
        $runParams["ExperimentalHiddenExactPostOpenProbeMaxMsTenths"] =
            $ExperimentalHiddenExactPostOpenProbeMaxMsTenths
    }
    if ($ExperimentalHiddenExactPostOpenGenerationBudget -ge 0) {
        $runParams["ExperimentalHiddenExactPostOpenGenerationBudget"] =
            $ExperimentalHiddenExactPostOpenGenerationBudget
    }
    if ($ExperimentalHiddenExactPostOpenSurfaceBudget -ge 0) {
        $runParams["ExperimentalHiddenExactPostOpenSurfaceBudget"] =
            $ExperimentalHiddenExactPostOpenSurfaceBudget
    }
    if ($ExperimentalPrePublishHiddenTrackedSurfaceBudget -ge 0) {
        $runParams["ExperimentalPrePublishHiddenTrackedSurfaceBudget"] =
            $ExperimentalPrePublishHiddenTrackedSurfaceBudget
    }
    if ($ExperimentalMidClipmapPumpHardBudgetMs -ge 0) {
        $runParams["ExperimentalMidClipmapPumpHardBudgetMs"] =
            $ExperimentalMidClipmapPumpHardBudgetMs
    }
    if ($ExperimentalSurfaceAsyncPerCoordEditGate) {
        $runParams["ExperimentalSurfaceAsyncPerCoordEditGate"] = $true
    }
    if ($ExperimentalSurfaceRouteGeneralAsync -ge 0) {
        $runParams["ExperimentalSurfaceRouteGeneralAsync"] =
            $ExperimentalSurfaceRouteGeneralAsync
    }
    if ($ExperimentalSurfaceRouteAsyncBacklogLimit -ge 0) {
        $runParams["ExperimentalSurfaceRouteAsyncBacklogLimit"] =
            $ExperimentalSurfaceRouteAsyncBacklogLimit
    }
    if ($ExperimentalSurfaceRoutePublishPendingLimit -ge 0) {
        $runParams["ExperimentalSurfaceRoutePublishPendingLimit"] =
            $ExperimentalSurfaceRoutePublishPendingLimit
    }
    if ($ExperimentalSurfaceRoutePublishAgeLimit -ge 0) {
        $runParams["ExperimentalSurfaceRoutePublishAgeLimit"] =
            $ExperimentalSurfaceRoutePublishAgeLimit
    }
    if ($ExperimentalSurfaceRoutePagePublishLimit -ge 0) {
        $runParams["ExperimentalSurfaceRoutePagePublishLimit"] =
            $ExperimentalSurfaceRoutePagePublishLimit
    }
    if ($OwnershipInterval -gt 0) {
        $runParams["SparseOwnershipInterval"] = $OwnershipInterval
    }

    if ($Scenario -eq "walk" -or $Scenario -eq "yaw" -or $Scenario -eq "edit") {
        Set-ProcessEnv "VENPOD_SPARSE_WALK_TEST" "1"
        Set-ProcessEnv "VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS" "16"
        Set-ProcessEnv "VENPOD_SPARSE_WALK_TEST_PITCH_DEG" "-12"
        if ($AbsoluteWalkFrame) {
            Set-ProcessEnv "VENPOD_SPARSE_WALK_TEST_ABSOLUTE_YAW_FRAME" "1"
        }
    }

    if ($Scenario -eq "walk") {
        $speed = if ($WalkSpeed -ge 0) { "$WalkSpeed" } else { "25" }
        Set-ProcessEnv "VENPOD_SPARSE_WALK_TEST_SPEED" $speed
        # Honor -WalkYawDegPerSec (default 10) so a truly-static look-down repro
        # lane is possible: -WalkSpeed 0 -WalkYawDegPerSec 0 -WalkPitchDeg -90.
        Set-ProcessEnv "VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC" $(
            if ($WalkYawDegPerSec -ge 0) { "$WalkYawDegPerSec" } else { "10" })
        if ($WalkFly) {
            Set-ProcessEnv "VENPOD_SPARSE_WALK_TEST_FLY" "1"
        }
        if ($WalkEyeOffsetY -ne 0) {
            Set-ProcessEnv "VENPOD_SPARSE_WALK_TEST_EYE_OFFSET_Y" "$WalkEyeOffsetY"
        }
    } elseif ($Scenario -eq "yaw") {
        # (pitch/debug overrides applied below for all scenarios)
        Set-ProcessEnv "VENPOD_SPARSE_WALK_TEST_SPEED" "0"
        Set-ProcessEnv "VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC" "90"
    } elseif ($Scenario -eq "edit") {
        Set-ProcessEnv "VENPOD_EDIT_TELEMETRY" "1"
        if ($EditTelemetryInterval -ge 1) {
            Set-ProcessEnv "VENPOD_EDIT_TELEMETRY_LOG_INTERVAL" "$EditTelemetryInterval"
        }
        if ($HeightSerialTrace) {
            Set-ProcessEnv "VENPOD_HEIGHT_SERIAL_TRACE" "1"
        }
        # Speed override (Loop 111 harness hole: this was hardcoded 25, so the
        # "static" edit lanes actually WALKED at 0.4u/frame — -WalkSpeed 0 must
        # produce a truly static painter).
        Set-ProcessEnv "VENPOD_SPARSE_WALK_TEST_SPEED" $(
            if ($WalkSpeed -ge 0) { "$WalkSpeed" } else { "25" })
        # Yaw override (stroke-latency lane needs a FULLY static camera: speed 0
        # alone still yawed 10 deg/s, contaminating paint-growth pixel curves).
        Set-ProcessEnv "VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC" $(
            if ($WalkYawDegPerSec -ge 0) { "$WalkYawDegPerSec" } else { "10" })
        if ($BrushSmokeUserPath) {
            $runParams["SparseBrushSmokeUserPath"] = $true
        } else {
            $runParams["SparseBrushPaintMovingSmoke"] = $true
        }
        if ($BrushSmokeErase) {
            Set-ProcessEnv "VENPOD_SPARSE_BRUSH_SMOKE_ERASE" "1"
        }
        if ($BrushSmokeCase -ge 0) {
            Set-ProcessEnv "VENPOD_SPARSE_BRUSH_SMOKE_CASE" "$BrushSmokeCase"
        }
        if ($BrushSmokeRealAim) {
            Set-ProcessEnv "VENPOD_SPARSE_BRUSH_SMOKE_REAL_AIM" "1"
        }
        if ($BrushSmokeRadiusTenths -ge 0) {
            Set-ProcessEnv "VENPOD_SPARSE_BRUSH_SMOKE_RADIUS_TENTHS" "$BrushSmokeRadiusTenths"
        }
        if ($HiddenExactProbeInterval -ge 1) {
            Set-ProcessEnv "VENPOD_SPARSE_HIDDEN_EXACT_MISS_PROBE_INTERVAL" "$HiddenExactProbeInterval"
        }
        if ($BrushApplyCadence -ge 1) {
            Set-ProcessEnv "VENPOD_SPARSE_BRUSH_APPLY_CADENCE" "$BrushApplyCadence"
        }
        if ($BrushStampSpacingLargePct -ge 1) {
            Set-ProcessEnv "VENPOD_SPARSE_BRUSH_STAMP_SPACING_LARGE_PCT" "$BrushStampSpacingLargePct"
        }
        if ($BrushSmokeEndFrame -ge 0) {
            Set-ProcessEnv "VENPOD_SPARSE_BRUSH_PAINT_SMOKE_END_FRAME" "$BrushSmokeEndFrame"
        }
    }
    if ($Scenario -eq "walk" -or $Scenario -eq "yaw" -or $Scenario -eq "edit") {
        if ($WalkPitchDeg -gt -1000) {
            Set-ProcessEnv "VENPOD_SPARSE_WALK_TEST_PITCH_DEG" "$WalkPitchDeg"
        }
        if ($WalkEyeOffsetY -ne 0) {
            Set-ProcessEnv "VENPOD_SPARSE_WALK_TEST_EYE_OFFSET_Y" "$WalkEyeOffsetY"
        }
    }
    if ($SparseDebugMode -ge 0) {
        $runParams["SparseDebugMode"] = $SparseDebugMode
    }

    & (Join-Path $projectRoot "rebrun.ps1") @runParams *> $stdoutPath
    $exitCode = if ($null -eq $LASTEXITCODE) { 0 } else { $LASTEXITCODE }

    if (Test-Path $runtimeLog) {
        Copy-Item -LiteralPath $runtimeLog -Destination $savedLog -Force
    }

    "END scenario=$Scenario label=$Label exit=$exitCode time=$(Get-Date -Format o)" | Add-Content -LiteralPath $statusPath
} catch {
    "ERROR scenario=$Scenario label=$Label message=$($_.Exception.Message) time=$(Get-Date -Format o)" | Add-Content -LiteralPath $statusPath
    if (Test-Path $runtimeLog) {
        Copy-Item -LiteralPath $runtimeLog -Destination $savedLog -Force
    }
    $exitCode = 1
} finally {
    foreach ($name in $managedEnv) {
        [Environment]::SetEnvironmentVariable($name, $savedEnv[$name], "Process")
    }
    Get-Process VENPOD -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

exit $exitCode
