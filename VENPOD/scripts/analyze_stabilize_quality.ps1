param(
    [string]$InputDir = "build\captures\stabilize_quality",
    [int]$WarmupFrame = 300
)

$ErrorActionPreference = "Stop"

function Percentile($Values, [double]$Quantile) {
    $items = @($Values | Where-Object { $null -ne $_ } | Sort-Object)
    if ($items.Count -eq 0) { return $null }
    $index = [Math]::Min($items.Count - 1, [Math]::Floor(($items.Count - 1) * $Quantile))
    return [double]$items[$index]
}

function MaxOrNull($Values) {
    $items = @($Values | Where-Object { $null -ne $_ })
    if ($items.Count -eq 0) { return $null }
    return [double](($items | Measure-Object -Maximum).Maximum)
}

function SumOrNull($Values) {
    $items = @($Values | Where-Object { $null -ne $_ })
    if ($items.Count -eq 0) { return $null }
    return [double](($items | Measure-Object -Sum).Sum)
}

function Get-Frame($Map, [int]$Frame, [string]$Scenario) {
    if (-not $Map.ContainsKey($Frame)) {
        $Map[$Frame] = [ordered]@{
            scenario = $Scenario
            frame = $Frame
            rawMs = $null
            rawAlignedMs = $null
            frameEndRawMs = $null
            rawSource = $null
            bodyMs = $null
            gapPrevMs = $null
            bodyForRawMs = $null
            gapAlignedMs = $null
            loopTailMs = $null
            frameEndLogMs = $null
            knownAfterBodyMs = $null
            gapTailDeltaMs = $null
            postWaitMs = $null
            fenceWaitMs = $null
            pumpWaitMs = $null
            exactGenWaitMs = $null
            surfaceWaitMs = $null
            noncritWaitMs = $null
            prePhysMs = $null
            preRenderMs = $null
            postRenderMs = $null
            gpuReadMs = $null
            endFrameMs = $null
            cmdFinalizeMs = $null
            swapMs = $null
            signalGenMs = $null
            loggingMs = $null
            postRenderResidualMs = $null
            residualUntrackedMs = $null
            sparsePostSumMs = $null
            postWaitResidualMs = $null
            feedbackMs = $null
            cmdMs = $null
            beginFrameMs = $null
            midSnapMs = $null
            uploadPlanMs = $null
            uploadMs = $null
            publishMs = $null
            midUploadMs = $null
            statsMs = $null
            surfExtractMs = $null
            surfPlanMs = $null
            surfSnapMs = $null
            surfaceQueuedTotal = $null
            surfaceQueuedEdited = $null
            surfaceQueuedCollision = $null
            surfaceQueuedVisible = $null
            surfaceQueuedSpeculative = $null
            surfaceProtectedTotal = $null
            surfaceProtectedEdited = $null
            surfaceProtectedCollision = $null
            surfaceProtectedVisible = $null
            surfaceProtectedSpeculative = $null
            surfaceExtractedLastFrame = $null
            surfaceExtractionBudgetLastFrame = $null
            asyncSurfaceQueueDepth = $null
            asyncSurfaceResultDepth = $null
            asyncSurfacePending = $null
            asyncSurfaceEnqueued = $null
            asyncSurfaceApplied = $null
            asyncSurfaceDiscarded = $null
            asyncSurfaceRequeued = $null
            asyncSurfaceRejected = $null
            asyncSurfaceWorkerMs = $null
            asyncSurfaceEnqueueMs = $null
            surfaceInlineExtractionBricks = $null
            surfaceInlineExtractionMs = $null
            parallelSurfaceExtractionActive = $null
            parallelSurfaceExtractionBricks = $null
            parallelSurfaceExtractionWorkers = $null
            parallelSurfaceExtractionWallMs = $null
            surfaceRouteAsync = $null
            surfaceRouteAsyncSaturated = $null
            surfaceRoutePublishSaturated = $null
            surfaceRouteAsyncQueueDepth = $null
            surfaceRouteAsyncResultDepth = $null
            surfaceReadyPublishPending = $null
            surfaceReadyPublishOldestAge = $null
            publishPending = $null
            publishReady = $null
            publishWaitSurface = $null
            publishWaitFence = $null
            publishSurfaceGateDefers = $null
            publishSurfaceGateExtracts = $null
            surfaceStageExtracted = $null
            surfaceStageTerrainCritical = $null
            surfaceStageHiddenCritical = $null
            surfaceStageGeneral = $null
            surfaceStageCritical = $null
            surfaceStageNonCritical = $null
            surfaceStageBudget = $null
            surfaceStageElapsedMs = $null
            surfaceStageMaxMs = $null
            surfaceStageQueued = $null
            surfaceStageProtected = $null
            surfaceStageCatchup = $null
            surfaceStageSkipGeneral = $null
            surfaceStageSkipPrePublishBudget = $null
            prePublishSurfaceExtracted = $null
            prePublishSurfaceTerrainCritical = $null
            prePublishSurfaceHiddenCritical = $null
            prePublishSurfaceHiddenCriticalBudget = $null
            prePublishSurfaceHiddenTracked = $null
            prePublishSurfaceHiddenTrackedBudget = $null
            prePublishSurfaceGeneral = $null
            prePublishSurfaceBudget = $null
            prePublishSurfaceGeneralBudget = $null
            prePublishSurfaceEditActive = $null
            prePublishSurfaceEditGeneralBudget = $null
            prePublishSurfacePostEditSpill = $null
            prePublishSurfacePostEditGeneralBudget = $null
            prePublishSurfacePostEditSpillFrames = $null
            prePublishSurfacePostEditSpillPressureMs = $null
            prePublishSurfaceSplitByOwnership = $null
            prePublishSurfaceGeneralCriticalBudget = $null
            prePublishSurfaceGeneralNonCriticalBudget = $null
            prePublishSurfaceStackedCap = $null
            prePublishSurfaceStackedClipMs = $null
            prePublishSurfaceStackedClipThresholdMs = $null
            prePublishSurfaceStackedGeneralBudget = $null
            prePublishSurfaceGeneralOwnCrit = $null
            prePublishSurfaceGeneralOwnNon = $null
            prePublishSurfaceGeneralEdit = $null
            prePublishSurfaceGeneralCollision = $null
            prePublishSurfaceGeneralVisible = $null
            prePublishSurfaceGeneralSpeculative = $null
            prePublishSurfaceGeneralTimed = $null
            prePublishSurfaceElapsedMs = $null
            prePublishSurfaceMaxMs = $null
            prePublishSurfaceStartup = $null
            prePublishSurfacePostOpen = $null
            prePublishSurfaceQueuedPublishes = $null
            surfStageMs = $null
            surfEmitMs = $null
            framePrepMs = $null
            prepSchedMs = $null
            prepInputMs = $null
            prepSparseMs = $null
            prepCollisionMs = $null
            sparseReqPrepMs = $null
            sparseGenPrepMs = $null
            sparseClipPrepMs = $null
            sparseTrimPrepMs = $null
            sparseReqTerrainCriticalMs = $null
            sparseReqHierarchyMs = $null
            sparseReqStatsFlushMs = $null
            sparseReqPressureTrimMs = $null
            sparseReqBrushRetryMs = $null
            sparseReqOwnerFeedbackMs = $null
            sparseReqMissFeedbackMs = $null
            sparseReqHiddenExactMs = $null
            sparseReqHiddenExactWaterProbeMs = $null
            sparseReqHiddenExactGeneralProbeMs = $null
            sparseReqHiddenExactCandidatesMs = $null
            sparseReqHiddenExactAuditMs = $null
            genPrepAsyncExactApplyMs = $null
            genPrepAsyncExactWorkerMs = $null
            genPrepParallelWallMs = $null
            genPrepParallelActive = $null
            genPrepParallelBricks = $null
            genPrepLoopsMs = $null
            genPrepPumpMs = $null
            genPrepFlushMs = $null
            genPrepTerrainCriticalGenerated = $null
            genPrepHiddenExactGenerated = $null
            genPrepHiddenExactTracked = $null
            sparsePrepMs = $null
            sparseInterestMs = $null
            clipInterestProfileReuse = $null
            clipInterestProfileFullRebuild = $null
            clipInterestSetSizeBefore = $null
            clipInterestSetSizeAfter = $null
            clipInterestVoxelSizeBefore = $null
            clipInterestVoxelSizeAfter = $null
            clipInterestSigMs = $null
            clipInterestRefreshMs = $null
            clipInterestStatsMs = $null
            clipInterestHeightMs = $null
            clipInterestVoxelMs = $null
            clipInterestRebuildMs = $null
            clipInterestTotalMs = $null
            sparseReuse = $null
            sparseBudgetMs = $null
            sparsePumpMs = $null
            sparsePumpHeightMs = $null
            sparsePumpVoxelMs = $null
            sparseHeightQueueMs = $null
            sparseHeightDispatchMs = $null
            sparseHeightJoinMs = $null
            sparseHeightWorkerMaxMs = $null
            sparseHeightGenerateMs = $null
            sparseHeightCommitMs = $null
            sparseHeightPending = $null
            sparseHeightWorkers = $null
            sparseBudgetMid = $null
            sparseGenHeight = $null
            sparseGenVoxel = $null
            sparseQueuedHeight = $null
            sparseQueuedVoxel = $null
            sparseMissingHeight = $null
            sparseMissingVoxel = $null
            sparsePumpBudgetHit = $null
            sparseParallelPumpBricks = $null
            sparseParallelPumpWorkers = $null
            sparseParallelPumpWallMs = $null
            sparseAsyncPending = $null
            sparseAsyncApplied = $null
            sparseAsyncWorkerMs = $null
            sparseVoxelInterestRings = $null
            sparseVoxelInterestBudgeted = $null
            sparseVoxelInterestLineMs = $null
            sparseVoxelInterestAnchorMs = $null
            sparseVoxelInterestSortEmitMs = $null
            sparseVoxelInterestBacklogMs = $null
            sparseVoxelInterestDiagMs = $null
            gpuFrameMs = $null
            raymarchMs = $null
            gpuRawRatio = $null
            rayRawRatio = $null
            rawMinusGpuMs = $null
            sparseSurfaceGpuMs = $null
            sparseUploadGpuMs = $null
            screenPixels = $null
            backgroundPixels = $null
            backgroundShare = $null
            surfaceOwnedPixels = $null
            surfaceOwnedShare = $null
            overdrawRatio = $null
            miss = $null
            unsafeNearMiss = $null
            residentMissingSurface = $null
            visibleMissing = $null
            midBuildMs = $null
            midStageEmitMs = $null
            surfaceUploadNeeds = $null
            surfaceUploadCompleted = $null
            surfaceUploadDirtyAttempt = $null
            surfaceUploadFullCatchup = $null
            surfaceUploadPendingDirty = $null
            surfaceUploadPendingRemoved = $null
            surfaceUploadStagedFaces = $null
            surfaceUploadCopyRegions = $null
            surfaceUploadDirtyCopied = $null
            surfaceUploadCleanSkipped = $null
            surfaceUploadDeferred = $null
            surfaceUploadPatchBricks = $null
            surfaceUploadPatchFaces = $null
            surfaceUploadPatchRegions = $null
            surfaceUploadStagedMB = $null
            surfaceDirtyStageDirty = $null
            surfaceDirtyStageRemoved = $null
            surfaceDirtyStageAllocChanged = $null
            surfaceDirtyStageCopyBricks = $null
            surfaceDirtyStageCopyFaces = $null
            surfaceDirtyStageFullCopyBricks = $null
            surfaceDirtyStageFullCopyFaces = $null
            surfaceDirtyStagePatchBricks = $null
            surfaceDirtyStagePatchFaces = $null
            surfaceDirtyStagePatchRegions = $null
            surfaceDirtyStageMirrorCmpBricks = $null
            surfaceDirtyStageCleanMirrorBricks = $null
            surfaceDirtyStageChangedRuns = $null
            surfaceDirtyStageChangedRunFaces = $null
            surfaceDirtyStageNewBricks = $null
            surfaceDirtyStageDeferred = $null
            surfaceDirtyStageRangeCopies = $null
            surfaceDirtyStageDrawCopies = $null
            surfaceDirtyStageRecordCopies = $null
            surfaceDirtyStageClusterCopies = $null
            surfaceDirtyStageMetadataFull = $null
            surfaceDirtyStageMetadataIncr = $null
            surfaceDirtyStageStagedMB = $null
            surfaceDirtyStageSetupMs = $null
            surfaceDirtyStageRemovedMs = $null
            surfaceDirtyStageDirtyLoopMs = $null
            surfaceDirtyStageFinalMs = $null
            surfaceDirtyStageTotalMs = $null
        }
    }
    return $Map[$Frame]
}

function Set-SparsePostDerived($Row) {
    $parts = @(
        $Row.feedbackMs,
        $Row.cmdMs,
        $Row.beginFrameMs,
        $Row.midSnapMs,
        $Row.uploadPlanMs,
        $Row.uploadMs,
        $Row.publishMs,
        $Row.midUploadMs,
        $Row.statsMs,
        $Row.surfExtractMs,
        $Row.surfPlanMs,
        $Row.surfSnapMs,
        $Row.surfStageMs,
        $Row.surfEmitMs
    )
    if (@($parts | Where-Object { $null -eq $_ }).Count -ne 0) {
        return
    }
    $sum = 0.0
    foreach ($part in $parts) {
        $sum += [double]$part
    }
    $Row.sparsePostSumMs = $sum
    if ($null -ne $Row.postWaitMs) {
        $Row.postWaitResidualMs = [Math]::Max(0.0, [double]$Row.postWaitMs - $sum)
    }
}

function Set-SurfaceWorkFromLine($Row, [string]$Line) {
    $inlineMatch = [regex]::Match(
        $Line,
        'surf(?:ace)?Inline=bricks/ms:(?<bricks>\d+)/(?<ms>[-+0-9.]+)')
    if ($inlineMatch.Success) {
        $Row.surfaceInlineExtractionBricks = [int64]$inlineMatch.Groups["bricks"].Value
        $Row.surfaceInlineExtractionMs = [double]$inlineMatch.Groups["ms"].Value
    }

    $parallelMatch = [regex]::Match(
        $Line,
        'surf(?:ace)?Parallel=active/bricks/workers/wallMs:(?<active>\d+)/(?<bricks>\d+)/(?<workers>\d+)/(?<wall>[-+0-9.]+)')
    if ($parallelMatch.Success) {
        $Row.parallelSurfaceExtractionActive = [int64]$parallelMatch.Groups["active"].Value
        $Row.parallelSurfaceExtractionBricks = [int64]$parallelMatch.Groups["bricks"].Value
        $Row.parallelSurfaceExtractionWorkers = [int64]$parallelMatch.Groups["workers"].Value
        $Row.parallelSurfaceExtractionWallMs = [double]$parallelMatch.Groups["wall"].Value
    }

    $frameSurfaceMatch = [regex]::Match(
        $Line,
        'frameSurface=inlineBricks/inlineMs/parallelBricks/parallelWorkers/parallelWallMs/asyncEnqueued/asyncRejected/asyncEnqueueMs/asyncWorkerMs:(?<inlineBricks>\d+)/(?<inlineMs>[-+0-9.]+)/(?<parallelBricks>\d+)/(?<parallelWorkers>\d+)/(?<parallelWall>[-+0-9.]+)/(?<asyncEnqueued>\d+)/(?<asyncRejected>\d+)/(?<asyncEnqueueMs>[-+0-9.]+)/(?<asyncWorkerMs>[-+0-9.]+)')
    if ($frameSurfaceMatch.Success) {
        $Row.surfaceInlineExtractionBricks = [int64]$frameSurfaceMatch.Groups["inlineBricks"].Value
        $Row.surfaceInlineExtractionMs = [double]$frameSurfaceMatch.Groups["inlineMs"].Value
        $Row.parallelSurfaceExtractionBricks = [int64]$frameSurfaceMatch.Groups["parallelBricks"].Value
        $Row.parallelSurfaceExtractionWorkers = [int64]$frameSurfaceMatch.Groups["parallelWorkers"].Value
        $Row.parallelSurfaceExtractionWallMs = [double]$frameSurfaceMatch.Groups["parallelWall"].Value
        $Row.parallelSurfaceExtractionActive =
            if ($Row.parallelSurfaceExtractionBricks -gt 0) { 1 } else { 0 }
        $Row.asyncSurfaceEnqueued = [int64]$frameSurfaceMatch.Groups["asyncEnqueued"].Value
        $Row.asyncSurfaceRejected = [int64]$frameSurfaceMatch.Groups["asyncRejected"].Value
        $Row.asyncSurfaceEnqueueMs = [double]$frameSurfaceMatch.Groups["asyncEnqueueMs"].Value
        $Row.asyncSurfaceWorkerMs = [double]$frameSurfaceMatch.Groups["asyncWorkerMs"].Value
    }

    $asyncDetailMatch = [regex]::Match(
        $Line,
        'surf(?:ace)?AsyncDetail=rejected/workerMs/enqueueMs:(?<rejected>\d+)/(?<worker>[-+0-9.]+)/(?<enqueue>[-+0-9.]+)')
    if ($asyncDetailMatch.Success) {
        $Row.asyncSurfaceRejected = [int64]$asyncDetailMatch.Groups["rejected"].Value
        $Row.asyncSurfaceWorkerMs = [double]$asyncDetailMatch.Groups["worker"].Value
        $Row.asyncSurfaceEnqueueMs = [double]$asyncDetailMatch.Groups["enqueue"].Value
    }
}

function DominantCause($Row) {
    $gapPrevCandidate = if ($null -eq $Row.gapAlignedMs) { $Row.gapPrevMs } else { $null }
    $residualUntrackedCandidate = if ($null -eq $Row.gapTailDeltaMs) { $Row.residualUntrackedMs } else { $null }
    $hasRequestSplit =
        $null -ne $Row.sparseReqHiddenExactWaterProbeMs -or
        $null -ne $Row.sparseReqHiddenExactGeneralProbeMs -or
        $null -ne $Row.sparseReqHiddenExactCandidatesMs -or
        $null -ne $Row.sparseReqHiddenExactAuditMs
    $sparseReqPrepCandidate = if ($hasRequestSplit) { $null } else { $Row.sparseReqPrepMs }
    $hasClipInterestProfile =
        $null -ne $Row.clipInterestSigMs -or
        $null -ne $Row.clipInterestRefreshMs -or
        $null -ne $Row.clipInterestStatsMs -or
        $null -ne $Row.clipInterestHeightMs -or
        $null -ne $Row.clipInterestVoxelMs
    $sparseInterestCandidate = if ($hasClipInterestProfile) { $null } else { $Row.sparseInterestMs }
    # Prefer specific frame phases over umbrella buckets. "postWait" is a historical
    # name for the whole sparse/update region, so reporting it as the cause hides
    # the real substep such as surface staging, clipmap interest, or raymarch.
    $specificCandidates = @(
        @{ name = "raymarch"; value = $Row.raymarchMs },
        @{ name = "sparseSurfaceGpu"; value = $Row.sparseSurfaceGpuMs },
        @{ name = "sparseUploadGpu"; value = $Row.sparseUploadGpuMs },
        @{ name = "sparseReqPrep"; value = $sparseReqPrepCandidate },
        @{ name = "hiddenExactWaterProbe"; value = $Row.sparseReqHiddenExactWaterProbeMs },
        @{ name = "hiddenExactGeneralProbe"; value = $Row.sparseReqHiddenExactGeneralProbeMs },
        @{ name = "hiddenExactCandidates"; value = $Row.sparseReqHiddenExactCandidatesMs },
        @{ name = "hiddenExactAudit"; value = $Row.sparseReqHiddenExactAuditMs },
        @{ name = "sparseGenPrep"; value = $Row.sparseGenPrepMs },
        @{ name = "sparseClipPrep"; value = $Row.sparseClipPrepMs },
        @{ name = "sparseTrimPrep"; value = $Row.sparseTrimPrepMs },
        @{ name = "clipInterestHeight"; value = $Row.clipInterestHeightMs },
        @{ name = "clipInterestVoxel"; value = $Row.clipInterestVoxelMs },
        @{ name = "clipInterestStats"; value = $Row.clipInterestStatsMs },
        @{ name = "clipInterestRefresh"; value = $Row.clipInterestRefreshMs },
        @{ name = "clipInterestSig"; value = $Row.clipInterestSigMs },
        @{ name = "voxelInterestAnchor"; value = $Row.sparseVoxelInterestAnchorMs },
        @{ name = "voxelInterestSortEmit"; value = $Row.sparseVoxelInterestSortEmitMs },
        @{ name = "voxelInterestLine"; value = $Row.sparseVoxelInterestLineMs },
        @{ name = "voxelInterestBacklog"; value = $Row.sparseVoxelInterestBacklogMs },
        @{ name = "voxelInterestDiag"; value = $Row.sparseVoxelInterestDiagMs },
        @{ name = "sparseInterest"; value = $sparseInterestCandidate },
        @{ name = "heightWorkerMax"; value = $Row.sparseHeightWorkerMaxMs },
        @{ name = "heightJoin"; value = $Row.sparseHeightJoinMs },
        @{ name = "heightDispatch"; value = $Row.sparseHeightDispatchMs },
        @{ name = "heightCommit"; value = $Row.sparseHeightCommitMs },
        @{ name = "heightQueue"; value = $Row.sparseHeightQueueMs },
        @{ name = "pumpHeight"; value = $Row.sparsePumpHeightMs },
        @{ name = "pumpVoxel"; value = $Row.sparsePumpVoxelMs },
        @{ name = "parallelPumpWall"; value = $Row.sparseParallelPumpWallMs },
        @{ name = "asyncClipWorker"; value = $Row.sparseAsyncWorkerMs },
        @{ name = "feedback"; value = $Row.feedbackMs },
        @{ name = "cmd"; value = $Row.cmdMs },
        @{ name = "beginFrame"; value = $Row.beginFrameMs },
        @{ name = "midSnap"; value = $Row.midSnapMs },
        @{ name = "uploadPlan"; value = $Row.uploadPlanMs },
        @{ name = "surfaceInline"; value = $Row.surfaceInlineExtractionMs },
        @{ name = "surfaceParallelWall"; value = $Row.parallelSurfaceExtractionWallMs },
        @{ name = "asyncSurfaceEnqueue"; value = $Row.asyncSurfaceEnqueueMs },
        @{ name = "surfStage"; value = $Row.surfStageMs },
        @{ name = "surfExtract"; value = $Row.surfExtractMs },
        @{ name = "surfSnap"; value = $Row.surfSnapMs },
        @{ name = "surfEmit"; value = $Row.surfEmitMs },
        @{ name = "surfPlan"; value = $Row.surfPlanMs },
        @{ name = "midUpload"; value = $Row.midUploadMs },
        @{ name = "upload"; value = $Row.uploadMs },
        @{ name = "publish"; value = $Row.publishMs },
        @{ name = "midBuild"; value = $Row.midBuildMs },
        @{ name = "midStageEmit"; value = $Row.midStageEmitMs },
        @{ name = "fenceWait"; value = $Row.fenceWaitMs },
        @{ name = "surfaceWait"; value = $Row.surfaceWaitMs },
        @{ name = "exactGenWait"; value = $Row.exactGenWaitMs },
        @{ name = "noncritWait"; value = $Row.noncritWaitMs },
        @{ name = "pumpWait"; value = $Row.pumpWaitMs },
        @{ name = "gpuRead"; value = $Row.gpuReadMs },
        @{ name = "endFrame"; value = $Row.endFrameMs },
        @{ name = "swap"; value = $Row.swapMs },
        @{ name = "cmdFinalize"; value = $Row.cmdFinalizeMs },
        @{ name = "signalGen"; value = $Row.signalGenMs },
        @{ name = "logging"; value = $Row.loggingMs },
        # Unaccounted post-render time (e.g. large surface-copy submits stalling in
        # the driver): without this, such frames mislabel as a small prep phase.
        @{ name = "postRenderResidual"; value = $Row.postRenderResidualMs }
    ) | Where-Object { $null -ne $_.value } | Sort-Object { [double]$_.value } -Descending
    if ($specificCandidates.Count -ne 0 -and [double]$specificCandidates[0].value -gt 0.0) {
        return $specificCandidates[0].name
    }
    $fallbackCandidates = @(
        @{ name = "postRenderResidual"; value = $Row.postRenderResidualMs },
        @{ name = "residualUntracked"; value = $residualUntrackedCandidate },
        @{ name = "gapTailDelta"; value = $Row.gapTailDeltaMs },
        @{ name = "loopTail"; value = $Row.loopTailMs },
        @{ name = "frameEndLog"; value = $Row.frameEndLogMs },
        @{ name = "gapAligned"; value = $Row.gapAlignedMs },
        @{ name = "postWaitResidual"; value = $Row.postWaitResidualMs },
        @{ name = "gapPrev"; value = $gapPrevCandidate }
    ) | Where-Object { $null -ne $_.value } | Sort-Object { [double]$_.value } -Descending
    if ($fallbackCandidates.Count -eq 0) { return "" }
    return $fallbackCandidates[0].name
}

function CauseBreakdown($Rows, [double]$ThresholdMs) {
    $items = @($Rows | Where-Object {
        $null -ne $_.rawMs -and [double]$_.rawMs -gt $ThresholdMs
    })
    if ($items.Count -eq 0) { return "" }
    return (($items |
        Group-Object dominantCause |
        Sort-Object Count -Descending |
        ForEach-Object { "$($_.Name):$($_.Count)" }) -join ";")
}

function Get-LogValue($Line, [string]$Name) {
    $match = [regex]::Match($Line, "(?:^|\s)$([regex]::Escape($Name))=([^\s]+)")
    if (-not $match.Success) { return $null }
    return $match.Groups[1].Value
}

function Get-LogDouble($Line, [string]$Name) {
    $value = Get-LogValue $Line $Name
    if ($null -eq $value) { return $null }
    return [double]$value
}

function Get-LogInt64($Line, [string]$Name) {
    $value = Get-LogValue $Line $Name
    if ($null -eq $value) { return $null }
    return [int64]$value
}

$inputPath = Resolve-Path $InputDir
$logs = Get-ChildItem -LiteralPath $inputPath -Filter "*.log" |
    Where-Object { $_.BaseName -match "_quality$" } |
    Sort-Object Name

if ($logs.Count -eq 0) {
    throw "No *_quality.log files found in $inputPath"
}

$allRows = New-Object System.Collections.Generic.List[object]
$summary = New-Object System.Collections.Generic.List[object]

foreach ($log in $logs) {
    $scenario = $log.BaseName -replace "_quality$", ""
    $frames = @{}

    Get-Content -LiteralPath $log.FullName | ForEach-Object {
        $line = $_
        if ($line -match 'PERF_RAWALIGN frame=(?<frame>\d+) rawMs=(?<raw>[-+0-9.]+) body=(?<body>[-+0-9.]+) gapAligned=(?<gapAligned>[-+0-9.]+) loopTail=(?<loopTail>[-+0-9.]+) frameEndLog=(?<frameEndLog>[-+0-9.]+) knownAfterBody=(?<knownAfterBody>[-+0-9.]+) gapTailDelta=(?<gapTailDelta>[-+0-9.]+) present=(?<present>[-+0-9.]+) fenceWait=(?<fence>[-+0-9.]+).*?gaps=postWait/prePhys/preRender/postRender:(?<postWait>[-+0-9.]+)/(?<prePhys>[-+0-9.]+)/(?<preRender>[-+0-9.]+)/(?<postRender>[-+0-9.]+).*?sparsePost=feedback/cmd/begin/midSnap/plan/upload/publish/midUpload/stats/surfExtract/surfPlan/surfSnap/surfStage/surfEmit:(?<feedback>[-+0-9.]+)/(?<cmd>[-+0-9.]+)/(?<begin>[-+0-9.]+)/(?<midSnap>[-+0-9.]+)/(?<plan>[-+0-9.]+)/(?<upload>[-+0-9.]+)/(?<publish>[-+0-9.]+)/(?<midUpload>[-+0-9.]+)/(?<stats>[-+0-9.]+)/(?<surfExtract>[-+0-9.]+)/(?<surfPlan>[-+0-9.]+)/(?<surfSnap>[-+0-9.]+)/(?<surfStage>[-+0-9.]+)/(?<surfEmit>[-+0-9.]+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $row.rawAlignedMs = [double]$Matches.raw
                $row.rawMs = [double]$Matches.raw
                $row.rawSource = "rawalign"
                $row.bodyMs = [double]$Matches.body
                $row.bodyForRawMs = [double]$Matches.body
                $row.gapAlignedMs = [double]$Matches.gapAligned
                $row.loopTailMs = [double]$Matches.loopTail
                $row.frameEndLogMs = [double]$Matches.frameEndLog
                $row.knownAfterBodyMs = [double]$Matches.knownAfterBody
                $row.gapTailDeltaMs = [double]$Matches.gapTailDelta
                $row.fenceWaitMs = [double]$Matches.fence
                $row.postWaitMs = [double]$Matches.postWait
                $row.prePhysMs = [double]$Matches.prePhys
                $row.preRenderMs = [double]$Matches.preRender
                $row.postRenderMs = [double]$Matches.postRender
                $row.feedbackMs = [double]$Matches.feedback
                $row.cmdMs = [double]$Matches.cmd
                $row.beginFrameMs = [double]$Matches.begin
                $row.midSnapMs = [double]$Matches.midSnap
                $row.uploadPlanMs = [double]$Matches.plan
                $row.uploadMs = [double]$Matches.upload
                $row.publishMs = [double]$Matches.publish
                $row.midUploadMs = [double]$Matches.midUpload
                $row.statsMs = [double]$Matches.stats
                $row.surfExtractMs = [double]$Matches.surfExtract
                $row.surfPlanMs = [double]$Matches.surfPlan
                $row.surfSnapMs = [double]$Matches.surfSnap
                $row.surfStageMs = [double]$Matches.surfStage
                $row.surfEmitMs = [double]$Matches.surfEmit
                Set-SparsePostDerived $row
            }
        } elseif ($line -match 'PERF_FRAME_END frame=(?<frame>\d+).*?gaps=postWait/prePhys/preRender/postRender:(?<postWait>[-+0-9.]+)/(?<prePhys>[-+0-9.]+)/(?<preRender>[-+0-9.]+)/(?<postRender>[-+0-9.]+).*?sparsePost=feedback/cmd/begin/midSnap/plan/upload/publish/midUpload/stats/surfExtract/surfPlan/surfSnap/surfStage/surfEmit:(?<feedback>[-+0-9.]+)/(?<cmd>[-+0-9.]+)/(?<begin>[-+0-9.]+)/(?<midSnap>[-+0-9.]+)/(?<plan>[-+0-9.]+)/(?<upload>[-+0-9.]+)/(?<publish>[-+0-9.]+)/(?<midUpload>[-+0-9.]+)/(?<stats>[-+0-9.]+)/(?<surfExtract>[-+0-9.]+)/(?<surfPlan>[-+0-9.]+)/(?<surfSnap>[-+0-9.]+)/(?<surfStage>[-+0-9.]+)/(?<surfEmit>[-+0-9.]+).*?body=(?<body>[-+0-9.]+).*?gapPrev=(?<gapPrev>[-+0-9.]+).*?rawMs=(?<raw>[-+0-9.]+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $row.frameEndRawMs = [double]$Matches.raw
                if ($null -eq $row.rawMs) {
                    $row.rawMs = [double]$Matches.raw
                    $row.rawSource = "frame_end_legacy"
                }
                $row.bodyMs = [double]$Matches.body
                $row.gapPrevMs = [double]$Matches.gapPrev
                $bodyForRaw = Get-LogDouble $line "bodyForRaw"
                if ($null -ne $bodyForRaw) { $row.bodyForRawMs = $bodyForRaw }
                $gapAligned = Get-LogDouble $line "gapAligned"
                if ($null -ne $gapAligned) { $row.gapAlignedMs = $gapAligned }
                $loopTail = Get-LogDouble $line "loopTail"
                if ($null -ne $loopTail) { $row.loopTailMs = $loopTail }
                $frameEndLog = Get-LogDouble $line "frameEndLog"
                if ($null -ne $frameEndLog) { $row.frameEndLogMs = $frameEndLog }
                $knownAfterBody = Get-LogDouble $line "knownAfterBody"
                if ($null -ne $knownAfterBody) { $row.knownAfterBodyMs = $knownAfterBody }
                $gapTailDelta = Get-LogDouble $line "gapTailDelta"
                if ($null -ne $gapTailDelta) { $row.gapTailDeltaMs = $gapTailDelta }
                $row.postWaitMs = [double]$Matches.postWait
                $row.prePhysMs = [double]$Matches.prePhys
                $row.preRenderMs = [double]$Matches.preRender
                $row.postRenderMs = [double]$Matches.postRender
                $row.feedbackMs = [double]$Matches.feedback
                $row.cmdMs = [double]$Matches.cmd
                $row.beginFrameMs = [double]$Matches.begin
                $row.midSnapMs = [double]$Matches.midSnap
                $row.uploadPlanMs = [double]$Matches.plan
                $row.uploadMs = [double]$Matches.upload
                $row.publishMs = [double]$Matches.publish
                $row.midUploadMs = [double]$Matches.midUpload
                $row.statsMs = [double]$Matches.stats
                $row.surfExtractMs = [double]$Matches.surfExtract
                $row.surfPlanMs = [double]$Matches.surfPlan
                $row.surfSnapMs = [double]$Matches.surfSnap
                $row.surfStageMs = [double]$Matches.surfStage
                $row.surfEmitMs = [double]$Matches.surfEmit
                Set-SparsePostDerived $row
            }
        } elseif ($line -match 'PERF frame=(?<frame>\d+).*?\sprep=(?<prep>[-+0-9.]+) prepSplit=sched/input/sparse/coll:(?<sched>[-+0-9.]+)/(?<input>[-+0-9.]+)/(?<sparse>[-+0-9.]+)/(?<coll>[-+0-9.]+) sparseSplit=req/gen/clip/trim:(?<req>[-+0-9.]+)/(?<gen>[-+0-9.]+)/(?<clip>[-+0-9.]+)/(?<trim>[-+0-9.]+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $row.framePrepMs = [double]$Matches.prep
                $row.prepSchedMs = [double]$Matches.sched
                $row.prepInputMs = [double]$Matches.input
                $row.prepSparseMs = [double]$Matches.sparse
                $row.prepCollisionMs = [double]$Matches.coll
                $row.sparseReqPrepMs = [double]$Matches.req
                $row.sparseGenPrepMs = [double]$Matches.gen
                $row.sparseClipPrepMs = [double]$Matches.clip
                $row.sparseTrimPrepMs = [double]$Matches.trim
            }
        } elseif ($line -match 'PERF_SPARSE_STEPS frame=(?<frame>\d+).*?reqPrep=(?<req>[-+0-9.]+) genPrep=(?<gen>[-+0-9.]+) clipInterest=(?<clip>[-+0-9.]+) clipBudget=(?<budget>[-+0-9.]+) clipPump=(?<pump>[-+0-9.]+) trim=(?<trim>[-+0-9.]+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $row.sparseReqPrepMs = [double]$Matches.req
                $row.sparseGenPrepMs = [double]$Matches.gen
                if ($null -eq $row.sparseClipPrepMs) {
                    $row.sparseClipPrepMs = [double]$Matches.clip
                }
                if ($null -eq $row.sparseInterestMs) {
                    $row.sparseInterestMs = [double]$Matches.clip
                }
                if ($null -eq $row.sparseBudgetMs) {
                    $row.sparseBudgetMs = [double]$Matches.budget
                }
                if ($null -eq $row.sparsePumpMs) {
                    $row.sparsePumpMs = [double]$Matches.pump
                }
                $row.sparseTrimPrepMs = [double]$Matches.trim
            }
        } elseif ($line -match 'PERF_SPARSE_REQ frame=(?<frame>\d+) reqPrep=(?<req>[-+0-9.]+) = terrainCrit=(?<terrain>[-+0-9.]+) hierarchy=(?<hierarchy>[-+0-9.]+) statsFlush=(?<stats>[-+0-9.]+) pressureTrim=(?<pressure>[-+0-9.]+) brushRetry=(?<brush>[-+0-9.]+) ownerFeedback=(?<owner>[-+0-9.]+) missFeedback=(?<miss>[-+0-9.]+) hiddenExact=(?<hidden>[-+0-9.]+)(?: hiddenExactSplit=water/general/candidates/audit:(?<water>[-+0-9.]+)/(?<general>[-+0-9.]+)/(?<candidates>[-+0-9.]+)/(?<audit>[-+0-9.]+))? \|\| genPrep=(?<gen>[-+0-9.]+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $row.sparseReqPrepMs = [double]$Matches.req
                $row.sparseReqTerrainCriticalMs = [double]$Matches.terrain
                $row.sparseReqHierarchyMs = [double]$Matches.hierarchy
                $row.sparseReqStatsFlushMs = [double]$Matches.stats
                $row.sparseReqPressureTrimMs = [double]$Matches.pressure
                $row.sparseReqBrushRetryMs = [double]$Matches.brush
                $row.sparseReqOwnerFeedbackMs = [double]$Matches.owner
                $row.sparseReqMissFeedbackMs = [double]$Matches.miss
                $row.sparseReqHiddenExactMs = [double]$Matches.hidden
                if ($Matches.ContainsKey("water") -and $Matches.water.Length -ne 0) {
                    $row.sparseReqHiddenExactWaterProbeMs = [double]$Matches.water
                    $row.sparseReqHiddenExactGeneralProbeMs = [double]$Matches.general
                    $row.sparseReqHiddenExactCandidatesMs = [double]$Matches.candidates
                    $row.sparseReqHiddenExactAuditMs = [double]$Matches.audit
                }
                $row.sparseGenPrepMs = [double]$Matches.gen
            }
        } elseif ($line -match 'PERF_GENPREP frame=(?<frame>\d+) genPrep=(?<gen>[-+0-9.]+) = asyncExactApplyMs=(?<apply>[-+0-9.]+) asyncExactWorkerMs=(?<worker>[-+0-9.]+) parallelWallMs=(?<wall>[-+0-9.]+) parallelActive=(?<active>\d+) parallelBricks=(?<bricks>\d+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $row.sparseGenPrepMs = [double]$Matches.gen
                $row.genPrepAsyncExactApplyMs = [double]$Matches.apply
                $row.genPrepAsyncExactWorkerMs = [double]$Matches.worker
                $row.genPrepParallelWallMs = [double]$Matches.wall
                $row.genPrepParallelActive = [int64]$Matches.active
                $row.genPrepParallelBricks = [int64]$Matches.bricks
                $genPrepSplit = [regex]::Match(
                    $line,
                    'split=loops/pump/flush:(?<loops>[-+0-9.]+)/(?<pump>[-+0-9.]+)/(?<flush>[-+0-9.]+) terrainCritGen=(?<terrainGen>\d+) hiddenExactGen=(?<hiddenGen>\d+) hiddenExactTracked=(?<hiddenTracked>\d+)')
                if ($genPrepSplit.Success) {
                    $row.genPrepLoopsMs = [double]$genPrepSplit.Groups["loops"].Value
                    $row.genPrepPumpMs = [double]$genPrepSplit.Groups["pump"].Value
                    $row.genPrepFlushMs = [double]$genPrepSplit.Groups["flush"].Value
                    $row.genPrepTerrainCriticalGenerated = [int64]$genPrepSplit.Groups["terrainGen"].Value
                    $row.genPrepHiddenExactGenerated = [int64]$genPrepSplit.Groups["hiddenGen"].Value
                    $row.genPrepHiddenExactTracked = [int64]$genPrepSplit.Groups["hiddenTracked"].Value
                }
            }
        } elseif ($line -match 'PERF_UNTRACKED frame=(?<frame>\d+).*?slivers=gpuRead/endFrame/cmdFinalize/swap/signalGen/logging:(?<gpuRead>[-+0-9.]+)/(?<endFrame>[-+0-9.]+)/(?<cmdFinalize>[-+0-9.]+)/(?<swap>[-+0-9.]+)/(?<signalGen>[-+0-9.]+)/(?<logging>[-+0-9.]+).*?postRenderResidual=(?<postRenderResidual>[-+0-9.]+).*?residualUntracked=(?<residualUntracked>[-+0-9.]+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $bodyPrev = Get-LogDouble $line "bodyPrev"
                if ($null -ne $bodyPrev) { $row.bodyForRawMs = $bodyPrev }
                $gapAligned = Get-LogDouble $line "gapAligned"
                if ($null -ne $gapAligned) {
                    $row.gapAlignedMs = $gapAligned
                } elseif ($null -ne $bodyPrev) {
                    $rawForUntracked = Get-LogDouble $line "rawMs"
                    if ($null -ne $rawForUntracked) {
                        $row.gapAlignedMs = [Math]::Max(0.0, [double]$rawForUntracked - [double]$bodyPrev)
                    }
                }
                $loopTail = Get-LogDouble $line "loopTail"
                if ($null -ne $loopTail) { $row.loopTailMs = $loopTail }
                $frameEndLog = Get-LogDouble $line "frameEndLog"
                if ($null -ne $frameEndLog) { $row.frameEndLogMs = $frameEndLog }
                $knownAfterBody = Get-LogDouble $line "knownAfterBody"
                if ($null -ne $knownAfterBody) { $row.knownAfterBodyMs = $knownAfterBody }
                $gapTailDelta = Get-LogDouble $line "gapTailDelta"
                if ($null -ne $gapTailDelta) { $row.gapTailDeltaMs = $gapTailDelta }
                $row.gpuReadMs = [double]$Matches.gpuRead
                $row.endFrameMs = [double]$Matches.endFrame
                $row.cmdFinalizeMs = [double]$Matches.cmdFinalize
                $row.swapMs = [double]$Matches.swap
                $row.signalGenMs = [double]$Matches.signalGen
                $row.loggingMs = [double]$Matches.logging
                $row.postRenderResidualMs = [double]$Matches.postRenderResidual
                $row.residualUntrackedMs = [double]$Matches.residualUntracked
            }
        } elseif ($line -match 'PERF_GPU frame=(?<frame>\d+) gpuFrameMs=(?<gpu>[-+0-9.]+) raymarchMs=(?<ray>[-+0-9.]+) sparseSurfaceMs=(?<surface>[-+0-9.]+) sparseUploadMs=(?<upload>[-+0-9.]+).*valid=(?<valid>\d+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame -and [int]$Matches.valid -eq 1) {
                $row = Get-Frame $frames $frame $scenario
                $row.gpuFrameMs = [double]$Matches.gpu
                $row.raymarchMs = [double]$Matches.ray
                $row.sparseSurfaceGpuMs = [double]$Matches.surface
                $row.sparseUploadGpuMs = [double]$Matches.upload
            }
        } elseif ($line -match 'PERF_WAITSPLIT frame=(?<frame>\d+) pumpWaitMs=(?<pump>[-+0-9.]+) exactGenWaitMs=(?<exact>[-+0-9.]+) surfaceWaitMs=(?<surface>[-+0-9.]+) noncritWaitMs=(?<noncrit>[-+0-9.]+) fenceWaitMs=(?<fence>[-+0-9.]+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $row.pumpWaitMs = [double]$Matches.pump
                $row.exactGenWaitMs = [double]$Matches.exact
                $row.surfaceWaitMs = [double]$Matches.surface
                $row.noncritWaitMs = [double]$Matches.noncrit
                $row.fenceWaitMs = [double]$Matches.fence
                $clipPumpHeight = Get-LogDouble $line "clipPumpHeightMs"
                if ($null -ne $clipPumpHeight) {
                    $row.sparsePumpHeightMs = $clipPumpHeight
                }
                $clipPumpVoxel = Get-LogDouble $line "clipPumpVoxelMs"
                if ($null -ne $clipPumpVoxel) {
                    $row.sparsePumpVoxelMs = $clipPumpVoxel
                }
                $row.sparseHeightQueueMs = Get-LogDouble $line "heightQueueMs"
                $row.sparseHeightDispatchMs = Get-LogDouble $line "heightDispatchMs"
                $row.sparseHeightJoinMs = Get-LogDouble $line "heightJoinMs"
                $row.sparseHeightWorkerMaxMs = Get-LogDouble $line "heightWorkerMaxMs"
                $row.sparseHeightGenerateMs = Get-LogDouble $line "heightGenerateMs"
                $row.sparseHeightCommitMs = Get-LogDouble $line "heightCommitMs"
                $row.sparseHeightPending = Get-LogInt64 $line "heightPending"
                $row.sparseHeightWorkers = Get-LogInt64 $line "heightWorkers"
                $clipGenHeight = Get-LogInt64 $line "clipGenHeight"
                if ($null -ne $clipGenHeight) {
                    $row.sparseGenHeight = $clipGenHeight
                }
                $clipGenVoxel = Get-LogInt64 $line "clipGenVoxel"
                if ($null -ne $clipGenVoxel) {
                    $row.sparseGenVoxel = $clipGenVoxel
                }
                $clipQueuedHeight = Get-LogInt64 $line "clipQueuedHeight"
                if ($null -ne $clipQueuedHeight) {
                    $row.sparseQueuedHeight = $clipQueuedHeight
                }
                $clipQueuedVoxel = Get-LogInt64 $line "clipQueuedVoxel"
                if ($null -ne $clipQueuedVoxel) {
                    $row.sparseQueuedVoxel = $clipQueuedVoxel
                }
                $clipMissingHeight = Get-LogInt64 $line "clipMissingHeight"
                if ($null -ne $clipMissingHeight) {
                    $row.sparseMissingHeight = $clipMissingHeight
                }
                $clipMissingVoxel = Get-LogInt64 $line "clipMissingVoxel"
                if ($null -ne $clipMissingVoxel) {
                    $row.sparseMissingVoxel = $clipMissingVoxel
                }
                $row.sparsePumpBudgetHit = Get-LogInt64 $line "clipBudgetHit"
                $row.sparseParallelPumpBricks = Get-LogInt64 $line "clipParallelBricks"
                $row.sparseParallelPumpWorkers = Get-LogInt64 $line "clipParallelWorkers"
                $row.sparseParallelPumpWallMs = Get-LogDouble $line "clipParallelWallMs"
                $row.sparseAsyncPending = Get-LogInt64 $line "clipAsyncPending"
                $row.sparseAsyncApplied = Get-LogInt64 $line "clipAsyncApplied"
                $row.sparseAsyncWorkerMs = Get-LogDouble $line "clipAsyncWorkerMs"
                $row.sparseVoxelInterestRings = Get-LogInt64 $line "clipInterestRings"
                $row.sparseVoxelInterestBudgeted = Get-LogInt64 $line "clipInterestBudgeted"
                $row.sparseVoxelInterestLineMs = Get-LogDouble $line "clipInterestLineMs"
                $row.sparseVoxelInterestAnchorMs = Get-LogDouble $line "clipInterestAnchorMs"
                $row.sparseVoxelInterestSortEmitMs = Get-LogDouble $line "clipInterestSortEmitMs"
                $row.sparseVoxelInterestBacklogMs = Get-LogDouble $line "clipInterestBacklogMs"
                $row.sparseVoxelInterestDiagMs = Get-LogDouble $line "clipInterestDiagMs"
                $surfaceInlineBricks = Get-LogInt64 $line "surfaceInlineBricks"
                if ($null -ne $surfaceInlineBricks) {
                    $row.surfaceInlineExtractionBricks = $surfaceInlineBricks
                }
                $surfaceInlineMs = Get-LogDouble $line "surfaceInlineMs"
                if ($null -ne $surfaceInlineMs) {
                    $row.surfaceInlineExtractionMs = $surfaceInlineMs
                }
                $surfaceParallelBricks = Get-LogInt64 $line "surfaceParallelBricks"
                if ($null -ne $surfaceParallelBricks) {
                    $row.parallelSurfaceExtractionBricks = $surfaceParallelBricks
                    $row.parallelSurfaceExtractionActive =
                        if ($surfaceParallelBricks -gt 0) { 1 } else { 0 }
                }
                $surfaceParallelWorkers = Get-LogInt64 $line "surfaceParallelWorkers"
                if ($null -ne $surfaceParallelWorkers) {
                    $row.parallelSurfaceExtractionWorkers = $surfaceParallelWorkers
                }
                $surfaceParallelWall = Get-LogDouble $line "surfaceParallelWallMs"
                if ($null -ne $surfaceParallelWall) {
                    $row.parallelSurfaceExtractionWallMs = $surfaceParallelWall
                }
                $surfaceAsyncRejected = Get-LogInt64 $line "surfaceAsyncRejected"
                if ($null -ne $surfaceAsyncRejected) {
                    $row.asyncSurfaceRejected = $surfaceAsyncRejected
                }
                $surfaceAsyncEnqueueMs = Get-LogDouble $line "surfaceAsyncEnqueueMs"
                if ($null -ne $surfaceAsyncEnqueueMs) {
                    $row.asyncSurfaceEnqueueMs = $surfaceAsyncEnqueueMs
                }
                $surfaceAsyncWorkerMs = Get-LogDouble $line "surfaceAsyncWorkerMs"
                if ($null -ne $surfaceAsyncWorkerMs) {
                    $row.asyncSurfaceWorkerMs = $surfaceAsyncWorkerMs
                }
                # Loop 86 surface work route. NOTE: surfaceReadyPub* is the
                # surface-ready publish HOLDING queue, a different queue from
                # PERF_SPARSE publishPending/publishReady (page publish queue).
                $surfaceRouteAsync = Get-LogInt64 $line "surfaceRouteAsync"
                if ($null -ne $surfaceRouteAsync) {
                    $row.surfaceRouteAsync = $surfaceRouteAsync
                }
                $surfaceRouteAsyncSat = Get-LogInt64 $line "surfaceRouteAsyncSat"
                if ($null -ne $surfaceRouteAsyncSat) {
                    $row.surfaceRouteAsyncSaturated = $surfaceRouteAsyncSat
                }
                $surfaceRoutePubSat = Get-LogInt64 $line "surfaceRoutePubSat"
                if ($null -ne $surfaceRoutePubSat) {
                    $row.surfaceRoutePublishSaturated = $surfaceRoutePubSat
                }
                $surfaceRouteAsyncQueue = Get-LogInt64 $line "surfaceRouteAsyncQueue"
                if ($null -ne $surfaceRouteAsyncQueue) {
                    $row.surfaceRouteAsyncQueueDepth = $surfaceRouteAsyncQueue
                }
                $surfaceRouteAsyncResults = Get-LogInt64 $line "surfaceRouteAsyncResults"
                if ($null -ne $surfaceRouteAsyncResults) {
                    $row.surfaceRouteAsyncResultDepth = $surfaceRouteAsyncResults
                }
                $surfaceReadyPubPending = Get-LogInt64 $line "surfaceReadyPubPending"
                if ($null -ne $surfaceReadyPubPending) {
                    $row.surfaceReadyPublishPending = $surfaceReadyPubPending
                }
                $surfaceReadyPubOldestAge = Get-LogInt64 $line "surfaceReadyPubOldestAge"
                if ($null -ne $surfaceReadyPubOldestAge) {
                    $row.surfaceReadyPublishOldestAge = $surfaceReadyPubOldestAge
                }
            }
        } elseif ($line -match 'PERF_SPARSE frame=(?<frame>\d+).*? qsurf=(?<qSpec>\d+)/(?<qVis>\d+)/(?<qColl>\d+)/(?<qEdit>\d+) psurf=(?<pSpec>\d+)/(?<pVis>\d+)/(?<pColl>\d+)/(?<pEdit>\d+).*? surfExtract=(?<surfaceExtracted>\d+)/(?<surfaceQueued>\d+)/(?<surfaceBudget>\d+).*?publishPending=(?<publishPending>\d+) publishReady=(?<publishReady>\d+) publishWait=(?<publishWaitSurface>\d+)/(?<publishWaitFence>\d+).*?publishSurfGate=(?<publishSurfaceGateDefers>\d+)/(?<publishSurfaceGateExtracts>\d+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $row.surfaceQueuedSpeculative = [int64]$Matches.qSpec
                $row.surfaceQueuedVisible = [int64]$Matches.qVis
                $row.surfaceQueuedCollision = [int64]$Matches.qColl
                $row.surfaceQueuedEdited = [int64]$Matches.qEdit
                $row.surfaceQueuedTotal = [int64]$Matches.surfaceQueued
                $row.surfaceProtectedSpeculative = [int64]$Matches.pSpec
                $row.surfaceProtectedVisible = [int64]$Matches.pVis
                $row.surfaceProtectedCollision = [int64]$Matches.pColl
                $row.surfaceProtectedEdited = [int64]$Matches.pEdit
                $row.surfaceProtectedTotal =
                    $row.surfaceProtectedSpeculative +
                    $row.surfaceProtectedVisible +
                    $row.surfaceProtectedCollision +
                    $row.surfaceProtectedEdited
                $row.surfaceExtractedLastFrame = [int64]$Matches.surfaceExtracted
                $row.surfaceExtractionBudgetLastFrame = [int64]$Matches.surfaceBudget
                $row.publishPending = [int64]$Matches.publishPending
                $row.publishReady = [int64]$Matches.publishReady
                $row.publishWaitSurface = [int64]$Matches.publishWaitSurface
                $row.publishWaitFence = [int64]$Matches.publishWaitFence
                $row.publishSurfaceGateDefers = [int64]$Matches.publishSurfaceGateDefers
                $row.publishSurfaceGateExtracts = [int64]$Matches.publishSurfaceGateExtracts
                if ($line -match 'surfAsync=queue/result/pending/enqueued/applied/discarded/requeued:(?<asyncQueue>\d+)/(?<asyncResult>\d+)/(?<asyncPending>\d+)/(?<asyncEnqueued>\d+)/(?<asyncApplied>\d+)/(?<asyncDiscarded>\d+)/(?<asyncRequeued>\d+)') {
                    $row.asyncSurfaceQueueDepth = [int64]$Matches.asyncQueue
                    $row.asyncSurfaceResultDepth = [int64]$Matches.asyncResult
                    $row.asyncSurfacePending = [int64]$Matches.asyncPending
                    $row.asyncSurfaceEnqueued = [int64]$Matches.asyncEnqueued
                    $row.asyncSurfaceApplied = [int64]$Matches.asyncApplied
                    $row.asyncSurfaceDiscarded = [int64]$Matches.asyncDiscarded
                    $row.asyncSurfaceRequeued = [int64]$Matches.asyncRequeued
                }
                Set-SurfaceWorkFromLine $row $line
            }
        } elseif ($line -match 'PERF_SPARSE_SURFACE_STAGE frame=(?<frame>\d+) extracted=(?<extracted>\d+) terrainCritical=(?<terrain>\d+) hiddenCritical=(?<hiddenCritical>\d+) general=(?<general>\d+) critical=(?<critical>\d+) nonCritical=(?<nonCritical>\d+) budget=(?<budget>\d+) elapsedMs=(?<elapsed>[-+0-9.]+) maxMs=(?<maxMs>[-+0-9.]+) queued=(?<queued>\d+) protected=(?<protected>\d+) catchup=(?<catchup>\d+) skipGeneral=(?<skipGeneral>\d+)(?: skipPrePublishBudget=(?<skipPrePublishBudget>\d+))?') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $row.surfaceStageExtracted = [int64]$Matches.extracted
                $row.surfaceStageTerrainCritical = [int64]$Matches.terrain
                $row.surfaceStageHiddenCritical = [int64]$Matches.hiddenCritical
                $row.surfaceStageGeneral = [int64]$Matches.general
                $row.surfaceStageCritical = [int64]$Matches.critical
                $row.surfaceStageNonCritical = [int64]$Matches.nonCritical
                $row.surfaceStageBudget = [int64]$Matches.budget
                $row.surfaceStageElapsedMs = [double]$Matches.elapsed
                $row.surfaceStageMaxMs = [double]$Matches.maxMs
                $row.surfaceStageQueued = [int64]$Matches.queued
                $row.surfaceStageProtected = [int64]$Matches.protected
                $row.surfaceStageCatchup = [int64]$Matches.catchup
                $row.surfaceStageSkipGeneral = [int64]$Matches.skipGeneral
                if ($Matches.skipPrePublishBudget) {
                    $row.surfaceStageSkipPrePublishBudget = [int64]$Matches.skipPrePublishBudget
                }
                Set-SurfaceWorkFromLine $row $line
            }
        } elseif ($line -match 'PERF_SPARSE_PRE_PUBLISH_SURFACE frame=(?<frame>\d+) extracted=(?<extracted>\d+) terrainCritical=(?<terrain>\d+) hiddenCritical=(?<hiddenCritical>\d+) hiddenTracked=(?<hiddenTracked>\d+) general=(?<general>\d+) budget=(?<budget>\d+) elapsedMs=(?<elapsed>[-+0-9.]+) maxMs=(?<maxMs>[-+0-9.]+) startup=(?<startup>\d+) postOpen=(?<postOpen>\d+).*?queuedPublishes=(?<queuedPublishes>\d+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $row.prePublishSurfaceExtracted = [int64]$Matches.extracted
                $row.prePublishSurfaceTerrainCritical = [int64]$Matches.terrain
                $row.prePublishSurfaceHiddenCritical = [int64]$Matches.hiddenCritical
                $row.prePublishSurfaceHiddenTracked = [int64]$Matches.hiddenTracked
                $row.prePublishSurfaceGeneral = [int64]$Matches.general
                $row.prePublishSurfaceBudget = [int64]$Matches.budget
                $row.prePublishSurfaceGeneralBudget = Get-LogInt64 $line "generalBudget"
                $row.prePublishSurfaceHiddenCriticalBudget =
                    Get-LogInt64 $line "hiddenCriticalBudget"
                $row.prePublishSurfaceHiddenTrackedBudget =
                    Get-LogInt64 $line "hiddenTrackedBudget"
                $row.prePublishSurfaceEditActive = Get-LogInt64 $line "editActive"
                $row.prePublishSurfaceEditGeneralBudget =
                    Get-LogInt64 $line "editGeneralBudget"
                $row.prePublishSurfacePostEditSpill =
                    Get-LogInt64 $line "postEditSpill"
                $row.prePublishSurfacePostEditGeneralBudget =
                    Get-LogInt64 $line "postEditGeneralBudget"
                $row.prePublishSurfacePostEditSpillFrames =
                    Get-LogInt64 $line "postEditSpillFrames"
                $row.prePublishSurfacePostEditSpillPressureMs =
                    Get-LogDouble $line "postEditSpillPressureMs"
                $row.prePublishSurfaceSplitByOwnership =
                    Get-LogInt64 $line "splitByOwnership"
                $row.prePublishSurfaceGeneralCriticalBudget =
                    Get-LogInt64 $line "generalCriticalBudget"
                $row.prePublishSurfaceGeneralNonCriticalBudget =
                    Get-LogInt64 $line "generalNonCriticalBudget"
                $row.prePublishSurfaceStackedCap =
                    Get-LogInt64 $line "stackedCap"
                $row.prePublishSurfaceStackedClipMs =
                    Get-LogDouble $line "stackedClipMs"
                $row.prePublishSurfaceStackedClipThresholdMs =
                    Get-LogDouble $line "stackedClipThresholdMs"
                $row.prePublishSurfaceStackedGeneralBudget =
                    Get-LogInt64 $line "stackedGeneralBudget"
                $generalSplitMatch = [regex]::Match(
                    $line,
                    'generalSplit=ownCrit/ownNon/edit/coll/vis/spec/timed:(?<ownCrit>\d+)/(?<ownNon>\d+)/(?<edit>\d+)/(?<coll>\d+)/(?<vis>\d+)/(?<spec>\d+)/(?<timed>\d+)')
                if ($generalSplitMatch.Success) {
                    $row.prePublishSurfaceGeneralOwnCrit =
                        [int64]$generalSplitMatch.Groups["ownCrit"].Value
                    $row.prePublishSurfaceGeneralOwnNon =
                        [int64]$generalSplitMatch.Groups["ownNon"].Value
                    $row.prePublishSurfaceGeneralEdit =
                        [int64]$generalSplitMatch.Groups["edit"].Value
                    $row.prePublishSurfaceGeneralCollision =
                        [int64]$generalSplitMatch.Groups["coll"].Value
                    $row.prePublishSurfaceGeneralVisible =
                        [int64]$generalSplitMatch.Groups["vis"].Value
                    $row.prePublishSurfaceGeneralSpeculative =
                        [int64]$generalSplitMatch.Groups["spec"].Value
                    $row.prePublishSurfaceGeneralTimed =
                        [int64]$generalSplitMatch.Groups["timed"].Value
                }
                $row.prePublishSurfaceElapsedMs = [double]$Matches.elapsed
                $row.prePublishSurfaceMaxMs = [double]$Matches.maxMs
                $row.prePublishSurfaceStartup = [int64]$Matches.startup
                $row.prePublishSurfacePostOpen = [int64]$Matches.postOpen
                $row.prePublishSurfaceQueuedPublishes = [int64]$Matches.queuedPublishes
                Set-SurfaceWorkFromLine $row $line
            }
        } elseif ($line -match 'CLIPINTEREST frame=(?<frame>\d+)\s+reuse=(?<reuse>\S+)\s+fullRebuild=(?<fullRebuild>\d+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $row.clipInterestProfileReuse = $Matches.reuse
                $row.clipInterestProfileFullRebuild = [int64]$Matches.fullRebuild
                $row.clipInterestSigMs = Get-LogDouble $line "sigMs"
                $row.clipInterestRefreshMs = Get-LogDouble $line "refreshMs"
                $row.clipInterestStatsMs = Get-LogDouble $line "statsMs"
                $row.clipInterestHeightMs = Get-LogDouble $line "heightMs"
                $row.clipInterestVoxelMs = Get-LogDouble $line "voxelMs"
                $row.clipInterestRebuildMs = Get-LogDouble $line "rebuildMs"
                $row.clipInterestTotalMs = Get-LogDouble $line "totalMs"
                if ($null -eq $row.sparseInterestMs -and $null -ne $row.clipInterestTotalMs) {
                    $row.sparseInterestMs = $row.clipInterestTotalMs
                }

                $setTransition = [regex]::Match(
                    $line,
                    'setSize=(?<before>\d+)->(?<after>\d+)')
                if ($setTransition.Success) {
                    $row.clipInterestSetSizeBefore =
                        [int64]$setTransition.Groups["before"].Value
                    $row.clipInterestSetSizeAfter =
                        [int64]$setTransition.Groups["after"].Value
                } else {
                    $setSize = Get-LogInt64 $line "setSize"
                    if ($null -ne $setSize) {
                        $row.clipInterestSetSizeBefore = $setSize
                        $row.clipInterestSetSizeAfter = $setSize
                    }
                }

                $voxelTransition = [regex]::Match(
                    $line,
                    'voxelSize=(?<before>\d+)->(?<after>\d+)')
                if ($voxelTransition.Success) {
                    $row.clipInterestVoxelSizeBefore =
                        [int64]$voxelTransition.Groups["before"].Value
                    $row.clipInterestVoxelSizeAfter =
                        [int64]$voxelTransition.Groups["after"].Value
                } else {
                    $voxelSize = Get-LogInt64 $line "voxelSize"
                    if ($null -ne $voxelSize) {
                        $row.clipInterestVoxelSizeBefore = $voxelSize
                        $row.clipInterestVoxelSizeAfter = $voxelSize
                    }
                }
            }
        } elseif ($line -match 'PERF_SPARSE_CLIPMAP frame=(?<frame>\d+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $row.sparsePrepMs = Get-LogDouble $line "prep"
                $row.sparseInterestMs = Get-LogDouble $line "interest"
                $row.sparseReuse = Get-LogInt64 $line "reuse"
                $row.sparseBudgetMs = Get-LogDouble $line "budget"
                $row.sparsePumpMs = Get-LogDouble $line "pump"
                $row.sparsePumpHeightMs = Get-LogDouble $line "pumpHeight"
                $row.sparsePumpVoxelMs = Get-LogDouble $line "pumpVoxel"
                $row.sparseHeightQueueMs = Get-LogDouble $line "heightQueueMs"
                $row.sparseHeightDispatchMs = Get-LogDouble $line "heightDispatchMs"
                $row.sparseHeightJoinMs = Get-LogDouble $line "heightJoinMs"
                $row.sparseHeightWorkerMaxMs = Get-LogDouble $line "heightWorkerMaxMs"
                $row.sparseHeightGenerateMs = Get-LogDouble $line "heightGenerateMs"
                $row.sparseHeightCommitMs = Get-LogDouble $line "heightCommitMs"
                $row.sparseHeightPending = Get-LogInt64 $line "heightPending"
                $row.sparseHeightWorkers = Get-LogInt64 $line "heightWorkers"
                $row.sparseBudgetMid = Get-LogInt64 $line "budgetMid"
                $row.sparseGenHeight = Get-LogInt64 $line "genHeight"
                $row.sparseGenVoxel = Get-LogInt64 $line "genVoxel"
                $row.sparseQueuedHeight = Get-LogInt64 $line "queuedHeight"
                $row.sparseQueuedVoxel = Get-LogInt64 $line "queuedVoxel"
                $row.sparseMissingHeight = Get-LogInt64 $line "missingHeight"
                $row.sparseMissingVoxel = Get-LogInt64 $line "missingVoxel"
            }
        } elseif ($line -match 'PERF_SPARSE_SURFACE_UPLOAD frame=(?<frame>\d+).*?needs=(?<needs>\d+) intervalReady=(?<intervalReady>\d+) completed=(?<completed>\d+) dirtyAttempt=(?<dirtyAttempt>\d+) fullCatchup=(?<fullCatchup>\d+).*?pendingDirty=(?<pendingDirty>\d+) pendingRemoved=(?<pendingRemoved>\d+) stagedFaces=(?<stagedFaces>\d+).*?copyRegions=(?<copyRegions>\d+) dirtyCopied=(?<dirtyCopied>\d+) cleanSkipped=(?<cleanSkipped>\d+) deferred=(?<deferred>\d+) patch=(?<patchBricks>\d+)/(?<patchFaces>\d+)/(?<patchRegions>\d+) stagedMB=(?<stagedMB>[-+0-9.]+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $row.surfaceUploadNeeds = [int64]$Matches.needs
                $row.surfaceUploadCompleted = [int64]$Matches.completed
                $row.surfaceUploadDirtyAttempt = [int64]$Matches.dirtyAttempt
                $row.surfaceUploadFullCatchup = [int64]$Matches.fullCatchup
                $row.surfaceUploadPendingDirty = [int64]$Matches.pendingDirty
                $row.surfaceUploadPendingRemoved = [int64]$Matches.pendingRemoved
                $row.surfaceUploadStagedFaces = [int64]$Matches.stagedFaces
                $row.surfaceUploadCopyRegions = [int64]$Matches.copyRegions
                $row.surfaceUploadDirtyCopied = [int64]$Matches.dirtyCopied
                $row.surfaceUploadCleanSkipped = [int64]$Matches.cleanSkipped
                $row.surfaceUploadDeferred = [int64]$Matches.deferred
                $row.surfaceUploadPatchBricks = [int64]$Matches.patchBricks
                $row.surfaceUploadPatchFaces = [int64]$Matches.patchFaces
                $row.surfaceUploadPatchRegions = [int64]$Matches.patchRegions
                $row.surfaceUploadStagedMB = [double]$Matches.stagedMB
            }
        } elseif ($line -match 'PERF_SPARSE_DIRTY_STAGE frame=(?<frame>\d+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $row.surfaceDirtyStageDirty = Get-LogInt64 $line "dirty"
                $row.surfaceDirtyStageRemoved = Get-LogInt64 $line "removed"
                $row.surfaceDirtyStageAllocChanged = Get-LogInt64 $line "allocChanged"
                $row.surfaceDirtyStageCopyBricks = Get-LogInt64 $line "copyBricks"
                $row.surfaceDirtyStageCopyFaces = Get-LogInt64 $line "copyFaces"
                $row.surfaceDirtyStageFullCopyBricks = Get-LogInt64 $line "fullCopyBricks"
                $row.surfaceDirtyStageFullCopyFaces = Get-LogInt64 $line "fullCopyFaces"
                $row.surfaceDirtyStagePatchBricks = Get-LogInt64 $line "patchBricks"
                $row.surfaceDirtyStagePatchFaces = Get-LogInt64 $line "patchFaces"
                $row.surfaceDirtyStagePatchRegions = Get-LogInt64 $line "patchRegions"
                $row.surfaceDirtyStageMirrorCmpBricks = Get-LogInt64 $line "mirrorCmpBricks"
                $row.surfaceDirtyStageCleanMirrorBricks = Get-LogInt64 $line "cleanMirrorBricks"
                $row.surfaceDirtyStageChangedRuns = Get-LogInt64 $line "changedRuns"
                $row.surfaceDirtyStageChangedRunFaces = Get-LogInt64 $line "changedRunFaces"
                $row.surfaceDirtyStageNewBricks = Get-LogInt64 $line "newBricks"
                $row.surfaceDirtyStageDeferred = Get-LogInt64 $line "deferred"
                $row.surfaceDirtyStageRangeCopies = Get-LogInt64 $line "rangeCopies"
                $row.surfaceDirtyStageDrawCopies = Get-LogInt64 $line "drawCopies"
                $row.surfaceDirtyStageRecordCopies = Get-LogInt64 $line "recordCopies"
                $row.surfaceDirtyStageClusterCopies = Get-LogInt64 $line "clusterCopies"
                $row.surfaceDirtyStageMetadataFull = Get-LogInt64 $line "metadataFull"
                $row.surfaceDirtyStageMetadataIncr = Get-LogInt64 $line "metadataIncr"
                $row.surfaceDirtyStageStagedMB = Get-LogDouble $line "stagedMB"
                $row.surfaceDirtyStageSetupMs = Get-LogDouble $line "setupMs"
                $row.surfaceDirtyStageRemovedMs = Get-LogDouble $line "removedMs"
                $row.surfaceDirtyStageDirtyLoopMs = Get-LogDouble $line "dirtyLoopMs"
                $row.surfaceDirtyStageFinalMs = Get-LogDouble $line "finalMs"
                $row.surfaceDirtyStageTotalMs = Get-LogDouble $line "totalMs"
            }
        } elseif ($line -match 'PERF_RENDER_COMPOSITION frame=(?<frame>\d+) screen=(?<screen>\d+) backgroundPixels=(?<background>\d+).*?surfaceOwnedPixels=(?<surface>\d+).*?overdrawRatio=(?<overdraw>[-+0-9.]+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $screenPixels = [int64]$Matches.screen
                $row.screenPixels = $screenPixels
                $row.backgroundPixels = [int64]$Matches.background
                $row.surfaceOwnedPixels = [int64]$Matches.surface
                if ($screenPixels -gt 0) {
                    $row.backgroundShare = [double]$row.backgroundPixels / [double]$screenPixels
                    $row.surfaceOwnedShare = [double]$row.surfaceOwnedPixels / [double]$screenPixels
                }
                $row.overdrawRatio = [double]$Matches.overdraw
            }
        } elseif ($line -match 'PERF_RENDER_OWNERSHIP .*?shaderFrame=(?<frame>\d+).*?miss=(?<miss>\d+).*?unsafeNearMiss=(?<unsafe>\d+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $row.miss = [int64]$Matches.miss
                $row.unsafeNearMiss = [int64]$Matches.unsafe
            }
        } elseif ($line -match 'PERF_SPARSE_READINESS frame=(?<frame>\d+).*?residentMissingSurface=(?<residentMissingSurface>\d+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $row.residentMissingSurface = [int64]$Matches.residentMissingSurface
            }
        } elseif ($line -match 'MIDMESH_SELFTIME frame=(?<frame>\d+).*?buildMs=(?<build>[-+0-9.]+).*?stageEmitMs=(?<stage>[-+0-9.]+).*?visibleMissing=(?<visibleMissing>\d+)') {
            $frame = [int]$Matches.frame
            if ($frame -ge $WarmupFrame) {
                $row = Get-Frame $frames $frame $scenario
                $row.midBuildMs = [double]$Matches.build
                $row.midStageEmitMs = [double]$Matches.stage
                $row.visibleMissing = [int64]$Matches.visibleMissing
            }
        }
    }

    $orderedFrameRows = @($frames.Values | Sort-Object { $_.frame })
    $previousBodyMs = $null
    foreach ($frameRow in $orderedFrameRows) {
        if ($null -eq $frameRow.bodyForRawMs -and $null -ne $previousBodyMs) {
            $frameRow.bodyForRawMs = $previousBodyMs
        }
        if ($null -eq $frameRow.gapAlignedMs -and $null -ne $frameRow.rawMs -and $null -ne $frameRow.bodyForRawMs) {
            $frameRow.gapAlignedMs = [Math]::Max(0.0, [double]$frameRow.rawMs - [double]$frameRow.bodyForRawMs)
        }
        if ($null -ne $frameRow.bodyMs) {
            $previousBodyMs = [double]$frameRow.bodyMs
        }
    }

    $rows = @($orderedFrameRows | ForEach-Object {
        if ($null -eq $_.rawAlignedMs -and $null -ne $_.bodyMs -and $null -ne $_.gapAlignedMs) {
            $_.rawAlignedMs = [double]$_.bodyMs + [double]$_.gapAlignedMs
        }
        if ($null -ne $_.rawAlignedMs) {
            $_.rawMs = [double]$_.rawAlignedMs
            if ($null -eq $_.rawSource -or $_.rawSource -eq "frame_end_legacy") {
                $_.rawSource = "body_plus_gap"
            }
        } elseif ($null -ne $_.frameEndRawMs) {
            $_.rawMs = [double]$_.frameEndRawMs
            if ($null -eq $_.rawSource) {
                $_.rawSource = "frame_end_legacy"
            }
        }
        if ($null -ne $_.rawMs -and [double]$_.rawMs -gt 0.0 -and $null -ne $_.gpuFrameMs) {
            $_.gpuRawRatio = [double]$_.gpuFrameMs / [double]$_.rawMs
            $_.rawMinusGpuMs = [double]$_.rawMs - [double]$_.gpuFrameMs
        }
        if ($null -ne $_.rawMs -and [double]$_.rawMs -gt 0.0 -and $null -ne $_.raymarchMs) {
            $_.rayRawRatio = [double]$_.raymarchMs / [double]$_.rawMs
        }
        if ($null -eq $_.gapAlignedMs -and $null -ne $_.rawMs -and $null -ne $_.bodyForRawMs) {
            $_.gapAlignedMs = [Math]::Max(0.0, [double]$_.rawMs - [double]$_.bodyForRawMs)
        }
        if ($null -eq $_.knownAfterBodyMs -and $null -ne $_.loopTailMs -and $null -ne $_.frameEndLogMs) {
            $_.knownAfterBodyMs = [double]$_.loopTailMs + [double]$_.frameEndLogMs
        }
        if ($null -eq $_.gapTailDeltaMs -and $null -ne $_.gapAlignedMs -and $null -ne $_.knownAfterBodyMs) {
            $_.gapTailDeltaMs = [double]$_.gapAlignedMs - [double]$_.knownAfterBodyMs
        }
        $_.dominantCause = DominantCause $_
        [pscustomobject]$_
    })
    foreach ($row in $rows) {
        $allRows.Add($row)
    }

    $rawRows = @($rows | Where-Object { $null -ne $_.rawMs })
    $rawAlignedRows = @($rows | Where-Object { $null -ne $_.rawAlignedMs })
    $frameEndRawRows = @($rows | Where-Object { $null -ne $_.frameEndRawMs })
    $bodyRows = @($rows | Where-Object { $null -ne $_.bodyMs })
    $summary.Add([pscustomobject]@{
        scenario = $scenario
        samples = $rawRows.Count
        rawMetric = "current_frame_aligned"
        rawP50 = Percentile $rawRows.rawMs 0.50
        rawP95 = Percentile $rawRows.rawMs 0.95
        rawP99 = Percentile $rawRows.rawMs 0.99
        rawMax = MaxOrNull $rawRows.rawMs
        rawAlignedP50 = Percentile $rawAlignedRows.rawAlignedMs 0.50
        rawAlignedP95 = Percentile $rawAlignedRows.rawAlignedMs 0.95
        rawAlignedP99 = Percentile $rawAlignedRows.rawAlignedMs 0.99
        rawAlignedMax = MaxOrNull $rawAlignedRows.rawAlignedMs
        rawAlignedSamples = $rawAlignedRows.Count
        frameEndRawP50 = Percentile $frameEndRawRows.frameEndRawMs 0.50
        frameEndRawP95 = Percentile $frameEndRawRows.frameEndRawMs 0.95
        frameEndRawP99 = Percentile $frameEndRawRows.frameEndRawMs 0.99
        frameEndRawMax = MaxOrNull $frameEndRawRows.frameEndRawMs
        frameEndRawSamples = $frameEndRawRows.Count
        bodyP50 = Percentile $bodyRows.bodyMs 0.50
        bodyP95 = Percentile $bodyRows.bodyMs 0.95
        bodyP99 = Percentile $bodyRows.bodyMs 0.99
        bodyMax = MaxOrNull $bodyRows.bodyMs
        framesOver16_7 = @($rawRows | Where-Object { $_.rawMs -gt 16.7 }).Count
        framesOver33 = @($rawRows | Where-Object { $_.rawMs -gt 33.0 }).Count
        framesOver16_7Causes = CauseBreakdown $rawRows 16.7
        framesOver33Causes = CauseBreakdown $rawRows 33.0
        gpuP50 = Percentile $rows.gpuFrameMs 0.50
        gpuP95 = Percentile $rows.gpuFrameMs 0.95
        gpuP99 = Percentile $rows.gpuFrameMs 0.99
        gpuMax = MaxOrNull $rows.gpuFrameMs
        gpuRawRatioP50 = Percentile $rows.gpuRawRatio 0.50
        gpuRawRatioP95 = Percentile $rows.gpuRawRatio 0.95
        rawMinusGpuP50 = Percentile $rows.rawMinusGpuMs 0.50
        rawMinusGpuP95 = Percentile $rows.rawMinusGpuMs 0.95
        rayP50 = Percentile $rows.raymarchMs 0.50
        rayP95 = Percentile $rows.raymarchMs 0.95
        rayP99 = Percentile $rows.raymarchMs 0.99
        rayMax = MaxOrNull $rows.raymarchMs
        rayRawRatioP50 = Percentile $rows.rayRawRatio 0.50
        rayRawRatioP95 = Percentile $rows.rayRawRatio 0.95
        postWaitP50 = Percentile $rawRows.postWaitMs 0.50
        postWaitP95 = Percentile $rawRows.postWaitMs 0.95
        postWaitMax = MaxOrNull $rawRows.postWaitMs
        sparsePostSumP95 = Percentile $rawRows.sparsePostSumMs 0.95
        sparsePostSumMax = MaxOrNull $rawRows.sparsePostSumMs
        postWaitResidualP95 = Percentile $rawRows.postWaitResidualMs 0.95
        postWaitResidualMax = MaxOrNull $rawRows.postWaitResidualMs
        framePrepSamples = @($rows | Where-Object { $null -ne $_.framePrepMs }).Count
        framePrepP95 = Percentile $rows.framePrepMs 0.95
        framePrepMax = MaxOrNull $rows.framePrepMs
        prepSparseSamples = @($rows | Where-Object { $null -ne $_.prepSparseMs }).Count
        prepSparseP95 = Percentile $rows.prepSparseMs 0.95
        prepSparseMax = MaxOrNull $rows.prepSparseMs
        sparseReqPrepSamples = @($rows | Where-Object { $null -ne $_.sparseReqPrepMs }).Count
        sparseReqPrepP95 = Percentile $rows.sparseReqPrepMs 0.95
        sparseReqPrepMax = MaxOrNull $rows.sparseReqPrepMs
        sparseGenPrepSamples = @($rows | Where-Object { $null -ne $_.sparseGenPrepMs }).Count
        sparseGenPrepP95 = Percentile $rows.sparseGenPrepMs 0.95
        sparseGenPrepMax = MaxOrNull $rows.sparseGenPrepMs
        sparseClipPrepSamples = @($rows | Where-Object { $null -ne $_.sparseClipPrepMs }).Count
        sparseClipPrepP95 = Percentile $rows.sparseClipPrepMs 0.95
        sparseClipPrepMax = MaxOrNull $rows.sparseClipPrepMs
        sparseTrimPrepP95 = Percentile $rows.sparseTrimPrepMs 0.95
        sparseTrimPrepMax = MaxOrNull $rows.sparseTrimPrepMs
        sparseReqHierarchyP95 = Percentile $rows.sparseReqHierarchyMs 0.95
        sparseReqHiddenExactP95 = Percentile $rows.sparseReqHiddenExactMs 0.95
        sparseReqHiddenExactMax = MaxOrNull $rows.sparseReqHiddenExactMs
        sparseReqHiddenExactWaterProbeP95 = Percentile $rows.sparseReqHiddenExactWaterProbeMs 0.95
        sparseReqHiddenExactWaterProbeMax = MaxOrNull $rows.sparseReqHiddenExactWaterProbeMs
        sparseReqHiddenExactGeneralProbeP95 = Percentile $rows.sparseReqHiddenExactGeneralProbeMs 0.95
        sparseReqHiddenExactGeneralProbeMax = MaxOrNull $rows.sparseReqHiddenExactGeneralProbeMs
        sparseReqHiddenExactCandidatesP95 = Percentile $rows.sparseReqHiddenExactCandidatesMs 0.95
        sparseReqHiddenExactCandidatesMax = MaxOrNull $rows.sparseReqHiddenExactCandidatesMs
        sparseReqHiddenExactAuditP95 = Percentile $rows.sparseReqHiddenExactAuditMs 0.95
        sparseReqHiddenExactAuditMax = MaxOrNull $rows.sparseReqHiddenExactAuditMs
        genPrepDetailSamples = @($rows | Where-Object { $null -ne $_.genPrepAsyncExactApplyMs }).Count
        genPrepAsyncExactApplyP95 = Percentile $rows.genPrepAsyncExactApplyMs 0.95
        genPrepParallelWallP95 = Percentile $rows.genPrepParallelWallMs 0.95
        genPrepLoopsP95 = Percentile $rows.genPrepLoopsMs 0.95
        genPrepLoopsMax = MaxOrNull $rows.genPrepLoopsMs
        genPrepPumpP95 = Percentile $rows.genPrepPumpMs 0.95
        genPrepPumpMax = MaxOrNull $rows.genPrepPumpMs
        genPrepFlushP95 = Percentile $rows.genPrepFlushMs 0.95
        genPrepFlushMax = MaxOrNull $rows.genPrepFlushMs
        genPrepHiddenExactGeneratedMax = MaxOrNull $rows.genPrepHiddenExactGenerated
        genPrepHiddenExactTrackedMax = MaxOrNull $rows.genPrepHiddenExactTracked
        fenceWaitP50 = Percentile $rows.fenceWaitMs 0.50
        fenceWaitP95 = Percentile $rows.fenceWaitMs 0.95
        fenceWaitMax = MaxOrNull $rows.fenceWaitMs
        pumpWaitP95 = Percentile $rows.pumpWaitMs 0.95
        exactGenWaitP95 = Percentile $rows.exactGenWaitMs 0.95
        surfaceWaitP95 = Percentile $rows.surfaceWaitMs 0.95
        noncritWaitP95 = Percentile $rows.noncritWaitMs 0.95
        gapPrevP95 = Percentile $rawRows.gapPrevMs 0.95
        gapPrevMax = MaxOrNull $rawRows.gapPrevMs
        gapAlignedP95 = Percentile $rawRows.gapAlignedMs 0.95
        gapAlignedMax = MaxOrNull $rawRows.gapAlignedMs
        loopTailP95 = Percentile $rawRows.loopTailMs 0.95
        loopTailMax = MaxOrNull $rawRows.loopTailMs
        frameEndLogP95 = Percentile $rawRows.frameEndLogMs 0.95
        frameEndLogMax = MaxOrNull $rawRows.frameEndLogMs
        knownAfterBodyP95 = Percentile $rawRows.knownAfterBodyMs 0.95
        knownAfterBodyMax = MaxOrNull $rawRows.knownAfterBodyMs
        gapTailDeltaP95 = Percentile $rawRows.gapTailDeltaMs 0.95
        gapTailDeltaMax = MaxOrNull $rawRows.gapTailDeltaMs
        loggingP95 = Percentile $rows.loggingMs 0.95
        loggingMax = MaxOrNull $rows.loggingMs
        gpuReadP95 = Percentile $rows.gpuReadMs 0.95
        gpuReadMax = MaxOrNull $rows.gpuReadMs
        endFrameP95 = Percentile $rows.endFrameMs 0.95
        endFrameMax = MaxOrNull $rows.endFrameMs
        cmdFinalizeP95 = Percentile $rows.cmdFinalizeMs 0.95
        cmdFinalizeMax = MaxOrNull $rows.cmdFinalizeMs
        swapP95 = Percentile $rows.swapMs 0.95
        swapMax = MaxOrNull $rows.swapMs
        signalGenP95 = Percentile $rows.signalGenMs 0.95
        signalGenMax = MaxOrNull $rows.signalGenMs
        residualUntrackedP95 = Percentile $rows.residualUntrackedMs 0.95
        residualUntrackedMax = MaxOrNull $rows.residualUntrackedMs
        sparsePostBeginP95 = Percentile $rows.beginFrameMs 0.95
        sparsePostBeginMax = MaxOrNull $rows.beginFrameMs
        sparsePostMidSnapP95 = Percentile $rows.midSnapMs 0.95
        sparsePostMidSnapMax = MaxOrNull $rows.midSnapMs
        sparsePostUploadPlanP95 = Percentile $rows.uploadPlanMs 0.95
        sparsePostUploadPlanMax = MaxOrNull $rows.uploadPlanMs
        surfExtractP50 = Percentile $rawRows.surfExtractMs 0.50
        surfExtractP95 = Percentile $rawRows.surfExtractMs 0.95
        surfExtractMax = MaxOrNull $rawRows.surfExtractMs
        surfPlanP95 = Percentile $rows.surfPlanMs 0.95
        surfPlanMax = MaxOrNull $rows.surfPlanMs
        surfSnapP95 = Percentile $rows.surfSnapMs 0.95
        surfSnapMax = MaxOrNull $rows.surfSnapMs
        surfStageP95 = Percentile $rows.surfStageMs 0.95
        surfStageMax = MaxOrNull $rows.surfStageMs
        surfEmitP95 = Percentile $rows.surfEmitMs 0.95
        surfEmitMax = MaxOrNull $rows.surfEmitMs
        surfaceExtractedLastFrameP95 = Percentile $rows.surfaceExtractedLastFrame 0.95
        surfaceExtractedLastFrameMax = MaxOrNull $rows.surfaceExtractedLastFrame
        surfaceQueuedTotalMax = MaxOrNull $rows.surfaceQueuedTotal
        surfaceQueuedVisibleMax = MaxOrNull $rows.surfaceQueuedVisible
        surfaceQueuedSpeculativeMax = MaxOrNull $rows.surfaceQueuedSpeculative
        surfaceProtectedTotalMax = MaxOrNull $rows.surfaceProtectedTotal
        surfaceExtractionBudgetLastFrameMax = MaxOrNull $rows.surfaceExtractionBudgetLastFrame
        asyncSurfaceQueueDepthMax = MaxOrNull $rows.asyncSurfaceQueueDepth
        asyncSurfaceResultDepthMax = MaxOrNull $rows.asyncSurfaceResultDepth
        asyncSurfacePendingMax = MaxOrNull $rows.asyncSurfacePending
        asyncSurfaceEnqueuedSum = SumOrNull $rows.asyncSurfaceEnqueued
        asyncSurfaceAppliedSum = SumOrNull $rows.asyncSurfaceApplied
        asyncSurfaceDiscardedSum = SumOrNull $rows.asyncSurfaceDiscarded
        asyncSurfaceRequeuedSum = SumOrNull $rows.asyncSurfaceRequeued
        asyncSurfaceRejectedSum = SumOrNull $rows.asyncSurfaceRejected
        asyncSurfaceWorkerP95 = Percentile $rows.asyncSurfaceWorkerMs 0.95
        asyncSurfaceWorkerMax = MaxOrNull $rows.asyncSurfaceWorkerMs
        asyncSurfaceEnqueueP95 = Percentile $rows.asyncSurfaceEnqueueMs 0.95
        asyncSurfaceEnqueueMax = MaxOrNull $rows.asyncSurfaceEnqueueMs
        surfaceInlineExtractionBricksP95 = Percentile $rows.surfaceInlineExtractionBricks 0.95
        surfaceInlineExtractionBricksMax = MaxOrNull $rows.surfaceInlineExtractionBricks
        surfaceInlineExtractionP95 = Percentile $rows.surfaceInlineExtractionMs 0.95
        surfaceInlineExtractionMax = MaxOrNull $rows.surfaceInlineExtractionMs
        parallelSurfaceExtractionBricksP95 = Percentile $rows.parallelSurfaceExtractionBricks 0.95
        parallelSurfaceExtractionBricksMax = MaxOrNull $rows.parallelSurfaceExtractionBricks
        parallelSurfaceExtractionWorkersMax = MaxOrNull $rows.parallelSurfaceExtractionWorkers
        parallelSurfaceExtractionWallP95 = Percentile $rows.parallelSurfaceExtractionWallMs 0.95
        parallelSurfaceExtractionWallMax = MaxOrNull $rows.parallelSurfaceExtractionWallMs
        surfaceRouteAsyncFrames = @($rows | Where-Object { $_.surfaceRouteAsync -eq 1 }).Count
        surfaceRouteAsyncSaturatedFrames =
            @($rows | Where-Object { $_.surfaceRouteAsyncSaturated -eq 1 }).Count
        surfaceRoutePublishSaturatedFrames =
            @($rows | Where-Object { $_.surfaceRoutePublishSaturated -eq 1 }).Count
        surfaceRouteAsyncQueueDepthMax = MaxOrNull $rows.surfaceRouteAsyncQueueDepth
        surfaceRouteAsyncResultDepthMax = MaxOrNull $rows.surfaceRouteAsyncResultDepth
        surfaceReadyPublishPendingMax = MaxOrNull $rows.surfaceReadyPublishPending
        surfaceReadyPublishOldestAgeMax = MaxOrNull $rows.surfaceReadyPublishOldestAge
        publishPendingMax = MaxOrNull $rows.publishPending
        publishReadyMax = MaxOrNull $rows.publishReady
        publishWaitSurfaceMax = MaxOrNull $rows.publishWaitSurface
        publishWaitFenceMax = MaxOrNull $rows.publishWaitFence
        publishSurfaceGateDefersMax = MaxOrNull $rows.publishSurfaceGateDefers
        publishSurfaceGateExtractsMax = MaxOrNull $rows.publishSurfaceGateExtracts
        surfaceStageElapsedP95 = Percentile $rows.surfaceStageElapsedMs 0.95
        surfaceStageElapsedMax = MaxOrNull $rows.surfaceStageElapsedMs
        surfaceStageExtractedP95 = Percentile $rows.surfaceStageExtracted 0.95
        surfaceStageExtractedMax = MaxOrNull $rows.surfaceStageExtracted
        surfaceStageTerrainCriticalMax = MaxOrNull $rows.surfaceStageTerrainCritical
        surfaceStageHiddenCriticalMax = MaxOrNull $rows.surfaceStageHiddenCritical
        surfaceStageGeneralMax = MaxOrNull $rows.surfaceStageGeneral
        surfaceStageCriticalMax = MaxOrNull $rows.surfaceStageCritical
        surfaceStageNonCriticalMax = MaxOrNull $rows.surfaceStageNonCritical
        surfaceStageQueuedMax = MaxOrNull $rows.surfaceStageQueued
        surfaceStageProtectedMax = MaxOrNull $rows.surfaceStageProtected
        surfaceStageCatchupFrames = @($rows | Where-Object { $_.surfaceStageCatchup -eq 1 }).Count
        surfaceStageSkipGeneralFrames = @($rows | Where-Object { $_.surfaceStageSkipGeneral -eq 1 }).Count
        surfaceStageSkipPrePublishBudgetFrames = @($rows | Where-Object { $_.surfaceStageSkipPrePublishBudget -eq 1 }).Count
        prePublishSurfaceElapsedP95 = Percentile $rows.prePublishSurfaceElapsedMs 0.95
        prePublishSurfaceElapsedMax = MaxOrNull $rows.prePublishSurfaceElapsedMs
        prePublishSurfaceExtractedP95 = Percentile $rows.prePublishSurfaceExtracted 0.95
        prePublishSurfaceExtractedMax = MaxOrNull $rows.prePublishSurfaceExtracted
        prePublishSurfaceHiddenCriticalMax = MaxOrNull $rows.prePublishSurfaceHiddenCritical
        prePublishSurfaceHiddenCriticalBudgetMax = MaxOrNull $rows.prePublishSurfaceHiddenCriticalBudget
        prePublishSurfaceHiddenTrackedMax = MaxOrNull $rows.prePublishSurfaceHiddenTracked
        prePublishSurfaceHiddenTrackedBudgetMax = MaxOrNull $rows.prePublishSurfaceHiddenTrackedBudget
        prePublishSurfaceGeneralMax = MaxOrNull $rows.prePublishSurfaceGeneral
        prePublishSurfaceGeneralBudgetMax = MaxOrNull $rows.prePublishSurfaceGeneralBudget
        prePublishSurfaceEditActiveFrames = @($rows | Where-Object { $_.prePublishSurfaceEditActive -eq 1 }).Count
        prePublishSurfaceEditGeneralBudgetMax = MaxOrNull $rows.prePublishSurfaceEditGeneralBudget
        prePublishSurfacePostEditSpillFrames =
            @($rows | Where-Object { $_.prePublishSurfacePostEditSpill -eq 1 }).Count
        prePublishSurfacePostEditGeneralBudgetMax =
            MaxOrNull $rows.prePublishSurfacePostEditGeneralBudget
        prePublishSurfacePostEditSpillFramesMax =
            MaxOrNull $rows.prePublishSurfacePostEditSpillFrames
        prePublishSurfacePostEditSpillPressureMsMax =
            MaxOrNull $rows.prePublishSurfacePostEditSpillPressureMs
        prePublishSurfaceSplitByOwnershipFrames =
            @($rows | Where-Object { $_.prePublishSurfaceSplitByOwnership -eq 1 }).Count
        prePublishSurfaceGeneralCriticalBudgetMax =
            MaxOrNull $rows.prePublishSurfaceGeneralCriticalBudget
        prePublishSurfaceGeneralNonCriticalBudgetMax =
            MaxOrNull $rows.prePublishSurfaceGeneralNonCriticalBudget
        prePublishSurfaceStackedCapFrames =
            @($rows | Where-Object { $_.prePublishSurfaceStackedCap -eq 1 }).Count
        prePublishSurfaceStackedClipMsP95 =
            Percentile $rows.prePublishSurfaceStackedClipMs 0.95
        prePublishSurfaceStackedClipMsMax =
            MaxOrNull $rows.prePublishSurfaceStackedClipMs
        prePublishSurfaceStackedGeneralBudgetMax =
            MaxOrNull $rows.prePublishSurfaceStackedGeneralBudget
        prePublishSurfaceGeneralOwnCritMax = MaxOrNull $rows.prePublishSurfaceGeneralOwnCrit
        prePublishSurfaceGeneralOwnNonMax = MaxOrNull $rows.prePublishSurfaceGeneralOwnNon
        prePublishSurfaceGeneralEditMax = MaxOrNull $rows.prePublishSurfaceGeneralEdit
        prePublishSurfaceGeneralCollisionMax = MaxOrNull $rows.prePublishSurfaceGeneralCollision
        prePublishSurfaceGeneralVisibleMax = MaxOrNull $rows.prePublishSurfaceGeneralVisible
        prePublishSurfaceGeneralSpeculativeMax = MaxOrNull $rows.prePublishSurfaceGeneralSpeculative
        prePublishSurfaceGeneralTimedMax = MaxOrNull $rows.prePublishSurfaceGeneralTimed
        prePublishSurfaceQueuedPublishesMax = MaxOrNull $rows.prePublishSurfaceQueuedPublishes
        prePublishSurfaceStartupFrames = @($rows | Where-Object { $_.prePublishSurfaceStartup -eq 1 }).Count
        prePublishSurfacePostOpenFrames = @($rows | Where-Object { $_.prePublishSurfacePostOpen -eq 1 }).Count
        surfaceUploadFrames = @($rows | Where-Object { $_.surfaceUploadNeeds -eq 1 }).Count
        surfaceUploadCompletedFrames = @($rows | Where-Object { $_.surfaceUploadCompleted -eq 1 }).Count
        surfaceUploadDirtyAttemptFrames = @($rows | Where-Object { $_.surfaceUploadDirtyAttempt -eq 1 }).Count
        surfaceUploadFullCatchupFrames = @($rows | Where-Object { $_.surfaceUploadFullCatchup -eq 1 }).Count
        surfaceUploadCopyRegionsP95 = Percentile $rows.surfaceUploadCopyRegions 0.95
        surfaceUploadCopyRegionsMax = MaxOrNull $rows.surfaceUploadCopyRegions
        surfaceUploadDirtyCopiedP95 = Percentile $rows.surfaceUploadDirtyCopied 0.95
        surfaceUploadDirtyCopiedMax = MaxOrNull $rows.surfaceUploadDirtyCopied
        surfaceUploadCleanSkippedP95 = Percentile $rows.surfaceUploadCleanSkipped 0.95
        surfaceUploadDeferredMax = MaxOrNull $rows.surfaceUploadDeferred
        surfaceUploadPatchFacesMax = MaxOrNull $rows.surfaceUploadPatchFaces
        surfaceUploadStagedMbP95 = Percentile $rows.surfaceUploadStagedMB 0.95
        surfaceUploadStagedMbMax = MaxOrNull $rows.surfaceUploadStagedMB
        surfaceDirtyStageFrames = @($rows | Where-Object { $null -ne $_.surfaceDirtyStageTotalMs }).Count
        surfaceDirtyStageTotalP95 = Percentile $rows.surfaceDirtyStageTotalMs 0.95
        surfaceDirtyStageTotalMax = MaxOrNull $rows.surfaceDirtyStageTotalMs
        surfaceDirtyStageSetupP95 = Percentile $rows.surfaceDirtyStageSetupMs 0.95
        surfaceDirtyStageSetupMax = MaxOrNull $rows.surfaceDirtyStageSetupMs
        surfaceDirtyStageRemovedP95 = Percentile $rows.surfaceDirtyStageRemovedMs 0.95
        surfaceDirtyStageDirtyLoopP95 = Percentile $rows.surfaceDirtyStageDirtyLoopMs 0.95
        surfaceDirtyStageDirtyLoopMax = MaxOrNull $rows.surfaceDirtyStageDirtyLoopMs
        surfaceDirtyStageCopyBricksP95 = Percentile $rows.surfaceDirtyStageCopyBricks 0.95
        surfaceDirtyStageCopyBricksMax = MaxOrNull $rows.surfaceDirtyStageCopyBricks
        surfaceDirtyStageCopyFacesP95 = Percentile $rows.surfaceDirtyStageCopyFaces 0.95
        surfaceDirtyStageCopyFacesMax = MaxOrNull $rows.surfaceDirtyStageCopyFaces
        surfaceDirtyStageFullCopyFacesMax = MaxOrNull $rows.surfaceDirtyStageFullCopyFaces
        surfaceDirtyStagePatchFacesMax = MaxOrNull $rows.surfaceDirtyStagePatchFaces
        surfaceDirtyStageAllocChangedMax = MaxOrNull $rows.surfaceDirtyStageAllocChanged
        surfaceDirtyStageMetadataFullFrames = @($rows | Where-Object { $_.surfaceDirtyStageMetadataFull -eq 1 }).Count
        surfaceDirtyStageMetadataIncrFrames = @($rows | Where-Object { $_.surfaceDirtyStageMetadataIncr -eq 1 }).Count
        sparseInterestP50 = Percentile $rows.sparseInterestMs 0.50
        sparseInterestP95 = Percentile $rows.sparseInterestMs 0.95
        sparseInterestMax = MaxOrNull $rows.sparseInterestMs
        clipInterestProfileSamples = @($rows | Where-Object { $null -ne $_.clipInterestTotalMs }).Count
        clipInterestFullRebuildFrames =
            @($rows | Where-Object { $_.clipInterestProfileFullRebuild -eq 1 }).Count
        clipInterestReuseFrames =
            @($rows | Where-Object { $_.clipInterestProfileFullRebuild -eq 0 }).Count
        clipInterestSetSizeMax = MaxOrNull $rows.clipInterestSetSizeAfter
        clipInterestVoxelSizeMax = MaxOrNull $rows.clipInterestVoxelSizeAfter
        clipInterestSigP95 = Percentile $rows.clipInterestSigMs 0.95
        clipInterestSigMax = MaxOrNull $rows.clipInterestSigMs
        clipInterestRefreshP95 = Percentile $rows.clipInterestRefreshMs 0.95
        clipInterestRefreshMax = MaxOrNull $rows.clipInterestRefreshMs
        clipInterestStatsP95 = Percentile $rows.clipInterestStatsMs 0.95
        clipInterestStatsMax = MaxOrNull $rows.clipInterestStatsMs
        clipInterestHeightP95 = Percentile $rows.clipInterestHeightMs 0.95
        clipInterestHeightMax = MaxOrNull $rows.clipInterestHeightMs
        clipInterestVoxelP95 = Percentile $rows.clipInterestVoxelMs 0.95
        clipInterestVoxelMax = MaxOrNull $rows.clipInterestVoxelMs
        clipInterestRebuildP95 = Percentile $rows.clipInterestRebuildMs 0.95
        clipInterestRebuildMax = MaxOrNull $rows.clipInterestRebuildMs
        clipInterestTotalP95 = Percentile $rows.clipInterestTotalMs 0.95
        clipInterestTotalMax = MaxOrNull $rows.clipInterestTotalMs
        sparsePumpHeightP50 = Percentile $rows.sparsePumpHeightMs 0.50
        sparsePumpHeightP95 = Percentile $rows.sparsePumpHeightMs 0.95
        sparsePumpHeightMax = MaxOrNull $rows.sparsePumpHeightMs
        sparseHeightQueueP95 = Percentile $rows.sparseHeightQueueMs 0.95
        sparseHeightQueueMax = MaxOrNull $rows.sparseHeightQueueMs
        sparseHeightDispatchP95 = Percentile $rows.sparseHeightDispatchMs 0.95
        sparseHeightDispatchMax = MaxOrNull $rows.sparseHeightDispatchMs
        sparseHeightJoinP95 = Percentile $rows.sparseHeightJoinMs 0.95
        sparseHeightJoinMax = MaxOrNull $rows.sparseHeightJoinMs
        sparseHeightWorkerMaxP95 = Percentile $rows.sparseHeightWorkerMaxMs 0.95
        sparseHeightWorkerMaxMax = MaxOrNull $rows.sparseHeightWorkerMaxMs
        sparseHeightGenerateP95 = Percentile $rows.sparseHeightGenerateMs 0.95
        sparseHeightGenerateMax = MaxOrNull $rows.sparseHeightGenerateMs
        sparseHeightCommitP95 = Percentile $rows.sparseHeightCommitMs 0.95
        sparseHeightCommitMax = MaxOrNull $rows.sparseHeightCommitMs
        sparseHeightPendingMax = MaxOrNull $rows.sparseHeightPending
        sparseHeightWorkersMax = MaxOrNull $rows.sparseHeightWorkers
        sparsePumpVoxelP50 = Percentile $rows.sparsePumpVoxelMs 0.50
        sparsePumpVoxelP95 = Percentile $rows.sparsePumpVoxelMs 0.95
        sparsePumpVoxelMax = MaxOrNull $rows.sparsePumpVoxelMs
        sparsePumpBudgetHitFrames = @($rows | Where-Object { $_.sparsePumpBudgetHit -eq 1 }).Count
        sparseParallelPumpBricksMax = MaxOrNull $rows.sparseParallelPumpBricks
        sparseParallelPumpWorkersMax = MaxOrNull $rows.sparseParallelPumpWorkers
        sparseParallelPumpWallP95 = Percentile $rows.sparseParallelPumpWallMs 0.95
        sparseParallelPumpWallMax = MaxOrNull $rows.sparseParallelPumpWallMs
        sparseAsyncPendingMax = MaxOrNull $rows.sparseAsyncPending
        sparseAsyncAppliedMax = MaxOrNull $rows.sparseAsyncApplied
        sparseAsyncWorkerP95 = Percentile $rows.sparseAsyncWorkerMs 0.95
        sparseAsyncWorkerMax = MaxOrNull $rows.sparseAsyncWorkerMs
        sparseVoxelInterestRingsP95 = Percentile $rows.sparseVoxelInterestRings 0.95
        sparseVoxelInterestRingsMax = MaxOrNull $rows.sparseVoxelInterestRings
        sparseVoxelInterestBudgetedFrames =
            @($rows | Where-Object { $_.sparseVoxelInterestBudgeted -eq 1 }).Count
        sparseVoxelInterestLineP95 = Percentile $rows.sparseVoxelInterestLineMs 0.95
        sparseVoxelInterestLineMax = MaxOrNull $rows.sparseVoxelInterestLineMs
        sparseVoxelInterestAnchorP95 = Percentile $rows.sparseVoxelInterestAnchorMs 0.95
        sparseVoxelInterestAnchorMax = MaxOrNull $rows.sparseVoxelInterestAnchorMs
        sparseVoxelInterestSortEmitP95 = Percentile $rows.sparseVoxelInterestSortEmitMs 0.95
        sparseVoxelInterestSortEmitMax = MaxOrNull $rows.sparseVoxelInterestSortEmitMs
        sparseVoxelInterestBacklogP95 = Percentile $rows.sparseVoxelInterestBacklogMs 0.95
        sparseVoxelInterestBacklogMax = MaxOrNull $rows.sparseVoxelInterestBacklogMs
        sparseVoxelInterestDiagP95 = Percentile $rows.sparseVoxelInterestDiagMs 0.95
        sparseVoxelInterestDiagMax = MaxOrNull $rows.sparseVoxelInterestDiagMs
        sparseQueuedHeightMax = MaxOrNull $rows.sparseQueuedHeight
        sparseQueuedVoxelMax = MaxOrNull $rows.sparseQueuedVoxel
        sparseMissingHeightMax = MaxOrNull $rows.sparseMissingHeight
        sparseMissingVoxelMax = MaxOrNull $rows.sparseMissingVoxel
        midUploadP95 = Percentile $rawRows.midUploadMs 0.95
        midUploadMax = MaxOrNull $rawRows.midUploadMs
        screenPixels = MaxOrNull $rows.screenPixels
        backgroundPixelsP50 = Percentile $rawRows.backgroundPixels 0.50
        backgroundPixelsP95 = Percentile $rawRows.backgroundPixels 0.95
        maxBackgroundPixels = MaxOrNull $rows.backgroundPixels
        backgroundShareP50 = Percentile $rawRows.backgroundShare 0.50
        backgroundShareP95 = Percentile $rawRows.backgroundShare 0.95
        surfaceOwnedPixelsP50 = Percentile $rawRows.surfaceOwnedPixels 0.50
        surfaceOwnedPixelsP95 = Percentile $rawRows.surfaceOwnedPixels 0.95
        surfaceOwnedShareP50 = Percentile $rawRows.surfaceOwnedShare 0.50
        surfaceOwnedShareP95 = Percentile $rawRows.surfaceOwnedShare 0.95
        overdrawP95 = Percentile $rawRows.overdrawRatio 0.95
        maxMiss = MaxOrNull $rows.miss
        maxUnsafeNearMiss = MaxOrNull $rows.unsafeNearMiss
        maxResidentMissingSurface = MaxOrNull $rows.residentMissingSurface
        maxVisibleMissing = MaxOrNull $rows.visibleMissing
        topFrames = (($rawRows | Sort-Object rawMs -Descending | Select-Object -First 5 | ForEach-Object {
            "f$($_.frame):$([Math]::Round($_.rawMs, 2)):$($_.dominantCause)"
        }) -join ";")
    })
}

$frameMapPath = Join-Path $inputPath "frame_map.csv"
$summaryPath = Join-Path $inputPath "summary.csv"
$allRows | Export-Csv -LiteralPath $frameMapPath -NoTypeInformation
$summary | Export-Csv -LiteralPath $summaryPath -NoTypeInformation

"SUMMARY=$summaryPath"
"FRAME_MAP=$frameMapPath"
$summary | Format-Table -AutoSize
