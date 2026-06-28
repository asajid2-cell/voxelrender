param(
    [Parameter(Mandatory = $true)]
    [string]$LogPath,
    [int]$MinFrame = 200,
    [string]$Label = "farfield",
    [int]$MaxVisibleMissingNonzero = 0,
    [int]$MaxResidentMissingNonzero = 0,
    [switch]$RequireDrained,
    [int]$MaxPublishPending = 0,
    [int]$MaxUploading = 0,
    [int]$MaxPublishLag = 0,
    [int]$RequireStableFrames = 0,
    [int]$MinAcceptedSamples = 1
)

$ErrorActionPreference = "Stop"

if (!(Test-Path -LiteralPath $LogPath)) {
    throw "Log not found: $LogPath"
}

function Percentile([double[]]$Values, [double]$P) {
    if ($null -eq $Values -or $Values.Count -eq 0) {
        return $null
    }
    $sorted = @($Values | Sort-Object)
    $index = [Math]::Min($sorted.Count - 1, [int]($sorted.Count * $P))
    return $sorted[$index]
}

function PercentOf([Nullable[double]]$Part, [Nullable[double]]$Total) {
    if ($null -eq $Part -or $null -eq $Total -or $Total -le 0.0) {
        return $null
    }
    return 100.0 * $Part / $Total
}

$gpuFrame = New-Object System.Collections.Generic.List[double]
$raymarch = New-Object System.Collections.Generic.List[double]
$renderPreOwner = New-Object System.Collections.Generic.List[double]
$farMaxCache = New-Object System.Collections.Generic.List[double]
$farNoHitMask = New-Object System.Collections.Generic.List[double]
$renderPreOwnerOther = New-Object System.Collections.Generic.List[double]
$farSkyOwner = New-Object System.Collections.Generic.List[double]
$backgroundCore = New-Object System.Collections.Generic.List[double]
$renderTail = New-Object System.Collections.Generic.List[double]
$surface = New-Object System.Collections.Generic.List[double]
$nearSurface = New-Object System.Collections.Generic.List[double]
$nearCull = New-Object System.Collections.Generic.List[double]
$nearDraw = New-Object System.Collections.Generic.List[double]
$midMesh = New-Object System.Collections.Generic.List[double]
$midSetup = New-Object System.Collections.Generic.List[double]
$midDraw = New-Object System.Collections.Generic.List[double]
$background = New-Object System.Collections.Generic.List[double]
$owned = New-Object System.Collections.Generic.List[double]
$screen = New-Object System.Collections.Generic.List[double]
$horizonTileMaskTiles = New-Object System.Collections.Generic.List[double]
$horizonTileMaskTotalTiles = New-Object System.Collections.Generic.List[double]
$horizonTileMaskPixelUpper = New-Object System.Collections.Generic.List[double]
$horizonTileMaskMaxEdge255 = New-Object System.Collections.Generic.List[double]
$horizonTileMaskBandTiles = New-Object System.Collections.Generic.List[double]
$horizonTileListCount = New-Object System.Collections.Generic.List[double]
$horizonTileDrawInstances = New-Object System.Collections.Generic.List[double]
$farTerrainCalls = New-Object System.Collections.Generic.List[double]
$farTerrainEarly = New-Object System.Collections.Generic.List[double]
$farTerrainFirstHit = New-Object System.Collections.Generic.List[double]
$farTerrainLoopHit = New-Object System.Collections.Generic.List[double]
$farTerrainMiss = New-Object System.Collections.Generic.List[double]
$farTerrainSkyBreak = New-Object System.Collections.Generic.List[double]
$farTerrainSteps = New-Object System.Collections.Generic.List[double]
$farTerrainRefine = New-Object System.Collections.Generic.List[double]
$farTerrainHeightEval = New-Object System.Collections.Generic.List[double]
$farTerrainSkyBreakSteps = New-Object System.Collections.Generic.List[double]
$farTerrainSkyBreakHeightEval = New-Object System.Collections.Generic.List[double]
$farTerrainDeepMissSteps = New-Object System.Collections.Generic.List[double]
$farTerrainDeepMissHeightEval = New-Object System.Collections.Generic.List[double]
$farTerrainHitSteps = New-Object System.Collections.Generic.List[double]
$farTerrainHitHeightEval = New-Object System.Collections.Generic.List[double]
$farTerrainHitRefine = New-Object System.Collections.Generic.List[double]
$farTerrainFirstHitHeightEval = New-Object System.Collections.Generic.List[double]
$farTerrainLoopHitHeightEval = New-Object System.Collections.Generic.List[double]
$farTerrainCacheReject = New-Object System.Collections.Generic.List[double]
$drainedGpuFrame = New-Object System.Collections.Generic.List[double]
$drainedRaymarch = New-Object System.Collections.Generic.List[double]
$backloggedGpuFrame = New-Object System.Collections.Generic.List[double]
$backloggedRaymarch = New-Object System.Collections.Generic.List[double]
$publishPending = New-Object 'System.Collections.Generic.Dictionary[int,int]'
$publishReady = New-Object 'System.Collections.Generic.Dictionary[int,int]'
$publishLag = New-Object 'System.Collections.Generic.Dictionary[int,int]'
$uploading = New-Object 'System.Collections.Generic.Dictionary[int,int]'
$stateFramesSeen = New-Object 'System.Collections.Generic.HashSet[int]'
$visibleMissingNonzero = 0
$residentMissingNonzero = 0
$stateRejected = 0
$stateAccepted = 0
$unknownStateSamples = 0
$firstBacklogFrame = $null
$lastDrainedFrame = $null
$maxPublishPendingSeen = 0
$maxUploadingSeen = 0
$maxPublishLagSeen = 0

function Get-DictValue([object]$Dict, [int]$Key, [int]$Default) {
    $value = 0
    if ($Dict.TryGetValue($Key, [ref]$value)) {
        return $value
    }
    return $Default
}

function Add-GpuMetricIfPresent([string]$Line, [string]$Name, [object]$List) {
    if ($Line -match "$Name=([0-9.]+)") {
        $List.Add([double]$Matches[1])
    }
}

function Test-FrameDrained([int]$Frame) {
    $stateFrame = Get-StateFrame $Frame
    if ($stateFrame -eq $null) {
        return $false
    }
    $pending = Get-DictValue $publishPending $stateFrame ([int]::MaxValue)
    $inFlight = Get-DictValue $uploading $stateFrame ([int]::MaxValue)
    $lag = Get-DictValue $publishLag $stateFrame ([int]::MaxValue)
    return $pending -le $MaxPublishPending -and
        $inFlight -le $MaxUploading -and
        $lag -le $MaxPublishLag
}

function Get-StateFrame([int]$Frame) {
    $candidate = $null
    foreach ($stateFrame in @($stateFramesSeen | Sort-Object)) {
        if ($stateFrame -le $Frame) {
            $candidate = $stateFrame
        } else {
            break
        }
    }
    return $candidate
}

function Get-FrameStateFrame([int]$Frame) {
    return Get-StateFrame $Frame
}

function Test-FrameAccepted([int]$Frame, [object]$StableDrainedFrames) {
    if (!$RequireDrained) {
        return $true
    }
    $stateFrame = Get-StateFrame $Frame
    if ($stateFrame -eq $null) {
        return $false
    }
    return $StableDrainedFrames.Contains($stateFrame)
}

function Get-StableDrainedFrames {
    $stable = New-Object 'System.Collections.Generic.HashSet[int]'
    if (!$RequireDrained -or $RequireStableFrames -le 1) {
        foreach ($frame in $stateFramesSeen) {
            if (Test-FrameDrained $frame) {
                [void]$stable.Add($frame)
            }
        }
        return $stable
    }

    $consecutive = 0
    $sortedFrames = @($stateFramesSeen | Sort-Object)
    foreach ($frame in $sortedFrames) {
        if (Test-FrameDrained $frame) {
            ++$consecutive
        } else {
            $consecutive = 0
        }
        if ($consecutive -ge $RequireStableFrames) {
            [void]$stable.Add($frame)
        }
    }
    return $stable
}

foreach ($line in Get-Content -LiteralPath $LogPath) {
    if ($line -match "visibleMissing=([1-9][0-9]*)") {
        ++$visibleMissingNonzero
    }
    if ($line -match "residentMissingSurface=([1-9][0-9]*)") {
        ++$residentMissingNonzero
    }
    if ($line -match "PERF_SPARSE .*frame=([0-9]+).*publishPending=([0-9]+).*publishReady=([0-9]+).*publishLag=([0-9]+)") {
        $frame = [int]$Matches[1]
        $publishPending[$frame] = [int]$Matches[2]
        $publishReady[$frame] = [int]$Matches[3]
        $publishLag[$frame] = [int]$Matches[4]
        $maxPublishPendingSeen = [Math]::Max($maxPublishPendingSeen, [int]$Matches[2])
        $maxPublishLagSeen = [Math]::Max($maxPublishLagSeen, [int]$Matches[4])
        [void]$stateFramesSeen.Add($frame)
    }
    if ($line -match "PERF_SPARSE_READINESS.*frame=([0-9]+).*uploading=([0-9]+)") {
        $frame = [int]$Matches[1]
        $uploading[$frame] = [int]$Matches[2]
        $maxUploadingSeen = [Math]::Max($maxUploadingSeen, [int]$Matches[2])
        [void]$stateFramesSeen.Add($frame)
    }
}

$stableDrainedFrames = Get-StableDrainedFrames

foreach ($line in Get-Content -LiteralPath $LogPath) {
    if ($line -match "PERF_GPU.*frame=([0-9]+).*gpuFrameMs=([0-9.]+).*raymarchMs=([0-9.]+).*sparseSurfaceMs=([0-9.]+).*valid=1") {
        $frame = [int]$Matches[1]
        if ($frame -gt $MinFrame) {
            $stateFrame = Get-FrameStateFrame $frame
            if ($stateFrame -eq $null) {
                ++$unknownStateSamples
            } elseif (Test-FrameDrained $frame) {
                $drainedGpuFrame.Add([double]$Matches[2])
                $drainedRaymarch.Add([double]$Matches[3])
                $lastDrainedFrame = $frame
            } else {
                $backloggedGpuFrame.Add([double]$Matches[2])
                $backloggedRaymarch.Add([double]$Matches[3])
                if ($firstBacklogFrame -eq $null) {
                    $firstBacklogFrame = $frame
                }
            }
            if (Test-FrameAccepted $frame $stableDrainedFrames) {
                $gpuFrame.Add([double]$Matches[2])
                $raymarch.Add([double]$Matches[3])
                $surface.Add([double]$Matches[4])
                Add-GpuMetricIfPresent $line "renderPreOwnerMs" $renderPreOwner
                Add-GpuMetricIfPresent $line "farMaxCacheMs" $farMaxCache
                Add-GpuMetricIfPresent $line "farNoHitMaskMs" $farNoHitMask
                Add-GpuMetricIfPresent $line "renderPreOwnerOtherMs" $renderPreOwnerOther
                Add-GpuMetricIfPresent $line "farSkyOwnerMs" $farSkyOwner
                Add-GpuMetricIfPresent $line "backgroundCoreMs" $backgroundCore
                Add-GpuMetricIfPresent $line "renderTailMs" $renderTail
                Add-GpuMetricIfPresent $line "sparseNearSurfaceMs" $nearSurface
                Add-GpuMetricIfPresent $line "sparseNearCullMs" $nearCull
                Add-GpuMetricIfPresent $line "sparseNearDrawMs" $nearDraw
                Add-GpuMetricIfPresent $line "sparseMidMeshMs" $midMesh
                Add-GpuMetricIfPresent $line "sparseMidSetupMs" $midSetup
                Add-GpuMetricIfPresent $line "sparseMidDrawMs" $midDraw
                ++$stateAccepted
            } else {
                ++$stateRejected
            }
        }
    }
    if ($line -match "PERF_RENDER_COMPOSITION.*frame=([0-9]+).*screen=([0-9]+).*backgroundPixels=([0-9]+).*surfaceOwnedPixels=([0-9]+)") {
        $frame = [int]$Matches[1]
        if ($frame -gt $MinFrame -and (Test-FrameAccepted $frame $stableDrainedFrames)) {
            $screen.Add([double]$Matches[2])
            $background.Add([double]$Matches[3])
            $owned.Add([double]$Matches[4])
            if ($line -match "horizonTileMask=tiles/total/pixelUpper/maxEdge255/frame/bandTiles:([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)") {
                $horizonTileMaskTiles.Add([double]$Matches[1])
                $horizonTileMaskTotalTiles.Add([double]$Matches[2])
                $horizonTileMaskPixelUpper.Add([double]$Matches[3])
                $horizonTileMaskMaxEdge255.Add([double]$Matches[4])
                $horizonTileMaskBandTiles.Add([double]$Matches[6])
            }
            if ($line -match "horizonTileList=count/instances:([0-9]+)/([0-9]+)") {
                $horizonTileListCount.Add([double]$Matches[1])
                $horizonTileDrawInstances.Add([double]$Matches[2])
            }
        }
    }
    if ($line -match "PERF_RENDER_OWNERSHIP.*shaderFrame=([0-9]+).*farTerrainWork=calls/early/firstHit/loopHit/miss/skyBreak/steps/refine/heightEval/cacheReject:([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)") {
        $frame = [int]$Matches[1]
        if ($frame -gt $MinFrame -and (Test-FrameAccepted $frame $stableDrainedFrames)) {
            $farTerrainCalls.Add([double]$Matches[2])
            $farTerrainEarly.Add([double]$Matches[3])
            $farTerrainFirstHit.Add([double]$Matches[4])
            $farTerrainLoopHit.Add([double]$Matches[5])
            $farTerrainMiss.Add([double]$Matches[6])
            $farTerrainSkyBreak.Add([double]$Matches[7])
            $farTerrainSteps.Add([double]$Matches[8])
            $farTerrainRefine.Add([double]$Matches[9])
            $farTerrainHeightEval.Add([double]$Matches[10])
            $farTerrainCacheReject.Add([double]$Matches[11])
        }
    } elseif ($line -match "PERF_RENDER_OWNERSHIP.*shaderFrame=([0-9]+).*farTerrainWork=calls/early/firstHit/loopHit/miss/skyBreak/steps/refine/heightEval:([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)") {
        $frame = [int]$Matches[1]
        if ($frame -gt $MinFrame -and (Test-FrameAccepted $frame $stableDrainedFrames)) {
            $farTerrainCalls.Add([double]$Matches[2])
            $farTerrainEarly.Add([double]$Matches[3])
            $farTerrainFirstHit.Add([double]$Matches[4])
            $farTerrainLoopHit.Add([double]$Matches[5])
            $farTerrainMiss.Add([double]$Matches[6])
            $farTerrainSkyBreak.Add([double]$Matches[7])
            $farTerrainSteps.Add([double]$Matches[8])
            $farTerrainRefine.Add([double]$Matches[9])
            $farTerrainHeightEval.Add([double]$Matches[10])
            $farTerrainCacheReject.Add(0.0)
        }
    }
    if ($line -match "PERF_RENDER_OWNERSHIP.*shaderFrame=([0-9]+).*farTerrainWorkByOutcome=skyBreakSteps/skyBreakHeightEval/deepMissSteps/deepMissHeightEval/hitSteps/hitHeightEval/hitRefine/firstHitHeightEval/loopHitHeightEval:([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)") {
        $frame = [int]$Matches[1]
        if ($frame -gt $MinFrame -and (Test-FrameAccepted $frame $stableDrainedFrames)) {
            $farTerrainSkyBreakSteps.Add([double]$Matches[2])
            $farTerrainSkyBreakHeightEval.Add([double]$Matches[3])
            $farTerrainDeepMissSteps.Add([double]$Matches[4])
            $farTerrainDeepMissHeightEval.Add([double]$Matches[5])
            $farTerrainHitSteps.Add([double]$Matches[6])
            $farTerrainHitHeightEval.Add([double]$Matches[7])
            $farTerrainHitRefine.Add([double]$Matches[8])
            $farTerrainFirstHitHeightEval.Add([double]$Matches[9])
            $farTerrainLoopHitHeightEval.Add([double]$Matches[10])
        }
    }
}

$gpuP50 = Percentile $gpuFrame.ToArray() 0.50
$gpuP90 = Percentile $gpuFrame.ToArray() 0.90
$rayP50 = Percentile $raymarch.ToArray() 0.50
$rayP90 = Percentile $raymarch.ToArray() 0.90
$renderPreOwnerP50 = Percentile $renderPreOwner.ToArray() 0.50
$farMaxCacheP50 = Percentile $farMaxCache.ToArray() 0.50
$farNoHitMaskP50 = Percentile $farNoHitMask.ToArray() 0.50
$renderPreOwnerOtherP50 = Percentile $renderPreOwnerOther.ToArray() 0.50
$farSkyOwnerP50 = Percentile $farSkyOwner.ToArray() 0.50
$backgroundCoreP50 = Percentile $backgroundCore.ToArray() 0.50
$renderTailP50 = Percentile $renderTail.ToArray() 0.50
$surfaceP50 = Percentile $surface.ToArray() 0.50
$nearSurfaceP50 = Percentile $nearSurface.ToArray() 0.50
$nearCullP50 = Percentile $nearCull.ToArray() 0.50
$nearDrawP50 = Percentile $nearDraw.ToArray() 0.50
$midMeshP50 = Percentile $midMesh.ToArray() 0.50
$midSetupP50 = Percentile $midSetup.ToArray() 0.50
$midDrawP50 = Percentile $midDraw.ToArray() 0.50
$backgroundP50 = Percentile $background.ToArray() 0.50
$ownedP50 = Percentile $owned.ToArray() 0.50
$screenP50 = Percentile $screen.ToArray() 0.50
$horizonTileMaskTilesP50 = Percentile $horizonTileMaskTiles.ToArray() 0.50
$horizonTileMaskTotalTilesP50 = Percentile $horizonTileMaskTotalTiles.ToArray() 0.50
$horizonTileMaskPixelUpperP50 = Percentile $horizonTileMaskPixelUpper.ToArray() 0.50
$horizonTileMaskMaxEdge255P50 = Percentile $horizonTileMaskMaxEdge255.ToArray() 0.50
$horizonTileMaskBandTilesP50 = Percentile $horizonTileMaskBandTiles.ToArray() 0.50
$horizonTileListCountP50 = Percentile $horizonTileListCount.ToArray() 0.50
$horizonTileDrawInstancesP50 = Percentile $horizonTileDrawInstances.ToArray() 0.50
$farTerrainCallsP50 = Percentile $farTerrainCalls.ToArray() 0.50
$farTerrainEarlyP50 = Percentile $farTerrainEarly.ToArray() 0.50
$farTerrainFirstHitP50 = Percentile $farTerrainFirstHit.ToArray() 0.50
$farTerrainLoopHitP50 = Percentile $farTerrainLoopHit.ToArray() 0.50
$farTerrainMissP50 = Percentile $farTerrainMiss.ToArray() 0.50
$farTerrainSkyBreakP50 = Percentile $farTerrainSkyBreak.ToArray() 0.50
$farTerrainStepsP50 = Percentile $farTerrainSteps.ToArray() 0.50
$farTerrainRefineP50 = Percentile $farTerrainRefine.ToArray() 0.50
$farTerrainHeightEvalP50 = Percentile $farTerrainHeightEval.ToArray() 0.50
$farTerrainCacheRejectP50 = Percentile $farTerrainCacheReject.ToArray() 0.50
$farTerrainSkyBreakStepsP50 = Percentile $farTerrainSkyBreakSteps.ToArray() 0.50
$farTerrainSkyBreakHeightEvalP50 = Percentile $farTerrainSkyBreakHeightEval.ToArray() 0.50
$farTerrainDeepMissStepsP50 = Percentile $farTerrainDeepMissSteps.ToArray() 0.50
$farTerrainDeepMissHeightEvalP50 = Percentile $farTerrainDeepMissHeightEval.ToArray() 0.50
$farTerrainHitStepsP50 = Percentile $farTerrainHitSteps.ToArray() 0.50
$farTerrainHitHeightEvalP50 = Percentile $farTerrainHitHeightEval.ToArray() 0.50
$farTerrainHitRefineP50 = Percentile $farTerrainHitRefine.ToArray() 0.50
$farTerrainFirstHitHeightEvalP50 = Percentile $farTerrainFirstHitHeightEval.ToArray() 0.50
$farTerrainLoopHitHeightEvalP50 = Percentile $farTerrainLoopHitHeightEval.ToArray() 0.50
$farTerrainHitCallsP50 = $null
$farTerrainNonHitCallsP50 = $null
if ($farTerrainFirstHitP50 -ne $null -and $farTerrainLoopHitP50 -ne $null) {
    $farTerrainHitCallsP50 = $farTerrainFirstHitP50 + $farTerrainLoopHitP50
}
if ($farTerrainEarlyP50 -ne $null -and $farTerrainMissP50 -ne $null -and $farTerrainCacheRejectP50 -ne $null) {
    $farTerrainNonHitCallsP50 = $farTerrainEarlyP50 + $farTerrainMissP50 + $farTerrainCacheRejectP50
}

$backgroundPct = $null
if ($screenP50 -and $screenP50 -gt 0 -and $backgroundP50 -ne $null) {
    $backgroundPct = 100.0 * $backgroundP50 / $screenP50
}
$horizonTileMaskPixelUpperPctP50 = $null
if ($screenP50 -and $screenP50 -gt 0 -and $horizonTileMaskPixelUpperP50 -ne $null) {
    $horizonTileMaskPixelUpperPctP50 = 100.0 * $horizonTileMaskPixelUpperP50 / $screenP50
}
$horizonTileMaskTilePctP50 = PercentOf $horizonTileMaskTilesP50 $horizonTileMaskTotalTilesP50

$ok = $true
if ($visibleMissingNonzero -gt $MaxVisibleMissingNonzero) {
    $ok = $false
}
if ($residentMissingNonzero -gt $MaxResidentMissingNonzero) {
    $ok = $false
}
if ($gpuFrame.Count -eq 0 -or $background.Count -eq 0) {
    $ok = $false
}
if ($RequireDrained -and $gpuFrame.Count -lt $MinAcceptedSamples) {
    $ok = $false
}

$totalStateClassified = $drainedGpuFrame.Count + $backloggedGpuFrame.Count
$drainedPct = $null
$backloggedPct = $null
if ($totalStateClassified -gt 0) {
    $drainedPct = 100.0 * $drainedGpuFrame.Count / $totalStateClassified
    $backloggedPct = 100.0 * $backloggedGpuFrame.Count / $totalStateClassified
}

$summary = [pscustomobject]@{
    label = $Label
    minFrame = $MinFrame
    gpuSamples = $gpuFrame.Count
    compositionSamples = $background.Count
    gpuFrameMsP50 = $gpuP50
    gpuFrameMsP90 = $gpuP90
    raymarchMsP50 = $rayP50
    raymarchMsP90 = $rayP90
    renderPreOwnerMsP50 = $renderPreOwnerP50
    farMaxCacheMsP50 = $farMaxCacheP50
    farNoHitMaskMsP50 = $farNoHitMaskP50
    renderPreOwnerOtherMsP50 = $renderPreOwnerOtherP50
    farSkyOwnerMsP50 = $farSkyOwnerP50
    backgroundCoreMsP50 = $backgroundCoreP50
    renderTailMsP50 = $renderTailP50
    sparseSurfaceMsP50 = $surfaceP50
    sparseNearSurfaceMsP50 = $nearSurfaceP50
    sparseNearCullMsP50 = $nearCullP50
    sparseNearDrawMsP50 = $nearDrawP50
    sparseMidMeshMsP50 = $midMeshP50
    sparseMidSetupMsP50 = $midSetupP50
    sparseMidDrawMsP50 = $midDrawP50
    drainedSamples = $drainedGpuFrame.Count
    drainedPct = $drainedPct
    drainedGpuFrameMsP50 = Percentile $drainedGpuFrame.ToArray() 0.50
    drainedRaymarchMsP50 = Percentile $drainedRaymarch.ToArray() 0.50
    backloggedSamples = $backloggedGpuFrame.Count
    backloggedPct = $backloggedPct
    backloggedGpuFrameMsP50 = Percentile $backloggedGpuFrame.ToArray() 0.50
    backloggedRaymarchMsP50 = Percentile $backloggedRaymarch.ToArray() 0.50
    unknownStateSamples = $unknownStateSamples
    firstBacklogFrame = $firstBacklogFrame
    lastDrainedFrame = $lastDrainedFrame
    maxPublishPendingSeen = $maxPublishPendingSeen
    maxUploadingSeen = $maxUploadingSeen
    maxPublishLagSeen = $maxPublishLagSeen
    screenP50 = $screenP50
    backgroundPixelsP50 = $backgroundP50
    backgroundPctP50 = $backgroundPct
    surfaceOwnedPixelsP50 = $ownedP50
    horizonTileMaskSamples = $horizonTileMaskTiles.Count
    horizonTileMaskTilesP50 = $horizonTileMaskTilesP50
    horizonTileMaskTotalTilesP50 = $horizonTileMaskTotalTilesP50
    horizonTileMaskBandTilesP50 = $horizonTileMaskBandTilesP50
    horizonTileMaskTilePctP50 = $horizonTileMaskTilePctP50
    horizonTileMaskPixelUpperP50 = $horizonTileMaskPixelUpperP50
    horizonTileMaskPixelUpperPctP50 = $horizonTileMaskPixelUpperPctP50
    horizonTileMaskMaxEdge255P50 = $horizonTileMaskMaxEdge255P50
    horizonTileListSamples = $horizonTileListCount.Count
    horizonTileListCountP50 = $horizonTileListCountP50
    horizonTileDrawInstancesP50 = $horizonTileDrawInstancesP50
    farTerrainSamples = $farTerrainCalls.Count
    farTerrainCallsP50 = $farTerrainCallsP50
    farTerrainEarlyP50 = $farTerrainEarlyP50
    farTerrainFirstHitP50 = $farTerrainFirstHitP50
    farTerrainLoopHitP50 = $farTerrainLoopHitP50
    farTerrainMissP50 = $farTerrainMissP50
    farTerrainSkyBreakP50 = $farTerrainSkyBreakP50
    farTerrainStepsP50 = $farTerrainStepsP50
    farTerrainRefineP50 = $farTerrainRefineP50
    farTerrainHeightEvalP50 = $farTerrainHeightEvalP50
    farTerrainCacheRejectP50 = $farTerrainCacheRejectP50
    farTerrainHitCallsP50 = $farTerrainHitCallsP50
    farTerrainHitCallPctP50 = PercentOf $farTerrainHitCallsP50 $farTerrainCallsP50
    farTerrainNonHitCallsP50 = $farTerrainNonHitCallsP50
    farTerrainNonHitCallPctP50 = PercentOf $farTerrainNonHitCallsP50 $farTerrainCallsP50
    farTerrainMissCallPctP50 = PercentOf $farTerrainMissP50 $farTerrainCallsP50
    farTerrainSkyBreakMissPctP50 = PercentOf $farTerrainSkyBreakP50 $farTerrainMissP50
    farTerrainSkyBreakStepsP50 = $farTerrainSkyBreakStepsP50
    farTerrainSkyBreakHeightEvalP50 = $farTerrainSkyBreakHeightEvalP50
    farTerrainSkyBreakHeightEvalPctP50 = PercentOf $farTerrainSkyBreakHeightEvalP50 $farTerrainHeightEvalP50
    farTerrainDeepMissStepsP50 = $farTerrainDeepMissStepsP50
    farTerrainDeepMissHeightEvalP50 = $farTerrainDeepMissHeightEvalP50
    farTerrainDeepMissHeightEvalPctP50 = PercentOf $farTerrainDeepMissHeightEvalP50 $farTerrainHeightEvalP50
    farTerrainHitStepsP50 = $farTerrainHitStepsP50
    farTerrainHitHeightEvalP50 = $farTerrainHitHeightEvalP50
    farTerrainHitHeightEvalPctP50 = PercentOf $farTerrainHitHeightEvalP50 $farTerrainHeightEvalP50
    farTerrainHitRefineP50 = $farTerrainHitRefineP50
    farTerrainFirstHitHeightEvalP50 = $farTerrainFirstHitHeightEvalP50
    farTerrainLoopHitHeightEvalP50 = $farTerrainLoopHitHeightEvalP50
    visibleMissingNonzero = $visibleMissingNonzero
    residentMissingNonzero = $residentMissingNonzero
    requireDrained = [bool]$RequireDrained
    maxPublishPending = if ($RequireDrained) { $MaxPublishPending } else { $null }
    maxUploading = if ($RequireDrained) { $MaxUploading } else { $null }
    maxPublishLag = if ($RequireDrained) { $MaxPublishLag } else { $null }
    requireStableFrames = if ($RequireDrained) { $RequireStableFrames } else { $null }
    minAcceptedSamples = if ($RequireDrained) { $MinAcceptedSamples } else { $null }
    stateAcceptedSamples = $stateAccepted
    stateRejectedSamples = $stateRejected
    ok = $ok
}

$summary | Format-List

if (!$ok) {
    exit 1
}
