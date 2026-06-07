# =============================================================================
# VENPOD - Sparse Backend Regression Gate
# Builds once, runs CPU unit tests, then runs the render and GPU-physics sparse
# smoke presets. Use this before trusting larger sparse renderer refactor work.
# =============================================================================

param(
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$SkipTests,
    [switch]$ForceSync,
    [switch]$SkipFlickerSmoke,
    [switch]$SkipSurfaceSmoke,
    [switch]$SkipGpuRaycastSmoke,
    [switch]$SkipMissFeedbackSmoke,
    [switch]$SkipBrushFeedbackSmoke,
    [switch]$SkipBrushFeedbackApplySmoke,
    [switch]$SkipBrushFeedbackAuthoritativeSmoke,
    [switch]$SkipBrushFeedbackStrictResidentSmoke,
    [switch]$SkipBrushFeedbackMovingSmoke,
    [switch]$SkipBrushPaintSmoke,
    [switch]$SkipEditUiPersistenceSmoke,
    [switch]$SkipDefaultPhysicsSmoke,
    [switch]$SkipPhysicsSmoke,
    [switch]$SkipGpuPhysicsStrictSmoke,
    [switch]$SkipAsyncPagePublishSmoke,
    [switch]$SkipDenseLegacySmoke,
    [switch]$SkipRenderSmoke,
    [switch]$SkipStartupEngineCaptureSmoke,
    [switch]$SkipEngineCaptureSmoke,
    [switch]$SkipStressEngineCaptureSmoke,
    [switch]$SkipSkylineEngineCaptureSmoke,
    [switch]$SkipFastFlightEngineCaptureSmoke,
    [switch]$SkipFastWaterTransitionEngineCaptureSmoke,
    [switch]$SkipWalkEngineCaptureSmoke,
    [switch]$SkipTerrainGapEngineCaptureSmoke,
    [switch]$SkipBrushDomeEngineCaptureSmoke,
    [switch]$SkipLongWalkEngineCaptureSmoke,
    [switch]$SkipWaterlineEngineCaptureSmoke,
    [switch]$SkipOwnershipDebugCaptureSmoke,
    [switch]$SkipPublicDemoCapture,
    [switch]$SkipPublicReviewReelCapture,
    [switch]$SkipPublicReviewDocs,
    [int]$RenderExitAfterFrames = 240,
    [int]$PhysicsExitAfterFrames = 240,
    [int]$GpuPhysicsStrictExitAfterFrames = 300,
    [int]$GpuPhysicsStrictCaptureExitAfterFrames = 620,
    [int]$GpuPhysicsStrictCaptureStartFrame = 220,
    [int]$GpuPhysicsStrictCaptureIntervalFrames = 50,
    [int]$GpuPhysicsStrictCaptureCount = 6,
    [int]$GpuPhysicsStrictStressExitAfterFrames = 620,
    [int]$GpuPhysicsStrictStressStartFrame = 220,
    [int]$GpuPhysicsStrictStressIntervalFrames = 50,
    [int]$GpuPhysicsStrictStressCount = 6,
    [int]$GpuPhysicsStrictLongWalkExitAfterFrames = 1320,
    [int]$GpuPhysicsStrictLongWalkStartFrame = 360,
    [int]$GpuPhysicsStrictLongWalkIntervalFrames = 60,
    [int]$GpuPhysicsStrictLongWalkCount = 16,
    [int]$AsyncPagePublishExitAfterFrames = 420,
    [int]$AsyncPagePublishFenceExitAfterFrames = 420,
    [int]$AsyncPagePublishWalkExitAfterFrames = 900,
    [int]$AsyncPagePublishWalkStartFrame = 240,
    [int]$AsyncPagePublishWalkIntervalFrames = 60,
    [int]$AsyncPagePublishWalkCount = 8,
    [int]$AsyncPagePublishLongWalkExitAfterFrames = 1320,
    [int]$AsyncPagePublishLongWalkStartFrame = 360,
    [int]$AsyncPagePublishLongWalkIntervalFrames = 60,
    [int]$AsyncPagePublishLongWalkCount = 16,
    [int]$DefaultPhysicsExitAfterFrames = 240,
    [int]$DenseLegacyExitAfterFrames = 60,
    [int]$FlickerExitAfterFrames = 180,
    [int]$SurfaceExitAfterFrames = 240,
    [int]$GpuRaycastExitAfterFrames = 300,
    [int]$MissFeedbackExitAfterFrames = 240,
    [int]$BrushFeedbackExitAfterFrames = 360,
    [int]$BrushFeedbackApplyExitAfterFrames = 360,
    [int]$BrushFeedbackAuthoritativeExitAfterFrames = 390,
    [int]$BrushFeedbackStrictResidentExitAfterFrames = 840,
    [int]$BrushFeedbackMovingExitAfterFrames = 1050,
    [int]$BrushPaintExitAfterFrames = 600,
    [int]$BrushPaintMovingExitAfterFrames = 600,
    [int]$BrushPaintNonresidentExitAfterFrames = 900,
    [int]$BrushPaintGpuPhysicsExitAfterFrames = 900,
    [int]$StartupEngineCaptureExitAfterFrames = 180,
    [int]$StartupEngineCaptureStartFrame = 5,
    [int]$StartupEngineCaptureIntervalFrames = 15,
    [int]$StartupEngineCaptureCount = 8,
    [int]$EngineCaptureExitAfterFrames = 245,
    [int]$EngineCaptureStartFrame = 120,
    [int]$EngineCaptureIntervalFrames = 20,
    [int]$EngineCaptureCount = 6,
    [int]$StressEngineCaptureExitAfterFrames = 260,
    [int]$StressEngineCaptureStartFrame = 160,
    [int]$StressEngineCaptureIntervalFrames = 20,
    [int]$StressEngineCaptureCount = 5,
    [int]$SkylineEngineCaptureExitAfterFrames = 340,
    [int]$SkylineEngineCaptureStartFrame = 220,
    [int]$SkylineEngineCaptureIntervalFrames = 40,
    [int]$SkylineEngineCaptureCount = 3,
    [int]$FastFlightEngineCaptureExitAfterFrames = 720,
    [int]$FastFlightEngineCaptureStartFrame = 240,
    [int]$FastFlightEngineCaptureIntervalFrames = 30,
    [int]$FastFlightEngineCaptureCount = 12,
    [int]$LongFastFlightEngineCaptureExitAfterFrames = 1320,
    [int]$LongFastFlightEngineCaptureStartFrame = 390,
    [int]$LongFastFlightEngineCaptureIntervalFrames = 60,
    [int]$LongFastFlightEngineCaptureCount = 16,
    [int]$FastWaterTransitionEngineCaptureExitAfterFrames = 760,
    [int]$FastWaterTransitionEngineCaptureStartFrame = 220,
    [int]$FastWaterTransitionEngineCaptureIntervalFrames = 40,
    [int]$FastWaterTransitionEngineCaptureCount = 12,
    [int]$LongFastWaterTransitionEngineCaptureExitAfterFrames = 1320,
    [int]$LongFastWaterTransitionEngineCaptureStartFrame = 360,
    [int]$LongFastWaterTransitionEngineCaptureIntervalFrames = 60,
    [int]$LongFastWaterTransitionEngineCaptureCount = 16,
    [int]$WalkEngineCaptureExitAfterFrames = 410,
    [int]$WalkEngineCaptureStartFrame = 220,
    [int]$WalkEngineCaptureIntervalFrames = 50,
    [int]$WalkEngineCaptureCount = 4,
    [int]$TerrainGapEngineCaptureExitAfterFrames = 146,
    [int]$TerrainGapEngineCaptureStartFrame = 137,
    [int]$TerrainGapEngineCaptureIntervalFrames = 1,
    [int]$TerrainGapEngineCaptureCount = 4,
    [int]$BrushDomeEngineCaptureExitAfterFrames = 500,
    [int]$BrushDomeEngineCaptureStartFrame = 90,
    [int]$BrushDomeEngineCaptureIntervalFrames = 20,
    [int]$BrushDomeEngineCaptureCount = 4,
    [int]$LongWalkEngineCaptureExitAfterFrames = 1900,
    [int]$LongWalkEngineCaptureStartFrame = 220,
    [int]$LongWalkEngineCaptureIntervalFrames = 30,
    [int]$LongWalkEngineCaptureCount = 56,
    [int]$WaterlineEngineCaptureExitAfterFrames = 660,
    [int]$WaterlineEngineCaptureStartFrame = 200,
    [int]$WaterlineEngineCaptureIntervalFrames = 35,
    [int]$WaterlineEngineCaptureCount = 10,
    [int]$LongWaterlineEngineCaptureExitAfterFrames = 1900,
    [int]$LongWaterlineEngineCaptureStartFrame = 220,
    [int]$LongWaterlineEngineCaptureIntervalFrames = 30,
    [int]$LongWaterlineEngineCaptureCount = 56,
    [int]$OwnershipDebugCaptureCount = 3,
    [int]$PublicDemoCaptureStartFrame = 120,
    [int]$PublicDemoCaptureFrames = 16,
    [int]$PublicDemoPlaybackFps = 8,
    [int]$PublicReviewReelFrames = 12,
    [int]$PublicReviewReelPlaybackFps = 8
)

$ErrorActionPreference = "Stop"

function Write-Step { Write-Host "`n==> $args" -ForegroundColor Cyan }
function Write-Success { Write-Host "[OK] $args" -ForegroundColor Green }
function Write-Info { Write-Host "  $args" -ForegroundColor Gray }
function Stop-OnFailure {
    param(
        [int]$Code,
        [string]$Stage
    )
    if ($Code -ne 0) {
        Write-Host "[ERROR] $Stage failed with exit code $Code" -ForegroundColor Red
        exit $Code
    }
}
function Normalize-MinFrameBudget {
    param(
        [string]$Name,
        [int]$Value,
        [int]$Minimum
    )

    if ($Value -lt $Minimum) {
        Write-Info "$Name raised from $Value to $Minimum frames so post-ready telemetry can retire before shutdown"
        return $Minimum
    }
    return $Value
}
function Assert-CaptureWindowParameters {
    param(
        [string]$Label,
        [int]$ExitAfterFrames,
        [int]$CaptureStartFrame,
        [int]$CaptureIntervalFrames,
        [int]$CaptureCount
    )

    if ($ExitAfterFrames -lt 1) {
        throw "$Label ExitAfterFrames must be >= 1, got $ExitAfterFrames"
    }
    if ($CaptureStartFrame -lt 0) {
        throw "$Label CaptureStartFrame must be >= 0, got $CaptureStartFrame"
    }
    if ($CaptureIntervalFrames -lt 1) {
        throw "$Label CaptureIntervalFrames must be >= 1, got $CaptureIntervalFrames"
    }
    if ($CaptureCount -lt 1) {
        throw "$Label CaptureCount must be >= 1, got $CaptureCount"
    }

    $lastCaptureFrame64 =
        [int64]$CaptureStartFrame +
        ([int64]$CaptureIntervalFrames * [int64]($CaptureCount - 1))
    $minExitAfterFrames64 = $lastCaptureFrame64 + 5L
    if ($minExitAfterFrames64 -gt [int64][int]::MaxValue) {
        throw "$Label capture window is too large to compute ExitAfterFrames safely"
    }
}
function Assert-PublicDemoCaptureParameters {
    param(
        [int]$CaptureStartFrame,
        [int]$CaptureFrames,
        [int]$PlaybackFps
    )

    if ($CaptureStartFrame -lt 0) {
        throw "Public demo CaptureStartFrame must be >= 0, got $CaptureStartFrame"
    }
    if ($CaptureFrames -lt 1) {
        throw "Public demo CaptureFrames must be >= 1, got $CaptureFrames"
    }
    if ($PlaybackFps -lt 1) {
        throw "Public demo PlaybackFps must be >= 1, got $PlaybackFps"
    }
}
function Save-AndSummarizeRuntimeLog {
    param(
        [string]$Label,
        [string]$FileStem
    )

    $runtimeLog = Join-Path $projectRoot "build\bin\venpod_runtime.log"
    if (-not (Test-Path $runtimeLog)) {
        Write-Host "[WARN] $Label runtime log was not found at $runtimeLog" -ForegroundColor Yellow
        return $null
    }

    $logDir = Join-Path $projectRoot "build\logs"
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
    $savedLog = Join-Path $logDir "$FileStem.log"
    Copy-Item -Path $runtimeLog -Destination $savedLog -Force
    Write-Info "$Label runtime log: $savedLog"

    $summaryLines = Select-String `
        -Path $savedLog `
        -Pattern "PERF_BACKEND_PIPE|PERF_RENDER_OWNERSHIP|PERF_SPARSE frame=.*midClip=1|PERF_SPARSE_OWNERSHIP_PRESSURE|PERF_SPARSE_SURFACE|PERF_SPARSE_PHYSICS frame=|PERF_SPARSE_PHYSICS_GPU_RESULT|PERF_SPARSE_GPU_RAYCAST|farCov=|look=|missPending=|missRetired=|missConsumed=|brushGpuFb=|brushGpuFbMiss=|brushGpuFbHint=|brushGpuFbFallback=|Sparse brush feedback diagnostic queued|SPARSE_BRUSH_FEEDBACK parity observed|SPARSE_BRUSH_FEEDBACK parity failed|SPARSE_BRUSH_FEEDBACK diagnostic suite passed|SPARSE_BRUSH_FEEDBACK GPU apply|SPARSE_BRUSH_FEEDBACK CPU fallback|SPARSE_BRUSH_PAINT_SMOKE|Sparse surface diagnostic seed queued|SPARSE_SURFACE_FRAGMENTS failed|SPARSE_GPU_RAYCAST health observed|SPARSE_GPU_RAYCAST health failed|SPARSE_BACKEND_PIPE readiness failed|SPARSE_RENDER_OWNERSHIP quality failed|SPARSE_RENDER_OWNERSHIP stability failed|Sparse render ownership dropped stale readback payload|Sparse physics diagnostics dropped stale readback payload|Sparse physics result dropped stale or mismatched readback rows|\] \[(critical|error)\]|device removed|device-removed|timeout" `
        -CaseSensitive:$false
    if ($summaryLines) {
        Write-Info "$Label key runtime lines:"
        $summaryLines | Select-Object -Last 12 | ForEach-Object {
            Write-Host "    $($_.Line)" -ForegroundColor DarkGray
        }
    } else {
        Write-Info "$Label key runtime lines: none found"
    }

    return $savedLog
}

function Assert-NoRuntimeFailureMarkersFromLog {
    param(
        [string]$Label,
        [string]$SavedLog
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label failure-marker check has no runtime log"
    }

    $badLines = Select-String `
        -Path $SavedLog `
        -Pattern "\] \[(critical|error)\]|device removed|device-removed|timeout|SPARSE_BACKEND_PIPE readiness failed|SPARSE_RENDER_OWNERSHIP quality failed|SPARSE_RENDER_OWNERSHIP stability failed|Sparse render ownership dropped stale readback payload|Sparse physics diagnostics dropped stale readback payload|Sparse physics result dropped stale or mismatched readback rows" `
        -CaseSensitive:$false
    if ($badLines) {
        Write-Host "[ERROR] $Label found runtime failure markers:" -ForegroundColor Red
        $badLines | Select-Object -First 20 | ForEach-Object {
            Write-Host "  $($_.Line)" -ForegroundColor Red
        }
        exit 17
    }
}

function Assert-CompletionLedgerStatusCounts {
    param(
        [string]$Path
    )

    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) {
        throw "Completion ledger status-count check has no ledger file"
    }

    $validStatuses = @(
        "DONE_VERIFIED",
        "DONE_UNVERIFIED",
        "PARTIAL",
        "NOT_STARTED",
        "BLOCKED",
        "DEFERRED_BY_USER_ONLY"
    )
    $actualCounts = @{}
    $tableCounts = @{}
    foreach ($status in $validStatuses) {
        $actualCounts[$status] = 0
    }

    $lines = Get-Content -LiteralPath $Path
    $inStatusTable = $false
    $ledgerItemCount = 0
    foreach ($line in $lines) {
        if ($line -match "^## Status Counts\s*$") {
            $inStatusTable = $true
            continue
        }
        if ($inStatusTable -and $line -match "^## Ledger Items\s*$") {
            $inStatusTable = $false
        }

        if ($inStatusTable -and $line -match "^\|\s*([A-Z_]+)\s*\|\s*([0-9]+)\s*\|$") {
            $status = $Matches[1]
            if ($validStatuses -notcontains $status) {
                throw "Completion ledger status-count table contains unknown status: $status"
            }
            $tableCounts[$status] = [int]$Matches[2]
        }

        if ($line -match "^4\. Current status:\s*([A-Z_]+)\s*$") {
            $status = $Matches[1]
            if ($validStatuses -notcontains $status) {
                throw "Completion ledger item contains unknown status: $status"
            }
            ++$actualCounts[$status]
            ++$ledgerItemCount
        }
    }

    if ($ledgerItemCount -le 0) {
        throw "Completion ledger contains no item status lines"
    }

    $mismatches = New-Object System.Collections.Generic.List[string]
    foreach ($status in $validStatuses) {
        if (-not $tableCounts.ContainsKey($status)) {
            $mismatches.Add("$status missing from Status Counts table") | Out-Null
            continue
        }
        if ($tableCounts[$status] -ne $actualCounts[$status]) {
            $mismatches.Add("$status table=$($tableCounts[$status]) actual=$($actualCounts[$status])") | Out-Null
        }
    }
    if ($mismatches.Count -gt 0) {
        throw "Completion ledger Status Counts table does not match item statuses:`n  $($mismatches -join "`n  ")"
    }

    Write-Info "Completion ledger status counts verified: items=$ledgerItemCount partial=$($actualCounts["PARTIAL"]) done=$($actualCounts["DONE_VERIFIED"])"
}

function Assert-RenderPerformanceFromLog {
    param(
        [string]$Label,
        [string]$SavedLog,
        [int]$ReadyFrame = 120,
        [int]$MinSamples = 1,
        [double]$MaxFrameMs = 95.0,
        [double]$MaxPrepMs = 70.0,
        [double]$MaxGpuRayMs = 65.0
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label performance check has no runtime log"
    }

    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    $sampleCount = 0
    $maxObservedFrameMs = 0.0
    $maxObservedSmoothedFrameMs = 0.0
    $maxObservedPrepMs = 0.0
    $maxObservedGpuRayMs = 0.0
    $badRows = New-Object System.Collections.Generic.List[string]

    Select-String -Path $SavedLog -Pattern "PERF frame=" | ForEach-Object {
        $line = $_.Line
        $frameMatch = [regex]::Match($line, "PERF frame=(\d+)")
        if (-not $frameMatch.Success) {
            return
        }
        $frame = [int]$frameMatch.Groups[1].Value
        if ($frame -lt $ReadyFrame) {
            return
        }

        $msMatch = [regex]::Match($line, "ms=([0-9.]+)/([0-9.]+)")
        $prepMatch = [regex]::Match($line, "prep=([0-9.]+)")
        $gpuMatch = [regex]::Match(
            $line,
            "gpu=frame/upload/pre/surface/ray/overlay/ui:([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)")
        if (-not $msMatch.Success -or -not $prepMatch.Success -or -not $gpuMatch.Success) {
            $badRows.Add("frame ${frame}: missing PERF timing fields") | Out-Null
            return
        }

        ++$sampleCount
        $frameMs = [double]::Parse($msMatch.Groups[1].Value, $culture)
        $smoothedFrameMs = [double]::Parse($msMatch.Groups[2].Value, $culture)
        $prepMs = [double]::Parse($prepMatch.Groups[1].Value, $culture)
        $gpuRayMs = [double]::Parse($gpuMatch.Groups[5].Value, $culture)
        $maxObservedFrameMs = [Math]::Max($maxObservedFrameMs, $frameMs)
        $maxObservedSmoothedFrameMs = [Math]::Max($maxObservedSmoothedFrameMs, $smoothedFrameMs)
        $maxObservedPrepMs = [Math]::Max($maxObservedPrepMs, $prepMs)
        $maxObservedGpuRayMs = [Math]::Max($maxObservedGpuRayMs, $gpuRayMs)

        if ($frameMs -gt $MaxFrameMs) {
            $badRows.Add("frame ${frame}: frameMs=$('{0:F2}' -f $frameMs) max=$MaxFrameMs") | Out-Null
        }
        if ($prepMs -gt $MaxPrepMs) {
            $badRows.Add("frame ${frame}: prepMs=$('{0:F2}' -f $prepMs) max=$MaxPrepMs") | Out-Null
        }
        if ($gpuRayMs -gt $MaxGpuRayMs) {
            $badRows.Add("frame ${frame}: gpuRayMs=$('{0:F2}' -f $gpuRayMs) max=$MaxGpuRayMs") | Out-Null
        }
    }

    if ($sampleCount -lt $MinSamples) {
        throw "$Label performance check observed only $sampleCount PERF samples at/after frame $ReadyFrame, expected at least $MinSamples"
    }
    if ($badRows.Count -gt 0) {
        Write-Host "[ERROR] $Label performance gate failed:" -ForegroundColor Red
        $badRows | Select-Object -First 20 | ForEach-Object {
            Write-Host "  $_" -ForegroundColor Red
        }
        exit 18
    }

    Write-Info "$Label performance observed: samples=$sampleCount maxFrameMs=$('{0:F2}' -f $maxObservedFrameMs) maxSmoothedFrameMs=$('{0:F2}' -f $maxObservedSmoothedFrameMs) maxPrepMs=$('{0:F2}' -f $maxObservedPrepMs) maxGpuRayMs=$('{0:F2}' -f $maxObservedGpuRayMs) thresholds=$MaxFrameMs/$MaxPrepMs/$MaxGpuRayMs"
}

function Assert-DenseLegacyFallbackFromLog {
    param(
        [string]$Label,
        [string]$SavedLog
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label dense legacy fallback check has no runtime log"
    }

    $requestedDense = $false
    $activeDense = $false
    $sparseRaymarchDisabled = $false
    $densePhysicsDispatcher = $false

    Select-String -Path $SavedLog -Pattern "Render backend requested:|Sparse raymarch visual path:|PhysicsDispatcher initialized" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "Render backend requested:\s*dense-legacy") {
                $requestedDense = $true
            }
            if ($line -match "active:\s*dense-legacy") {
                $activeDense = $true
            }
            if ($line -match "Sparse raymarch visual path:\s*disabled") {
                $sparseRaymarchDisabled = $true
            }
            if ($line -match "PhysicsDispatcher initialized .*denseSim=1.*sparseRaycast=0.*sparseFeedback=0.*sparsePhysicsPackets=0") {
                $densePhysicsDispatcher = $true
            }
        }

    if (-not $requestedDense -or -not $activeDense) {
        throw "$Label did not run the dense-legacy render backend (requested=$requestedDense active=$activeDense)"
    }
    if (-not $sparseRaymarchDisabled) {
        throw "$Label did not disable the sparse raymarch visual path"
    }
    if (-not $densePhysicsDispatcher) {
        throw "$Label did not initialize the dense dispatcher contract for fallback comparison"
    }

    Write-Info "$Label dense legacy fallback observed: requested=$requestedDense active=$activeDense sparseRaymarchDisabled=$sparseRaymarchDisabled denseDispatcher=$densePhysicsDispatcher"
}

function Assert-SparseOnlyDefaultFromLog {
    param(
        [string]$Label,
        [string]$SavedLog
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label sparse-only default check has no runtime log"
    }

    $requestedSparse = $false
    $activeSparse = $false
    $sparseRaymarchEnabled = $false
    $runtimeBypassedDense = $false
    $denseCompatibilityDisabledStreaming = $false
    $skippedDenseInit = $false
    $sparseDispatcher = $false
    $voxelOnlyTerrainContract = $false

    Select-String -Path $SavedLog -Pattern "Render backend requested:|Sparse raymarch visual path:|Sparse runtime test mode:|PhysicsDispatcher initialized|Sparse terrain render contract:" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "Render backend requested:\s*sparse-brick") {
                $requestedSparse = $true
            }
            if ($line -match "active:\s*sparse-brick") {
                $activeSparse = $true
            }
            if ($line -match "Sparse raymarch visual path:\s*enabled") {
                $sparseRaymarchEnabled = $true
            }
            if ($line -match "Sparse runtime test mode:\s*enabled \(legacy dense streaming bypassed\)") {
                $runtimeBypassedDense = $true
            }
            if ($line -match "dense compatibility VoxelWorld .* infinite chunk streaming disabled") {
                $denseCompatibilityDisabledStreaming = $true
            }
            if ($line -match "skipping dense CS_Initialize; sparse pages are authoritative") {
                $skippedDenseInit = $true
            }
            if ($line -match "PhysicsDispatcher initialized .*denseSim=0.*denseRaycast=0") {
                $sparseDispatcher = $true
            }
            if ($line -match "Sparse terrain render contract:\s*voxel-only terrain; procedural mid/far height and far-water fallback disabled") {
                $voxelOnlyTerrainContract = $true
            }
        }

    if (-not $requestedSparse -or -not $activeSparse) {
        throw "$Label did not run the default sparse render backend (requested=$requestedSparse active=$activeSparse)"
    }
    if (-not $sparseRaymarchEnabled) {
        throw "$Label did not enable the sparse raymarch visual path"
    }
    if (-not $runtimeBypassedDense -or -not $denseCompatibilityDisabledStreaming -or -not $skippedDenseInit) {
        throw "$Label did not prove dense streaming/init was bypassed in sparse-only mode (runtimeBypassedDense=$runtimeBypassedDense denseCompatibilityDisabledStreaming=$denseCompatibilityDisabledStreaming skippedDenseInit=$skippedDenseInit)"
    }
    if (-not $sparseDispatcher) {
        throw "$Label did not initialize a sparse dispatcher contract with dense simulation/raycast disabled"
    }
    if (-not $voxelOnlyTerrainContract) {
        throw "$Label did not enable the voxel-only terrain render contract"
    }

    Write-Info "$Label sparse-only default observed: requested=$requestedSparse active=$activeSparse sparseRaymarch=$sparseRaymarchEnabled denseStreamingBypassed=$runtimeBypassedDense denseInitSkipped=$skippedDenseInit sparseDispatcher=$sparseDispatcher voxelOnlyTerrain=$voxelOnlyTerrainContract"
}

function Assert-FlickerOwnershipStabilityFromLog {
    param(
        [string]$Label,
        [string]$SavedLog,
        [int]$ReadyFrame = 120,
        [int]$MinSamples = 10,
        [double]$MaxTerrainDeltaPct = 8.0,
        [double]$MaxMissDeltaPct = 4.0
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label flicker ownership stability check has no runtime log"
    }

    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    $sampleCount = 0
    $minTerrainPct = 100.0
    $maxTerrainPct = 0.0
    $minMissPct = 100.0
    $maxMissPct = 0.0
    $maxMissPixels = 0
    $maxUnsafeNearMissPixels = 0

    Select-String -Path $SavedLog -Pattern "PERF_RENDER_OWNERSHIP" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "PERF_RENDER_OWNERSHIP .*retireFrame=([0-9]+).*total=([0-9]+).*sky=([0-9]+).*miss=([0-9]+).*unsafeNearMiss=([0-9]+)") {
                $frame = [int]$Matches[1]
                if ($frame -lt $ReadyFrame) {
                    return
                }
                $total = [double]::Parse($Matches[2], $culture)
                if ($total -le 0.0) {
                    return
                }
                $sky = [double]::Parse($Matches[3], $culture)
                $miss = [double]::Parse($Matches[4], $culture)
                $unsafeNearMiss = [double]::Parse($Matches[5], $culture)
                $terrain = [Math]::Max(0.0, $total - $sky - $miss)
                $terrainPct = ($terrain * 100.0) / $total
                $missPct = ($miss * 100.0) / $total
                $sampleCount++
                $minTerrainPct = [Math]::Min($minTerrainPct, $terrainPct)
                $maxTerrainPct = [Math]::Max($maxTerrainPct, $terrainPct)
                $minMissPct = [Math]::Min($minMissPct, $missPct)
                $maxMissPct = [Math]::Max($maxMissPct, $missPct)
                $maxMissPixels = [Math]::Max($maxMissPixels, [int64]$miss)
                $maxUnsafeNearMissPixels = [Math]::Max($maxUnsafeNearMissPixels, [int64]$unsafeNearMiss)
            }
        }

    if ($sampleCount -lt $MinSamples) {
        throw "$Label did not emit enough post-ready ownership samples (samples=$sampleCount required=$MinSamples readyFrame=$ReadyFrame)"
    }
    $terrainDelta = $maxTerrainPct - $minTerrainPct
    $missDelta = $maxMissPct - $minMissPct
    if ($terrainDelta -gt $MaxTerrainDeltaPct) {
        throw "$Label terrain ownership changed too much after warmup (delta=$('{0:F2}' -f $terrainDelta)% max=$MaxTerrainDeltaPct%)"
    }
    if ($missDelta -gt $MaxMissDeltaPct) {
        throw "$Label miss ownership changed too much after warmup (delta=$('{0:F2}' -f $missDelta)% max=$MaxMissDeltaPct%)"
    }
    if ($maxMissPixels -gt 0 -or $maxUnsafeNearMissPixels -gt 0) {
        throw "$Label observed post-ready miss or unsafe near-miss pixels (miss=$maxMissPixels unsafeNearMiss=$maxUnsafeNearMissPixels)"
    }

    Write-Info "$Label ownership stability observed: samples=$sampleCount terrain=$('{0:F2}-{1:F2}' -f $minTerrainPct, $maxTerrainPct)% missDelta=$('{0:F2}' -f $missDelta)% miss=$maxMissPixels unsafeNearMiss=$maxUnsafeNearMissPixels"
}

function Assert-EngineCaptureOwnershipFromLog {
    param(
        [string]$Label,
        [string]$OutputDir,
        [int]$ReadyFrame = 160,
        [int]$MinSamples = 5,
        [double]$MinTerrainPct = 80.0,
        [bool]$RequireSurfaceFragments = $true,
        [double]$MinMidVoxelPct = 0.0,
        [double]$MaxFarWaterPct = 100.0,
        [double]$MinFarWaterPct = 0.0,
        [double]$MinWaterContextPct = 0.0,
        [double]$MaxHeightProxyPct = 100.0,
        [double]$MinFarSvoPct = 0.0,
        [bool]$RequireMidFarTerrain = $true,
        [bool]$RequireFarSvo = $true,
        [bool]$RequireVoxelOnlyTerrain = $false
    )

    $runtimeLog = Join-Path $OutputDir "venpod_runtime.log"
    if (-not (Test-Path -LiteralPath $runtimeLog)) {
        throw "$Label ownership check has no runtime log at $runtimeLog"
    }

    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    $sampleCount = 0
    $observedMinTerrainPct = 100.0
    $maxMissPixels = 0L
    $maxUnsafeNearMissPixels = 0L
    $maxMidPixels = 0L
    $maxFarPixels = 0L
    $maxFarSvoPixels = 0L
    $maxSurfaceFragments = 0L
    $maxMidHeightPixels = 0L
    $maxFarHeightPixels = 0L
    $maxFarWaterPixels = 0L
    $maxMidVoxelPct = 0.0
    $maxFarWaterPctObserved = 0.0
    $maxWaterContextPctObserved = 0.0
    $maxHeightProxyPctObserved = 0.0
    $maxFarSvoPctObserved = 0.0
    $screenPixels = 0L

    $imageStatsPath = Join-Path $OutputDir "image_stats.csv"
    if (Test-Path -LiteralPath $imageStatsPath) {
        Import-Csv -Path $imageStatsPath |
            Select-Object -First 1 |
            ForEach-Object {
                $width = [int64]$_.width
                $height = [int64]$_.height
                if ($width -gt 0 -and $height -gt 0) {
                    $screenPixels = $width * $height
                }
            }
    }

    if ($RequireVoxelOnlyTerrain) {
        $contractObserved = Select-String -Path $runtimeLog -Pattern "Sparse terrain render contract:\s*voxel-only terrain; procedural mid/far height and far-water fallback disabled" -Quiet
        if (-not $contractObserved) {
            throw "$Label ownership did not run under the voxel-only terrain contract"
        }
    }

    Select-String -Path $runtimeLog -Pattern "PERF_RENDER_OWNERSHIP" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "PERF_RENDER_OWNERSHIP .*retireFrame=([0-9]+).*total=([0-9]+).*near=([0-9]+).*surfaceFragments=([0-9]+).*midVoxel=([0-9]+).*midHeight=([0-9]+).*farSvo=([0-9]+).*farHeight=([0-9]+)(?:.*farWater=([0-9]+))?.*sky=([0-9]+).*miss=([0-9]+).*unsafeNearMiss=([0-9]+)") {
                $ownershipMatches = $Matches.Clone()
                $frame = [int]$ownershipMatches[1]
                if ($frame -lt $ReadyFrame) {
                    return
                }
                $total = [double]::Parse($ownershipMatches[2], $culture)
                if ($total -le 0.0) {
                    return
                }
                $nearPixels = [int64]$ownershipMatches[3]
                $surfaceFragments = [int64]$ownershipMatches[4]
                $midVoxelPixels = [int64]$ownershipMatches[5]
                $midHeightPixels = [int64]$ownershipMatches[6]
                $midPixels = $midVoxelPixels + $midHeightPixels
                $farSvoPixels = [int64]$ownershipMatches[7]
                $farHeightPixels = [int64]$ownershipMatches[8]
                $farPixels = $farSvoPixels + $farHeightPixels
                $farWater = 0L
                if ($ownershipMatches.Count -gt 9 -and -not [string]::IsNullOrWhiteSpace($ownershipMatches[9])) {
                    $farWater = [int64]$ownershipMatches[9]
                }
                $waterContext = 0L
                if ($line -match "waterContext=([0-9]+)") {
                    $waterContext = [int64]$Matches[1]
                }
                $sky = [double]::Parse($ownershipMatches[10], $culture)
                $miss = [double]::Parse($ownershipMatches[11], $culture)
                $unsafeNearMiss = [int64]$ownershipMatches[12]
                $screenTotal = $total
                if ($screenPixels -gt 0) {
                    $screenTotal = [double]$screenPixels
                }
                $surfaceOwnedPixels = 0.0
                if ($screenPixels -gt 0 -and $screenTotal -gt $total) {
                    $surfaceOwnedPixels = $screenTotal - $total
                }
                $terrain = [Math]::Min(
                    $screenTotal,
                    [Math]::Max(0.0, $surfaceOwnedPixels + $nearPixels + $midPixels + $farPixels))
                $ownershipTestPixels = [Math]::Max(1.0, $screenTotal - $sky)
                $terrainPct = ($terrain * 100.0) / $ownershipTestPixels
                $midVoxelPct = ([double]$midVoxelPixels * 100.0) / $ownershipTestPixels
                $farWaterPct = ([double]$farWater * 100.0) / $ownershipTestPixels
                $waterContextPct = ([double]$waterContext * 100.0) / $ownershipTestPixels
                $heightProxyPct = ([double]($midHeightPixels + $farHeightPixels) * 100.0) / $ownershipTestPixels
                $farSvoPct = ([double]$farSvoPixels * 100.0) / $ownershipTestPixels
                ++$sampleCount
                $observedMinTerrainPct = [Math]::Min($observedMinTerrainPct, $terrainPct)
                $maxMidVoxelPct = [Math]::Max($maxMidVoxelPct, $midVoxelPct)
                $maxFarWaterPctObserved = [Math]::Max($maxFarWaterPctObserved, $farWaterPct)
                $maxWaterContextPctObserved = [Math]::Max($maxWaterContextPctObserved, $waterContextPct)
                $maxHeightProxyPctObserved = [Math]::Max($maxHeightProxyPctObserved, $heightProxyPct)
                $maxFarSvoPctObserved = [Math]::Max($maxFarSvoPctObserved, $farSvoPct)
                $maxMissPixels = [Math]::Max($maxMissPixels, [int64]$miss)
                $maxUnsafeNearMissPixels = [Math]::Max($maxUnsafeNearMissPixels, $unsafeNearMiss)
                $maxMidPixels = [Math]::Max($maxMidPixels, $midPixels)
                $maxFarPixels = [Math]::Max($maxFarPixels, $farPixels)
                $maxFarSvoPixels = [Math]::Max($maxFarSvoPixels, $farSvoPixels)
                $maxSurfaceFragments = [Math]::Max($maxSurfaceFragments, $surfaceFragments)
                $maxMidHeightPixels = [Math]::Max($maxMidHeightPixels, $midHeightPixels)
                $maxFarHeightPixels = [Math]::Max($maxFarHeightPixels, $farHeightPixels)
                $maxFarWaterPixels = [Math]::Max($maxFarWaterPixels, $farWater)
            }
        }

    if ($sampleCount -lt $MinSamples) {
        throw "$Label did not emit enough post-ready ownership samples (samples=$sampleCount required=$MinSamples readyFrame=$ReadyFrame)"
    }
    if ($observedMinTerrainPct -lt $MinTerrainPct) {
        throw "$Label ownership terrain fell below threshold (minTerrain=$('{0:F2}' -f $observedMinTerrainPct)% threshold=$MinTerrainPct%)"
    }
    if ($maxUnsafeNearMissPixels -gt 0 -or $maxMissPixels -gt 512) {
        throw "$Label ownership observed miss or unsafe near-miss pixels (miss=$maxMissPixels unsafeNearMiss=$maxUnsafeNearMissPixels)"
    }
    if ($RequireMidFarTerrain -and ($maxMidPixels -le 0 -or $maxFarPixels -le 0)) {
        throw "$Label ownership did not exercise both mid and far terrain ownership (mid=$maxMidPixels far=$maxFarPixels)"
    }
    if ($RequireFarSvo -and $maxFarSvoPixels -le 0) {
        throw "$Label ownership did not observe visible far SVO pixels"
    }
    if ($RequireSurfaceFragments -and $maxSurfaceFragments -le 0) {
        throw "$Label ownership did not observe sparse surface fragments"
    }
    if ($maxMidVoxelPct -lt $MinMidVoxelPct) {
        throw "$Label ownership did not observe enough real mid-voxel representation (maxMidVoxel=$('{0:F2}' -f $maxMidVoxelPct)% threshold=$MinMidVoxelPct%)"
    }
    if ($maxFarWaterPctObserved -gt $MaxFarWaterPct) {
        throw "$Label ownership observed too much far-water fallback (maxFarWater=$('{0:F2}' -f $maxFarWaterPctObserved)% threshold=$MaxFarWaterPct%)"
    }
    if ($maxFarWaterPctObserved -lt $MinFarWaterPct) {
        throw "$Label ownership did not observe enough water-surface representation (maxFarWater=$('{0:F2}' -f $maxFarWaterPctObserved)% threshold=$MinFarWaterPct%)"
    }
    if ($maxWaterContextPctObserved -lt $MinWaterContextPct) {
        throw "$Label ownership did not observe enough local water traversal context (maxWaterContext=$('{0:F2}' -f $maxWaterContextPctObserved)% threshold=$MinWaterContextPct%)"
    }
    if ($maxHeightProxyPctObserved -gt $MaxHeightProxyPct) {
        throw "$Label ownership is dominated by mid/far height proxy pixels (maxHeightProxy=$('{0:F2}' -f $maxHeightProxyPctObserved)% threshold=$MaxHeightProxyPct%)"
    }
    if ($maxFarSvoPctObserved -lt $MinFarSvoPct) {
        throw "$Label ownership did not observe enough far-SVO terrain (maxFarSvo=$('{0:F2}' -f $maxFarSvoPctObserved)% threshold=$MinFarSvoPct%)"
    }
    if ($RequireVoxelOnlyTerrain -and ($maxMidHeightPixels -gt 0 -or $maxFarHeightPixels -gt 0 -or $maxFarWaterPixels -gt 0)) {
        throw "$Label voxel-only terrain contract leaked fallback ownership (midHeight=$maxMidHeightPixels farHeight=$maxFarHeightPixels farWater=$maxFarWaterPixels)"
    }

    Write-Info "$Label ownership observed: samples=$sampleCount minTerrain=$('{0:F2}' -f $observedMinTerrainPct)% maxMidVoxel=$('{0:F2}' -f $maxMidVoxelPct)% maxFarWater=$('{0:F2}' -f $maxFarWaterPctObserved)% maxWaterContext=$('{0:F2}' -f $maxWaterContextPctObserved)% maxHeightProxy=$('{0:F2}' -f $maxHeightProxyPctObserved)% maxFarSvo=$('{0:F2}' -f $maxFarSvoPctObserved)% miss=$maxMissPixels unsafeNearMiss=$maxUnsafeNearMissPixels mid=$maxMidPixels far=$maxFarPixels farSvo=$maxFarSvoPixels surfaceFragments=$maxSurfaceFragments voxelTerrainOnly=$RequireVoxelOnlyTerrain screenPixels=$screenPixels"
}

function Assert-OwnershipDebugPairs {
    param(
        [string]$Label,
        [string]$NormalOutputDir,
        [string]$DebugOutputDir,
        [int]$MinPairs = 1,
        [int]$MinNormalUniqueColors = 25,
        [int]$MinDebugUniqueColors = 2,
        [int]$MaxDebugUniqueColors = 24,
        [double]$MaxDebugFarFallbackPct = -1.0,
        [double]$MaxDebugHeightProxyPct = -1.0,
        [double]$MaxDebugMissUnsafePct = 0.0
    )

    if (-not (Test-Path -LiteralPath $NormalOutputDir)) {
        throw "$Label ownership pair check has no normal output directory at $NormalOutputDir"
    }
    if (-not (Test-Path -LiteralPath $DebugOutputDir)) {
        throw "$Label ownership pair check has no debug output directory at $DebugOutputDir"
    }

    $normalStatsPath = Join-Path $NormalOutputDir "image_stats.csv"
    $debugStatsPath = Join-Path $DebugOutputDir "image_stats.csv"
    if (-not (Test-Path -LiteralPath $normalStatsPath)) {
        throw "$Label ownership pair check has no normal image stats at $normalStatsPath"
    }
    if (-not (Test-Path -LiteralPath $debugStatsPath)) {
        throw "$Label ownership pair check has no debug image stats at $debugStatsPath"
    }

    function Get-CsvDoubleOrBlank {
        param(
            [object]$Row,
            [string]$Name
        )
        if ($Row.PSObject.Properties.Name -contains $Name) {
            return [double]$Row.$Name
        }
        return ""
    }

    $normalByFrame = @{}
    Import-Csv -Path $normalStatsPath | ForEach-Object {
        $normalByFrame[$_.file] = $_
    }

    $pairs = @()
    Import-Csv -Path $debugStatsPath | ForEach-Object {
        $debug = $_
        if (-not $normalByFrame.ContainsKey($debug.file)) {
            throw "$Label ownership pair check is missing same-frame normal capture for $($debug.file)"
        }

        $normal = $normalByFrame[$debug.file]
        if ([int]$normal.width -ne [int]$debug.width -or [int]$normal.height -ne [int]$debug.height) {
            throw "$Label ownership pair check dimension mismatch for $($debug.file) normal=$($normal.width)x$($normal.height) debug=$($debug.width)x$($debug.height)"
        }

        $normalFrame = Join-Path $NormalOutputDir $debug.file
        $debugFrame = Join-Path $DebugOutputDir $debug.file
        if (-not (Test-Path -LiteralPath $normalFrame)) {
            throw "$Label ownership pair check missing normal frame file $normalFrame"
        }
        if (-not (Test-Path -LiteralPath $debugFrame)) {
            throw "$Label ownership pair check missing debug frame file $debugFrame"
        }

        $normalUnique = [int]$normal.uniqueSampleColors
        $debugUnique = [int]$debug.uniqueSampleColors
        if ($normalUnique -lt $MinNormalUniqueColors) {
            throw "$Label ownership pair check normal frame $($debug.file) has too few sampled colors ($normalUnique < $MinNormalUniqueColors)"
        }
        if ($debugUnique -lt $MinDebugUniqueColors -or $debugUnique -gt $MaxDebugUniqueColors) {
            throw "$Label ownership pair check debug frame $($debug.file) has unexpected ownership palette diversity ($debugUnique outside $MinDebugUniqueColors-$MaxDebugUniqueColors)"
        }
        if ([double]$debug.skyLikePct -gt 85.0) {
            throw "$Label ownership pair check debug frame $($debug.file) is mostly sky-like ($($debug.skyLikePct)%)"
        }
        if ($debug.PSObject.Properties.Name -contains "ownerMissPct" -and
            $debug.PSObject.Properties.Name -contains "ownerUnsafeNearMissPct") {
            $missUnsafePct = [double]$debug.ownerMissPct + [double]$debug.ownerUnsafeNearMissPct
            if ($missUnsafePct -gt $MaxDebugMissUnsafePct) {
                throw "$Label ownership pair check debug frame $($debug.file) has miss/unsafe-near-miss ownership $missUnsafePct% (max=$MaxDebugMissUnsafePct)"
            }
        }
        $farFallbackPct = ""
        if ($debug.PSObject.Properties.Name -contains "ownerFarSvoPct" -and
            $debug.PSObject.Properties.Name -contains "ownerFarHeightPct" -and
            $debug.PSObject.Properties.Name -contains "ownerFarWaterPct") {
            $farFallbackPct = [double]$debug.ownerFarSvoPct + [double]$debug.ownerFarHeightPct + [double]$debug.ownerFarWaterPct
        }
        if ($MaxDebugFarFallbackPct -ge 0.0 -and $farFallbackPct -ne "") {
            if ($farFallbackPct -gt $MaxDebugFarFallbackPct) {
                throw "$Label ownership pair check debug frame $($debug.file) has far fallback ownership $farFallbackPct% (max=$MaxDebugFarFallbackPct)"
            }
        }
        $heightProxyPct = ""
        if ($debug.PSObject.Properties.Name -contains "ownerMidHeightPct" -and
            $debug.PSObject.Properties.Name -contains "ownerFarHeightPct" -and
            $debug.PSObject.Properties.Name -contains "ownerFarWaterPct") {
            $heightProxyPct = [double]$debug.ownerMidHeightPct + [double]$debug.ownerFarHeightPct + [double]$debug.ownerFarWaterPct
        }
        if ($MaxDebugHeightProxyPct -ge 0.0 -and $heightProxyPct -ne "") {
            if ($heightProxyPct -gt $MaxDebugHeightProxyPct) {
                throw "$Label ownership pair check debug frame $($debug.file) has height-proxy ownership $heightProxyPct% (max=$MaxDebugHeightProxyPct)"
            }
        }

        $pairs += [pscustomobject]@{
            file = $debug.file
            width = $debug.width
            height = $debug.height
            normalUniqueSampleColors = $normal.uniqueSampleColors
            normalSkyLikePct = $normal.skyLikePct
            normalTerrainLikePct = $normal.terrainLikePct
            debugUniqueSampleColors = $debug.uniqueSampleColors
            debugSkyLikePct = $debug.skyLikePct
            debugTerrainLikePct = $debug.terrainLikePct
            debugOwnerNearPct = Get-CsvDoubleOrBlank -Row $debug -Name "ownerNearPct"
            debugOwnerMidVoxelPct = Get-CsvDoubleOrBlank -Row $debug -Name "ownerMidVoxelPct"
            debugOwnerMidHeightPct = Get-CsvDoubleOrBlank -Row $debug -Name "ownerMidHeightPct"
            debugOwnerFarSvoPct = Get-CsvDoubleOrBlank -Row $debug -Name "ownerFarSvoPct"
            debugOwnerFarHeightPct = Get-CsvDoubleOrBlank -Row $debug -Name "ownerFarHeightPct"
            debugOwnerFarWaterPct = Get-CsvDoubleOrBlank -Row $debug -Name "ownerFarWaterPct"
            debugOwnerFarFallbackPct = $farFallbackPct
            debugOwnerHeightProxyPct = $heightProxyPct
            debugOwnerSkyPct = Get-CsvDoubleOrBlank -Row $debug -Name "ownerSkyPct"
            debugOwnerMissPct = Get-CsvDoubleOrBlank -Row $debug -Name "ownerMissPct"
            debugOwnerUnsafeNearMissPct = Get-CsvDoubleOrBlank -Row $debug -Name "ownerUnsafeNearMissPct"
            normalFrame = $normalFrame
            debugFrame = $debugFrame
        }
    }

    if ($pairs.Count -lt $MinPairs) {
        throw "$Label ownership pair check found only $($pairs.Count) paired frames (required=$MinPairs)"
    }

    $reviewPath = Join-Path $DebugOutputDir "ownership_pair_review.csv"
    $pairs | Export-Csv -Path $reviewPath -NoTypeInformation
    Write-Info "$Label ownership pair check observed $($pairs.Count) same-frame normal/debug pairs; review=$reviewPath"
}

function Assert-SurfaceLookaheadTelemetryFromLog {
    param(
        [string]$Label,
        [string]$SavedLog
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label surface lookahead telemetry check has no runtime log"
    }

    $hasSurfaceTelemetry = $false
    Select-String -Path $SavedLog -Pattern "PERF_SPARSE_SURFACE.*look=([0-9]+)" |
        Select-Object -First 1 |
        ForEach-Object {
            $hasSurfaceTelemetry = $true
        }

    if (-not $hasSurfaceTelemetry) {
        throw "$Label did not emit PERF_SPARSE_SURFACE look=<count> telemetry"
    }

    Write-Info "$Label surface lookahead telemetry observed"
}

function Assert-SparseSurfaceGpuPathFromLog {
    param(
        [string]$Label,
        [string]$SavedLog
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label sparse surface GPU path check has no runtime log"
    }

    $seedObserved = $false
    $maxSeedVoxels = 0
    $maxGpuFaces = 0
    $maxGpuDraws = 0
    $maxGpuActiveDraws = 0
    $maxGpuRecords = 0
    $maxGpuClusters = 0
    $maxClusterSize = 0
    $maxClusterExtent = 0
    $maxClusterFastRecords = 0
    $maxClusterFastCapacity = 0
    $maxGpuCullCandidates = 0
    $maxGpuCullAccepted = 0
    $maxResidentPayload = 0
    $maxRasterFaces = 0
    $maxSurfaceFragments = 0
    $stableDrawObserved = $false
    $compactDrawObserved = $false
    $gpuCullDispatchObserved = $false
    $settledSurfaceFrameObserved = $false
    $overflowLines = @()
    $backlogLines = @()

    Select-String -Path $SavedLog -Pattern "Sparse surface diagnostic seed queued|PERF_SPARSE_SURFACE|PERF_RENDER_OWNERSHIP" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "Sparse surface diagnostic seed queued .*voxels=([0-9]+).*bricks=([0-9]+)") {
                $seedObserved = $true
                $maxSeedVoxels = [Math]::Max($maxSeedVoxels, [int]$Matches[1])
            }
            if ($line -match "PERF_RENDER_OWNERSHIP .*surfaceFragments=([0-9]+)") {
                $maxSurfaceFragments = [Math]::Max($maxSurfaceFragments, [int]$Matches[1])
            }
            if ($line -match "PERF_SPARSE_SURFACE .*gpuFaces=([0-9]+).*gpuDrawCmds=([0-9]+).*gpuActiveDraw=([0-9]+).*gpuRecords=([0-9]+).*gpuClusters=([0-9]+).*clusterSize=([0-9]+).*clusterExtent=([0-9]+).*clusterFast=([0-9]+)/([0-9]+).*gpuCull=([0-9]+).*gpuCullDispatch=([0-9]+).*gpuCullCand=([0-9]+)/([0-9]+).*gpuCullAccepted=([0-9]+).*gpuCullOverflow=([0-9]+).*drawSlots=([0-9]+)/([0-9]+).*stableDraw=([0-9]+).*compactDraw=([0-9]+).*residentPayload=([0-9]+).*rasterFaces=([0-9]+).*retry=([0-9]+).*overflow=([0-9]+)") {
                $maxGpuFaces = [Math]::Max($maxGpuFaces, [int]$Matches[1])
                $maxGpuDraws = [Math]::Max($maxGpuDraws, [int]$Matches[2])
                $maxGpuActiveDraws = [Math]::Max($maxGpuActiveDraws, [int]$Matches[3])
                $maxGpuRecords = [Math]::Max($maxGpuRecords, [int]$Matches[4])
                $maxGpuClusters = [Math]::Max($maxGpuClusters, [int]$Matches[5])
                $maxClusterSize = [Math]::Max($maxClusterSize, [int]$Matches[6])
                $maxClusterExtent = [Math]::Max($maxClusterExtent, [int]$Matches[7])
                $maxClusterFastRecords = [Math]::Max($maxClusterFastRecords, [int]$Matches[8])
                $maxClusterFastCapacity = [Math]::Max($maxClusterFastCapacity, [int]$Matches[9])
                if ([int]$Matches[10] -gt 0 -and [int]$Matches[11] -gt 0) {
                    $gpuCullDispatchObserved = $true
                }
                $maxGpuCullCandidates = [Math]::Max($maxGpuCullCandidates, [int]$Matches[12])
                $maxGpuCullAccepted = [Math]::Max($maxGpuCullAccepted, [int]$Matches[14])
                if ([int]$Matches[15] -gt 0 -or [int]$Matches[23] -gt 0) {
                    $overflowLines += $line
                }
                if ([int]$Matches[18] -gt 0) {
                    $stableDrawObserved = $true
                }
                if ([int]$Matches[19] -gt 0) {
                    $compactDrawObserved = $true
                }
                $maxResidentPayload = [Math]::Max($maxResidentPayload, [int]$Matches[20])
                $maxRasterFaces = [Math]::Max($maxRasterFaces, [int]$Matches[21])
            }
            if ($line -match "PERF_SPARSE_SURFACE .*gpuCullAccepted=([0-9]+).*stagedFaces=([0-9]+).*stagedRanges=([0-9]+).*stagedDrawCmds=([0-9]+).*stagedRecords=([0-9]+).*stagedClusters=([0-9]+).*deferred=([0-9]+).*pendingSnapDirty=([0-9]+).*pendingSnapRemoved=([0-9]+).*allocFail=([0-9]+).*retry=([0-9]+).*overflow=([0-9]+)") {
                $accepted = [int]$Matches[1]
                $stagedFaces = [int]$Matches[2]
                $stagedRanges = [int]$Matches[3]
                $stagedDrawCmds = [int]$Matches[4]
                $stagedRecords = [int]$Matches[5]
                $stagedClusters = [int]$Matches[6]
                $deferred = [int]$Matches[7]
                $pendingSnapDirty = [int]$Matches[8]
                $pendingSnapRemoved = [int]$Matches[9]
                $allocFail = [int]$Matches[10]
                $retry = [int]$Matches[11]
                $overflow = [int]$Matches[12]
                if ($allocFail -gt 0 -or $retry -gt 0 -or $overflow -gt 0) {
                    $backlogLines += $line
                }
                if ($accepted -gt 0 -and $stagedFaces -eq 0 -and $stagedRanges -eq 0 -and
                    $stagedDrawCmds -eq 0 -and $stagedRecords -eq 0 -and $stagedClusters -eq 0 -and
                    $deferred -eq 0 -and $pendingSnapDirty -eq 0 -and $pendingSnapRemoved -eq 0 -and
                    $allocFail -eq 0 -and $retry -eq 0 -and $overflow -eq 0) {
                    $settledSurfaceFrameObserved = $true
                }
            }
        }

    if (-not $seedObserved -or $maxSeedVoxels -le 0) {
        throw "$Label did not queue a sparse surface diagnostic seed"
    }
    if ($maxGpuFaces -le 0 -or $maxGpuDraws -le 0 -or $maxGpuActiveDraws -le 0 -or
        $maxGpuRecords -le 0 -or $maxGpuClusters -le 0 -or $maxResidentPayload -le 0) {
        throw "$Label did not publish nonzero sparse surface GPU faces/draws/records/payload"
    }
    if ($maxClusterSize -le 0 -or $maxClusterExtent -le 0 -or
        $maxClusterFastRecords -le 0 -or $maxClusterFastCapacity -le 0) {
        throw "$Label did not publish sparse surface cluster metadata/fast-cluster capacity"
    }
    if (-not $gpuCullDispatchObserved -or $maxGpuCullCandidates -le 0 -or $maxGpuCullAccepted -le 0) {
        throw "$Label did not observe sparse surface GPU culling dispatch with accepted draws"
    }
    if (-not $stableDrawObserved -or -not $compactDrawObserved) {
        throw "$Label did not observe stable+compact sparse surface draw mode"
    }
    if ($maxRasterFaces -le 0 -or $maxSurfaceFragments -le 0) {
        throw "$Label did not observe sparse surface raster faces/fragments"
    }
    if (-not $settledSurfaceFrameObserved) {
        throw "$Label did not observe a settled sparse surface GPU frame with accepted cull draws and no staged/deferred/retry backlog"
    }
    if ($backlogLines.Count -gt 0) {
        $sample = $backlogLines | Select-Object -First 5
        throw "$Label observed sparse surface allocation failure, retry, or overflow backlog:`n  $($sample -join "`n  ")"
    }
    if ($overflowLines.Count -gt 0) {
        $sample = $overflowLines | Select-Object -First 5
        throw "$Label observed sparse surface overflow:`n  $($sample -join "`n  ")"
    }

    Write-Info "$Label sparse surface GPU path observed: seedVoxels=$maxSeedVoxels gpuFaces=$maxGpuFaces activeDraws=$maxGpuActiveDraws records=$maxGpuRecords clusters=$maxGpuClusters clusterFast=$maxClusterFastRecords/$maxClusterFastCapacity cullAccepted=$maxGpuCullAccepted fragments=$maxSurfaceFragments settled=$settledSurfaceFrameObserved"
}

function Assert-OwnershipPressureTelemetryFromLog {
    param(
        [string]$Label,
        [string]$SavedLog
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label ownership-pressure telemetry check has no runtime log"
    }

    $hasOwnershipPressureTelemetry = $false
    Select-String -Path $SavedLog -Pattern "PERF_SPARSE_OWNERSHIP_PRESSURE.*level=([0-9]+).*deficit=([0-9]+).*excess=([0-9]+)" |
        Select-Object -First 1 |
        ForEach-Object {
            $hasOwnershipPressureTelemetry = $true
        }

    if (-not $hasOwnershipPressureTelemetry) {
        throw "$Label did not emit PERF_SPARSE_OWNERSHIP_PRESSURE level/deficit/excess telemetry"
    }

    Write-Info "$Label ownership-pressure telemetry observed"
}

function Assert-FastRequestTelemetryFromLog {
    param(
        [string]$Label,
        [string]$SavedLog,
        [int]$MinSamples = 1
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label fast-request telemetry check has no runtime log"
    }

    $sampleCount = 0
    $maxScale = 0
    $maxSpeculative = 0
    $maxVisible = 0
    $maxCollision = 0
    $maxTotal = 0
    $maxFreeSkips = 0
    $maxClassSkips = 0
    $maxTotalSkips = 0

    Select-String -Path $SavedLog -Pattern "PERF_SPARSE_FAST_REQUEST" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "PERF_SPARSE_FAST_REQUEST .*scale=([0-9]+).*spec/vis/coll=([0-9]+) / ([0-9]+) / ([0-9]+).*total=([0-9]+).*skips=([0-9]+)/([0-9]+)/([0-9]+)") {
                ++$sampleCount
                $scale = [int]$Matches[1]
                $speculative = [int]$Matches[2]
                $visible = [int]$Matches[3]
                $collision = [int]$Matches[4]
                $total = [int]$Matches[5]
                $freeSkips = [int]$Matches[6]
                $classSkips = [int]$Matches[7]
                $totalSkips = [int]$Matches[8]
                $maxScale = [Math]::Max($maxScale, $scale)
                $maxSpeculative = [Math]::Max($maxSpeculative, $speculative)
                $maxVisible = [Math]::Max($maxVisible, $visible)
                $maxCollision = [Math]::Max($maxCollision, $collision)
                $maxTotal = [Math]::Max($maxTotal, $total)
                $maxFreeSkips = [Math]::Max($maxFreeSkips, $freeSkips)
                $maxClassSkips = [Math]::Max($maxClassSkips, $classSkips)
                $maxTotalSkips = [Math]::Max($maxTotalSkips, $totalSkips)
            }
        }

    if ($sampleCount -lt $MinSamples) {
        throw "$Label did not emit enough fast-request telemetry samples (samples=$sampleCount required=$MinSamples)"
    }
    if ($maxScale -le 1) {
        throw "$Label did not exercise scaled fast-flight request planning (maxScale=$maxScale)"
    }
    if ($maxVisible -le 0 -or $maxCollision -le 0 -or $maxTotal -le 0) {
        throw "$Label fast-request telemetry did not reserve visible/collision request budgets (vis=$maxVisible coll=$maxCollision total=$maxTotal)"
    }
    if ($maxFreeSkips -gt 0 -or $maxClassSkips -gt 0 -or $maxTotalSkips -gt 0) {
        throw "$Label fast-request telemetry observed request skips (free/class/total=$maxFreeSkips/$maxClassSkips/$maxTotalSkips)"
    }

    Write-Info "$Label fast-request telemetry observed: samples=$sampleCount scale=$maxScale spec/vis/coll=$maxSpeculative/$maxVisible/$maxCollision total=$maxTotal skips=$maxFreeSkips/$maxClassSkips/$maxTotalSkips"
}

function Assert-WalkTelemetryFromLog {
    param(
        [string]$Label,
        [string]$SavedLog,
        [int]$MinSamples = 10
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label walk telemetry check has no runtime log"
    }

    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    $sampleCount = 0
    $groundedSamples = 0
    $maxSampled = 0
    $maxSolid = 0
    $maxLiquid = 0
    $minFeetY = [double]::PositiveInfinity
    $maxFeetY = [double]::NegativeInfinity

    Select-String -Path $SavedLog -Pattern "PERF_SPARSE_WALK" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "feet=\((-?[0-9.]+),(-?[0-9.]+),(-?[0-9.]+)\).*bodyColl=([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9.]+)") {
                ++$sampleCount
                $feetY = [double]::Parse($Matches[2], $culture)
                $grounded = [int]$Matches[9]
                $sampled = [int]$Matches[11]
                $solid = [int]$Matches[12]
                $liquid = [int]$Matches[13]
                if ($grounded -gt 0) {
                    ++$groundedSamples
                }
                $maxSampled = [Math]::Max($maxSampled, $sampled)
                $maxSolid = [Math]::Max($maxSolid, $solid)
                $maxLiquid = [Math]::Max($maxLiquid, $liquid)
                $minFeetY = [Math]::Min($minFeetY, $feetY)
                $maxFeetY = [Math]::Max($maxFeetY, $feetY)
            }
        }

    if ($sampleCount -lt $MinSamples) {
        throw "$Label did not emit enough walk telemetry samples (samples=$sampleCount required=$MinSamples)"
    }
    if ($groundedSamples -lt [Math]::Max(1, [int][Math]::Floor($sampleCount * 0.90))) {
        throw "$Label walk telemetry was not consistently grounded (grounded=$groundedSamples samples=$sampleCount)"
    }
    if ($maxSampled -le 0 -or ($maxSolid -le 0 -and $maxLiquid -le 0)) {
        throw "$Label walk telemetry did not sample support voxels (sampled=$maxSampled solid=$maxSolid liquid=$maxLiquid)"
    }
    if ($minFeetY -lt -256.0) {
        throw "$Label walk telemetry fell below the safety floor (minFeetY=$('{0:F2}' -f $minFeetY))"
    }

    Write-Info "$Label walk telemetry observed: samples=$sampleCount grounded=$groundedSamples minFeetY=$('{0:F2}' -f $minFeetY) maxFeetY=$('{0:F2}' -f $maxFeetY) sampled=$maxSampled solid=$maxSolid liquid=$maxLiquid"
}

function Assert-MidFarContinuityTelemetryFromLog {
    param(
        [string]$Label,
        [string]$SavedLog
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label mid/far continuity telemetry check has no runtime log"
    }

    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    $midClipObserved = $false
    $midClipDidWork = $false
    $maxMidUploadCoverage = 0.0
    $maxMidPageCoverage = 0.0
    $maxMidTilesResident = 0
    $maxMidTilesPending = 0
    $maxMidVoxelResident = 0
    $maxMidVoxelPending = 0
    $maxMidOwnedPixels = 0
    $maxFarOwnedPixels = 0
    $maxOwnershipPixels = 0
    $simultaneousMidFarOwnership = $false

    Select-String -Path $SavedLog -Pattern "PERF_SPARSE frame=.*midClip=1|PERF_RENDER_OWNERSHIP" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "PERF_SPARSE frame=.*midClip=1.*midCov=([0-9.]+)/([0-9.]+).*midTiles=([0-9]+)/([0-9]+).*midVoxels=([0-9]+)/([0-9]+)") {
                $midClipObserved = $true
                $uploadCoverage = [double]::Parse($Matches[1], $culture)
                $pageCoverage = [double]::Parse($Matches[2], $culture)
                $tileResident = [int]$Matches[3]
                $tilePending = [int]$Matches[4]
                $voxelResident = [int]$Matches[5]
                $voxelPending = [int]$Matches[6]
                $maxMidUploadCoverage = [Math]::Max($maxMidUploadCoverage, $uploadCoverage)
                $maxMidPageCoverage = [Math]::Max($maxMidPageCoverage, $pageCoverage)
                $maxMidTilesResident = [Math]::Max($maxMidTilesResident, $tileResident)
                $maxMidTilesPending = [Math]::Max($maxMidTilesPending, $tilePending)
                $maxMidVoxelResident = [Math]::Max($maxMidVoxelResident, $voxelResident)
                $maxMidVoxelPending = [Math]::Max($maxMidVoxelPending, $voxelPending)
                if ($uploadCoverage -gt 0.0 -or $pageCoverage -gt 0.0 -or
                    $tileResident -gt 0 -or $tilePending -gt 0 -or
                    $voxelResident -gt 0 -or $voxelPending -gt 0) {
                    $midClipDidWork = $true
                }
            }
            if ($line -match "PERF_RENDER_OWNERSHIP .*total=([0-9]+).*midVoxel=([0-9]+).*midHeight=([0-9]+).*farSvo=([0-9]+).*farHeight=([0-9]+)(?:.*farWater=([0-9]+))?") {
                $totalPixels = [int64]$Matches[1]
                $midOwned = [int64]$Matches[2] + [int64]$Matches[3]
                $farOwned = [int64]$Matches[4] + [int64]$Matches[5]
                $maxMidOwnedPixels = [Math]::Max($maxMidOwnedPixels, $midOwned)
                $maxFarOwnedPixels = [Math]::Max($maxFarOwnedPixels, $farOwned)
                $maxOwnershipPixels = [Math]::Max($maxOwnershipPixels, $totalPixels)
                $ownershipFloor = [Math]::Max(64L, [int64][Math]::Ceiling([double]$totalPixels * 0.005))
                if ($midOwned -ge $ownershipFloor -and $farOwned -ge $ownershipFloor) {
                    $simultaneousMidFarOwnership = $true
                }
            } elseif ($line -match "PERF_RENDER_OWNERSHIP .*midVoxel=([0-9]+).*midHeight=([0-9]+).*farSvo=([0-9]+).*farHeight=([0-9]+)(?:.*farWater=([0-9]+))?") {
                $midOwned = [int64]$Matches[1] + [int64]$Matches[2]
                $farOwned = [int64]$Matches[3] + [int64]$Matches[4]
                $maxMidOwnedPixels = [Math]::Max($maxMidOwnedPixels, $midOwned)
                $maxFarOwnedPixels = [Math]::Max($maxFarOwnedPixels, $farOwned)
            }
        }

    if (-not $midClipObserved -or -not $midClipDidWork) {
        throw "$Label did not exercise mid clipmap generation/upload telemetry (observed=$midClipObserved maxMidCov=$('{0:F2}/{1:F2}' -f $maxMidUploadCoverage, $maxMidPageCoverage) midTiles=$maxMidTilesResident/$maxMidTilesPending midVoxels=$maxMidVoxelResident/$maxMidVoxelPending)"
    }
    if ($maxMidOwnedPixels -le 0) {
        throw "$Label did not observe mid terrain ownership in PERF_RENDER_OWNERSHIP"
    }
    if ($maxFarOwnedPixels -le 0) {
        throw "$Label did not observe far terrain ownership in PERF_RENDER_OWNERSHIP"
    }
    if ($maxOwnershipPixels -gt 0) {
        $ownershipFloor = [Math]::Max(64L, [int64][Math]::Ceiling([double]$maxOwnershipPixels * 0.005))
        if (-not $simultaneousMidFarOwnership) {
            throw "$Label did not observe a meaningful simultaneous mid/far ownership sample in PERF_RENDER_OWNERSHIP (threshold=$ownershipFloor maxOwnedMid=$maxMidOwnedPixels maxOwnedFar=$maxFarOwnedPixels total=$maxOwnershipPixels)"
        }
    }

    Write-Info "$Label mid/far continuity telemetry observed: maxMidCov=$('{0:F2}/{1:F2}' -f $maxMidUploadCoverage, $maxMidPageCoverage) midTiles=$maxMidTilesResident/$maxMidTilesPending midVoxels=$maxMidVoxelResident/$maxMidVoxelPending maxOwnedMid=$maxMidOwnedPixels maxOwnedFar=$maxFarOwnedPixels simultaneous=$simultaneousMidFarOwnership"
}

function Assert-MissFeedbackPressureFromLog {
    param(
        [string]$Label,
        [string]$SavedLog
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label miss-feedback pressure check has no runtime log"
    }

    $observedPressure = $false
    $observedPendingTelemetry = $false
    $maxPending = 0
    $maxRetired = 0
    $maxConsumed = 0
    $maxEffectiveLevel = 0
    $maxFeedbackGrid = 0
    $maxFeedbackDistance = 0
    $maxFeedbackStride = 0
    $missFeedbackStatusObserved = $false
    $missFeedbackFailureLines = @()
    $urgentObserved = $false
    $ownershipObserved = $false
    $maxMissPixels = 0
    $maxUnsafeNearMissPixels = 0
    $maxOwnershipTotalPixels = 0
    $maxMissPercent = 0.0

    Select-String -Path $SavedLog -Pattern "PERF_RENDER_OWNERSHIP|PERF_SPARSE frame=.*missRetired=|PERF_SPARSE_OWNERSHIP_PRESSURE.*effectiveLevel=" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "PERF_RENDER_OWNERSHIP .*total=([0-9]+).*miss=([0-9]+).*unsafeNearMiss=([0-9]+)") {
                $ownershipObserved = $true
                $totalPixels = [Math]::Max(1, [int]$Matches[1])
                $missPixels = [int]$Matches[2]
                $unsafeNearMissPixels = [int]$Matches[3]
                $maxOwnershipTotalPixels = [Math]::Max($maxOwnershipTotalPixels, $totalPixels)
                $maxMissPixels = [Math]::Max($maxMissPixels, $missPixels)
                $maxUnsafeNearMissPixels = [Math]::Max($maxUnsafeNearMissPixels, $unsafeNearMissPixels)
                $maxMissPercent = [Math]::Max($maxMissPercent, ($missPixels * 100.0) / $totalPixels)
            }
            if ($line -match "PERF_SPARSE frame=.*missRetired=([0-9]+).*missPending=([0-9]+).*missConsumed=([0-9]+)") {
                $retired = [int]$Matches[1]
                $pending = [int]$Matches[2]
                $consumed = [int]$Matches[3]
                $maxRetired = [Math]::Max($maxRetired, $retired)
                $maxPending = [Math]::Max($maxPending, $pending)
                $maxConsumed = [Math]::Max($maxConsumed, $consumed)
                if ($pending -gt 0) {
                    $observedPendingTelemetry = $true
                }
            }
            if ($line -match "PERF_SPARSE frame=.*missRetired=[0-9]+.*missPending=[0-9]+.*missConsumed=[0-9]+.*missFbStale=([0-9]+).*missFbOverflow=([0-9]+)") {
                $missFeedbackStatusObserved = $true
                if ([int]$Matches[1] -gt 0 -or [int]$Matches[2] -gt 0) {
                    $missFeedbackFailureLines += $line
                }
            }
            if ($line -match "effectiveLevel=([0-9]+).*pendingMiss=([0-9]+).*feedback=([0-9]+)/([0-9]+)/([0-9]+).*urgent=([0-9]+)") {
                $effectiveLevel = [int]$Matches[1]
                $pendingMiss = [int]$Matches[2]
                $feedbackGrid = [int]$Matches[3]
                $feedbackDistance = [int]$Matches[4]
                $feedbackStride = [int]$Matches[5]
                $urgent = [int]$Matches[6]
                $maxEffectiveLevel = [Math]::Max($maxEffectiveLevel, $effectiveLevel)
                $maxPending = [Math]::Max($maxPending, $pendingMiss)
                $maxFeedbackGrid = [Math]::Max($maxFeedbackGrid, $feedbackGrid)
                $maxFeedbackDistance = [Math]::Max($maxFeedbackDistance, $feedbackDistance)
                $maxFeedbackStride = [Math]::Max($maxFeedbackStride, $feedbackStride)
                if ($urgent -gt 0) {
                    $urgentObserved = $true
                }
                if ($effectiveLevel -gt 0 -and $pendingMiss -gt 0) {
                    $observedPressure = $true
                }
            }
        }

    if (-not $observedPendingTelemetry) {
        if ($ownershipObserved -and $maxMissPixels -eq 0 -and $maxUnsafeNearMissPixels -eq 0) {
            Write-Info "$Label miss-feedback pressure response quiescent: pipeline telemetry present and render ownership produced zero miss/unsafe-near pixels"
        } elseif ($ownershipObserved -and $maxUnsafeNearMissPixels -eq 0 -and $maxMissPercent -le 5.0) {
            Write-Info "$Label miss-feedback pressure response quiescent: only low transient non-near miss ownership observed ($('{0:F2}' -f $maxMissPercent)%), below residency pressure threshold"
        } else {
            throw "$Label did not emit nonzero missPending telemetry in PERF_SPARSE"
        }
    }
    if ($maxFeedbackGrid -le 0 -or $maxFeedbackDistance -le 0 -or $maxFeedbackStride -le 0) {
        throw "$Label did not emit a valid miss-feedback sampling plan"
    }
    if (-not $missFeedbackStatusObserved) {
        throw "$Label did not emit miss-feedback stale/overflow status in PERF_SPARSE"
    }
    if ($missFeedbackFailureLines.Count -gt 0) {
        $sample = $missFeedbackFailureLines | Select-Object -First 5
        throw "$Label observed miss-feedback stale readback drops or overflow:`n  $($sample -join "`n  ")"
    }
    if ($observedPendingTelemetry -and -not $observedPressure) {
        throw "$Label did not show pending miss feedback producing nonzero effective ownership pressure"
    }

    Write-Info "$Label miss-feedback pressure response observed: pending=$maxPending retired=$maxRetired consumed=$maxConsumed effectiveLevel=$maxEffectiveLevel feedback=$maxFeedbackGrid/$maxFeedbackDistance/$maxFeedbackStride urgent=$urgentObserved stale=0 overflow=0 missPixels=$maxMissPixels unsafeNearMissPixels=$maxUnsafeNearMissPixels"
}

function Assert-GpuRaycastHealthFromLog {
    param(
        [string]$Label,
        [string]$SavedLog
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label GPU raycast health check has no runtime log"
    }

    $healthObserved = $false
    $maxAccepted = 0
    $maxFallback = 0
    $maxRejected = 0
    $maxMiss = 0
    $maxFallbackPct = 100
    $requiredAccepted = 1
    $perfHealthObserved = $false

    Select-String -Path $SavedLog -Pattern "SPARSE_GPU_RAYCAST health observed|PERF_SPARSE_GPU_RAYCAST" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "SPARSE_GPU_RAYCAST health observed .*accepted=([0-9]+).*fallback=([0-9]+).*rejected=([0-9]+).*miss=([0-9]+).*fallbackPct=([0-9]+).*maxFallbackPct=([0-9]+).*minAccepted=([0-9]+)") {
                $healthObserved = $true
                $accepted = [int]$Matches[1]
                $fallback = [int]$Matches[2]
                $rejected = [int]$Matches[3]
                $miss = [int]$Matches[4]
                $fallbackPct = [int]$Matches[5]
                $maxAllowedFallbackPct = [int]$Matches[6]
                $minAccepted = [int]$Matches[7]
                $maxAccepted = [Math]::Max($maxAccepted, $accepted)
                $maxFallback = [Math]::Max($maxFallback, $fallback)
                $maxRejected = [Math]::Max($maxRejected, $rejected)
                $maxMiss = [Math]::Max($maxMiss, $miss)
                $maxFallbackPct = [Math]::Min($maxFallbackPct, $maxAllowedFallbackPct)
                $requiredAccepted = [Math]::Max($requiredAccepted, $minAccepted)
                if ($fallbackPct -gt $maxAllowedFallbackPct) {
                    throw "$Label GPU raycast fallback percentage exceeded health threshold: fallbackPct=$fallbackPct max=$maxAllowedFallbackPct"
                }
                if ($rejected -gt 0 -or $miss -gt 0) {
                    throw "$Label GPU raycast health reported rejected or missed strict diagnostic rays (rejected=$rejected miss=$miss)"
                }
            }
            if ($line -match "PERF_SPARSE_GPU_RAYCAST .*readyA/R/M/F=([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+).*fallbackPct=([0-9]+).*health=1") {
                $perfHealthObserved = $true
                $maxAccepted = [Math]::Max($maxAccepted, [int]$Matches[1])
                $maxRejected = [Math]::Max($maxRejected, [int]$Matches[2])
                $maxMiss = [Math]::Max($maxMiss, [int]$Matches[3])
                $maxFallback = [Math]::Max($maxFallback, [int]$Matches[4])
            }
        }

    if (-not $healthObserved) {
        throw "$Label did not observe sparse GPU raycast health"
    }
    if ($maxAccepted -lt $requiredAccepted) {
        throw "$Label did not observe enough accepted sparse GPU raycasts (accepted=$maxAccepted required=$requiredAccepted)"
    }

    Write-Info "$Label GPU raycast health observed: accepted=$maxAccepted fallback=$maxFallback rejected=$maxRejected miss=$maxMiss maxFallbackPct=$maxFallbackPct perfHealth=$perfHealthObserved"
}

function Assert-BrushFeedbackDiagnosticsFromLog {
    param(
        [string]$Label,
        [string]$SavedLog,
        [switch]$RequireGpuApply,
        [switch]$RequireAuthoritativeApply,
        [switch]$RequireCpuFallback,
        [switch]$ForbidCpuFallback,
        [switch]$RequireMissingResidentRetry,
        [switch]$RequireMovingDiagnostic,
        [int]$MinMovingCenters = 3,
        [int]$MinCases = 7
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label brush-feedback diagnostic check has no runtime log"
    }

    $parityObserved = 0
    $suitePassed = $false
    $gpuApplyObserved = $false
    $authoritativeApplyObserved = $false
    $cpuFallbackObserved = $false
    $cpuFallbackHintsObserved = $false
    $missingResidentRetryQueued = $false
    $missingResidentRetryRequested = $false
    $overflowLines = @()
    $staleLines = @()
    $parityFailureLines = @()
    $duplicateFallbackLines = @()
    $movingSuitePassed = $false
    $movingCenters = @{}

    Select-String -Path $SavedLog -Pattern "SPARSE_BRUSH_FEEDBACK" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "SPARSE_BRUSH_FEEDBACK parity failed") {
                $parityFailureLines += $line
            }
            if ($line -match "SPARSE_BRUSH_FEEDBACK parity observed") {
                ++$parityObserved
            }
            if ($line -match "SPARSE_BRUSH_FEEDBACK diagnostic suite passed cases=([0-9]+)" -and
                [int]$Matches[1] -ge $MinCases) {
                $suitePassed = $true
            }
            if ($line -match "SPARSE_BRUSH_FEEDBACK diagnostic suite passed .*moving=1") {
                $movingSuitePassed = $true
            }
            if ($line -match "SPARSE_BRUSH_FEEDBACK GPU apply .*records=([1-9][0-9]*)") {
                $gpuApplyObserved = $true
            }
            if ($line -match "SPARSE_BRUSH_FEEDBACK GPU apply .*authoritative=1.*completedStrokes=([1-9][0-9]*)") {
                $authoritativeApplyObserved = $true
            }
            if ($line -match "SPARSE_BRUSH_FEEDBACK CPU fallback .*missingResident=([1-9][0-9]*)") {
                $cpuFallbackObserved = $true
            }
            if ($line -match "SPARSE_BRUSH_FEEDBACK CPU fallback .*hints=([1-9][0-9]*)") {
                $cpuFallbackHintsObserved = $true
            }
            if ($line -match "SPARSE_BRUSH_FEEDBACK missing-resident retry queued .*queuedBricks=([1-9][0-9]*)") {
                $missingResidentRetryQueued = $true
            }
            if ($line -match "SPARSE_BRUSH_FEEDBACK missing-resident retry requested .*class=edited") {
                $missingResidentRetryRequested = $true
            }
            if ($line -match "SPARSE_BRUSH_FEEDBACK CPU fallback .*duplicate=([1-9][0-9]*)") {
                $duplicateFallbackLines += $line
            }
        }

    Select-String -Path $SavedLog -Pattern "Sparse brush feedback diagnostic .*moving=1" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "center=([-0-9.]+),([-0-9.]+),([-0-9.]+).*moving=1") {
                $key = "$($Matches[1]),$($Matches[2]),$($Matches[3])"
                $movingCenters[$key] = $true
            }
        }

    Select-String -Path $SavedLog -Pattern "PERF_SPARSE .*brushGpuFb=" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "brushGpuFb=[0-9]+/[0-9]+/[0-9]+/([1-9][0-9]*)/(?:[0-9]+)") {
                $overflowLines += $line
            }
            if ($line -match "brushGpuFb=[0-9]+/[0-9]+/[0-9]+/[0-9]+/([1-9][0-9]*)") {
                $staleLines += $line
            }
        }

    if ($parityFailureLines.Count -gt 0) {
        $sample = $parityFailureLines | Select-Object -First 5
        throw "$Label reported sparse brush-feedback parity failure:`n  $($sample -join "`n  ")"
    }
    if ($overflowLines.Count -gt 0) {
        $sample = $overflowLines | Select-Object -First 5
        throw "$Label reported sparse brush-feedback overflow in runtime telemetry:`n  $($sample -join "`n  ")"
    }
    if ($staleLines.Count -gt 0) {
        $sample = $staleLines | Select-Object -First 5
        throw "$Label reported stale sparse brush-feedback readback drops in runtime telemetry:`n  $($sample -join "`n  ")"
    }
    if ($duplicateFallbackLines.Count -gt 0) {
        $sample = $duplicateFallbackLines | Select-Object -First 5
        throw "$Label reported duplicate sparse brush-feedback payload records in a clean diagnostic smoke:`n  $($sample -join "`n  ")"
    }
    if ($parityObserved -lt $MinCases -or -not $suitePassed) {
        throw "$Label did not complete the sparse brush feedback diagnostic suite (parity=$parityObserved suite=$suitePassed)"
    }
    if ($RequireGpuApply -and -not $gpuApplyObserved) {
        throw "$Label did not observe GPU brush feedback apply records"
    }
    if ($RequireAuthoritativeApply -and -not $authoritativeApplyObserved) {
        throw "$Label did not observe authoritative GPU brush feedback apply completing a pending stroke"
    }
    if ($RequireCpuFallback -and (-not $cpuFallbackObserved -or -not $cpuFallbackHintsObserved)) {
        throw "$Label did not observe CPU fallback for both missing-resident brush feedback signals"
    }
    if ($RequireMissingResidentRetry -and (-not $missingResidentRetryQueued -or -not $missingResidentRetryRequested)) {
        throw "$Label did not observe missing-resident retry queue/request proof (queued=$missingResidentRetryQueued requested=$missingResidentRetryRequested)"
    }
    if ($ForbidCpuFallback -and ($cpuFallbackObserved -or $cpuFallbackHintsObserved)) {
        throw "$Label observed CPU fallback even though strict resident-only mode forbids it"
    }
    if ($RequireMovingDiagnostic) {
        if (-not $movingSuitePassed) {
            throw "$Label did not run the moving sparse brush-feedback diagnostic suite"
        }
        if ($movingCenters.Count -lt $MinMovingCenters) {
            throw "$Label did not observe enough unique moving brush diagnostic centers (unique=$($movingCenters.Count) required=$MinMovingCenters)"
        }
    }

    Write-Info "$Label brush-feedback diagnostics observed: parity=$parityObserved suite=$suitePassed gpuApply=$gpuApplyObserved authoritativeApply=$authoritativeApplyObserved cpuFallback=$cpuFallbackObserved cpuFallbackHints=$cpuFallbackHintsObserved retryQueued=$missingResidentRetryQueued retryRequested=$missingResidentRetryRequested movingSuite=$movingSuitePassed movingCenters=$($movingCenters.Count) overflow=False stale=False duplicate=False"
}

function Assert-BrushPaintSmokeFromLog {
    param(
        [string]$Label,
        [string]$SavedLog,
        [int]$MinFrames = 60,
        [int]$MinQueued = 3,
        [int]$MinApplied = 1,
        [int]$MinCases = 1,
        [switch]$RequireMoving,
        [int]$MinPathCells = 1,
        [switch]$RequireNonresidentRetry
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label brush paint smoke check has no runtime log"
    }

    $passLine = $null
    Select-String -Path $SavedLog -Pattern "SPARSE_BRUSH_PAINT_SMOKE passed" |
        Select-Object -First 1 |
        ForEach-Object { $passLine = $_.Line }
    if (-not $passLine) {
        throw "$Label did not log SPARSE_BRUSH_PAINT_SMOKE passed"
    }

    if ($passLine -notmatch "frames=([0-9]+).*queued=([0-9]+).*retired=([0-9]+).*applied=([0-9]+).*deltas=([0-9]+).*fallback=([0-9]+).*missingResident=([0-9]+).*hints=([0-9]+).*overflow=([0-9]+).*deltaMismatch=([0-9]+)") {
        throw "$Label brush paint smoke pass line had an unexpected format: $passLine"
    }

    $frames = [int]$Matches[1]
    $queued = [int]$Matches[2]
    $retired = [int]$Matches[3]
    $applied = [int]$Matches[4]
    $deltas = [int]$Matches[5]
    $fallback = [int]$Matches[6]
    $missingResident = [int]$Matches[7]
    $hints = [int]$Matches[8]
    $overflow = [int]$Matches[9]
    $deltaMismatch = [int]$Matches[10]
    $casesCovered = 1
    $caseTarget = 1
    if ($passLine -match "cases=([0-9]+)/([0-9]+)") {
        $casesCovered = [int]$Matches[1]
        $caseTarget = [int]$Matches[2]
    } elseif ($MinCases -gt 1) {
        throw "$Label brush paint smoke pass line did not report case coverage: $passLine"
    }

    if ($frames -lt $MinFrames -or $queued -lt $MinQueued -or $applied -lt $MinApplied) {
        throw "$Label brush paint smoke was too weak: frames=$frames queued=$queued applied=$applied required=$MinFrames/$MinQueued/$MinApplied"
    }
    if ($retired -lt $applied) {
        throw "$Label brush paint smoke applied more records than retired (retired=$retired applied=$applied)"
    }
    if ($fallback -ne 0 -or $missingResident -ne 0 -or $hints -ne 0 -or $overflow -ne 0 -or $deltaMismatch -ne 0) {
        throw "$Label brush paint smoke reported fallback/missing/overflow/mismatch: fallback=$fallback missingResident=$missingResident hints=$hints overflow=$overflow deltaMismatch=$deltaMismatch"
    }
    if ($casesCovered -lt $MinCases -or $caseTarget -lt $MinCases) {
        throw "$Label brush paint smoke did not cover enough cases: cases=$casesCovered/$caseTarget required=$MinCases"
    }
    $moving = 0
    $pathCells = 0
    if ($passLine -match "moving=([0-9]+).*pathCells=([0-9]+)") {
        $moving = [int]$Matches[1]
        $pathCells = [int]$Matches[2]
    } elseif ($RequireMoving) {
        throw "$Label brush paint smoke pass line did not report moving path coverage: $passLine"
    }
    if ($RequireMoving -and ($moving -ne 1 -or $pathCells -lt $MinPathCells)) {
        throw "$Label brush paint smoke did not cover enough moving path cells: moving=$moving pathCells=$pathCells required=$MinPathCells"
    }
    $nonresident = 0
    $deferred = 0
    $requests = 0
    if ($passLine -match "nonresident=([0-9]+).*deferred=([0-9]+).*requests=([0-9]+)") {
        $nonresident = [int]$Matches[1]
        $deferred = [int]$Matches[2]
        $requests = [int]$Matches[3]
    } elseif ($RequireNonresidentRetry) {
        throw "$Label brush paint smoke pass line did not report nonresident retry coverage: $passLine"
    }
    if ($RequireNonresidentRetry -and ($nonresident -ne 1 -or $deferred -lt 1 -or $requests -lt 1)) {
        throw "$Label brush paint smoke did not cover nonresident strict-resident retry/deferral: nonresident=$nonresident deferred=$deferred requests=$requests"
    }

    Write-Info "$Label brush paint smoke observed: frames=$frames queued=$queued retired=$retired applied=$applied deltas=$deltas fallback=$fallback missingResident=$missingResident hints=$hints overflow=$overflow deltaMismatch=$deltaMismatch cases=$casesCovered/$caseTarget moving=$moving pathCells=$pathCells nonresident=$nonresident deferred=$deferred requests=$requests"
}

function Assert-FarSvoReadyFromLog {
    param(
        [string]$Label,
        [string]$SavedLog
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label far SVO readiness check has no runtime log"
    }

    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    $farActive = $false
    $farCoverageReady = $false
    $maxUploadCoverage = 0.0
    $maxPageCoverage = 0.0
    $maxFarSvoOwnedPixels = 0
    $maxOwnershipPixels = 0
    $farSvoOwnershipReady = $false

    Select-String -Path $SavedLog -Pattern "PERF_BACKEND_PIPE.*far=1|farCov=([0-9.]+)/([0-9.]+)|PERF_RENDER_OWNERSHIP" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "PERF_BACKEND_PIPE.*far=1") {
                $farActive = $true
            }
            if ($line -match "farCov=([0-9.]+)/([0-9.]+)") {
                $uploadCoverage = [double]::Parse($Matches[1], $culture)
                $pageCoverage = [double]::Parse($Matches[2], $culture)
                $maxUploadCoverage = [Math]::Max($maxUploadCoverage, $uploadCoverage)
                $maxPageCoverage = [Math]::Max($maxPageCoverage, $pageCoverage)
                if ($uploadCoverage -ge 0.999 -and $pageCoverage -gt 0.0) {
                    $farCoverageReady = $true
                }
            }
            if ($line -match "PERF_RENDER_OWNERSHIP .*total=([0-9]+).*farSvo=([0-9]+)") {
                $totalPixels = [int64]$Matches[1]
                $farSvoOwned = [int64]$Matches[2]
                $maxFarSvoOwnedPixels = [Math]::Max($maxFarSvoOwnedPixels, $farSvoOwned)
                $maxOwnershipPixels = [Math]::Max($maxOwnershipPixels, $totalPixels)
                $ownershipFloor = [Math]::Max(64L, [int64][Math]::Ceiling([double]$totalPixels * 0.001))
                if ($farSvoOwned -ge $ownershipFloor) {
                    $farSvoOwnershipReady = $true
                }
            } elseif ($line -match "PERF_RENDER_OWNERSHIP .*farSvo=([0-9]+)") {
                $farSvoOwned = [int64]$Matches[1]
                $maxFarSvoOwnedPixels = [Math]::Max($maxFarSvoOwnedPixels, $farSvoOwned)
                if ($farSvoOwned -gt 0) {
                    $farSvoOwnershipReady = $true
                }
            }
        }

    if (-not $farActive -or -not $farCoverageReady) {
        throw "$Label far SVO did not become resident/active (active=$farActive maxCov=$('{0:F2}/{1:F2}' -f $maxUploadCoverage, $maxPageCoverage))"
    }
    if (-not $farSvoOwnershipReady) {
        $ownershipFloor = 1
        if ($maxOwnershipPixels -gt 0) {
            $ownershipFloor = [Math]::Max(64L, [int64][Math]::Ceiling([double]$maxOwnershipPixels * 0.001))
        }
        throw "$Label far SVO became resident but did not own meaningful pixels in PERF_RENDER_OWNERSHIP (threshold=$ownershipFloor maxFarSvo=$maxFarSvoOwnedPixels total=$maxOwnershipPixels)"
    }

    Write-Info "$Label far SVO readiness observed: active=$farActive maxCov=$('{0:F2}/{1:F2}' -f $maxUploadCoverage, $maxPageCoverage) maxFarSvo=$maxFarSvoOwnedPixels visible=$farSvoOwnershipReady"
}

function Assert-DefaultSparsePhysicsFromLog {
    param(
        [string]$Label,
        [string]$SavedLog
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label default sparse physics check has no runtime log"
    }

    $observedEnabled = $false
    $observedWork = $false
    $observedCpuMove = $false
    $gpuPacketLines = @()
    $gpuApplyLines = @()
    $overflowLines = @()
    $budgetViolationLines = @()
    $maxPackets = 0
    $maxProcessed = 0
    $maxMoved = 0
    $maxBrickBudget = 0
    $maxMoveBudget = 0

    Select-String -Path $SavedLog -Pattern "PERF_SPARSE_PHYSICS frame=" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "enabled=1") {
                $observedEnabled = $true
            }
            if ($line -match "packets=([1-9][0-9]*)") {
                $observedWork = $true
            }
            if ($line -match "processed=([1-9][0-9]*).*moved=([1-9][0-9]*)") {
                $observedCpuMove = $true
            }
            if ($line -match "gpuPackets=([1-9][0-9]*)") {
                $gpuPacketLines += $line
            }
            if ($line -match "gpuApply=([1-9][0-9]*)/|gpuApply=[0-9]+/([1-9][0-9]*)") {
                $gpuApplyLines += $line
            }
            if ($line -match "editOverflow=([1-9][0-9]*)|gpuOverflow=([1-9][0-9]*)|gpuMalformed=([1-9][0-9]*)") {
                $overflowLines += $line
            }
            if ($line -match "packets=([0-9]+).*processed=([0-9]+).*moved=([0-9]+).*budget=([0-9]+)/([0-9]+)") {
                $packets = [int]$Matches[1]
                $processed = [int]$Matches[2]
                $moved = [int]$Matches[3]
                $brickBudget = [int]$Matches[4]
                $moveBudget = [int]$Matches[5]
                $maxPackets = [Math]::Max($maxPackets, $packets)
                $maxProcessed = [Math]::Max($maxProcessed, $processed)
                $maxMoved = [Math]::Max($maxMoved, $moved)
                $maxBrickBudget = [Math]::Max($maxBrickBudget, $brickBudget)
                $maxMoveBudget = [Math]::Max($maxMoveBudget, $moveBudget)
                if ($packets -gt $brickBudget -or $processed -gt $brickBudget -or
                    $moved -gt $moveBudget -or $brickBudget -gt 32 -or
                    $moveBudget -gt 1024) {
                    $budgetViolationLines += $line
                }
            }
        }

    if (-not $observedEnabled) {
        throw "$Label did not run with default local sparse physics enabled"
    }
    if (-not $observedWork) {
        throw "$Label did not stage any local sparse physics work packets"
    }
    if (-not $observedCpuMove) {
        throw "$Label did not observe CPU local sparse physics applying a move"
    }
    if ($gpuPacketLines.Count -gt 0 -or $gpuApplyLines.Count -gt 0) {
        throw "$Label unexpectedly used GPU physics packets/apply in default local physics smoke"
    }
    if ($overflowLines.Count -gt 0) {
        $sample = $overflowLines | Select-Object -First 5
        throw "$Label observed sparse physics overflow or malformed GPU rows in default local physics:`n  $($sample -join "`n  ")"
    }
    if ($budgetViolationLines.Count -gt 0) {
        $sample = $budgetViolationLines | Select-Object -First 5
        throw "$Label exceeded bounded local sparse physics budgets:`n  $($sample -join "`n  ")"
    }

    Write-Info "$Label default local sparse physics observed without GPU packet/apply flags: packets=$maxPackets processed=$maxProcessed moved=$maxMoved budget=$maxBrickBudget/$maxMoveBudget"
}

function Assert-SparseBodyCollisionFromLog {
    param(
        [string]$Label,
        [string]$SavedLog
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label sparse body-collision check has no runtime log"
    }

    $observedCollisionPipe = $false
    $maxSampled = 0
    $maxSolid = 0
    $maxLiquid = 0
    $observedGrounded = $false
    $observedLandingOrVerticalBlock = $false

    Select-String -Path $SavedLog -Pattern "PERF_BACKEND_PIPE|PERF_SPARSE frame=.*bodyColl=" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "PERF_BACKEND_PIPE .*coll=1") {
                $observedCollisionPipe = $true
            }
            if ($line -match "bodyColl=([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9.]+)") {
                $verticalBlocked = [int]$Matches[3]
                $landed = [int]$Matches[4]
                $grounded = [int]$Matches[6]
                $sampled = [int]$Matches[8]
                $solid = [int]$Matches[9]
                $liquid = [int]$Matches[10]
                $maxSampled = [Math]::Max($maxSampled, $sampled)
                $maxSolid = [Math]::Max($maxSolid, $solid)
                $maxLiquid = [Math]::Max($maxLiquid, $liquid)
                if ($grounded -gt 0) {
                    $observedGrounded = $true
                }
                if ($landed -gt 0 -or $verticalBlocked -gt 0) {
                    $observedLandingOrVerticalBlock = $true
                }
            }
        }

    if (-not $observedCollisionPipe) {
        throw "$Label did not report the sparse collision backend as active"
    }
    if ($maxSampled -le 0) {
        throw "$Label did not sample sparse body-collision voxels"
    }
    if ($maxSolid -le 0) {
        throw "$Label did not observe solid sparse body-collision support"
    }
    if (-not $observedGrounded -or -not $observedLandingOrVerticalBlock) {
        throw "$Label did not observe grounded/landing sparse body-collision state (grounded=$observedGrounded landingOrVerticalBlock=$observedLandingOrVerticalBlock)"
    }

    Write-Info "$Label sparse body collision observed: sampled=$maxSampled solid=$maxSolid liquid=$maxLiquid grounded=$observedGrounded landingOrVerticalBlock=$observedLandingOrVerticalBlock"
}

function Assert-GpuSparsePhysicsFromLog {
    param(
        [string]$Label,
        [string]$SavedLog,
        [switch]$RequireDiagnosticSeed,
        [switch]$RequireFluidSeed,
        [switch]$ForbidDiagnosticSeed,
        [switch]$ForbidFluidSeed,
        [switch]$AllowRejectedProposals,
        [int]$MinResultProposals = 1,
        [int]$MinGpuApplyCompleted = 1,
        [int]$MinRejectedProposals = 0
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label GPU sparse physics check has no runtime log"
    }

    $observedEnabled = $false
    $maxGpuPackets = 0
    $maxGpuResults = 0
    $maxGpuProposals = 0
    $maxGpuApplyCompleted = 0
    $maxGpuApplyTotal = 0
    $maxRejectedProposals = 0
    $maxResultPackets = 0
    $maxResultProposals = 0
    $maxResultGeneration = 0
    $maxResultChecksum = 0
    $observedMaterialMask = [uint32]0
    $wellFormedResultStatusObserved = $false
    $malformedResultStatuses = @()
    $malformedRetireLines = @()
    $missingBelowLines = @()
    $rejectLines = @()
    $overflowLines = @()
    $diagnosticSeedObserved = $false
    $fluidSeedObserved = $false

    Select-String -Path $SavedLog -Pattern "PERF_SPARSE_PHYSICS frame=|PERF_SPARSE_PHYSICS_GPU_RESULT|SPARSE_GPU_PHYSICS apply|Sparse physics diagnostic seed queued|Sparse physics diagnostic fluid seed queued" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "Sparse physics diagnostic seed queued") {
                $diagnosticSeedObserved = $true
            }
            if ($line -match "Sparse physics diagnostic fluid seed queued") {
                $fluidSeedObserved = $true
            }
            if ($line -match "PERF_SPARSE_PHYSICS frame=") {
                if ($line -match "enabled=1") {
                    $observedEnabled = $true
                }
                if ($line -match "gpuPackets=([0-9]+)") {
                    $maxGpuPackets = [Math]::Max($maxGpuPackets, [int]$Matches[1])
                }
                if ($line -match "gpuResults=([0-9]+)") {
                    $maxGpuResults = [Math]::Max($maxGpuResults, [int]$Matches[1])
                }
                if ($line -match "gpuProposals=([0-9]+)") {
                    $maxGpuProposals = [Math]::Max($maxGpuProposals, [int]$Matches[1])
                }
                if ($line -match "gpuApply=([0-9]+)/([0-9]+)") {
                    $maxGpuApplyCompleted = [Math]::Max($maxGpuApplyCompleted, [int]$Matches[1])
                    $maxGpuApplyTotal = [Math]::Max($maxGpuApplyTotal, [int]$Matches[2])
                }
                if ($line -match "gpuReject=([1-9][0-9]*)") {
                    $maxRejectedProposals = [Math]::Max($maxRejectedProposals, [int]$Matches[1])
                    $rejectLines += $line
                }
                if ($line -match "gpuMask=0x([0-9A-Fa-f]+)") {
                    $observedMaterialMask = $observedMaterialMask -bor [Convert]::ToUInt32($Matches[1], 16)
                }
                if ($line -match "gpuMalformed=([1-9][0-9]*)") {
                    $malformedRetireLines += $line
                }
                if ($line -match "gpuMissingBelow=([1-9][0-9]*)") {
                    $missingBelowLines += $line
                }
                if ($line -match "editOverflow=([1-9][0-9]*)|gpuOverflow=([1-9][0-9]*)") {
                    $overflowLines += $line
                }
            }
            if ($line -match "SPARSE_GPU_PHYSICS apply .*applied=([0-9]+).*proposals=([0-9]+).*rejected=([0-9]+)") {
                $maxGpuApplyCompleted = [Math]::Max($maxGpuApplyCompleted, [int]$Matches[1])
                $maxGpuApplyTotal = [Math]::Max($maxGpuApplyTotal, [int]$Matches[2])
                if ([int]$Matches[3] -gt 0) {
                    $maxRejectedProposals = [Math]::Max($maxRejectedProposals, [int]$Matches[3])
                    $rejectLines += $line
                }
            }
            if ($line -match "PERF_SPARSE_PHYSICS_GPU_RESULT .*results=([0-9]+).*proposals=([0-9]+)") {
                $maxResultPackets = [Math]::Max($maxResultPackets, [int]$Matches[1])
                $maxGpuResults = [Math]::Max($maxGpuResults, [int]$Matches[1])
                $resultProposals = [int]$Matches[2]
                $maxResultProposals = [Math]::Max($maxResultProposals, $resultProposals)
                if ($line -match "missingBelow=([1-9][0-9]*)") {
                    $missingBelowLines += $line
                }
                if ($line -match "checksum=([0-9]+)") {
                    $maxResultChecksum = [Math]::Max($maxResultChecksum, [uint64]$Matches[1])
                }
                if ($line -match "firstGen=([0-9]+)") {
                    $maxResultGeneration = [Math]::Max($maxResultGeneration, [uint32]$Matches[1])
                }
                if ($resultProposals -gt 0 -and $line -match "firstStatus=([0-9]+)") {
                    $status = [int]$Matches[1]
                    $hasConsumed = (($status -band 1) -ne 0)
                    $hasProposal = (($status -band 16) -ne 0)
                    $hasUnknownBits = (($status -bor 127) -ne 127)
                    if ($hasConsumed -and $hasProposal -and -not $hasUnknownBits) {
                        $wellFormedResultStatusObserved = $true
                    } else {
                        $malformedResultStatuses += $line
                    }
                }
            }
        }

    if (-not $observedEnabled) {
        throw "$Label did not run with sparse GPU physics enabled"
    }
    if ($maxGpuPackets -le 0 -and $maxResultPackets -le 0) {
        throw "$Label did not stage or read back any GPU physics packets"
    }
    if ($maxResultPackets -le 0 -or $maxResultProposals -lt $MinResultProposals) {
        throw "$Label did not observe enough GPU physics result/proposal readback (maxProposals=$maxResultProposals required=$MinResultProposals)"
    }
    if ($maxResultGeneration -le 0 -or $maxResultChecksum -le 0) {
        throw "$Label did not observe nonzero GPU physics result generation/checksum metadata"
    }
    if ($maxGpuApplyCompleted -lt $MinGpuApplyCompleted -or $maxGpuApplyTotal -lt $MinGpuApplyCompleted) {
        throw "$Label did not observe enough CPU-authoritative application of GPU physics proposals (gpuApply=$maxGpuApplyCompleted/$maxGpuApplyTotal required=$MinGpuApplyCompleted)"
    }
    if ($RequireDiagnosticSeed -and -not $diagnosticSeedObserved) {
        throw "$Label did not observe the sparse material physics diagnostic seed"
    }
    if ($RequireFluidSeed -and -not $fluidSeedObserved) {
        throw "$Label did not observe the sparse fluid physics diagnostic seed"
    }
    if ($ForbidDiagnosticSeed -and $diagnosticSeedObserved) {
        throw "$Label unexpectedly observed the sparse material physics diagnostic seed"
    }
    if ($ForbidFluidSeed -and $fluidSeedObserved) {
        throw "$Label unexpectedly observed the sparse fluid physics diagnostic seed"
    }
    if (-not $wellFormedResultStatusObserved) {
        throw "$Label did not observe a well-formed consumed GPU proposal status"
    }
    if ($malformedResultStatuses.Count -gt 0) {
        $sample = $malformedResultStatuses | Select-Object -First 5
        throw "$Label observed malformed GPU physics proposal status:`n  $($sample -join "`n  ")"
    }
    if ($malformedRetireLines.Count -gt 0) {
        $sample = $malformedRetireLines | Select-Object -First 5
        throw "$Label observed malformed GPU physics result rows dropped at retire:`n  $($sample -join "`n  ")"
    }
    if ($missingBelowLines.Count -gt 0) {
        $sample = $missingBelowLines | Select-Object -First 5
        throw "$Label observed GPU physics proposals missing destination support:`n  $($sample -join "`n  ")"
    }
    if ($MinRejectedProposals -gt 0 -and $maxRejectedProposals -lt $MinRejectedProposals) {
        throw "$Label did not observe enough CPU rejection accounting for stale/conflicting GPU physics proposals (rejects=$maxRejectedProposals required=$MinRejectedProposals)"
    }
    if (-not $AllowRejectedProposals -and $rejectLines.Count -gt 0) {
        $sample = $rejectLines | Select-Object -First 5
        throw "$Label observed CPU rejection of GPU physics proposals in clean smoke:`n  $($sample -join "`n  ")"
    }
    if ($overflowLines.Count -gt 0) {
        $sample = $overflowLines | Select-Object -First 5
        throw "$Label observed sparse GPU physics upload/result overflow:`n  $($sample -join "`n  ")"
    }

    Write-Info "$Label GPU sparse physics observed: gpuPackets=$maxGpuPackets gpuResults=$maxGpuResults resultPackets=$maxResultPackets proposals=$maxResultProposals firstGen=$maxResultGeneration checksum=$maxResultChecksum gpuApply=$maxGpuApplyCompleted/$maxGpuApplyTotal gpuMask=0x$('{0:X}' -f $observedMaterialMask) diagnosticSeed=$diagnosticSeedObserved fluidSeed=$fluidSeedObserved wellFormedStatus=$wellFormedResultStatusObserved malformedRows=0 missingBelow=0 rejects=$maxRejectedProposals"
}

function Assert-AsyncPagePublishFromLog {
    param(
        [string]$Label,
        [string]$SavedLog,
        [int]$MinWaitingFence = 0
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label async page-publish check has no runtime log"
    }

    $maxPending = 0
    $maxReady = 0
    $maxWaitingFrame = 0
    $maxWaitingFence = 0
    $maxLag = 0
    $maxRetry = 0
    $maxStale = 0
    $maxPageEntries = 0
    $sampleCount = 0
    $sawPendingBacklog = $false
    $sawDrainedAfterPending = $false
    Select-String -Path $SavedLog -Pattern "PERF_SPARSE frame=.*publishPending=" |
        ForEach-Object {
            $line = $_.Line
            ++$sampleCount
            if ($line -match "publishPending=([0-9]+)") {
                $pending = [int]$Matches[1]
                $maxPending = [Math]::Max($maxPending, $pending)
                if ($pending -gt 0) {
                    $sawPendingBacklog = $true
                } elseif ($sawPendingBacklog) {
                    $sawDrainedAfterPending = $true
                }
            }
            if ($line -match "publishReady=([0-9]+)") {
                $maxReady = [Math]::Max($maxReady, [int]$Matches[1])
            }
            if ($line -match "publishWait=([0-9]+)/([0-9]+)") {
                $maxWaitingFrame = [Math]::Max($maxWaitingFrame, [int]$Matches[1])
                $maxWaitingFence = [Math]::Max($maxWaitingFence, [int]$Matches[2])
            }
            if ($line -match "publishLag=([0-9]+)") {
                $maxLag = [Math]::Max($maxLag, [int]$Matches[1])
            }
            if ($line -match "publishRetry=([0-9]+)") {
                $maxRetry = [Math]::Max($maxRetry, [int]$Matches[1])
            }
            if ($line -match "publishStale=([0-9]+)") {
                $maxStale = [Math]::Max($maxStale, [int]$Matches[1])
            }
            if ($line -match "pageEntries=([0-9]+)") {
                $maxPageEntries = [Math]::Max($maxPageEntries, [int]$Matches[1])
            }
        }

    if ($sampleCount -le 0) {
        throw "$Label did not emit sparse page-publish telemetry"
    }
    if ($maxPending -le 0) {
        throw "$Label did not queue any delayed page-table publishes"
    }
    if ($maxWaitingFrame -le 0) {
        throw "$Label did not exercise frame-delayed page-table publishes"
    }
    if ($maxWaitingFence -lt $MinWaitingFence) {
        throw "$Label did not exercise enough fence-delayed page-table publishes (waitFence=$maxWaitingFence required=$MinWaitingFence)"
    }
    if ($maxReady -le 0 -and $maxLag -le 0 -and
        (-not $sawDrainedAfterPending -or $maxPageEntries -le 0)) {
        throw "$Label did not observe delayed page-table publishes becoming GPU page-entry work/draining"
    }
    if ($maxRetry -gt 0 -or $maxStale -gt 0) {
        throw "$Label observed page-table publish retry/stale drops (retry=$maxRetry stale=$maxStale)"
    }

    Write-Info "$Label async page-publish observed: samples=$sampleCount pending=$maxPending ready=$maxReady waitFrame=$maxWaitingFrame waitFence=$maxWaitingFence lag=$maxLag pageEntries=$maxPageEntries drained=$sawDrainedAfterPending retry=$maxRetry stale=$maxStale"
}

function Assert-SparseEditPersistenceFile {
    param(
        [string]$Path
    )

    if (-not (Test-Path $Path)) {
        throw "Sparse edit persistence file was not created at $Path"
    }

    $fileInfo = Get-Item $Path
    if ($fileInfo.Length -le 32) {
        throw "Sparse edit persistence file is empty/header-only ($($fileInfo.Length) bytes)"
    }

    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
    try {
        $reader = [System.IO.BinaryReader]::new($stream)
        try {
            $magic = $reader.ReadUInt32()
            $version = $reader.ReadUInt32()
            $brickSize = $reader.ReadUInt32()
            $reserved = $reader.ReadUInt32()
            $overlayCount = $reader.ReadUInt64()
            $totalVoxelCount = $reader.ReadUInt64()

            if ($magic -ne 0x44455356) {
                throw "Sparse edit file magic mismatch: 0x$('{0:X8}' -f $magic)"
            }
            if ($version -ne 1) {
                throw "Sparse edit file version mismatch: $version"
            }
            if ($brickSize -ne 16) {
                throw "Sparse edit file brick size mismatch: $brickSize"
            }
            if ($reserved -ne 0) {
                throw "Sparse edit file reserved field is nonzero: $reserved"
            }
            if ($overlayCount -eq 0 -or $totalVoxelCount -eq 0) {
                throw "Sparse edit file did not persist any overlays/voxels: overlays=$overlayCount voxels=$totalVoxelCount"
            }

            [UInt64]$countedVoxels = 0
            for ([UInt64]$overlayIndex = 0; $overlayIndex -lt $overlayCount; ++$overlayIndex) {
                $null = $reader.ReadInt32()
                $null = $reader.ReadInt32()
                $null = $reader.ReadInt32()
                $revision = $reader.ReadUInt32()
                $voxelCount = $reader.ReadUInt32()
                if ($revision -eq 0) {
                    throw "Sparse edit overlay $overlayIndex has zero revision"
                }
                if ($voxelCount -eq 0 -or $voxelCount -gt 4096) {
                    throw "Sparse edit overlay $overlayIndex has invalid voxel count $voxelCount"
                }
                $seenLocals = [System.Collections.Generic.HashSet[UInt16]]::new()
                for ($voxelIndex = 0; $voxelIndex -lt $voxelCount; ++$voxelIndex) {
                    [UInt16]$localIndex = $reader.ReadUInt16()
                    $null = $reader.ReadUInt32()
                    if ($localIndex -ge 4096) {
                        throw "Sparse edit overlay $overlayIndex has invalid local index $localIndex"
                    }
                    if (-not $seenLocals.Add($localIndex)) {
                        throw "Sparse edit overlay $overlayIndex has duplicate local index $localIndex"
                    }
                }
                $countedVoxels += [UInt64]$voxelCount
            }

            if ($countedVoxels -ne $totalVoxelCount) {
                throw "Sparse edit file voxel count mismatch: header=$totalVoxelCount parsed=$countedVoxels"
            }
            if ($stream.Position -ne $stream.Length) {
                throw "Sparse edit file has trailing bytes: pos=$($stream.Position) length=$($stream.Length)"
            }

            Write-Info "Sparse edit persistence file verified: overlays=$overlayCount voxels=$totalVoxelCount bytes=$($fileInfo.Length)"
        } finally {
            $reader.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

function Convert-MarkdownHeadingToAnchor {
    param(
        [string]$Heading
    )

    $slug = $Heading.Trim().ToLowerInvariant()
    $slug = [regex]::Replace($slug, "``([^``]+)``", '$1')
    $slug = [regex]::Replace($slug, "\[([^\]]+)\]\([^)]+\)", '$1')
    $slug = [regex]::Replace($slug, "[^\p{L}\p{Nd}\s-]", "")
    $slug = [regex]::Replace($slug, "\s+", "-")
    return $slug.Trim("-")
}

function Get-MarkdownAnchorSet {
    param(
        [string]$Path
    )

    $anchors = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $seenCounts = @{}
    $lines = Get-Content -LiteralPath $Path
    foreach ($line in $lines) {
        if ($line -notmatch '^\s{0,3}#{1,6}\s+(.+?)\s*#*\s*$') {
            continue
        }

        $headingText = $Matches[1].Trim()
        $baseSlug = Convert-MarkdownHeadingToAnchor -Heading $headingText
        if ([string]::IsNullOrWhiteSpace($baseSlug)) {
            continue
        }

        if ($seenCounts.ContainsKey($baseSlug)) {
            $seenCounts[$baseSlug] = [int]$seenCounts[$baseSlug] + 1
            $null = $anchors.Add("$baseSlug-$($seenCounts[$baseSlug])")
        } else {
            $seenCounts[$baseSlug] = 0
            $null = $anchors.Add($baseSlug)
        }
    }

    return $anchors
}

function Assert-PngHeader {
    param(
        [string]$Path,
        [string]$Label
    )

    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
    try {
        $header = New-Object byte[] 24
        $bytesRead = $stream.Read($header, 0, $header.Length)
        if ($bytesRead -ne $header.Length) {
            throw "$Label PNG header is truncated: $bytesRead bytes"
        }

        $expectedSignature = [byte[]](0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A)
        for ($i = 0; $i -lt $expectedSignature.Length; ++$i) {
            if ($header[$i] -ne $expectedSignature[$i]) {
                throw "$Label is not a PNG file"
            }
        }

        if ($header[12] -ne [byte][char]'I' -or
            $header[13] -ne [byte][char]'H' -or
            $header[14] -ne [byte][char]'D' -or
            $header[15] -ne [byte][char]'R') {
            throw "$Label PNG is missing the initial IHDR chunk"
        }

        [uint32]$width =
            ([uint32]$header[16] -shl 24) -bor
            ([uint32]$header[17] -shl 16) -bor
            ([uint32]$header[18] -shl 8) -bor
            [uint32]$header[19]
        [uint32]$height =
            ([uint32]$header[20] -shl 24) -bor
            ([uint32]$header[21] -shl 16) -bor
            ([uint32]$header[22] -shl 8) -bor
            [uint32]$header[23]
        if ($width -eq 0 -or $height -eq 0) {
            throw "$Label PNG has invalid dimensions: ${width}x${height}"
        }

        Write-Info "$Label PNG header verified: ${width}x${height}"
    } finally {
        $stream.Dispose()
    }
}

function Get-RepoRelativePath {
    param(
        [string]$RepoRoot,
        [string]$Path
    )

    $rootPath = [System.IO.Path]::GetFullPath($RepoRoot)
    $targetPath = [System.IO.Path]::GetFullPath($Path)
    if (-not $rootPath.EndsWith([System.IO.Path]::DirectorySeparatorChar.ToString())) {
        $rootPath = $rootPath + [System.IO.Path]::DirectorySeparatorChar
    }

    $rootUri = [System.Uri]::new($rootPath)
    $targetUri = [System.Uri]::new($targetPath)
    $relativeUri = $rootUri.MakeRelativeUri($targetUri)
    return [System.Uri]::UnescapeDataString($relativeUri.ToString()).Replace('/', [System.IO.Path]::DirectorySeparatorChar)
}

function Assert-SparseReadbackGuardSources {
    param(
        [string]$RepoRoot
    )

    $guardRequirements = @{
        "VENPOD\src\Graphics\SparseVoxelGpuResources.h" = @(
            "physicsGpuStaleFrameDropsLastRetire",
            "physicsGpuChecksumDropsLastRetire",
            "m_physicsPacketExpectedChecksums",
            "renderOwnerStaleFrameDropsLastRetire",
            "IsSparseVoxelGpuByteRangeInBounds",
            "IsSparseVoxelGpuBrickCopyRangeInBounds"
        )
        "VENPOD\src\Graphics\SparseVoxelGpuResources.cpp" = @(
            "SparsePhysicsPacketChecksum",
            "expectedChecksums[mapped[i].packetIndex]",
            "Sparse physics result dropped stale or mismatched readback rows",
            "Sparse physics diagnostics dropped stale readback payload",
            "payloadFrame != queuedFrame",
            "Sparse render ownership dropped stale readback payload",
            "m_stats.renderOwnerFrameLastRetire = payloadFrame",
            "IsSparseVoxelGpuCopyRangeInBounds",
            "SparseVoxelGpuResources::EmitUploadCopy rejected out-of-bounds"
        )
        "VENPOD\src\Graphics\SparseSurfaceGpuResources.cpp" = @(
            "allocatorBeforeStage",
            "failRangeAllocatorStage",
            "m_faceRangeAllocator = allocatorBeforeStage",
            "restoreStagedStateFromTicket",
            "ticket.rangeAllocatorBeforeStage = allocatorBeforeStage",
            "IsSparseSurfaceGpuFaceCopyRegionInBounds",
            "SparseSurfaceGpuResources::EmitCopy rejected out-of-bounds"
        )
        "VENPOD\src\Graphics\SparseSurfaceGpuResources.h" = @(
            "hasRangeAllocatorRollback",
            "rangeAllocatorBeforeStage",
            "hasUploadWriteOffsetRollback",
            "IsSparseSurfaceGpuBufferCopyRegionInBounds",
            "IsSparseSurfaceCullStatsReadbackRetirable",
            "m_cullStatsReadbackQueuedFrames"
        )
        "VENPOD\src\Simulation\VoxelWorld.h" = @(
            "IsVoxelRaycastReadbackRetirable",
            "DecodeVoxelRaycastPackedWord",
            "m_brushRaycastReadbackQueuedFrame",
            "m_groundRaycastReadbackQueuedFrame"
        )
        "VENPOD\src\Simulation\VoxelWorld.cpp" = @(
            "DecodeVoxelRaycastPackedWord(data[3])",
            "m_brushRaycastReadbackReady[slotIndex] = false",
            "m_groundRaycastReadbackReady[slotIndex] = false",
            "InvalidVoxelRaycastReadbackFrame()"
        )
        "VENPOD\src\main_launcher.cpp" = @(
            "RetireBrushRaycastReadback(static_cast<uint32_t>(frameCount))",
            "RetireGroundRaycastReadback(static_cast<uint32_t>(frameCount))",
            "QueueBrushRaycastReadback(commandList.Get(), static_cast<uint32_t>(frameCount))",
            "QueueGroundRaycastReadback(commandList.Get(), static_cast<uint32_t>(frameCount))"
        )
        "VENPOD\src\Simulation\SparseEditStore.cpp" = @(
            "IsSparseEditPersistencePathAllowed",
            'extension == ".vsed"',
            "!IsSparseEditPersistencePathAllowed(path)"
        )
        "VENPOD\src\Graphics\Renderer.cpp" = @(
            "struct FrameConstantsCpu",
            "static_assert(sizeof(FrameConstantsCpu) == 352)",
            "offsetof(FrameConstantsCpu, farFieldGridParams) == 208u",
            "offsetof(FrameConstantsCpu, sparseNearParams) == 224u",
            "offsetof(FrameConstantsCpu, farOwnershipParams) == 320u",
            "offsetof(FrameConstantsCpu, exactNearParams) == 336u",
            "SetGraphicsRootDescriptorTable(18",
            "D3D12_DESCRIPTOR_RANGE_TYPE_UAV"
        )
        "VENPOD\src\Graphics\RHI\GPUBuffer.h" = @(
            "m_pendingInitializationUpload"
        )
        "VENPOD\src\Graphics\RHI\GPUBuffer.cpp" = @(
            "m_pendingInitializationUpload = uploadBuffer",
            "The command list may execute after this helper returns",
            "m_pendingInitializationUpload.Reset()"
        )
        "VENPOD\assets\shaders\Common\SharedTypes.hlsli" = @(
            "struct FrameConstants",
            "float4   farFieldGridParams",
            "ownership stats flag",
            "float4   sparseNearParams",
            "float4   farOwnershipParams"
        )
        "VENPOD\src\Simulation\SparseSurfaceCache.h" = @(
            "struct SparseSurfaceDrawArgs",
            "static_assert(sizeof(SparseSurfaceDrawArgs) == 20)",
            "struct SparseSurfaceRecord",
            "static_assert(sizeof(SparseSurfaceRecord) == 52)",
            "struct SparseSurfaceClusterRecord",
            "static_assert(sizeof(SparseSurfaceClusterRecord) == 40)"
        )
        "VENPOD\assets\shaders\Compute\CS_SparseSurfaceCullCompact.hlsl" = @(
            "struct SparseSurfaceRecord",
            "int3 coord",
            "uint generation",
            "int3 minVoxel",
            "struct SparseSurfaceDrawArgs",
            "int baseVertexLocation",
            "struct SparseSurfaceClusterRecord",
            "uint firstRecord"
        )
        "VENPOD\assets\shaders\Graphics\VS_SparseSurface.hlsl" = @(
            "struct SparseSurfaceRecord",
            "int3 coord",
            "uint generation",
            "int3 minVoxel",
            "struct SparseSurfaceClusterRecord",
            "uint firstRecord",
            "StructuredBuffer<SparseSurfaceClusterRecord>"
        )
    }
    foreach ($relativePath in $guardRequirements.Keys) {
        $absolutePath = Join-Path $RepoRoot $relativePath
        $sourceText = Get-Content -LiteralPath $absolutePath -Raw
        foreach ($requiredSnippet in $guardRequirements[$relativePath]) {
            if ($sourceText.IndexOf($requiredSnippet, [System.StringComparison]::Ordinal) -lt 0) {
                throw "Sparse readback guard is missing '$requiredSnippet' in $relativePath"
            }
        }
    }

    $forbiddenRequirements = @{
        "VENPOD\src\Graphics\Renderer.cpp" = @(
            "RootParamType::Constants32Bit"
        )
    }
    foreach ($relativePath in $forbiddenRequirements.Keys) {
        $absolutePath = Join-Path $RepoRoot $relativePath
        $sourceText = Get-Content -LiteralPath $absolutePath -Raw
        foreach ($forbiddenSnippet in $forbiddenRequirements[$relativePath]) {
            if ($sourceText.IndexOf($forbiddenSnippet, [System.StringComparison]::Ordinal) -ge 0) {
                throw "Sparse readback guard found stale forbidden snippet '$forbiddenSnippet' in $relativePath"
            }
        }
    }
}

function Assert-PublicReviewDocs {
    param(
        [string]$RepoRoot,
        [string]$ProjectRoot
    )

    $requiredArtifacts = @(
        "README.md",
        "LICENSE",
        "refactor.md",
        "docs\index.md",
        "docs\COMPLETION_LEDGER.md",
        "docs\explanation\architecture.md",
        "docs\reference\sparse-refactor-review.md",
        "docs\reference\sparse-completion-audit.md",
        "docs\reference\public-review-manifest.md",
        "docs\reference\asset-credits.md",
        "docs\reference\runtime.md",
        "docs\how-to\use-the-sandbox.md",
        "docs\how-to\capture-public-demo.md",
        "docs\media\sparse-engine-contact-sheet.png",
        "VENPOD\setup.ps1",
        "VENPOD\build.ps1",
        "VENPOD\run.ps1",
        "VENPOD\rebrun.ps1",
        "VENPOD\clean.ps1",
        "VENPOD\sparse_regression.ps1",
        "VENPOD\engine_capture_smoke.ps1",
        "VENPOD\public_demo_capture.ps1",
        "VENPOD\visual_review_capture.ps1"
    )
    $requiredPublicScripts = Get-ChildItem -LiteralPath (Join-Path $RepoRoot "VENPOD") -File -Filter "*.ps1" |
        ForEach-Object { Get-RepoRelativePath -RepoRoot $RepoRoot -Path $_.FullName } |
        Sort-Object

    foreach ($relativePath in $requiredArtifacts) {
        $absolutePath = Join-Path $RepoRoot $relativePath
        if (-not (Test-Path -LiteralPath $absolutePath)) {
            throw "Public review artifact is missing: $relativePath"
        }
        $item = Get-Item -LiteralPath $absolutePath
        if (-not $item.PSIsContainer -and $item.Length -le 0) {
            throw "Public review artifact is empty: $relativePath"
        }
    }
    Assert-CompletionLedgerStatusCounts -Path (Join-Path $RepoRoot "docs\COMPLETION_LEDGER.md")

    foreach ($relativePath in $requiredPublicScripts) {
        $absolutePath = Join-Path $RepoRoot $relativePath
        $tokens = $null
        $parseErrors = $null
        $null = [System.Management.Automation.Language.Parser]::ParseFile(
            $absolutePath,
            [ref]$tokens,
            [ref]$parseErrors)
        if ($parseErrors -and $parseErrors.Count -gt 0) {
            $firstErrors = $parseErrors |
                Select-Object -First 5 |
                ForEach-Object {
                    "$($_.Extent.StartLineNumber):$($_.Extent.StartColumnNumber) $($_.Message)"
                }
            throw "Public review script parse check failed for ${relativePath}:`n  $($firstErrors -join "`n  ")"
        }
    }
    $captureScriptGuardRequirements = @{
        "VENPOD\setup.ps1" = @(
            "function Assert-ProjectChildPath",
            "function Remove-ProjectChildPath",
            "venpod-imgui-",
            "Remove-Item -LiteralPath"
        )
        "VENPOD\clean.ps1" = @(
            "function Assert-ProjectCleanPath",
            "function Remove-ProjectCleanPath",
            "Refusing to clean",
            "Remove-Item -LiteralPath"
        )
        "VENPOD\sparse_regression.ps1" = @(
            "function Assert-OwnershipDebugPairs",
            "function Assert-SparseOnlyDefaultFromLog",
            "SkipRenderSmoke",
            "voxel-only terrain; procedural mid/far height and far-water fallback disabled",
            "function Assert-AsyncPagePublishFromLog",
            "VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES",
            "VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES",
            "sparse_async_page_publish_fence_smoke",
            "sparse_async_page_publish_long_walk_capture",
            "publishRetry",
            "publishStale",
            "sparse_brush_feedback_moving_strict_resident_smoke",
            "sparse_brush_paint_moving_smoke",
            "sparse_brush_paint_nonresident_smoke",
            "sparse_brush_dome_engine_capture",
            "SkipBrushDomeEngineCaptureSmoke",
            "EnableBrushInput",
            "sparse_brush_paint_gpu_physics_smoke",
            "RequireNonresidentRetry",
            "RequireMovingPath",
            "sparse_physics_strict_mixed_smoke",
            "sparse_physics_strict_long_walk_capture",
            "sparse_physics_strict_stress_capture",
            "MinGpuApplyTotal",
            "ForbidDiagnosticSeed",
            "ForbidFluidSeed",
            "ownership_pair_review.csv",
            "MaxDebugFarFallbackPct",
            "MaxDebugHeightProxyPct",
            "debugOwnerFarFallbackPct",
            "debugOwnerHeightProxyPct",
            "dense compatibility VoxelWorld",
            "Assert-CompletionLedgerStatusCounts"
        )
        "VENPOD\engine_capture_smoke.ps1" = @(
            "function Assert-SafeCaptureOutputDir",
            "function Test-IsPathUnder",
            "build/captures",
            "build/logs",
            "runtime binary tree",
            "SparseBrushPaintSmoke",
            "BrushRadiusTenths",
            "MinOverlayBrushPct",
            "Clear-EngineCaptureArtifacts"
        )
        "VENPOD\public_demo_capture.ps1" = @(
            "function Assert-CaptureParameters",
            "function Assert-SafeCaptureOutputDir",
            "function Test-IsPathUnder",
            "build/captures",
            "build/logs",
            "runtime binary tree",
            "ReviewReel",
            "PUBLIC_DEMO_REVIEW_REEL.md",
            "sparse-public-review-reel.mp4"
        )
        "VENPOD\visual_review_capture.ps1" = @(
            "function Assert-SafeOutputDir",
            "function Invoke-CaptureScenario",
            "function Write-VisualReviewSummary",
            "function Get-RenderOwnershipSummary",
            "function Test-IsPathUnder",
            "build/captures",
            "build/logs",
            "long-walk",
            "fast-water-transition",
            "long-fast-water-transition",
            "VISUAL_REVIEW_CHECKLIST.md",
            "VISUAL_REVIEW_SUMMARY.csv",
            "MaxHeightProxyObservedPct",
            "MaxFarSvoObservedPct",
            "MaxUnsafeNearMissPixels"
        )
    }
    foreach ($relativePath in $captureScriptGuardRequirements.Keys) {
        $absolutePath = Join-Path $RepoRoot $relativePath
        $scriptText = Get-Content -LiteralPath $absolutePath -Raw
        foreach ($requiredSnippet in $captureScriptGuardRequirements[$relativePath]) {
            if ($scriptText.IndexOf($requiredSnippet, [System.StringComparison]::Ordinal) -lt 0) {
                throw "Public review capture script guard is missing '$requiredSnippet' in $relativePath"
            }
        }
    }
    Assert-SparseReadbackGuardSources -RepoRoot $RepoRoot

    $trackedContactSheet = Join-Path $RepoRoot "docs\media\sparse-engine-contact-sheet.png"
    $contactSheet = Get-Item -LiteralPath $trackedContactSheet
    if ($contactSheet.Length -lt 1024) {
        throw "Tracked sparse contact sheet is too small to be useful: $($contactSheet.Length) bytes"
    }
    Assert-PngHeader -Path $trackedContactSheet -Label "Tracked sparse contact sheet"

    $gitIgnorePath = Join-Path $RepoRoot ".gitignore"
    if (-not (Test-Path -LiteralPath $gitIgnorePath)) {
        throw ".gitignore is missing"
    }
    $gitIgnoreText = Get-Content -LiteralPath $gitIgnorePath -Raw
    $requiredIgnorePatterns = @(
        "VENPOD/build/",
        "VENPOD/build/logs/",
        "*.vsed",
        "venpod_far_svo_cache_*.bin"
    )
    foreach ($pattern in $requiredIgnorePatterns) {
        if ($gitIgnoreText -notmatch [regex]::Escape($pattern)) {
            throw ".gitignore does not include required generated-artifact pattern: $pattern"
        }
    }
    $gitExe = Get-Command git -ErrorAction SilentlyContinue
    if (-not $gitExe) {
        throw "git is required for public review generated-artifact ignore verification"
    }
    $publicSourceArtifacts =
        @($requiredArtifacts + $requiredPublicScripts) |
        Sort-Object -Unique
    foreach ($sourcePath in $publicSourceArtifacts) {
        & git -C $RepoRoot ls-files --error-unmatch -- $sourcePath | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "Required public-review source artifact is not tracked or staged in git: $sourcePath"
        }
        & git -C $RepoRoot check-ignore --quiet -- $sourcePath
        if ($LASTEXITCODE -eq 0) {
            throw "Required public-review source artifact is ignored by git: $sourcePath"
        }
        if ($LASTEXITCODE -ne 1) {
            throw "Could not verify git visibility for public-review source artifact: $sourcePath"
        }
    }
    $generatedIgnoreProbes = @(
        "VENPOD/build/logs/sparse_render_smoke.log",
        "VENPOD/build/captures/public_demo/sparse-public-demo.mp4",
        "VENPOD/build/logs/sparse_surface_edits.vsed",
        "VENPOD/build/bin/venpod_runtime.log",
        "VENPOD/venpod_far_svo_cache_r4_d6_s12345.bin"
    )
    foreach ($probePath in $generatedIgnoreProbes) {
        & git -C $RepoRoot check-ignore --quiet -- $probePath
        if ($LASTEXITCODE -ne 0) {
            throw "Generated public-review artifact is not ignored by git: $probePath"
        }
    }

    $markdownFiles = @()
    $markdownFiles += Get-Item -LiteralPath (Join-Path $RepoRoot "README.md")
    $markdownFiles += Get-Item -LiteralPath (Join-Path $RepoRoot "refactor.md")
    $markdownFiles += Get-ChildItem -LiteralPath (Join-Path $RepoRoot "docs") -Recurse -File -Filter "*.md"
    $brokenLinks = New-Object System.Collections.Generic.List[string]
    $brokenAnchors = New-Object System.Collections.Generic.List[string]
    $anchorCache = @{}
    $linkPattern = [regex]'\[[^\]]+\]\(([^)]+)\)'
    $docsIndexPath = Join-Path $RepoRoot "docs\index.md"
    $docsIndexTargets = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $docsIndexText = Get-Content -LiteralPath $docsIndexPath -Raw
    foreach ($match in $linkPattern.Matches($docsIndexText)) {
        $target = $match.Groups[1].Value.Trim()
        if ([string]::IsNullOrWhiteSpace($target) -or
            $target.StartsWith("#") -or
            $target -match '^[a-zA-Z][a-zA-Z0-9+.-]*:' -or
            $target.StartsWith("mailto:")) {
            continue
        }

        $targetPath = $target.Split([char[]]@('#'), 2)[0].Trim()
        if ([string]::IsNullOrWhiteSpace($targetPath)) {
            continue
        }
        $targetPath = $targetPath.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
        $resolvedTarget = Join-Path (Split-Path $docsIndexPath -Parent) $targetPath
        if (Test-Path -LiteralPath $resolvedTarget) {
            $null = $docsIndexTargets.Add((Get-RepoRelativePath -RepoRoot $RepoRoot -Path $resolvedTarget))
        }
    }

    $missingDocsIndexLinks = New-Object System.Collections.Generic.List[string]
    foreach ($markdownFile in $markdownFiles) {
        if (-not $markdownFile.FullName.StartsWith((Join-Path $RepoRoot "docs"), [System.StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        if ($markdownFile.FullName.Equals($docsIndexPath, [System.StringComparison]::OrdinalIgnoreCase)) {
            continue
        }

        $relativeDoc = Get-RepoRelativePath -RepoRoot $RepoRoot -Path $markdownFile.FullName
        if (-not $docsIndexTargets.Contains($relativeDoc)) {
            $missingDocsIndexLinks.Add($relativeDoc)
        }
    }
    if ($missingDocsIndexLinks.Count -gt 0) {
        $sample = $missingDocsIndexLinks | Select-Object -First 20
        throw "docs/index.md is missing document map links:`n  $($sample -join "`n  ")"
    }

    foreach ($markdownFile in $markdownFiles) {
        $text = Get-Content -LiteralPath $markdownFile.FullName -Raw
        foreach ($match in $linkPattern.Matches($text)) {
            $target = $match.Groups[1].Value.Trim()
            if ([string]::IsNullOrWhiteSpace($target) -or
                $target -match '^[a-zA-Z][a-zA-Z0-9+.-]*:' -or
                $target.StartsWith("mailto:")) {
                continue
            }

            $targetParts = $target.Split([char[]]@('#'), 2)
            $targetPath = $targetParts[0].Trim()
            $targetAnchor = ""
            if ($targetParts.Count -gt 1) {
                $targetAnchor = [System.Uri]::UnescapeDataString($targetParts[1].Trim())
            }

            $resolvedTarget = $markdownFile.FullName
            if (-not [string]::IsNullOrWhiteSpace($targetPath)) {
                $targetPath = $targetPath.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
                $resolvedTarget = Join-Path $markdownFile.DirectoryName $targetPath
            }

            if (-not (Test-Path -LiteralPath $resolvedTarget)) {
                $relativeMarkdown = Get-RepoRelativePath -RepoRoot $RepoRoot -Path $markdownFile.FullName
                $brokenLinks.Add("$relativeMarkdown -> $target")
                continue
            }

            if (-not [string]::IsNullOrWhiteSpace($targetAnchor) -and
                [System.IO.Path]::GetExtension($resolvedTarget).Equals(".md", [System.StringComparison]::OrdinalIgnoreCase)) {
                $normalizedAnchor = Convert-MarkdownHeadingToAnchor -Heading $targetAnchor
                if (-not $anchorCache.ContainsKey($resolvedTarget)) {
                    $anchorCache[$resolvedTarget] = Get-MarkdownAnchorSet -Path $resolvedTarget
                }
                if (-not $anchorCache[$resolvedTarget].Contains($normalizedAnchor)) {
                    $relativeMarkdown = Get-RepoRelativePath -RepoRoot $RepoRoot -Path $markdownFile.FullName
                    $relativeTarget = Get-RepoRelativePath -RepoRoot $RepoRoot -Path $resolvedTarget
                    $brokenAnchors.Add("$relativeMarkdown -> $target (missing #$normalizedAnchor in $relativeTarget)")
                }
            }
        }
    }

    if ($brokenLinks.Count -gt 0) {
        $sample = $brokenLinks | Select-Object -First 20
        throw "Public review markdown link check failed:`n  $($sample -join "`n  ")"
    }
    if ($brokenAnchors.Count -gt 0) {
        $sample = $brokenAnchors | Select-Object -First 20
        throw "Public review markdown anchor check failed:`n  $($sample -join "`n  ")"
    }

    Write-Info "Public review docs verified: $($requiredArtifacts.Count) artifacts, $($requiredPublicScripts.Count) scripts, $($markdownFiles.Count) markdown files, $($publicSourceArtifacts.Count) tracked/staged source artifacts"
}

$projectRoot = $PSScriptRoot
$repoRoot = Split-Path -Parent $projectRoot
$buildScript = Join-Path $projectRoot "build.ps1"
$rebrunScript = Join-Path $projectRoot "rebrun.ps1"
$engineCaptureScript = Join-Path $projectRoot "engine_capture_smoke.ps1"
$publicDemoCaptureScript = Join-Path $projectRoot "public_demo_capture.ps1"
$buildDir = Join-Path $projectRoot "build"

if (-not (Test-Path $buildScript)) {
    throw "build.ps1 not found at $buildScript"
}
if (-not (Test-Path $rebrunScript)) {
    throw "rebrun.ps1 not found at $rebrunScript"
}
if (-not (Test-Path $engineCaptureScript)) {
    throw "engine_capture_smoke.ps1 not found at $engineCaptureScript"
}
if (-not (Test-Path $publicDemoCaptureScript)) {
    throw "public_demo_capture.ps1 not found at $publicDemoCaptureScript"
}

if (-not $SkipEngineCaptureSmoke) {
    Assert-CaptureWindowParameters `
        -Label "Engine capture smoke" `
        -ExitAfterFrames $EngineCaptureExitAfterFrames `
        -CaptureStartFrame $EngineCaptureStartFrame `
        -CaptureIntervalFrames $EngineCaptureIntervalFrames `
        -CaptureCount $EngineCaptureCount
}
if (-not $SkipStressEngineCaptureSmoke) {
    Assert-CaptureWindowParameters `
        -Label "Stress engine capture smoke" `
        -ExitAfterFrames $StressEngineCaptureExitAfterFrames `
        -CaptureStartFrame $StressEngineCaptureStartFrame `
        -CaptureIntervalFrames $StressEngineCaptureIntervalFrames `
        -CaptureCount $StressEngineCaptureCount
}
if (-not $SkipSkylineEngineCaptureSmoke) {
    Assert-CaptureWindowParameters `
        -Label "Skyline engine capture smoke" `
        -ExitAfterFrames $SkylineEngineCaptureExitAfterFrames `
        -CaptureStartFrame $SkylineEngineCaptureStartFrame `
        -CaptureIntervalFrames $SkylineEngineCaptureIntervalFrames `
        -CaptureCount $SkylineEngineCaptureCount
}
if (-not $SkipFastFlightEngineCaptureSmoke) {
    Assert-CaptureWindowParameters `
        -Label "Fast-flight engine capture smoke" `
        -ExitAfterFrames $FastFlightEngineCaptureExitAfterFrames `
        -CaptureStartFrame $FastFlightEngineCaptureStartFrame `
        -CaptureIntervalFrames $FastFlightEngineCaptureIntervalFrames `
        -CaptureCount $FastFlightEngineCaptureCount
}
if (-not $SkipFastWaterTransitionEngineCaptureSmoke) {
    Assert-CaptureWindowParameters `
        -Label "Fast water-transition engine capture smoke" `
        -ExitAfterFrames $FastWaterTransitionEngineCaptureExitAfterFrames `
        -CaptureStartFrame $FastWaterTransitionEngineCaptureStartFrame `
        -CaptureIntervalFrames $FastWaterTransitionEngineCaptureIntervalFrames `
        -CaptureCount $FastWaterTransitionEngineCaptureCount
}
if (-not $SkipWalkEngineCaptureSmoke) {
    Assert-CaptureWindowParameters `
        -Label "Walk engine capture smoke" `
        -ExitAfterFrames $WalkEngineCaptureExitAfterFrames `
        -CaptureStartFrame $WalkEngineCaptureStartFrame `
        -CaptureIntervalFrames $WalkEngineCaptureIntervalFrames `
        -CaptureCount $WalkEngineCaptureCount
}
if (-not $SkipTerrainGapEngineCaptureSmoke) {
    Assert-CaptureWindowParameters `
        -Label "Terrain gap engine capture smoke" `
        -ExitAfterFrames $TerrainGapEngineCaptureExitAfterFrames `
        -CaptureStartFrame $TerrainGapEngineCaptureStartFrame `
        -CaptureIntervalFrames $TerrainGapEngineCaptureIntervalFrames `
        -CaptureCount $TerrainGapEngineCaptureCount
}
if (-not $SkipBrushDomeEngineCaptureSmoke) {
    Assert-CaptureWindowParameters `
        -Label "Brush dome engine capture smoke" `
        -ExitAfterFrames $BrushDomeEngineCaptureExitAfterFrames `
        -CaptureStartFrame $BrushDomeEngineCaptureStartFrame `
        -CaptureIntervalFrames $BrushDomeEngineCaptureIntervalFrames `
        -CaptureCount $BrushDomeEngineCaptureCount
}
if (-not $SkipLongWalkEngineCaptureSmoke) {
    Assert-CaptureWindowParameters `
        -Label "Long walk engine capture smoke" `
        -ExitAfterFrames $LongWalkEngineCaptureExitAfterFrames `
        -CaptureStartFrame $LongWalkEngineCaptureStartFrame `
        -CaptureIntervalFrames $LongWalkEngineCaptureIntervalFrames `
        -CaptureCount $LongWalkEngineCaptureCount
}
if (-not $SkipAsyncPagePublishSmoke) {
    Assert-CaptureWindowParameters `
        -Label "Async page-publish walk capture smoke" `
        -ExitAfterFrames $AsyncPagePublishWalkExitAfterFrames `
        -CaptureStartFrame $AsyncPagePublishWalkStartFrame `
        -CaptureIntervalFrames $AsyncPagePublishWalkIntervalFrames `
        -CaptureCount $AsyncPagePublishWalkCount
    Assert-CaptureWindowParameters `
        -Label "Async page-publish long walk capture smoke" `
        -ExitAfterFrames $AsyncPagePublishLongWalkExitAfterFrames `
        -CaptureStartFrame $AsyncPagePublishLongWalkStartFrame `
        -CaptureIntervalFrames $AsyncPagePublishLongWalkIntervalFrames `
        -CaptureCount $AsyncPagePublishLongWalkCount
}
if (-not $SkipWaterlineEngineCaptureSmoke) {
    Assert-CaptureWindowParameters `
        -Label "Waterline engine capture smoke" `
        -ExitAfterFrames $WaterlineEngineCaptureExitAfterFrames `
        -CaptureStartFrame $WaterlineEngineCaptureStartFrame `
        -CaptureIntervalFrames $WaterlineEngineCaptureIntervalFrames `
        -CaptureCount $WaterlineEngineCaptureCount
    Assert-CaptureWindowParameters `
        -Label "Long waterline engine capture smoke" `
        -ExitAfterFrames $LongWaterlineEngineCaptureExitAfterFrames `
        -CaptureStartFrame $LongWaterlineEngineCaptureStartFrame `
        -CaptureIntervalFrames $LongWaterlineEngineCaptureIntervalFrames `
        -CaptureCount $LongWaterlineEngineCaptureCount
}
if (-not $SkipGpuPhysicsStrictSmoke) {
    Assert-CaptureWindowParameters `
        -Label "GPU physics strict movement capture" `
        -ExitAfterFrames $GpuPhysicsStrictCaptureExitAfterFrames `
        -CaptureStartFrame $GpuPhysicsStrictCaptureStartFrame `
        -CaptureIntervalFrames $GpuPhysicsStrictCaptureIntervalFrames `
        -CaptureCount $GpuPhysicsStrictCaptureCount
    Assert-CaptureWindowParameters `
        -Label "GPU physics strict stress-camera capture" `
        -ExitAfterFrames $GpuPhysicsStrictStressExitAfterFrames `
        -CaptureStartFrame $GpuPhysicsStrictStressStartFrame `
        -CaptureIntervalFrames $GpuPhysicsStrictStressIntervalFrames `
        -CaptureCount $GpuPhysicsStrictStressCount
    Assert-CaptureWindowParameters `
        -Label "GPU physics strict long-walk capture" `
        -ExitAfterFrames $GpuPhysicsStrictLongWalkExitAfterFrames `
        -CaptureStartFrame $GpuPhysicsStrictLongWalkStartFrame `
        -CaptureIntervalFrames $GpuPhysicsStrictLongWalkIntervalFrames `
        -CaptureCount $GpuPhysicsStrictLongWalkCount
}
if (-not $SkipPublicDemoCapture) {
    Assert-PublicDemoCaptureParameters `
        -CaptureStartFrame $PublicDemoCaptureStartFrame `
        -CaptureFrames $PublicDemoCaptureFrames `
        -PlaybackFps $PublicDemoPlaybackFps
}

$RenderExitAfterFrames = Normalize-MinFrameBudget `
    -Name "Render smoke frame budget" `
    -Value $RenderExitAfterFrames `
    -Minimum 240
$PhysicsExitAfterFrames = Normalize-MinFrameBudget `
    -Name "GPU physics smoke frame budget" `
    -Value $PhysicsExitAfterFrames `
    -Minimum 240
if (-not $SkipDefaultPhysicsSmoke) {
    $DefaultPhysicsExitAfterFrames = Normalize-MinFrameBudget `
        -Name "Default local physics smoke frame budget" `
        -Value $DefaultPhysicsExitAfterFrames `
        -Minimum 240
}
if (-not $SkipAsyncPagePublishSmoke) {
    $AsyncPagePublishExitAfterFrames = Normalize-MinFrameBudget `
        -Name "Async page-publish smoke frame budget" `
        -Value $AsyncPagePublishExitAfterFrames `
        -Minimum 660
    $AsyncPagePublishFenceExitAfterFrames = Normalize-MinFrameBudget `
        -Name "Async page-publish fence-stress frame budget" `
        -Value $AsyncPagePublishFenceExitAfterFrames `
        -Minimum 420
    $AsyncPagePublishLongWalkExitAfterFrames = Normalize-MinFrameBudget `
        -Name "Async page-publish long walk frame budget" `
        -Value $AsyncPagePublishLongWalkExitAfterFrames `
        -Minimum 1320
}
if (-not $SkipDenseLegacySmoke) {
    $DenseLegacyExitAfterFrames = Normalize-MinFrameBudget `
        -Name "Dense legacy smoke frame budget" `
        -Value $DenseLegacyExitAfterFrames `
        -Minimum 60
}
if (-not $SkipFlickerSmoke) {
    $FlickerExitAfterFrames = Normalize-MinFrameBudget `
        -Name "Flicker smoke frame budget" `
        -Value $FlickerExitAfterFrames `
        -Minimum 180
}
if (-not $SkipSurfaceSmoke) {
    $SurfaceExitAfterFrames = Normalize-MinFrameBudget `
        -Name "Seeded-surface smoke frame budget" `
        -Value $SurfaceExitAfterFrames `
        -Minimum 240
}
if (-not $SkipGpuRaycastSmoke) {
    $GpuRaycastExitAfterFrames = Normalize-MinFrameBudget `
        -Name "GPU raycast smoke frame budget" `
        -Value $GpuRaycastExitAfterFrames `
        -Minimum 300
}
if (-not $SkipMissFeedbackSmoke) {
    $MissFeedbackExitAfterFrames = Normalize-MinFrameBudget `
        -Name "Miss-feedback smoke frame budget" `
        -Value $MissFeedbackExitAfterFrames `
        -Minimum 240
}
if (-not $SkipBrushFeedbackSmoke) {
    $BrushFeedbackExitAfterFrames = Normalize-MinFrameBudget `
        -Name "Brush-feedback smoke frame budget" `
        -Value $BrushFeedbackExitAfterFrames `
        -Minimum 360
}
if (-not $SkipBrushFeedbackApplySmoke) {
    $BrushFeedbackApplyExitAfterFrames = Normalize-MinFrameBudget `
        -Name "Brush-feedback apply smoke frame budget" `
        -Value $BrushFeedbackApplyExitAfterFrames `
        -Minimum 360
}
if (-not $SkipBrushFeedbackAuthoritativeSmoke) {
    $BrushFeedbackAuthoritativeExitAfterFrames = Normalize-MinFrameBudget `
        -Name "Brush-feedback authoritative smoke frame budget" `
        -Value $BrushFeedbackAuthoritativeExitAfterFrames `
        -Minimum 390
}
if (-not $SkipBrushFeedbackStrictResidentSmoke) {
    $BrushFeedbackStrictResidentExitAfterFrames = Normalize-MinFrameBudget `
        -Name "Brush-feedback strict resident-only smoke frame budget" `
        -Value $BrushFeedbackStrictResidentExitAfterFrames `
        -Minimum 840
}
if (-not $SkipBrushFeedbackMovingSmoke) {
    $BrushFeedbackMovingExitAfterFrames = Normalize-MinFrameBudget `
        -Name "Brush-feedback moving smoke frame budget" `
        -Value $BrushFeedbackMovingExitAfterFrames `
        -Minimum 1050
}
if (-not $SkipBrushPaintSmoke) {
    $BrushPaintExitAfterFrames = Normalize-MinFrameBudget `
        -Name "Brush paint smoke frame budget" `
        -Value $BrushPaintExitAfterFrames `
        -Minimum 600
    $BrushPaintMovingExitAfterFrames = Normalize-MinFrameBudget `
        -Name "Brush paint moving smoke frame budget" `
        -Value $BrushPaintMovingExitAfterFrames `
        -Minimum 600
    $BrushPaintNonresidentExitAfterFrames = Normalize-MinFrameBudget `
        -Name "Brush paint nonresident smoke frame budget" `
        -Value $BrushPaintNonresidentExitAfterFrames `
        -Minimum 900
    $BrushPaintGpuPhysicsExitAfterFrames = Normalize-MinFrameBudget `
        -Name "Brush paint GPU-physics smoke frame budget" `
        -Value $BrushPaintGpuPhysicsExitAfterFrames `
        -Minimum 900
}
if (-not $SkipStartupEngineCaptureSmoke) {
    Assert-CaptureWindowParameters `
        -Label "Startup engine capture smoke" `
        -ExitAfterFrames $StartupEngineCaptureExitAfterFrames `
        -CaptureStartFrame $StartupEngineCaptureStartFrame `
        -CaptureIntervalFrames $StartupEngineCaptureIntervalFrames `
        -CaptureCount $StartupEngineCaptureCount
}

Write-Host "VENPOD - Sparse Regression Gate" -ForegroundColor Magenta
Write-Info "Config: $Config"
Write-Info "Render smoke frames: $RenderExitAfterFrames"
Write-Info "Physics smoke frames: $PhysicsExitAfterFrames"
if (-not $SkipDefaultPhysicsSmoke) {
    Write-Info "Default local physics smoke frames: $DefaultPhysicsExitAfterFrames"
}
if (-not $SkipDenseLegacySmoke) {
    Write-Info "Dense legacy smoke frames: $DenseLegacyExitAfterFrames"
}
if (-not $SkipFlickerSmoke) {
    Write-Info "Flicker smoke frames: $FlickerExitAfterFrames"
}
if (-not $SkipSurfaceSmoke) {
    Write-Info "Surface smoke frames: $SurfaceExitAfterFrames"
    Write-Info "Surface smoke sparse edit persistence: enabled"
}
if (-not $SkipEditUiPersistenceSmoke) {
    Write-Info "Sparse edit UI persistence smoke: enabled"
}
if (-not $SkipGpuRaycastSmoke) {
    Write-Info "GPU raycast smoke frames: $GpuRaycastExitAfterFrames"
}
if (-not $SkipAsyncPagePublishSmoke) {
    Write-Info "Async page-publish smoke frames: $AsyncPagePublishExitAfterFrames"
    Write-Info "Async page-publish fence-stress frames: $AsyncPagePublishFenceExitAfterFrames"
    Write-Info "Async page-publish walk samples: count=$AsyncPagePublishWalkCount start=$AsyncPagePublishWalkStartFrame interval=$AsyncPagePublishWalkIntervalFrames"
    Write-Info "Async page-publish long walk smoke frames: $AsyncPagePublishLongWalkExitAfterFrames"
    Write-Info "Async page-publish long walk samples: count=$AsyncPagePublishLongWalkCount start=$AsyncPagePublishLongWalkStartFrame interval=$AsyncPagePublishLongWalkIntervalFrames"
}
if (-not $SkipMissFeedbackSmoke) {
    Write-Info "Miss feedback smoke frames: $MissFeedbackExitAfterFrames"
}
if (-not $SkipBrushFeedbackSmoke) {
    Write-Info "Brush feedback smoke frames: $BrushFeedbackExitAfterFrames"
}
if (-not $SkipBrushFeedbackApplySmoke) {
    Write-Info "Brush feedback apply smoke frames: $BrushFeedbackApplyExitAfterFrames"
}
if (-not $SkipBrushFeedbackAuthoritativeSmoke) {
    Write-Info "Brush feedback authoritative smoke frames: $BrushFeedbackAuthoritativeExitAfterFrames"
}
if (-not $SkipBrushFeedbackStrictResidentSmoke) {
    Write-Info "Brush feedback strict resident-only smoke frames: $BrushFeedbackStrictResidentExitAfterFrames"
}
if (-not $SkipBrushFeedbackMovingSmoke) {
    Write-Info "Brush feedback moving strict resident-only smoke frames: $BrushFeedbackMovingExitAfterFrames"
}
if (-not $SkipBrushPaintSmoke) {
    Write-Info "Brush paint smoke frames: $BrushPaintExitAfterFrames"
    Write-Info "Brush paint moving smoke frames: $BrushPaintMovingExitAfterFrames"
    Write-Info "Brush paint nonresident smoke frames: $BrushPaintNonresidentExitAfterFrames"
    Write-Info "Brush paint GPU-physics smoke frames: $BrushPaintGpuPhysicsExitAfterFrames"
}
if (-not $SkipStartupEngineCaptureSmoke) {
    Write-Info "Startup engine capture smoke frames: $StartupEngineCaptureExitAfterFrames"
    Write-Info "Startup engine capture samples: count=$StartupEngineCaptureCount start=$StartupEngineCaptureStartFrame interval=$StartupEngineCaptureIntervalFrames"
}
if (-not $SkipEngineCaptureSmoke) {
    Write-Info "Engine capture smoke frames: $EngineCaptureExitAfterFrames"
    Write-Info "Engine capture samples: count=$EngineCaptureCount start=$EngineCaptureStartFrame interval=$EngineCaptureIntervalFrames"
}
if (-not $SkipStressEngineCaptureSmoke) {
    Write-Info "Stress engine capture smoke frames: $StressEngineCaptureExitAfterFrames"
    Write-Info "Stress engine capture samples: count=$StressEngineCaptureCount start=$StressEngineCaptureStartFrame interval=$StressEngineCaptureIntervalFrames"
}
if (-not $SkipFastFlightEngineCaptureSmoke) {
    Write-Info "Fast-flight engine capture smoke frames: $FastFlightEngineCaptureExitAfterFrames"
    Write-Info "Fast-flight engine capture samples: count=$FastFlightEngineCaptureCount start=$FastFlightEngineCaptureStartFrame interval=$FastFlightEngineCaptureIntervalFrames"
    Write-Info "Long fast-flight engine capture smoke frames: $LongFastFlightEngineCaptureExitAfterFrames"
    Write-Info "Long fast-flight engine capture samples: count=$LongFastFlightEngineCaptureCount start=$LongFastFlightEngineCaptureStartFrame interval=$LongFastFlightEngineCaptureIntervalFrames"
}
if (-not $SkipFastWaterTransitionEngineCaptureSmoke) {
    Write-Info "Fast water-transition engine capture smoke frames: $FastWaterTransitionEngineCaptureExitAfterFrames"
    Write-Info "Fast water-transition engine capture samples: count=$FastWaterTransitionEngineCaptureCount start=$FastWaterTransitionEngineCaptureStartFrame interval=$FastWaterTransitionEngineCaptureIntervalFrames"
    Write-Info "Long fast water-transition engine capture smoke frames: $LongFastWaterTransitionEngineCaptureExitAfterFrames"
    Write-Info "Long fast water-transition engine capture samples: count=$LongFastWaterTransitionEngineCaptureCount start=$LongFastWaterTransitionEngineCaptureStartFrame interval=$LongFastWaterTransitionEngineCaptureIntervalFrames"
}
if (-not $SkipWalkEngineCaptureSmoke) {
    Write-Info "Walk engine capture smoke frames: $WalkEngineCaptureExitAfterFrames"
    Write-Info "Walk engine capture samples: count=$WalkEngineCaptureCount start=$WalkEngineCaptureStartFrame interval=$WalkEngineCaptureIntervalFrames"
}
if (-not $SkipTerrainGapEngineCaptureSmoke) {
    Write-Info "Terrain gap engine capture smoke frames: $TerrainGapEngineCaptureExitAfterFrames"
    Write-Info "Terrain gap engine capture samples: count=$TerrainGapEngineCaptureCount start=$TerrainGapEngineCaptureStartFrame interval=$TerrainGapEngineCaptureIntervalFrames"
}
if (-not $SkipLongWalkEngineCaptureSmoke) {
    Write-Info "Long walk engine capture smoke frames: $LongWalkEngineCaptureExitAfterFrames"
    Write-Info "Long walk engine capture samples: count=$LongWalkEngineCaptureCount start=$LongWalkEngineCaptureStartFrame interval=$LongWalkEngineCaptureIntervalFrames"
}
if (-not $SkipWaterlineEngineCaptureSmoke) {
    Write-Info "Waterline engine capture smoke frames: $WaterlineEngineCaptureExitAfterFrames"
    Write-Info "Waterline engine capture samples: count=$WaterlineEngineCaptureCount start=$WaterlineEngineCaptureStartFrame interval=$WaterlineEngineCaptureIntervalFrames"
    Write-Info "Long waterline engine capture smoke frames: $LongWaterlineEngineCaptureExitAfterFrames"
    Write-Info "Long waterline engine capture samples: count=$LongWaterlineEngineCaptureCount start=$LongWaterlineEngineCaptureStartFrame interval=$LongWaterlineEngineCaptureIntervalFrames"
}
if (-not $SkipOwnershipDebugCaptureSmoke) {
    Write-Info "Ownership debug capture samples per scenario: $OwnershipDebugCaptureCount"
}
if (-not $SkipPublicDemoCapture) {
    Write-Info "Public demo capture start frame: $PublicDemoCaptureStartFrame"
    Write-Info "Public demo capture frames: $PublicDemoCaptureFrames"
    Write-Info "Public demo playback FPS: $PublicDemoPlaybackFps"
}
if (-not $SkipPublicReviewReelCapture) {
    Write-Info "Public review reel segment frames: $PublicReviewReelFrames"
    Write-Info "Public review reel playback FPS: $PublicReviewReelPlaybackFps"
}
if ($SkipPublicReviewDocs) {
    Write-Info "Public review docs check: skipped (-SkipPublicReviewDocs)"
}

if (-not $SkipPublicReviewDocs) {
    Write-Step "Verifying public review docs and source artifacts..."
    Assert-PublicReviewDocs -RepoRoot $repoRoot -ProjectRoot $projectRoot
}

Write-Step "Building latest code..."
if ($Clean) {
    & $buildScript -Config $Config -Clean
} else {
    & $buildScript -Config $Config
}
Stop-OnFailure -Code $LASTEXITCODE -Stage "Build"

if (-not $SkipTests) {
    Write-Step "Running sparse CPU/unit tests..."
    & ctest --test-dir $buildDir --output-on-failure -C $Config
    Stop-OnFailure -Code $LASTEXITCODE -Stage "CTest"
} else {
    Write-Info "CTest step: skipped (-SkipTests)"
}

if (-not $SkipDenseLegacySmoke) {
    Write-Step "Running dense legacy fallback smoke..."
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -DenseLegacy -DisablePhysics -ExitAfterFrames $DenseLegacyExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -DenseLegacy -DisablePhysics -ExitAfterFrames $DenseLegacyExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Dense legacy fallback smoke"
    $denseLegacyLog = Save-AndSummarizeRuntimeLog -Label "Dense legacy fallback smoke" -FileStem "dense_legacy_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Dense legacy fallback smoke" -SavedLog $denseLegacyLog
    Assert-DenseLegacyFallbackFromLog -Label "Dense legacy fallback smoke" -SavedLog $denseLegacyLog
} else {
    Write-Info "Dense legacy fallback smoke: skipped (-SkipDenseLegacySmoke)"
}

if (-not $SkipRenderSmoke) {
    Write-Step "Running sparse render/backend smoke..."
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparseSmoke -ExitAfterFrames $RenderExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparseSmoke -ExitAfterFrames $RenderExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse render smoke"
    $renderSmokeLog = Save-AndSummarizeRuntimeLog -Label "Sparse render smoke" -FileStem "sparse_render_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse render smoke" -SavedLog $renderSmokeLog
    Assert-SparseOnlyDefaultFromLog -Label "Sparse render smoke" -SavedLog $renderSmokeLog
    Assert-RenderPerformanceFromLog -Label "Sparse render smoke" -SavedLog $renderSmokeLog
    Assert-FarSvoReadyFromLog -Label "Sparse render smoke" -SavedLog $renderSmokeLog
    Assert-SurfaceLookaheadTelemetryFromLog -Label "Sparse render smoke" -SavedLog $renderSmokeLog
    Assert-OwnershipPressureTelemetryFromLog -Label "Sparse render smoke" -SavedLog $renderSmokeLog
    Assert-FastRequestTelemetryFromLog -Label "Sparse render smoke" -SavedLog $renderSmokeLog
    Assert-MidFarContinuityTelemetryFromLog -Label "Sparse render smoke" -SavedLog $renderSmokeLog
} else {
    Write-Info "Sparse render/backend smoke: skipped (-SkipRenderSmoke)"
}

if (-not $SkipAsyncPagePublishSmoke) {
    Write-Step "Running sparse async page-publish delay smoke..."
    $oldDelayFrames = $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES
    $oldDelayFences = $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES
    try {
        $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES = "5"
        $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES = "1"
        if ($ForceSync) {
            & $rebrunScript -Config $Config -NoBuild -SparseSmoke -ExitAfterFrames $AsyncPagePublishExitAfterFrames -ForceSync
        } else {
            & $rebrunScript -Config $Config -NoBuild -SparseSmoke -ExitAfterFrames $AsyncPagePublishExitAfterFrames
        }
        Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse async page-publish delay smoke"
    } finally {
        if ($null -eq $oldDelayFrames) {
            Remove-Item env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES -ErrorAction SilentlyContinue
        } else {
            $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES = $oldDelayFrames
        }
        if ($null -eq $oldDelayFences) {
            Remove-Item env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES -ErrorAction SilentlyContinue
        } else {
            $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES = $oldDelayFences
        }
    }
    $asyncPublishLog = Save-AndSummarizeRuntimeLog -Label "Sparse async page-publish delay smoke" -FileStem "sparse_async_page_publish_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse async page-publish delay smoke" -SavedLog $asyncPublishLog
    Assert-AsyncPagePublishFromLog -Label "Sparse async page-publish delay smoke" -SavedLog $asyncPublishLog

    Write-Step "Running sparse async page-publish fence-stress smoke..."
    $oldDelayFrames = $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES
    $oldDelayFences = $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES
    try {
        $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES = "1"
        $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES = "12"
        if ($ForceSync) {
            & $rebrunScript -Config $Config -NoBuild -SparseSmoke -ExitAfterFrames $AsyncPagePublishFenceExitAfterFrames -ForceSync
        } else {
            & $rebrunScript -Config $Config -NoBuild -SparseSmoke -ExitAfterFrames $AsyncPagePublishFenceExitAfterFrames
        }
        Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse async page-publish fence-stress smoke"
    } finally {
        if ($null -eq $oldDelayFrames) {
            Remove-Item env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES -ErrorAction SilentlyContinue
        } else {
            $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES = $oldDelayFrames
        }
        if ($null -eq $oldDelayFences) {
            Remove-Item env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES -ErrorAction SilentlyContinue
        } else {
            $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES = $oldDelayFences
        }
    }
    $asyncFenceLog = Save-AndSummarizeRuntimeLog -Label "Sparse async page-publish fence-stress smoke" -FileStem "sparse_async_page_publish_fence_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse async page-publish fence-stress smoke" -SavedLog $asyncFenceLog
    Assert-AsyncPagePublishFromLog -Label "Sparse async page-publish fence-stress smoke" -SavedLog $asyncFenceLog -MinWaitingFence 1

    Write-Step "Running sparse async page-publish walk capture smoke..."
    $asyncWalkOut = Join-Path $buildDir "logs\sparse_async_page_publish_walk_capture"
    if (Test-Path $asyncWalkOut) {
        Remove-Item -LiteralPath $asyncWalkOut -Recurse -Force
    }
    $oldDelayFrames = $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES
    $oldDelayFences = $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES
    try {
        $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES = "5"
        $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES = "1"
        & $engineCaptureScript `
            -Config $Config `
            -NoBuild `
            -WalkTest `
            -WalkTestSpeed 8 `
            -WalkTestYawDegPerSec 30 `
            -ExitAfterFrames $AsyncPagePublishWalkExitAfterFrames `
            -CaptureStartFrame $AsyncPagePublishWalkStartFrame `
            -CaptureIntervalFrames $AsyncPagePublishWalkIntervalFrames `
            -CaptureCount $AsyncPagePublishWalkCount `
            -MinUniqueSampleColors 50 `
            -MinAverageSkyLikePct 8 `
            -MaxAverageTopTerrainPct 60 `
            -MaxFrameTopTerrainPct 98 `
            -MinTerrainPct 35 `
            -MaxFarWaterPct 18 `
            -RequireSurfaceFragments `
            -MaxMissPct 0 `
            -MaxUnsafeNearMissPct 0 `
            -OutputDir $asyncWalkOut
        Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse async page-publish walk capture smoke"
    } finally {
        if ($null -eq $oldDelayFrames) {
            Remove-Item env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES -ErrorAction SilentlyContinue
        } else {
            $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES = $oldDelayFrames
        }
        if ($null -eq $oldDelayFences) {
            Remove-Item env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES -ErrorAction SilentlyContinue
        } else {
            $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES = $oldDelayFences
        }
    }
    $asyncWalkLog = Join-Path $asyncWalkOut "venpod_runtime.log"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse async page-publish walk capture smoke" -SavedLog $asyncWalkLog
    Assert-EngineCaptureOwnershipFromLog -Label "Sparse async page-publish walk capture smoke" -OutputDir $asyncWalkOut -ReadyFrame $AsyncPagePublishWalkStartFrame -MinSamples $AsyncPagePublishWalkCount -MinTerrainPct 35.0 -RequireSurfaceFragments $true -MaxFarWaterPct 18.0
    Assert-WalkTelemetryFromLog -Label "Sparse async page-publish walk capture smoke" -SavedLog $asyncWalkLog -MinSamples 60
    Assert-AsyncPagePublishFromLog -Label "Sparse async page-publish walk capture smoke" -SavedLog $asyncWalkLog
    Write-Info "Sparse async page-publish walk capture artifacts: $asyncWalkOut"

    Write-Step "Running sparse async page-publish long walk capture smoke..."
    $asyncLongWalkOut = Join-Path $buildDir "logs\sparse_async_page_publish_long_walk_capture"
    if (Test-Path $asyncLongWalkOut) {
        Remove-Item -LiteralPath $asyncLongWalkOut -Recurse -Force
    }
    $oldDelayFrames = $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES
    $oldDelayFences = $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES
    try {
        $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES = "5"
        $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES = "1"
        & $engineCaptureScript `
            -Config $Config `
            -NoBuild `
            -WalkTest `
            -WalkTestSpeed 8 `
            -WalkTestYawDegPerSec 30 `
            -ExitAfterFrames $AsyncPagePublishLongWalkExitAfterFrames `
            -CaptureStartFrame $AsyncPagePublishLongWalkStartFrame `
            -CaptureIntervalFrames $AsyncPagePublishLongWalkIntervalFrames `
            -CaptureCount $AsyncPagePublishLongWalkCount `
            -MinUniqueSampleColors 50 `
            -MinAverageSkyLikePct 8 `
            -MaxAverageTopTerrainPct 60 `
            -MaxFrameTopTerrainPct 98 `
            -MinTerrainPct 35 `
            -MaxFarWaterPct 18 `
            -RequireSurfaceFragments `
            -MaxMissPct 0 `
            -MaxUnsafeNearMissPct 0 `
            -OutputDir $asyncLongWalkOut
        Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse async page-publish long walk capture smoke"
    } finally {
        if ($null -eq $oldDelayFrames) {
            Remove-Item env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES -ErrorAction SilentlyContinue
        } else {
            $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES = $oldDelayFrames
        }
        if ($null -eq $oldDelayFences) {
            Remove-Item env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES -ErrorAction SilentlyContinue
        } else {
            $env:VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES = $oldDelayFences
        }
    }
    $asyncLongWalkLog = Join-Path $asyncLongWalkOut "venpod_runtime.log"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse async page-publish long walk capture smoke" -SavedLog $asyncLongWalkLog
    Assert-EngineCaptureOwnershipFromLog -Label "Sparse async page-publish long walk capture smoke" -OutputDir $asyncLongWalkOut -ReadyFrame $AsyncPagePublishLongWalkStartFrame -MinSamples $AsyncPagePublishLongWalkCount -MinTerrainPct 35.0 -RequireSurfaceFragments $true -MaxFarWaterPct 18.0
    Assert-WalkTelemetryFromLog -Label "Sparse async page-publish long walk capture smoke" -SavedLog $asyncLongWalkLog -MinSamples 120
    Assert-AsyncPagePublishFromLog -Label "Sparse async page-publish long walk capture smoke" -SavedLog $asyncLongWalkLog
    Write-Info "Sparse async page-publish long walk capture artifacts: $asyncLongWalkOut"
} else {
    Write-Info "Sparse async page-publish delay smoke: skipped (-SkipAsyncPagePublishSmoke)"
}

if (-not $SkipFlickerSmoke) {
    Write-Step "Running sparse every-frame flicker smoke..."
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparseFlickerSmoke -ExitAfterFrames $FlickerExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparseFlickerSmoke -ExitAfterFrames $FlickerExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse flicker smoke"
    $flickerLog = Save-AndSummarizeRuntimeLog -Label "Sparse flicker smoke" -FileStem "sparse_flicker_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse flicker smoke" -SavedLog $flickerLog
    Assert-FlickerOwnershipStabilityFromLog -Label "Sparse flicker smoke" -SavedLog $flickerLog
} else {
    Write-Info "Sparse flicker smoke: skipped (-SkipFlickerSmoke)"
}

if (-not $SkipSurfaceSmoke) {
    Write-Step "Running sparse seeded-surface smoke..."
    $editPersistencePath = Join-Path $buildDir "logs\sparse_surface_edits.vsed"
    if (Test-Path $editPersistencePath) {
        Remove-Item -LiteralPath $editPersistencePath -Force
    }
    $editPersistenceRelativePath = "build\logs\sparse_surface_edits.vsed"
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparseSurfaceSmoke -ExitAfterFrames $SurfaceExitAfterFrames -SparseEditFile $editPersistenceRelativePath -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparseSurfaceSmoke -ExitAfterFrames $SurfaceExitAfterFrames -SparseEditFile $editPersistenceRelativePath
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse seeded-surface smoke"
    $surfaceLog = Save-AndSummarizeRuntimeLog -Label "Sparse seeded-surface smoke" -FileStem "sparse_surface_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse seeded-surface smoke" -SavedLog $surfaceLog
    Assert-SparseSurfaceGpuPathFromLog -Label "Sparse seeded-surface smoke" -SavedLog $surfaceLog
    Assert-SparseEditPersistenceFile -Path $editPersistencePath
} else {
    Write-Info "Sparse seeded-surface smoke: skipped (-SkipSurfaceSmoke)"
}

if (-not $SkipEditUiPersistenceSmoke) {
    Write-Step "Running sparse pause-menu edit save/load smoke..."
    $editUiSmokeDir = Join-Path $buildDir "logs\sparse_edit_ui_smoke"
    New-Item -ItemType Directory -Force -Path $editUiSmokeDir | Out-Null
    $editUiSmokeRelativePath = "build/logs/sparse_edit_ui_smoke/review-edits.vsed"
    $editUiSmokePath = Join-Path $projectRoot $editUiSmokeRelativePath
    Remove-Item -LiteralPath $editUiSmokePath -Force -ErrorAction SilentlyContinue

    $previousUiSmoke = $env:VENPOD_SPARSE_EDIT_UI_SMOKE
    $previousUiSmokePath = $env:VENPOD_SPARSE_EDIT_UI_SMOKE_PATH
    try {
        $env:VENPOD_SPARSE_EDIT_UI_SMOKE = "1"
        $env:VENPOD_SPARSE_EDIT_UI_SMOKE_PATH = $editUiSmokeRelativePath
        if ($ForceSync) {
            & $rebrunScript -Config $Config -NoBuild -Sparse -DisablePhysics -ExitAfterFrames 45 -ForceSync
        } else {
            & $rebrunScript -Config $Config -NoBuild -Sparse -DisablePhysics -ExitAfterFrames 45
        }
        Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse pause-menu edit save/load smoke"
    } finally {
        if ($null -eq $previousUiSmoke) {
            Remove-Item "env:VENPOD_SPARSE_EDIT_UI_SMOKE" -ErrorAction SilentlyContinue
        } else {
            $env:VENPOD_SPARSE_EDIT_UI_SMOKE = $previousUiSmoke
        }
        if ($null -eq $previousUiSmokePath) {
            Remove-Item "env:VENPOD_SPARSE_EDIT_UI_SMOKE_PATH" -ErrorAction SilentlyContinue
        } else {
            $env:VENPOD_SPARSE_EDIT_UI_SMOKE_PATH = $previousUiSmokePath
        }
    }
    $editUiLog = Save-AndSummarizeRuntimeLog -Label "Sparse pause-menu edit save/load smoke" -FileStem "sparse_edit_ui_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse pause-menu edit save/load smoke" -SavedLog $editUiLog
    $editUiPassLine = Select-String -Path $editUiLog -Pattern "SPARSE_EDIT_UI_SMOKE passed" -SimpleMatch | Select-Object -First 1
    if (-not $editUiPassLine) {
        throw "Sparse pause-menu edit save/load smoke did not log SPARSE_EDIT_UI_SMOKE passed"
    }
    Assert-SparseEditPersistenceFile -Path $editUiSmokePath
} else {
    Write-Info "Sparse pause-menu edit save/load smoke: skipped (-SkipEditUiPersistenceSmoke)"
}

if (-not $SkipGpuRaycastSmoke) {
    Write-Step "Running sparse GPU-raycast smoke..."
    $gpuRaycastHealthReadyFrame = 150
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparseGpuRaycastSmoke -ExitAfterFrames $GpuRaycastExitAfterFrames -SparseGpuRaycastHealthReadyFrame $gpuRaycastHealthReadyFrame -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparseGpuRaycastSmoke -ExitAfterFrames $GpuRaycastExitAfterFrames -SparseGpuRaycastHealthReadyFrame $gpuRaycastHealthReadyFrame
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse GPU-raycast smoke"
    $gpuRaycastLog = Save-AndSummarizeRuntimeLog -Label "Sparse GPU-raycast smoke" -FileStem "sparse_gpu_raycast_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse GPU-raycast smoke" -SavedLog $gpuRaycastLog
    Assert-GpuRaycastHealthFromLog -Label "Sparse GPU-raycast smoke" -SavedLog $gpuRaycastLog
} else {
    Write-Info "Sparse GPU-raycast smoke: skipped (-SkipGpuRaycastSmoke)"
}

if (-not $SkipMissFeedbackSmoke) {
    Write-Step "Running sparse miss-feedback smoke..."
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparseMissFeedbackSmoke -ExitAfterFrames $MissFeedbackExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparseMissFeedbackSmoke -ExitAfterFrames $MissFeedbackExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse miss-feedback smoke"
    $missFeedbackLog = Save-AndSummarizeRuntimeLog -Label "Sparse miss-feedback smoke" -FileStem "sparse_miss_feedback_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse miss-feedback smoke" -SavedLog $missFeedbackLog
    Assert-MissFeedbackPressureFromLog -Label "Sparse miss-feedback smoke" -SavedLog $missFeedbackLog
} else {
    Write-Info "Sparse miss-feedback smoke: skipped (-SkipMissFeedbackSmoke)"
}

if (-not $SkipBrushFeedbackSmoke) {
    Write-Step "Running sparse brush-feedback smoke..."
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparseBrushFeedbackSmoke -ExitAfterFrames $BrushFeedbackExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparseBrushFeedbackSmoke -ExitAfterFrames $BrushFeedbackExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse brush-feedback smoke"
    $brushFeedbackLog = Save-AndSummarizeRuntimeLog -Label "Sparse brush-feedback smoke" -FileStem "sparse_brush_feedback_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse brush-feedback smoke" -SavedLog $brushFeedbackLog
    Assert-BrushFeedbackDiagnosticsFromLog -Label "Sparse brush-feedback smoke" -SavedLog $brushFeedbackLog
} else {
    Write-Info "Sparse brush-feedback smoke: skipped (-SkipBrushFeedbackSmoke)"
}

if (-not $SkipBrushFeedbackApplySmoke) {
    Write-Step "Running sparse brush-feedback apply smoke..."
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparseBrushFeedbackSmoke -SparseBrushFeedbackApply -ExitAfterFrames $BrushFeedbackApplyExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparseBrushFeedbackSmoke -SparseBrushFeedbackApply -ExitAfterFrames $BrushFeedbackApplyExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse brush-feedback apply smoke"
    $brushFeedbackApplyLog = Save-AndSummarizeRuntimeLog -Label "Sparse brush-feedback apply smoke" -FileStem "sparse_brush_feedback_apply_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse brush-feedback apply smoke" -SavedLog $brushFeedbackApplyLog
    Assert-BrushFeedbackDiagnosticsFromLog -Label "Sparse brush-feedback apply smoke" -SavedLog $brushFeedbackApplyLog -RequireGpuApply -RequireCpuFallback -RequireMissingResidentRetry
} else {
    Write-Info "Sparse brush-feedback apply smoke: skipped (-SkipBrushFeedbackApplySmoke)"
}

if (-not $SkipBrushFeedbackAuthoritativeSmoke) {
    Write-Step "Running sparse brush-feedback authoritative smoke..."
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparseBrushFeedbackSmoke -SparseBrushFeedbackAuthoritative -ExitAfterFrames $BrushFeedbackAuthoritativeExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparseBrushFeedbackSmoke -SparseBrushFeedbackAuthoritative -ExitAfterFrames $BrushFeedbackAuthoritativeExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse brush-feedback authoritative smoke"
    $brushFeedbackAuthoritativeLog = Save-AndSummarizeRuntimeLog -Label "Sparse brush-feedback authoritative smoke" -FileStem "sparse_brush_feedback_authoritative_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse brush-feedback authoritative smoke" -SavedLog $brushFeedbackAuthoritativeLog
    Assert-BrushFeedbackDiagnosticsFromLog -Label "Sparse brush-feedback authoritative smoke" -SavedLog $brushFeedbackAuthoritativeLog -RequireGpuApply -RequireAuthoritativeApply -RequireCpuFallback -RequireMissingResidentRetry
} else {
    Write-Info "Sparse brush-feedback authoritative smoke: skipped (-SkipBrushFeedbackAuthoritativeSmoke)"
}

if (-not $SkipBrushFeedbackStrictResidentSmoke) {
    Write-Step "Running sparse brush-feedback strict resident-only smoke..."
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparseBrushFeedbackSmoke -SparseBrushFeedbackStrictResidentOnly -ExitAfterFrames $BrushFeedbackStrictResidentExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparseBrushFeedbackSmoke -SparseBrushFeedbackStrictResidentOnly -ExitAfterFrames $BrushFeedbackStrictResidentExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse brush-feedback strict resident-only smoke"
    $brushFeedbackStrictResidentLog = Save-AndSummarizeRuntimeLog -Label "Sparse brush-feedback strict resident-only smoke" -FileStem "sparse_brush_feedback_strict_resident_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse brush-feedback strict resident-only smoke" -SavedLog $brushFeedbackStrictResidentLog
    Assert-BrushFeedbackDiagnosticsFromLog -Label "Sparse brush-feedback strict resident-only smoke" -SavedLog $brushFeedbackStrictResidentLog -RequireGpuApply -RequireAuthoritativeApply -ForbidCpuFallback -MinCases 6
} else {
    Write-Info "Sparse brush-feedback strict resident-only smoke: skipped (-SkipBrushFeedbackStrictResidentSmoke)"
}

if (-not $SkipBrushFeedbackMovingSmoke) {
    Write-Step "Running sparse brush-feedback moving strict resident-only smoke..."
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparseBrushFeedbackSmoke -SparseBrushFeedbackStrictResidentOnly -SparseBrushFeedbackMovingDiagnostic -SparseStressCamera -ExitAfterFrames $BrushFeedbackMovingExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparseBrushFeedbackSmoke -SparseBrushFeedbackStrictResidentOnly -SparseBrushFeedbackMovingDiagnostic -SparseStressCamera -ExitAfterFrames $BrushFeedbackMovingExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse brush-feedback moving strict resident-only smoke"
    $brushFeedbackMovingLog = Save-AndSummarizeRuntimeLog -Label "Sparse brush-feedback moving strict resident-only smoke" -FileStem "sparse_brush_feedback_moving_strict_resident_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse brush-feedback moving strict resident-only smoke" -SavedLog $brushFeedbackMovingLog
    Assert-BrushFeedbackDiagnosticsFromLog -Label "Sparse brush-feedback moving strict resident-only smoke" -SavedLog $brushFeedbackMovingLog -RequireGpuApply -RequireAuthoritativeApply -ForbidCpuFallback -RequireMovingDiagnostic -MinCases 6
} else {
    Write-Info "Sparse brush-feedback moving strict resident-only smoke: skipped (-SkipBrushFeedbackMovingSmoke)"
}

if (-not $SkipBrushPaintSmoke) {
    Write-Step "Running sparse brush paint smoke..."
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparseBrushPaintSmoke -SparseStressCamera -ExitAfterFrames $BrushPaintExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparseBrushPaintSmoke -SparseStressCamera -ExitAfterFrames $BrushPaintExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse brush paint smoke"
    $brushPaintLog = Save-AndSummarizeRuntimeLog -Label "Sparse brush paint smoke" -FileStem "sparse_brush_paint_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse brush paint smoke" -SavedLog $brushPaintLog
    Assert-BrushPaintSmokeFromLog -Label "Sparse brush paint smoke" -SavedLog $brushPaintLog -MinCases 4

    Write-Step "Running sparse brush paint moving smoke..."
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparseBrushPaintMovingSmoke -SparseStressCamera -ExitAfterFrames $BrushPaintMovingExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparseBrushPaintMovingSmoke -SparseStressCamera -ExitAfterFrames $BrushPaintMovingExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse brush paint moving smoke"
    $brushPaintMovingLog = Save-AndSummarizeRuntimeLog -Label "Sparse brush paint moving smoke" -FileStem "sparse_brush_paint_moving_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse brush paint moving smoke" -SavedLog $brushPaintMovingLog
    Assert-BrushPaintSmokeFromLog -Label "Sparse brush paint moving smoke" -SavedLog $brushPaintMovingLog -MinCases 4 -RequireMoving -MinPathCells 8

    Write-Step "Running sparse brush paint nonresident retry smoke..."
    if ($UseForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparseBrushPaintNonresidentSmoke -SparseStressCamera -ExitAfterFrames $BrushPaintNonresidentExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparseBrushPaintNonresidentSmoke -SparseStressCamera -ExitAfterFrames $BrushPaintNonresidentExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse brush paint nonresident retry smoke"
    $brushPaintNonresidentLog = Save-AndSummarizeRuntimeLog -Label "Sparse brush paint nonresident retry smoke" -FileStem "sparse_brush_paint_nonresident_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse brush paint nonresident retry smoke" -SavedLog $brushPaintNonresidentLog
    Assert-BrushPaintSmokeFromLog -Label "Sparse brush paint nonresident retry smoke" -SavedLog $brushPaintNonresidentLog -MinCases 4 -RequireNonresidentRetry

    Write-Step "Running sparse brush paint GPU-physics apply smoke..."
    if ($UseForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparseBrushPaintMovingSmoke -SparseStressCamera -SparseGpuPhysicsApply -ExitAfterFrames $BrushPaintGpuPhysicsExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparseBrushPaintMovingSmoke -SparseStressCamera -SparseGpuPhysicsApply -ExitAfterFrames $BrushPaintGpuPhysicsExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse brush paint GPU-physics apply smoke"
    $brushPaintGpuPhysicsLog = Save-AndSummarizeRuntimeLog -Label "Sparse brush paint GPU-physics apply smoke" -FileStem "sparse_brush_paint_gpu_physics_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse brush paint GPU-physics apply smoke" -SavedLog $brushPaintGpuPhysicsLog
    Assert-BrushPaintSmokeFromLog -Label "Sparse brush paint GPU-physics apply smoke" -SavedLog $brushPaintGpuPhysicsLog -MinCases 4 -RequireMoving -MinPathCells 8
    Assert-GpuSparsePhysicsFromLog -Label "Sparse brush paint GPU-physics apply smoke" -SavedLog $brushPaintGpuPhysicsLog -ForbidDiagnosticSeed -ForbidFluidSeed -AllowRejectedProposals -MinResultProposals 1 -MinGpuApplyCompleted 1 -MinRejectedProposals 1
} else {
    Write-Info "Sparse brush paint smoke: skipped (-SkipBrushPaintSmoke)"
}

if (-not $SkipDefaultPhysicsSmoke) {
    Write-Step "Running default sparse local-physics smoke..."
    if ($ForceSync) {
        & $rebrunScript `
            -Config $Config `
            -NoBuild `
            -SparsePhysicsDiagnosticSeed `
            -SparseValidatePool `
            -RequireSparsePipeReady `
            -RequireSparseOwnershipQuality `
            -RequireSparseOwnershipStability `
            -ExitAfterFrames $DefaultPhysicsExitAfterFrames `
            -ForceSync
    } else {
        & $rebrunScript `
            -Config $Config `
            -NoBuild `
            -SparsePhysicsDiagnosticSeed `
            -SparseValidatePool `
            -RequireSparsePipeReady `
            -RequireSparseOwnershipQuality `
            -RequireSparseOwnershipStability `
            -ExitAfterFrames $DefaultPhysicsExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Default sparse local-physics smoke"
    $defaultPhysicsLog = Save-AndSummarizeRuntimeLog -Label "Default sparse local-physics smoke" -FileStem "sparse_default_physics_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Default sparse local-physics smoke" -SavedLog $defaultPhysicsLog
    Assert-DefaultSparsePhysicsFromLog -Label "Default sparse local-physics smoke" -SavedLog $defaultPhysicsLog
    Assert-SparseBodyCollisionFromLog -Label "Default sparse local-physics smoke" -SavedLog $defaultPhysicsLog
} else {
    Write-Info "Default sparse local-physics smoke: skipped (-SkipDefaultPhysicsSmoke)"
}

if (-not $SkipPhysicsSmoke) {
    Write-Step "Running sparse GPU-physics smoke..."
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparsePhysicsSmoke -ExitAfterFrames $PhysicsExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparsePhysicsSmoke -ExitAfterFrames $PhysicsExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse GPU-physics smoke"
    $gpuPhysicsLog = Save-AndSummarizeRuntimeLog -Label "Sparse GPU-physics smoke" -FileStem "sparse_physics_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse GPU-physics smoke" -SavedLog $gpuPhysicsLog
    Assert-GpuSparsePhysicsFromLog -Label "Sparse GPU-physics smoke" -SavedLog $gpuPhysicsLog
} else {
    Write-Info "Sparse GPU-physics smoke: skipped (-SkipPhysicsSmoke)"
}

if (-not $SkipGpuPhysicsStrictSmoke) {
    Write-Step "Running sparse GPU-physics strict material smoke..."
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparsePhysicsSmoke -SparseGpuPhysicsStrict -SparsePhysicsDiagnosticSeed -ExitAfterFrames $GpuPhysicsStrictExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparsePhysicsSmoke -SparseGpuPhysicsStrict -SparsePhysicsDiagnosticSeed -ExitAfterFrames $GpuPhysicsStrictExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse GPU-physics strict material smoke"
    $gpuPhysicsStrictMaterialLog = Save-AndSummarizeRuntimeLog -Label "Sparse GPU-physics strict material smoke" -FileStem "sparse_physics_strict_material_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse GPU-physics strict material smoke" -SavedLog $gpuPhysicsStrictMaterialLog
    Assert-GpuSparsePhysicsFromLog -Label "Sparse GPU-physics strict material smoke" -SavedLog $gpuPhysicsStrictMaterialLog -RequireDiagnosticSeed -ForbidFluidSeed

    Write-Step "Running sparse GPU-physics strict fluid smoke..."
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparsePhysicsSmoke -SparseGpuPhysicsStrict -SparsePhysicsDiagnosticFluidSeed -ExitAfterFrames $GpuPhysicsStrictExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparsePhysicsSmoke -SparseGpuPhysicsStrict -SparsePhysicsDiagnosticFluidSeed -ExitAfterFrames $GpuPhysicsStrictExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse GPU-physics strict fluid smoke"
    $gpuPhysicsStrictLog = Save-AndSummarizeRuntimeLog -Label "Sparse GPU-physics strict fluid smoke" -FileStem "sparse_physics_strict_fluid_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse GPU-physics strict fluid smoke" -SavedLog $gpuPhysicsStrictLog
    Assert-GpuSparsePhysicsFromLog -Label "Sparse GPU-physics strict fluid smoke" -SavedLog $gpuPhysicsStrictLog -RequireFluidSeed -ForbidDiagnosticSeed

    Write-Step "Running sparse GPU-physics strict mixed material/fluid smoke..."
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparsePhysicsSmoke -SparseGpuPhysicsStrict -SparsePhysicsDiagnosticSeed -SparsePhysicsDiagnosticFluidSeed -ExitAfterFrames $GpuPhysicsStrictExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparsePhysicsSmoke -SparseGpuPhysicsStrict -SparsePhysicsDiagnosticSeed -SparsePhysicsDiagnosticFluidSeed -ExitAfterFrames $GpuPhysicsStrictExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse GPU-physics strict mixed material/fluid smoke"
    $gpuPhysicsStrictMixedLog = Save-AndSummarizeRuntimeLog -Label "Sparse GPU-physics strict mixed material/fluid smoke" -FileStem "sparse_physics_strict_mixed_smoke"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse GPU-physics strict mixed material/fluid smoke" -SavedLog $gpuPhysicsStrictMixedLog
    Assert-GpuSparsePhysicsFromLog -Label "Sparse GPU-physics strict mixed material/fluid smoke" -SavedLog $gpuPhysicsStrictMixedLog -RequireDiagnosticSeed -RequireFluidSeed -MinResultProposals 2 -MinGpuApplyCompleted 2
} else {
    Write-Info "Sparse GPU-physics strict material/fluid smoke: skipped (-SkipGpuPhysicsStrictSmoke)"
}

$startupCaptureOut = Join-Path $buildDir "logs\sparse_startup_engine_capture"
$captureOut = Join-Path $buildDir "logs\sparse_engine_capture"
$stressCaptureOut = Join-Path $buildDir "logs\sparse_stress_engine_capture"
$skylineCaptureOut = Join-Path $buildDir "logs\sparse_skyline_engine_capture"
$fastFlightCaptureOut = Join-Path $buildDir "logs\sparse_fast_flight_engine_capture"
$longFastFlightCaptureOut = Join-Path $buildDir "logs\sparse_long_fast_flight_engine_capture"
$fastWaterTransitionCaptureOut = Join-Path $buildDir "logs\sparse_fast_water_transition_engine_capture"
$longFastWaterTransitionCaptureOut = Join-Path $buildDir "logs\sparse_long_fast_water_transition_engine_capture"
$walkCaptureOut = Join-Path $buildDir "logs\sparse_walk_engine_capture"
$terrainGapCaptureOut = Join-Path $buildDir "logs\sparse_terrain_gap_engine_capture"
$brushDomeCaptureOut = Join-Path $buildDir "logs\sparse_brush_dome_engine_capture"
$longWalkCaptureOut = Join-Path $buildDir "logs\sparse_long_walk_engine_capture"
$waterlineCaptureOut = Join-Path $buildDir "logs\sparse_waterline_engine_capture"
$waterlineMaterialCaptureOut = Join-Path $buildDir "logs\sparse_waterline_material_capture"
$longWaterlineCaptureOut = Join-Path $buildDir "logs\sparse_long_waterline_engine_capture"
$gpuPhysicsStrictCaptureOut = Join-Path $buildDir "logs\sparse_physics_strict_walk_capture"
$gpuPhysicsStrictStressOut = Join-Path $buildDir "logs\sparse_physics_strict_stress_capture"
$gpuPhysicsStrictLongWalkOut = Join-Path $buildDir "logs\sparse_physics_strict_long_walk_capture"

if (-not $SkipStartupEngineCaptureSmoke) {
    Write-Step "Running sparse startup engine capture smoke..."
    if (Test-Path $startupCaptureOut) {
        Remove-Item -LiteralPath $startupCaptureOut -Recurse -Force
    }
    & $engineCaptureScript `
        -Config $Config `
        -NoBuild `
        -WalkTest `
        -WalkTestSpeed 8 `
        -WalkTestYawDegPerSec 30 `
        -ExitAfterFrames $StartupEngineCaptureExitAfterFrames `
        -CaptureStartFrame $StartupEngineCaptureStartFrame `
        -CaptureIntervalFrames $StartupEngineCaptureIntervalFrames `
        -CaptureCount $StartupEngineCaptureCount `
        -MinUniqueSampleColors 50 `
        -MaxFrameDarkPct 5 `
        -MaxAverageTopTerrainPct 98 `
        -MaxFrameTopTerrainPct 100 `
        -MinSurfaceScreenPct 18.0 `
        -MaxBackgroundScreenPct 82.0 `
        -MaxValleyAtmosphereScreenPct 32.0 `
        -OutputDir $startupCaptureOut
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse startup engine capture smoke"
    $startupCaptureLog = Join-Path $startupCaptureOut "venpod_runtime.log"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse startup engine capture smoke" -SavedLog $startupCaptureLog
} else {
    Write-Info "Sparse startup engine capture smoke: skipped (-SkipStartupEngineCaptureSmoke)"
}

if (-not $SkipGpuPhysicsStrictSmoke) {
    Write-Step "Running sparse GPU-physics strict movement capture..."
    if (Test-Path $gpuPhysicsStrictCaptureOut) {
        Remove-Item -LiteralPath $gpuPhysicsStrictCaptureOut -Recurse -Force
    }
    & $engineCaptureScript `
        -Config $Config `
        -NoBuild `
        -WalkTest `
        -WalkTestSpeed 8 `
        -WalkTestYawDegPerSec 30 `
        -SparseGpuPhysicsStrict `
        -SparsePhysicsDiagnosticSeed `
        -SparsePhysicsDiagnosticFluidSeed `
        -ExitAfterFrames $GpuPhysicsStrictCaptureExitAfterFrames `
        -CaptureStartFrame $GpuPhysicsStrictCaptureStartFrame `
        -CaptureIntervalFrames $GpuPhysicsStrictCaptureIntervalFrames `
        -CaptureCount $GpuPhysicsStrictCaptureCount `
        -MinUniqueSampleColors 50 `
        -MinAverageSkyLikePct 8 `
        -MaxAverageTopTerrainPct 55 `
        -MaxFrameTopTerrainPct 98 `
        -MaxFrameMs 120 `
        -MaxPrepMs 80 `
        -MaxGpuRayMs 35 `
        -OutputDir $gpuPhysicsStrictCaptureOut
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse GPU-physics strict movement capture"
    $gpuPhysicsStrictCaptureLog = Join-Path $gpuPhysicsStrictCaptureOut "venpod_runtime.log"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse GPU-physics strict movement capture" -SavedLog $gpuPhysicsStrictCaptureLog
    Assert-EngineCaptureOwnershipFromLog `
        -Label "Sparse GPU-physics strict movement capture" `
        -OutputDir $gpuPhysicsStrictCaptureOut `
        -ReadyFrame $GpuPhysicsStrictCaptureStartFrame `
        -MinSamples $GpuPhysicsStrictCaptureCount `
        -MinTerrainPct 35.0 `
        -RequireSurfaceFragments $true
    Assert-WalkTelemetryFromLog `
        -Label "Sparse GPU-physics strict movement capture" `
        -SavedLog $gpuPhysicsStrictCaptureLog `
        -MinSamples 30
    Assert-GpuSparsePhysicsFromLog `
        -Label "Sparse GPU-physics strict movement capture" `
        -SavedLog $gpuPhysicsStrictCaptureLog `
        -RequireDiagnosticSeed `
        -RequireFluidSeed `
        -MinResultProposals 2 `
        -MinGpuApplyCompleted 2

    Write-Step "Running sparse GPU-physics strict stress-camera capture..."
    if (Test-Path $gpuPhysicsStrictStressOut) {
        Remove-Item -LiteralPath $gpuPhysicsStrictStressOut -Recurse -Force
    }
    & $engineCaptureScript `
        -Config $Config `
        -NoBuild `
        -StressCamera `
        -StressCameraRadius 280 `
        -StressCameraHeight 40 `
        -StressCameraBaseHeight 80 `
        -StressCameraSpeed 80 `
        -SparseGpuPhysicsStrict `
        -SparsePhysicsDiagnosticSeed `
        -SparsePhysicsDiagnosticFluidSeed `
        -ExitAfterFrames $GpuPhysicsStrictStressExitAfterFrames `
        -CaptureStartFrame $GpuPhysicsStrictStressStartFrame `
        -CaptureIntervalFrames $GpuPhysicsStrictStressIntervalFrames `
        -CaptureCount $GpuPhysicsStrictStressCount `
        -MinUniqueSampleColors 50 `
        -MaxFrameMs 120 `
        -MaxPrepMs 80 `
        -MaxGpuRayMs 65 `
        -MaxHeightProxyPct 72 `
        -OutputDir $gpuPhysicsStrictStressOut
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse GPU-physics strict stress-camera capture"
    $gpuPhysicsStrictStressLog = Join-Path $gpuPhysicsStrictStressOut "venpod_runtime.log"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse GPU-physics strict stress-camera capture" -SavedLog $gpuPhysicsStrictStressLog
    Assert-EngineCaptureOwnershipFromLog `
        -Label "Sparse GPU-physics strict stress-camera capture" `
        -OutputDir $gpuPhysicsStrictStressOut `
        -ReadyFrame $GpuPhysicsStrictStressStartFrame `
        -MinSamples $GpuPhysicsStrictStressCount `
        -MinTerrainPct 35.0 `
        -RequireSurfaceFragments $false `
        -MaxHeightProxyPct 72.0
    Assert-GpuSparsePhysicsFromLog `
        -Label "Sparse GPU-physics strict stress-camera capture" `
        -SavedLog $gpuPhysicsStrictStressLog `
        -RequireDiagnosticSeed `
        -RequireFluidSeed `
        -MinResultProposals 1 `
        -MinGpuApplyCompleted 1

    Write-Step "Running sparse GPU-physics strict long-walk capture..."
    if (Test-Path $gpuPhysicsStrictLongWalkOut) {
        Remove-Item -LiteralPath $gpuPhysicsStrictLongWalkOut -Recurse -Force
    }
    & $engineCaptureScript `
        -Config $Config `
        -NoBuild `
        -WalkTest `
        -WalkTestSpeed 8 `
        -WalkTestYawDegPerSec 30 `
        -SparseGpuPhysicsStrict `
        -SparsePhysicsDiagnosticSeed `
        -SparsePhysicsDiagnosticFluidSeed `
        -ExitAfterFrames $GpuPhysicsStrictLongWalkExitAfterFrames `
        -CaptureStartFrame $GpuPhysicsStrictLongWalkStartFrame `
        -CaptureIntervalFrames $GpuPhysicsStrictLongWalkIntervalFrames `
        -CaptureCount $GpuPhysicsStrictLongWalkCount `
        -MinUniqueSampleColors 50 `
        -MinAverageSkyLikePct 8 `
        -MaxAverageTopTerrainPct 55 `
        -MaxFrameTopTerrainPct 98 `
        -MaxFrameMs 120 `
        -MaxPrepMs 80 `
        -MaxGpuRayMs 35 `
        -OutputDir $gpuPhysicsStrictLongWalkOut
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse GPU-physics strict long-walk capture"
    $gpuPhysicsStrictLongWalkLog = Join-Path $gpuPhysicsStrictLongWalkOut "venpod_runtime.log"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse GPU-physics strict long-walk capture" -SavedLog $gpuPhysicsStrictLongWalkLog
    Assert-EngineCaptureOwnershipFromLog `
        -Label "Sparse GPU-physics strict long-walk capture" `
        -OutputDir $gpuPhysicsStrictLongWalkOut `
        -ReadyFrame $GpuPhysicsStrictLongWalkStartFrame `
        -MinSamples $GpuPhysicsStrictLongWalkCount `
        -MinTerrainPct 35.0 `
        -RequireSurfaceFragments $true
    Assert-WalkTelemetryFromLog `
        -Label "Sparse GPU-physics strict long-walk capture" `
        -SavedLog $gpuPhysicsStrictLongWalkLog `
        -MinSamples 90
    Assert-GpuSparsePhysicsFromLog `
        -Label "Sparse GPU-physics strict long-walk capture" `
        -SavedLog $gpuPhysicsStrictLongWalkLog `
        -RequireDiagnosticSeed `
        -RequireFluidSeed `
        -MinResultProposals 2 `
        -MinGpuApplyCompleted 2
}

if (-not $SkipEngineCaptureSmoke) {
    Write-Step "Running sparse engine backbuffer capture smoke..."
    if (Test-Path $captureOut) {
        Remove-Item -LiteralPath $captureOut -Recurse -Force
    }
    & $engineCaptureScript `
        -Config $Config `
        -NoBuild `
        -ExitAfterFrames $EngineCaptureExitAfterFrames `
        -CaptureStartFrame $EngineCaptureStartFrame `
        -CaptureIntervalFrames $EngineCaptureIntervalFrames `
        -CaptureCount $EngineCaptureCount `
        -OutputDir $captureOut
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse engine capture smoke"
    Write-Info "Sparse engine capture artifacts: $captureOut"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse engine capture smoke" -SavedLog (Join-Path $captureOut "venpod_runtime.log")
    Assert-EngineCaptureOwnershipFromLog -Label "Sparse engine capture smoke" -OutputDir $captureOut -ReadyFrame $EngineCaptureStartFrame -MinSamples $EngineCaptureCount -MinTerrainPct 55.0 -MaxFarWaterPct 18.0 -MinFarSvoPct 5.0
} else {
    Write-Info "Sparse engine capture smoke: skipped (-SkipEngineCaptureSmoke)"
}

if (-not $SkipStressEngineCaptureSmoke) {
    Write-Step "Running sparse stress-camera engine backbuffer capture smoke..."
    if (Test-Path $stressCaptureOut) {
        Remove-Item -LiteralPath $stressCaptureOut -Recurse -Force
    }
    & $engineCaptureScript `
        -Config $Config `
        -NoBuild `
        -StressCamera `
        -ExitAfterFrames $StressEngineCaptureExitAfterFrames `
        -CaptureStartFrame $StressEngineCaptureStartFrame `
        -CaptureIntervalFrames $StressEngineCaptureIntervalFrames `
        -CaptureCount $StressEngineCaptureCount `
        -OutputDir $stressCaptureOut
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse stress-camera engine capture smoke"
    Write-Info "Sparse stress-camera engine capture artifacts: $stressCaptureOut"
    $stressRuntimeLog = Join-Path $stressCaptureOut "venpod_runtime.log"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse stress-camera engine capture smoke" -SavedLog $stressRuntimeLog
    Assert-EngineCaptureOwnershipFromLog -Label "Sparse stress-camera engine capture smoke" -OutputDir $stressCaptureOut -ReadyFrame $StressEngineCaptureStartFrame -MinSamples $StressEngineCaptureCount -RequireSurfaceFragments $false -MaxHeightProxyPct 72.0
    Assert-FastRequestTelemetryFromLog -Label "Sparse stress-camera engine capture smoke" -SavedLog $stressRuntimeLog -MinSamples 2
} else {
    Write-Info "Sparse stress-camera engine capture smoke: skipped (-SkipStressEngineCaptureSmoke)"
}

if (-not $SkipSkylineEngineCaptureSmoke) {
    Write-Step "Running sparse skyline/mountain ownership capture smoke..."
    if (Test-Path $skylineCaptureOut) {
        Remove-Item -LiteralPath $skylineCaptureOut -Recurse -Force
    }
    & $engineCaptureScript `
        -Config $Config `
        -NoBuild `
        -SkylineReview `
        -ExitAfterFrames $SkylineEngineCaptureExitAfterFrames `
        -CaptureStartFrame $SkylineEngineCaptureStartFrame `
        -CaptureIntervalFrames $SkylineEngineCaptureIntervalFrames `
        -CaptureCount $SkylineEngineCaptureCount `
        -MinUniqueSampleColors 1 `
        -MaxOwnershipMissPct 0.10 `
        -MaxHeightProxyScreenPct 0 `
        -MaxValleyAtmosphereScreenPct 0 `
        -MaxFarSvoScreenPct 8 `
        -MaxFarWaterScreenPct 1 `
        -MaxSkylineInteriorSkyRunPct 12 `
        -OutputDir $skylineCaptureOut
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse skyline/mountain ownership capture smoke"
    Write-Info "Sparse skyline/mountain ownership artifacts: $skylineCaptureOut"
    $skylineRuntimeLog = Join-Path $skylineCaptureOut "venpod_runtime.log"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse skyline/mountain ownership capture smoke" -SavedLog $skylineRuntimeLog
    Assert-EngineCaptureOwnershipFromLog -Label "Sparse skyline/mountain ownership capture smoke" -OutputDir $skylineCaptureOut -ReadyFrame $SkylineEngineCaptureStartFrame -MinSamples $SkylineEngineCaptureCount -MinTerrainPct 35.0 -RequireSurfaceFragments $false -MaxFarWaterPct 2.0 -MaxHeightProxyPct 1.0 -RequireMidFarTerrain $false
} else {
    Write-Info "Sparse skyline/mountain ownership capture smoke: skipped (-SkipSkylineEngineCaptureSmoke)"
}

if (-not $SkipFastFlightEngineCaptureSmoke) {
    Write-Step "Running sparse fast-flight engine backbuffer capture smoke..."
    if (Test-Path $fastFlightCaptureOut) {
        Remove-Item -LiteralPath $fastFlightCaptureOut -Recurse -Force
    }
    & $engineCaptureScript `
        -Config $Config `
        -NoBuild `
        -StressCamera `
        -StressCameraRadius 1400 `
        -StressCameraHeight 260 `
        -StressCameraBaseHeight 620 `
        -StressCameraSpeed 160 `
        -ExitAfterFrames $FastFlightEngineCaptureExitAfterFrames `
        -CaptureStartFrame $FastFlightEngineCaptureStartFrame `
        -CaptureIntervalFrames $FastFlightEngineCaptureIntervalFrames `
        -CaptureCount $FastFlightEngineCaptureCount `
        -MinUniqueSampleColors 50 `
        -MinFarSvoPct 35 `
        -MaxHeightProxyPct 60 `
        -OutputDir $fastFlightCaptureOut
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse fast-flight engine capture smoke"
    Write-Info "Sparse fast-flight engine capture artifacts: $fastFlightCaptureOut"
    $fastFlightRuntimeLog = Join-Path $fastFlightCaptureOut "venpod_runtime.log"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse fast-flight engine capture smoke" -SavedLog $fastFlightRuntimeLog
    Assert-EngineCaptureOwnershipFromLog -Label "Sparse fast-flight engine capture smoke" -OutputDir $fastFlightCaptureOut -ReadyFrame $FastFlightEngineCaptureStartFrame -MinSamples $FastFlightEngineCaptureCount -RequireSurfaceFragments $false -MaxHeightProxyPct 60.0 -MinFarSvoPct 35.0
    Assert-FastRequestTelemetryFromLog -Label "Sparse fast-flight engine capture smoke" -SavedLog $fastFlightRuntimeLog -MinSamples 3

    Write-Step "Running sparse long fast-flight engine backbuffer capture smoke..."
    if (Test-Path $longFastFlightCaptureOut) {
        Remove-Item -LiteralPath $longFastFlightCaptureOut -Recurse -Force
    }
    & $engineCaptureScript `
        -Config $Config `
        -NoBuild `
        -StressCamera `
        -StressCameraRadius 1900 `
        -StressCameraHeight 340 `
        -StressCameraBaseHeight 760 `
        -StressCameraSpeed 230 `
        -ExitAfterFrames $LongFastFlightEngineCaptureExitAfterFrames `
        -CaptureStartFrame $LongFastFlightEngineCaptureStartFrame `
        -CaptureIntervalFrames $LongFastFlightEngineCaptureIntervalFrames `
        -CaptureCount $LongFastFlightEngineCaptureCount `
        -MinUniqueSampleColors 50 `
        -MinFarSvoPct 35 `
        -MaxHeightProxyPct 45 `
        -OutputDir $longFastFlightCaptureOut
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse long fast-flight engine capture smoke"
    Write-Info "Sparse long fast-flight engine capture artifacts: $longFastFlightCaptureOut"
    $longFastFlightRuntimeLog = Join-Path $longFastFlightCaptureOut "venpod_runtime.log"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse long fast-flight engine capture smoke" -SavedLog $longFastFlightRuntimeLog
    Assert-EngineCaptureOwnershipFromLog -Label "Sparse long fast-flight engine capture smoke" -OutputDir $longFastFlightCaptureOut -ReadyFrame $LongFastFlightEngineCaptureStartFrame -MinSamples $LongFastFlightEngineCaptureCount -MinTerrainPct 35.0 -RequireSurfaceFragments $false -MaxFarWaterPct 18.0 -MaxHeightProxyPct 45.0 -MinFarSvoPct 35.0 -RequireMidFarTerrain $false
    Assert-FastRequestTelemetryFromLog -Label "Sparse long fast-flight engine capture smoke" -SavedLog $longFastFlightRuntimeLog -MinSamples 5
} else {
    Write-Info "Sparse fast-flight engine capture smoke: skipped (-SkipFastFlightEngineCaptureSmoke)"
}

if (-not $SkipFastWaterTransitionEngineCaptureSmoke) {
    Write-Step "Running sparse fast water-transition engine backbuffer capture smoke..."
    if (Test-Path $fastWaterTransitionCaptureOut) {
        Remove-Item -LiteralPath $fastWaterTransitionCaptureOut -Recurse -Force
    }
    & $engineCaptureScript `
        -Config $Config `
        -NoBuild `
        -StressCamera `
        -WaterlineCamera `
        -StressCameraRadius 220 `
        -StressCameraHeight 25 `
        -StressCameraBaseHeight 70 `
        -StressCameraSpeed 120 `
        -ExitAfterFrames $FastWaterTransitionEngineCaptureExitAfterFrames `
        -CaptureStartFrame $FastWaterTransitionEngineCaptureStartFrame `
        -CaptureIntervalFrames $FastWaterTransitionEngineCaptureIntervalFrames `
        -CaptureCount $FastWaterTransitionEngineCaptureCount `
        -MinUniqueSampleColors 50 `
        -MaxAverageTopTerrainPct 55 `
        -MaxFrameTopTerrainPct 92 `
        -MaxHeightProxyPct 60 `
        -OutputDir $fastWaterTransitionCaptureOut
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse fast water-transition engine capture smoke"
    Write-Info "Sparse fast water-transition engine capture artifacts: $fastWaterTransitionCaptureOut"
    $fastWaterTransitionRuntimeLog = Join-Path $fastWaterTransitionCaptureOut "venpod_runtime.log"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse fast water-transition engine capture smoke" -SavedLog $fastWaterTransitionRuntimeLog
    Assert-EngineCaptureOwnershipFromLog -Label "Sparse fast water-transition engine capture smoke" -OutputDir $fastWaterTransitionCaptureOut -ReadyFrame $FastWaterTransitionEngineCaptureStartFrame -MinSamples $FastWaterTransitionEngineCaptureCount -MinTerrainPct 35.0 -RequireSurfaceFragments $false -MaxFarWaterPct 18.0 -MaxHeightProxyPct 60.0 -RequireMidFarTerrain $false -RequireFarSvo $false
    Assert-FastRequestTelemetryFromLog -Label "Sparse fast water-transition engine capture smoke" -SavedLog $fastWaterTransitionRuntimeLog -MinSamples 3

    Write-Step "Running sparse long fast water-transition engine backbuffer capture smoke..."
    if (Test-Path $longFastWaterTransitionCaptureOut) {
        Remove-Item -LiteralPath $longFastWaterTransitionCaptureOut -Recurse -Force
    }
    & $engineCaptureScript `
        -Config $Config `
        -NoBuild `
        -StressCamera `
        -WaterlineCamera `
        -StressCameraRadius 220 `
        -StressCameraHeight 25 `
        -StressCameraBaseHeight 70 `
        -StressCameraSpeed 120 `
        -ExitAfterFrames $LongFastWaterTransitionEngineCaptureExitAfterFrames `
        -CaptureStartFrame $LongFastWaterTransitionEngineCaptureStartFrame `
        -CaptureIntervalFrames $LongFastWaterTransitionEngineCaptureIntervalFrames `
        -CaptureCount $LongFastWaterTransitionEngineCaptureCount `
        -MinUniqueSampleColors 50 `
        -MaxAverageTopTerrainPct 55 `
        -MaxFrameTopTerrainPct 92 `
        -MaxHeightProxyPct 60 `
        -OutputDir $longFastWaterTransitionCaptureOut
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse long fast water-transition engine capture smoke"
    Write-Info "Sparse long fast water-transition engine capture artifacts: $longFastWaterTransitionCaptureOut"
    $longFastWaterTransitionRuntimeLog = Join-Path $longFastWaterTransitionCaptureOut "venpod_runtime.log"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse long fast water-transition engine capture smoke" -SavedLog $longFastWaterTransitionRuntimeLog
    Assert-EngineCaptureOwnershipFromLog -Label "Sparse long fast water-transition engine capture smoke" -OutputDir $longFastWaterTransitionCaptureOut -ReadyFrame $LongFastWaterTransitionEngineCaptureStartFrame -MinSamples $LongFastWaterTransitionEngineCaptureCount -MinTerrainPct 35.0 -RequireSurfaceFragments $false -MaxFarWaterPct 18.0 -MaxHeightProxyPct 60.0 -RequireMidFarTerrain $false -RequireFarSvo $false
    Assert-FastRequestTelemetryFromLog -Label "Sparse long fast water-transition engine capture smoke" -SavedLog $longFastWaterTransitionRuntimeLog -MinSamples 3
} else {
    Write-Info "Sparse fast water-transition engine capture smoke: skipped (-SkipFastWaterTransitionEngineCaptureSmoke)"
}

if (-not $SkipWalkEngineCaptureSmoke) {
    Write-Step "Running sparse walk-movement engine backbuffer capture smoke..."
    if (Test-Path $walkCaptureOut) {
        Remove-Item -LiteralPath $walkCaptureOut -Recurse -Force
    }
    & $engineCaptureScript `
        -Config $Config `
        -NoBuild `
        -WalkTest `
        -WalkTestSpeed 8 `
        -WalkTestYawDegPerSec 30 `
        -ExitAfterFrames $WalkEngineCaptureExitAfterFrames `
        -CaptureStartFrame $WalkEngineCaptureStartFrame `
        -CaptureIntervalFrames $WalkEngineCaptureIntervalFrames `
        -CaptureCount $WalkEngineCaptureCount `
        -MaxAverageTopTerrainPct 52 `
        -MaxFrameTopTerrainPct 70 `
        -OutputDir $walkCaptureOut
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse walk-movement engine capture smoke"
    Write-Info "Sparse walk-movement engine capture artifacts: $walkCaptureOut"
    $walkRuntimeLog = Join-Path $walkCaptureOut "venpod_runtime.log"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse walk-movement engine capture smoke" -SavedLog $walkRuntimeLog
    Assert-EngineCaptureOwnershipFromLog -Label "Sparse walk-movement engine capture smoke" -OutputDir $walkCaptureOut -ReadyFrame $WalkEngineCaptureStartFrame -MinSamples $WalkEngineCaptureCount -MinTerrainPct 35.0 -RequireSurfaceFragments $true -MaxFarWaterPct 18.0
    Assert-WalkTelemetryFromLog -Label "Sparse walk-movement engine capture smoke" -SavedLog $walkRuntimeLog -MinSamples 10
} else {
    Write-Info "Sparse walk-movement engine capture smoke: skipped (-SkipWalkEngineCaptureSmoke)"
}

if (-not $SkipTerrainGapEngineCaptureSmoke) {
    Write-Step "Running sparse steep terrain-gap engine backbuffer capture smoke..."
    if (Test-Path $terrainGapCaptureOut) {
        Remove-Item -LiteralPath $terrainGapCaptureOut -Recurse -Force
    }
    & $engineCaptureScript `
        -Config $Config `
        -NoBuild `
        -WalkTest `
        -WalkTestSpeed 8 `
        -WalkTestYawDegPerSec 30 `
        -WalkTestPitchDeg -38 `
        -WalkTestFixedDtMs 16 `
        -ExitAfterFrames $TerrainGapEngineCaptureExitAfterFrames `
        -CaptureStartFrame $TerrainGapEngineCaptureStartFrame `
        -CaptureIntervalFrames $TerrainGapEngineCaptureIntervalFrames `
        -CaptureCount $TerrainGapEngineCaptureCount `
        -MaxUiContaminationPct 2.0 `
        -MaxSkyPct 5.0 `
        -MinSurfaceScreenPct 55.0 `
        -MaxBackgroundScreenPct 45.0 `
        -MaxHeightProxyPct 5.0 `
        -MaxHeightProxyScreenPct 5.0 `
        -MaxValleyAtmosphereScreenPct 8.0 `
        -MaxTemporalChangedPct 18.5 `
        -MaxTemporalLargeChangePct 3.5 `
        -MaxTemporalCenterChangedPct 16.5 `
        -MaxTemporalMeanLumaDelta 11.5 `
        -MaxTemporalP95LumaDelta 46.0 `
        -MaxTemporalOwnershipLayerDeltaPct 1.0 `
        -MaxTemporalOwnershipMissDeltaPct 0.0 `
        -MaxTemporalOwnershipUnsafeDeltaPct 0.0 `
        -MaxFrameBrushDomeLikePct 30.0 `
        -OutputDir $terrainGapCaptureOut
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse steep terrain-gap engine capture smoke"
    Write-Info "Sparse steep terrain-gap engine capture artifacts: $terrainGapCaptureOut"
    $terrainGapRuntimeLog = Join-Path $terrainGapCaptureOut "venpod_runtime.log"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse steep terrain-gap engine capture smoke" -SavedLog $terrainGapRuntimeLog
    Assert-EngineCaptureOwnershipFromLog -Label "Sparse steep terrain-gap engine capture smoke" -OutputDir $terrainGapCaptureOut -ReadyFrame $TerrainGapEngineCaptureStartFrame -MinSamples $TerrainGapEngineCaptureCount -MinTerrainPct 35.0 -RequireSurfaceFragments $true -MaxFarWaterPct 18.0 -RequireMidFarTerrain $false -RequireFarSvo $false
    Assert-WalkTelemetryFromLog -Label "Sparse steep terrain-gap engine capture smoke" -SavedLog $terrainGapRuntimeLog -MinSamples 8
} else {
    Write-Info "Sparse steep terrain-gap engine capture smoke: skipped (-SkipTerrainGapEngineCaptureSmoke)"
}

if (-not $SkipBrushDomeEngineCaptureSmoke) {
    Write-Step "Running sparse active-brush dome engine backbuffer capture smoke..."
    if (Test-Path $brushDomeCaptureOut) {
        Remove-Item -LiteralPath $brushDomeCaptureOut -Recurse -Force
    }
    & $engineCaptureScript `
        -Config $Config `
        -NoBuild `
        -WalkTest `
        -WalkTestSpeed 8 `
        -WalkTestYawDegPerSec 30 `
        -WalkTestPitchDeg -38 `
        -WalkTestFixedDtMs 16 `
        -EnableBrushInput `
        -SparseBrushPaintSmoke `
        -SparseBrushPaintMovingSmoke `
        -SparseBrushPaintStartFrame 60 `
        -SparseBrushPaintEndFrame 240 `
        -ExitAfterFrames $BrushDomeEngineCaptureExitAfterFrames `
        -CaptureStartFrame $BrushDomeEngineCaptureStartFrame `
        -CaptureIntervalFrames $BrushDomeEngineCaptureIntervalFrames `
        -CaptureCount $BrushDomeEngineCaptureCount `
        -MaxUiContaminationPct 2.0 `
        -MaxSkyPct 5.0 `
        -MaxHeightProxyPct 5.0 `
        -MaxHeightProxyScreenPct 5.0 `
        -MaxValleyAtmosphereScreenPct 15.0 `
        -MaxFrameBrushDomeLikePct 30.0 `
        -MaxAverageBrushDomeLikePct 20.0 `
        -OutputDir $brushDomeCaptureOut
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse active-brush dome engine capture smoke"
    Write-Info "Sparse active-brush dome engine capture artifacts: $brushDomeCaptureOut"
    $brushDomeRuntimeLog = Join-Path $brushDomeCaptureOut "venpod_runtime.log"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse active-brush dome engine capture smoke" -SavedLog $brushDomeRuntimeLog
    Assert-BrushPaintSmokeFromLog -Label "Sparse active-brush dome engine capture smoke" -SavedLog $brushDomeRuntimeLog -MinFrames 60 -MinQueued 3 -MinApplied 1 -MinCases 2 -RequireMoving -MinPathCells 1
    Assert-WalkTelemetryFromLog -Label "Sparse active-brush dome engine capture smoke" -SavedLog $brushDomeRuntimeLog -MinSamples 8
} else {
    Write-Info "Sparse active-brush dome engine capture smoke: skipped (-SkipBrushDomeEngineCaptureSmoke)"
}

if (-not $SkipLongWalkEngineCaptureSmoke) {
    Write-Step "Running sparse long walk-movement engine backbuffer capture smoke..."
    if (Test-Path $longWalkCaptureOut) {
        Remove-Item -LiteralPath $longWalkCaptureOut -Recurse -Force
    }
    & $engineCaptureScript `
        -Config $Config `
        -NoBuild `
        -WalkTest `
        -WalkTestSpeed 8 `
        -WalkTestYawDegPerSec 30 `
        -ExitAfterFrames $LongWalkEngineCaptureExitAfterFrames `
        -CaptureStartFrame $LongWalkEngineCaptureStartFrame `
        -CaptureIntervalFrames $LongWalkEngineCaptureIntervalFrames `
        -CaptureCount $LongWalkEngineCaptureCount `
        -MinUniqueSampleColors 50 `
        -MinAverageSkyLikePct 12 `
        -MaxAverageTopTerrainPct 55 `
        -MaxFrameTopTerrainPct 98 `
        -MaxHeightProxyPct 30 `
        -MaxHeightProxyScreenPct 18 `
        -MaxValleyAtmosphereScreenPct 12 `
        -OutputDir $longWalkCaptureOut
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse long walk-movement engine capture smoke"
    Write-Info "Sparse long walk-movement engine capture artifacts: $longWalkCaptureOut"
    $longWalkRuntimeLog = Join-Path $longWalkCaptureOut "venpod_runtime.log"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse long walk-movement engine capture smoke" -SavedLog $longWalkRuntimeLog
    Assert-EngineCaptureOwnershipFromLog -Label "Sparse long walk-movement engine capture smoke" -OutputDir $longWalkCaptureOut -ReadyFrame $LongWalkEngineCaptureStartFrame -MinSamples $LongWalkEngineCaptureCount -MinTerrainPct 35.0 -RequireSurfaceFragments $true -MaxFarWaterPct 18.0 -MaxHeightProxyPct 30.0
    Assert-WalkTelemetryFromLog -Label "Sparse long walk-movement engine capture smoke" -SavedLog $longWalkRuntimeLog -MinSamples 120
} else {
    Write-Info "Sparse long walk-movement engine capture smoke: skipped (-SkipLongWalkEngineCaptureSmoke)"
}

if (-not $SkipWaterlineEngineCaptureSmoke) {
    Write-Step "Running sparse waterline engine backbuffer capture smoke..."
    if (Test-Path $waterlineCaptureOut) {
        Remove-Item -LiteralPath $waterlineCaptureOut -Recurse -Force
    }
    & $engineCaptureScript `
        -Config $Config `
        -NoBuild `
        -StressCamera `
        -StressCameraRadius 28 `
        -StressCameraHeight 6 `
        -StressCameraBaseHeight -22 `
        -StressCameraSpeed 36 `
        -WaterlineCamera `
        -ExitAfterFrames $WaterlineEngineCaptureExitAfterFrames `
        -CaptureStartFrame $WaterlineEngineCaptureStartFrame `
        -CaptureIntervalFrames $WaterlineEngineCaptureIntervalFrames `
        -CaptureCount $WaterlineEngineCaptureCount `
        -MinUniqueSampleColors 50 `
        -MaxHeightProxyPct 25 `
        -MaxOwnershipMissPct 0.05 `
        -MaxFarSvoScreenPct 5 `
        -MaxFarWaterScreenPct 6 `
        -MaxSkylineInteriorSkyRunPct 12 `
        -OutputDir $waterlineCaptureOut
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse waterline engine capture smoke"
    Write-Info "Sparse waterline engine capture artifacts: $waterlineCaptureOut"
    $waterlineRuntimeLog = Join-Path $waterlineCaptureOut "venpod_runtime.log"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse waterline engine capture smoke" -SavedLog $waterlineRuntimeLog
    Assert-EngineCaptureOwnershipFromLog -Label "Sparse waterline engine capture smoke" -OutputDir $waterlineCaptureOut -ReadyFrame $WaterlineEngineCaptureStartFrame -MinSamples $WaterlineEngineCaptureCount -MinTerrainPct 5.0 -RequireSurfaceFragments $true -MaxFarWaterPct 18.0 -MaxHeightProxyPct 25.0 -RequireMidFarTerrain $false -RequireFarSvo $false

    Write-Step "Running sparse waterline material authority capture smoke..."
    if (Test-Path $waterlineMaterialCaptureOut) {
        Remove-Item -LiteralPath $waterlineMaterialCaptureOut -Recurse -Force
    }
    $waterlineMaterialCaptureCount = [Math]::Min($WaterlineEngineCaptureCount, 2)
    & $engineCaptureScript `
        -Config $Config `
        -NoBuild `
        -StressCamera `
        -StressCameraRadius 28 `
        -StressCameraHeight 6 `
        -StressCameraBaseHeight -22 `
        -StressCameraSpeed 36 `
        -WaterlineCamera `
        -ExitAfterFrames $WaterlineEngineCaptureExitAfterFrames `
        -CaptureStartFrame $WaterlineEngineCaptureStartFrame `
        -CaptureIntervalFrames $WaterlineEngineCaptureIntervalFrames `
        -CaptureCount $waterlineMaterialCaptureCount `
        -SparseDebugMode 54 `
        -MinUniqueSampleColors 2 `
        -MaxHeightProxyPct 25 `
        -MaxOwnershipMissPct 0.05 `
        -MaxFarSvoScreenPct 5 `
        -MaxFarWaterScreenPct 6 `
        -MaxMaterialSandPct 2 `
        -OutputDir $waterlineMaterialCaptureOut
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse waterline material authority capture smoke"
    Write-Info "Sparse waterline material authority artifacts: $waterlineMaterialCaptureOut"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse waterline material authority capture smoke" -SavedLog (Join-Path $waterlineMaterialCaptureOut "venpod_runtime.log")

    Write-Step "Running sparse long waterline engine backbuffer capture smoke..."
    if (Test-Path $longWaterlineCaptureOut) {
        Remove-Item -LiteralPath $longWaterlineCaptureOut -Recurse -Force
    }
    & $engineCaptureScript `
        -Config $Config `
        -NoBuild `
        -StressCamera `
        -StressCameraRadius 28 `
        -StressCameraHeight 6 `
        -StressCameraBaseHeight -22 `
        -StressCameraSpeed 36 `
        -WaterlineCamera `
        -ExitAfterFrames $LongWaterlineEngineCaptureExitAfterFrames `
        -CaptureStartFrame $LongWaterlineEngineCaptureStartFrame `
        -CaptureIntervalFrames $LongWaterlineEngineCaptureIntervalFrames `
        -CaptureCount $LongWaterlineEngineCaptureCount `
        -MinUniqueSampleColors 50 `
        -MaxHeightProxyPct 25 `
        -MaxFarSvoScreenPct 5 `
        -MaxFarWaterScreenPct 6 `
        -MaxSkylineInteriorSkyRunPct 12 `
        -OutputDir $longWaterlineCaptureOut
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse long waterline engine capture smoke"
    Write-Info "Sparse long waterline engine capture artifacts: $longWaterlineCaptureOut"
    $longWaterlineRuntimeLog = Join-Path $longWaterlineCaptureOut "venpod_runtime.log"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse long waterline engine capture smoke" -SavedLog $longWaterlineRuntimeLog
    Assert-EngineCaptureOwnershipFromLog -Label "Sparse long waterline engine capture smoke" -OutputDir $longWaterlineCaptureOut -ReadyFrame $LongWaterlineEngineCaptureStartFrame -MinSamples $LongWaterlineEngineCaptureCount -MinTerrainPct 5.0 -RequireSurfaceFragments $true -MaxFarWaterPct 18.0 -MaxHeightProxyPct 25.0 -RequireMidFarTerrain $false -RequireFarSvo $false
} else {
    Write-Info "Sparse waterline engine capture smoke: skipped (-SkipWaterlineEngineCaptureSmoke)"
}

if (-not $SkipOwnershipDebugCaptureSmoke) {
    Write-Step "Running sparse ownership debug capture smoke..."
    $ownershipDebugRoot = Join-Path $buildDir "logs\sparse_ownership_debug_capture"
    if (Test-Path $ownershipDebugRoot) {
        Remove-Item -LiteralPath $ownershipDebugRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $ownershipDebugRoot -Force | Out-Null

    $ownershipScenarios = @(
        @{
            Label = "normal"
            Output = Join-Path $ownershipDebugRoot "normal"
            Ready = 120
            Exit = 190
            Start = 120
            Interval = 20
            Args = @()
            MinTerrain = 55.0
            MinMidVoxel = 8.0
            MaxFarWater = 18.0
            RequireMidFar = $true
            RequireFarSvo = $true
            MinUnique = 3
            MaxDebugFarFallback = 25.0
            MaxDebugHeightProxy = 45.0
        },
        @{
            Label = "walk"
            Output = Join-Path $ownershipDebugRoot "walk"
            Ready = 220
            Exit = 360
            Start = 220
            Interval = 50
            Args = @("-WalkTest", "-WalkTestSpeed", "8", "-WalkTestYawDegPerSec", "30")
            MinTerrain = 35.0
            MinMidVoxel = 8.0
            MaxFarWater = 18.0
            RequireMidFar = $true
            RequireFarSvo = $true
            MinUnique = 3
            MaxDebugFarFallback = 10.0
            MaxDebugHeightProxy = 35.0
        },
        @{
            Label = "long-walk"
            Output = Join-Path $ownershipDebugRoot "long-walk"
            Ready = $LongWalkEngineCaptureStartFrame
            Exit = ($LongWalkEngineCaptureStartFrame + ($LongWalkEngineCaptureIntervalFrames * 3) + 20)
            Start = $LongWalkEngineCaptureStartFrame
            Interval = $LongWalkEngineCaptureIntervalFrames
            Args = @("-WalkTest", "-WalkTestSpeed", "8", "-WalkTestYawDegPerSec", "30")
            MinTerrain = 35.0
            MinMidVoxel = 8.0
            MaxFarWater = 18.0
            RequireMidFar = $true
            RequireFarSvo = $true
            MinUnique = 3
            MaxDebugFarFallback = 8.0
            MaxDebugHeightProxy = 35.0
        },
        @{
            Label = "stress"
            Output = Join-Path $ownershipDebugRoot "stress"
            Ready = 160
            Exit = 230
            Start = 160
            Interval = 20
            Args = @("-StressCamera")
            MinTerrain = 35.0
            MinMidVoxel = 0.0
            MaxFarWater = 100.0
            RequireMidFar = $false
            RequireFarSvo = $false
            MinUnique = 3
            MaxDebugHeightProxy = 20.0
        },
        @{
            Label = "fast-flight"
            Output = Join-Path $ownershipDebugRoot "fast-flight"
            Ready = $FastFlightEngineCaptureStartFrame
            Exit = $FastFlightEngineCaptureExitAfterFrames
            Start = $FastFlightEngineCaptureStartFrame
            Interval = $FastFlightEngineCaptureIntervalFrames
            Args = @("-StressCamera", "-StressCameraRadius", "1400", "-StressCameraHeight", "260", "-StressCameraBaseHeight", "620", "-StressCameraSpeed", "160")
            MinTerrain = 35.0
            MinMidVoxel = 0.0
            MaxFarWater = 100.0
            MaxHeightProxy = 60.0
            MinFarSvo = 35.0
            RequireMidFar = $false
            RequireFarSvo = $false
            MinUnique = 3
            MaxDebugHeightProxy = 20.0
        },
        @{
            Label = "long-fast-flight"
            Output = Join-Path $ownershipDebugRoot "long-fast-flight"
            Ready = $LongFastFlightEngineCaptureStartFrame
            Exit = $LongFastFlightEngineCaptureExitAfterFrames
            Start = $LongFastFlightEngineCaptureStartFrame
            Interval = $LongFastFlightEngineCaptureIntervalFrames
            Args = @("-StressCamera", "-StressCameraRadius", "1900", "-StressCameraHeight", "340", "-StressCameraBaseHeight", "760", "-StressCameraSpeed", "230")
            MinTerrain = 35.0
            MinMidVoxel = 0.0
            MaxFarWater = 18.0
            MaxHeightProxy = 45.0
            MinFarSvo = 35.0
            RequireMidFar = $false
            RequireFarSvo = $false
            MinUnique = 3
            MaxDebugHeightProxy = 20.0
        },
        @{
            Label = "fast-water-transition"
            Output = Join-Path $ownershipDebugRoot "fast-water-transition"
            Ready = $FastWaterTransitionEngineCaptureStartFrame
            Exit = $FastWaterTransitionEngineCaptureExitAfterFrames
            Start = $FastWaterTransitionEngineCaptureStartFrame
            Interval = $FastWaterTransitionEngineCaptureIntervalFrames
            Args = @("-StressCamera", "-StressCameraRadius", "220", "-StressCameraHeight", "25", "-StressCameraBaseHeight", "70", "-StressCameraSpeed", "120", "-WaterlineCamera")
            MinTerrain = 35.0
            MinMidVoxel = 0.0
            MaxFarWater = 18.0
            MaxHeightProxy = 60.0
            RequireMidFar = $false
            RequireFarSvo = $false
            MinUnique = 3
            MaxDebugFarFallback = 20.0
            MaxDebugHeightProxy = 45.0
        },
        @{
            Label = "long-fast-water-transition"
            Output = Join-Path $ownershipDebugRoot "long-fast-water-transition"
            Ready = $LongFastWaterTransitionEngineCaptureStartFrame
            Exit = $LongFastWaterTransitionEngineCaptureExitAfterFrames
            Start = $LongFastWaterTransitionEngineCaptureStartFrame
            Interval = $LongFastWaterTransitionEngineCaptureIntervalFrames
            Args = @("-StressCamera", "-StressCameraRadius", "220", "-StressCameraHeight", "25", "-StressCameraBaseHeight", "70", "-StressCameraSpeed", "120", "-WaterlineCamera")
            MinTerrain = 35.0
            MinMidVoxel = 0.0
            MaxFarWater = 18.0
            MaxHeightProxy = 60.0
            RequireMidFar = $false
            RequireFarSvo = $false
            MinUnique = 3
            MaxDebugFarFallback = 20.0
            MaxDebugHeightProxy = 45.0
        },
        @{
            Label = "waterline"
            Output = Join-Path $ownershipDebugRoot "waterline"
            Ready = $WaterlineEngineCaptureStartFrame
            Exit = $WaterlineEngineCaptureExitAfterFrames
            Start = $WaterlineEngineCaptureStartFrame
            Interval = $WaterlineEngineCaptureIntervalFrames
            Args = @("-StressCamera", "-StressCameraRadius", "28", "-StressCameraHeight", "6", "-StressCameraBaseHeight", "-22", "-StressCameraSpeed", "36", "-WaterlineCamera")
            MinTerrain = 5.0
            MinMidVoxel = 0.0
            MaxFarWater = 18.0
            MaxHeightProxy = 25.0
            RequireMidFar = $false
            RequireFarSvo = $false
            MinUnique = 2
            MaxDebugFarFallback = 2.0
            MaxDebugHeightProxy = 15.0
        },
        @{
            Label = "long-waterline"
            Output = Join-Path $ownershipDebugRoot "long-waterline"
            Ready = $LongWaterlineEngineCaptureStartFrame
            Exit = $LongWaterlineEngineCaptureExitAfterFrames
            Start = $LongWaterlineEngineCaptureStartFrame
            Interval = $LongWaterlineEngineCaptureIntervalFrames
            Args = @("-StressCamera", "-StressCameraRadius", "28", "-StressCameraHeight", "6", "-StressCameraBaseHeight", "-22", "-StressCameraSpeed", "36", "-WaterlineCamera")
            MinTerrain = 5.0
            MinMidVoxel = 0.0
            MaxFarWater = 18.0
            MaxHeightProxy = 25.0
            RequireMidFar = $false
            RequireFarSvo = $false
            MinUnique = 2
            MaxDebugFarFallback = 2.0
            MaxDebugHeightProxy = 15.0
        }
    )

    foreach ($scenario in $ownershipScenarios) {
        $captureCount = [Math]::Min($OwnershipDebugCaptureCount, 3)
        $scenarioLabel = [string]$scenario["Label"]
        $scenarioOutput = [string]$scenario["Output"]
        $scenarioMinFarWater = 0.0
        if ($scenario.ContainsKey("MinFarWater")) {
            $scenarioMinFarWater = [double]$scenario["MinFarWater"]
        }
        $scenarioMinWaterContext = 0.0
        if ($scenario.ContainsKey("MinWaterContext")) {
            $scenarioMinWaterContext = [double]$scenario["MinWaterContext"]
        }
        $scenarioMaxHeightProxy = 100.0
        if ($scenario.ContainsKey("MaxHeightProxy")) {
            $scenarioMaxHeightProxy = [double]$scenario["MaxHeightProxy"]
        }
        $scenarioMinFarSvo = 0.0
        if ($scenario.ContainsKey("MinFarSvo")) {
            $scenarioMinFarSvo = [double]$scenario["MinFarSvo"]
        }
        $scenarioMinUnique = 3
        if ($scenario.ContainsKey("MinUnique")) {
            $scenarioMinUnique = [int]$scenario["MinUnique"]
        }
        $scenarioMaxDebugFarFallback = -1.0
        if ($scenario.ContainsKey("MaxDebugFarFallback")) {
            $scenarioMaxDebugFarFallback = [double]$scenario["MaxDebugFarFallback"]
        }
        $scenarioMaxDebugHeightProxy = -1.0
        if ($scenario.ContainsKey("MaxDebugHeightProxy")) {
            $scenarioMaxDebugHeightProxy = [double]$scenario["MaxDebugHeightProxy"]
        }
        $captureArgs = @(
            "-ExecutionPolicy", "Bypass",
            "-File", $engineCaptureScript,
            "-Config", $Config,
            "-NoBuild",
            "-SparseDebugMode", "50",
            "-MinUniqueSampleColors", "$scenarioMinUnique",
            "-ExitAfterFrames", "$($scenario["Exit"])",
            "-CaptureStartFrame", "$($scenario["Start"])",
            "-CaptureIntervalFrames", "$($scenario["Interval"])",
            "-CaptureCount", "$captureCount",
            "-OutputDir", $scenarioOutput
        ) + $scenario["Args"]
        & powershell @captureArgs
        Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse ownership debug capture smoke ($scenarioLabel)"
        $scenarioLog = Join-Path $scenarioOutput "venpod_runtime.log"
        Assert-NoRuntimeFailureMarkersFromLog -Label "Sparse ownership debug capture smoke ($scenarioLabel)" -SavedLog $scenarioLog
        Assert-EngineCaptureOwnershipFromLog `
            -Label "Sparse ownership debug capture smoke ($scenarioLabel)" `
            -OutputDir $scenarioOutput `
            -ReadyFrame $scenario["Ready"] `
            -MinSamples $captureCount `
            -MinTerrainPct $scenario["MinTerrain"] `
            -RequireSurfaceFragments $false `
            -MinMidVoxelPct $scenario["MinMidVoxel"] `
            -MaxFarWaterPct $scenario["MaxFarWater"] `
            -MinFarWaterPct $scenarioMinFarWater `
            -MinWaterContextPct $scenarioMinWaterContext `
            -MaxHeightProxyPct $scenarioMaxHeightProxy `
            -MinFarSvoPct $scenarioMinFarSvo `
            -RequireMidFarTerrain $scenario["RequireMidFar"] `
            -RequireFarSvo $scenario["RequireFarSvo"]
        $normalPairOutput = $null
        switch ($scenarioLabel) {
            "normal" { $normalPairOutput = $captureOut }
            "walk" { $normalPairOutput = $walkCaptureOut }
            "long-walk" { $normalPairOutput = $longWalkCaptureOut }
            "stress" { $normalPairOutput = $stressCaptureOut }
            "fast-flight" { $normalPairOutput = $fastFlightCaptureOut }
            "long-fast-flight" { $normalPairOutput = $longFastFlightCaptureOut }
            "fast-water-transition" { $normalPairOutput = $fastWaterTransitionCaptureOut }
            "long-fast-water-transition" { $normalPairOutput = $longFastWaterTransitionCaptureOut }
            "waterline" { $normalPairOutput = $waterlineCaptureOut }
            "long-waterline" { $normalPairOutput = $longWaterlineCaptureOut }
        }
        if ($normalPairOutput -and (Test-Path -LiteralPath $normalPairOutput)) {
            Assert-OwnershipDebugPairs `
                -Label "Sparse ownership debug capture smoke ($scenarioLabel)" `
                -NormalOutputDir $normalPairOutput `
                -DebugOutputDir $scenarioOutput `
                -MinPairs $captureCount `
                -MinNormalUniqueColors 25 `
                -MinDebugUniqueColors 2 `
                -MaxDebugUniqueColors 128 `
                -MaxDebugFarFallbackPct $scenarioMaxDebugFarFallback `
                -MaxDebugHeightProxyPct $scenarioMaxDebugHeightProxy `
                -MaxDebugMissUnsafePct 0.0
        } else {
            Write-Info "Sparse ownership debug capture $scenarioLabel same-frame pair check skipped because the matching normal capture was skipped"
        }
        Write-Info "Sparse ownership debug capture $scenarioLabel artifacts: $scenarioOutput"
    }
} else {
    Write-Info "Sparse ownership debug capture smoke: skipped (-SkipOwnershipDebugCaptureSmoke)"
}

if (-not $SkipPublicDemoCapture) {
    Write-Step "Running public demo capture validation..."
    $publicDemoOut = Join-Path $buildDir "logs\public_demo_capture"
    if (Test-Path $publicDemoOut) {
        Remove-Item -LiteralPath $publicDemoOut -Recurse -Force
    }
    $publicDemoArgs = @(
        "-ExecutionPolicy", "Bypass",
        "-File", $publicDemoCaptureScript,
        "-Config", $Config,
        "-NoBuild",
        "-CaptureStartFrame", "$PublicDemoCaptureStartFrame",
        "-CaptureFrames", "$PublicDemoCaptureFrames",
        "-PlaybackFps", "$PublicDemoPlaybackFps",
        "-OutputDir", $publicDemoOut
    )
    if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
        Write-Info "ffmpeg not found; validating public demo contact sheet/stats without MP4 encode"
        $publicDemoArgs += "-SkipVideo"
    }
    & powershell @publicDemoArgs
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Public demo capture validation"
    Write-Info "Public demo capture artifacts: $publicDemoOut"
    Assert-NoRuntimeFailureMarkersFromLog -Label "Public demo capture validation" -SavedLog (Join-Path $publicDemoOut "venpod_runtime.log")
    Assert-EngineCaptureOwnershipFromLog -Label "Public demo capture validation" -OutputDir $publicDemoOut -ReadyFrame $PublicDemoCaptureStartFrame -MinSamples $PublicDemoCaptureFrames -MinTerrainPct 55.0 -MaxFarWaterPct 18.0 -MinFarSvoPct 5.0
} else {
    Write-Info "Public demo capture validation: skipped (-SkipPublicDemoCapture)"
}

if (-not $SkipPublicReviewReelCapture) {
    Write-Step "Running public review reel validation..."
    $publicReviewReelOut = Join-Path $buildDir "logs\public_review_reel_capture"
    if (Test-Path $publicReviewReelOut) {
        Remove-Item -LiteralPath $publicReviewReelOut -Recurse -Force
    }
    $publicReviewReelArgs = @(
        "-ExecutionPolicy", "Bypass",
        "-File", $publicDemoCaptureScript,
        "-Config", $Config,
        "-NoBuild",
        "-ReviewReel",
        "-CaptureFrames", "$PublicReviewReelFrames",
        "-PlaybackFps", "$PublicReviewReelPlaybackFps",
        "-OutputDir", $publicReviewReelOut
    )
    if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
        Write-Info "ffmpeg not found; validating public review reel segments without MP4 encode"
        $publicReviewReelArgs += "-SkipVideo"
    }
    & powershell @publicReviewReelArgs
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Public review reel validation"
    Write-Info "Public review reel artifacts: $publicReviewReelOut"
    foreach ($segmentName in @("normal", "high-flight", "waterline")) {
        $segmentDir = Join-Path $publicReviewReelOut $segmentName
        Assert-NoRuntimeFailureMarkersFromLog -Label "Public review reel $segmentName" -SavedLog (Join-Path $segmentDir "venpod_runtime.log")
    }
    Assert-EngineCaptureOwnershipFromLog -Label "Public review reel normal" -OutputDir (Join-Path $publicReviewReelOut "normal") -ReadyFrame 120 -MinSamples $PublicReviewReelFrames -MinTerrainPct 55.0 -MaxFarWaterPct 18.0 -MaxHeightProxyPct 65.0
    Assert-EngineCaptureOwnershipFromLog -Label "Public review reel high-flight" -OutputDir (Join-Path $publicReviewReelOut "high-flight") -ReadyFrame 240 -MinSamples $PublicReviewReelFrames -MinTerrainPct 55.0 -RequireSurfaceFragments $false -MinFarSvoPct 35.0 -MaxHeightProxyPct 60.0
    Assert-EngineCaptureOwnershipFromLog -Label "Public review reel waterline" -OutputDir (Join-Path $publicReviewReelOut "waterline") -ReadyFrame 200 -MinSamples $PublicReviewReelFrames -MinTerrainPct 55.0 -MaxFarWaterPct 18.0 -MaxHeightProxyPct 25.0
} else {
    Write-Info "Public review reel validation: skipped (-SkipPublicReviewReelCapture)"
}

Write-Success "Sparse regression gate passed."
