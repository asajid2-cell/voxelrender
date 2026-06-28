param(
    [int]$Frames = 1200,
    [int]$Warmup = 240,
    [string]$Label = "prod",
    [string]$Replay = "",
    [ValidateSet("none", "60fps", "30fps", "detail", "quality")]
    [string]$PerfMode = "quality",
    [switch]$NoBuild,
    [switch]$ShippingVSync,
    [switch]$DisableFarOwner,
    [switch]$FarDda,
    [switch]$ReplayBrush
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$rebrun = Join-Path $root "rebrun.ps1"
$buildDir = if (-not [string]::IsNullOrWhiteSpace($env:VENPOD_BUILD_DIR)) {
    $env:VENPOD_BUILD_DIR
} else {
    Join-Path $root "build"
}
$binDir = Join-Path $buildDir "bin"
$runtimeLog = Join-Path $binDir "venpod_runtime.log"
$outDir = Join-Path $binDir ("prodbench\" + $Label)
$outLog = Join-Path $outDir "run.log"

if (!(Test-Path -LiteralPath $rebrun)) {
    throw "rebrun.ps1 not found: $rebrun"
}

function Percentile([double[]]$Values, [double]$P) {
    if ($null -eq $Values -or $Values.Count -eq 0) {
        return $null
    }
    $sorted = @($Values | Sort-Object)
    $index = [Math]::Min($sorted.Count - 1, [int]($sorted.Count * $P))
    return $sorted[$index]
}

function Format-Ms($Value) {
    if ($null -eq $Value) {
        return "n/a"
    }
    return ("{0:N2}" -f [double]$Value)
}

function Add-SavedEnv([hashtable]$Saved, [string]$Name) {
    $Saved[$Name] = [Environment]::GetEnvironmentVariable($Name, "Process")
}

function Restore-SavedEnv([hashtable]$Saved) {
    foreach ($name in $Saved.Keys) {
        if ($null -eq $Saved[$name]) {
            [Environment]::SetEnvironmentVariable($name, $null, "Process")
        } else {
            [Environment]::SetEnvironmentVariable($name, [string]$Saved[$name], "Process")
        }
    }
}

function Clear-ProcessEnv([string[]]$Names) {
    foreach ($name in $Names) {
        [Environment]::SetEnvironmentVariable($name, $null, "Process")
    }
}

function Parse-FtLines([string]$LogPath, [int]$MinFrame) {
    $rows = New-Object System.Collections.Generic.List[object]
    foreach ($line in Get-Content -LiteralPath $LogPath) {
        if ($line -match "FT\s+([0-9]+)\s+body=([0-9.]+)\s+raw=([0-9.]+)") {
            $frame = [int]$Matches[1]
            if ($frame -gt $MinFrame) {
                $row = [pscustomobject]@{
                    frame = $frame
                    bodyMs = [double]$Matches[2]
                    rawMs = [double]$Matches[3]
                    prepMs = $null
                    sparsePrepMs = $null
                    sparseReqMs = $null
                    sparseGenMs = $null
                    sparseClipMs = $null
                    clipInterestMs = $null
                    clipBudgetMs = $null
                    clipPumpMs = $null
                    clipPumpHeightMs = $null
                    clipPumpVoxelMs = $null
                    clipPumpGeneratedHeight = $null
                    clipPumpGeneratedVoxel = $null
                    clipPumpQueuedHeight = $null
                    clipPumpQueuedVoxel = $null
                    waitMs = $null
                    chunkMs = $null
                    physicsMs = $null
                    brushMs = $null
                    renderSubmitMs = $null
                    presentMs = $null
                    postGenMs = $null
                    inputEndMs = $null
                    postWaitGapMs = $null
                    prePhysicsGapMs = $null
                    preRenderGapMs = $null
                    postRenderGapMs = $null
                    postWaitFeedbackMs = $null
                    postWaitCommandBeginMs = $null
                    postWaitBeginFrameMs = $null
                    postWaitMidSnapshotMs = $null
                    postWaitResidualMs = $null
                    postFeedbackLegacyMs = $null
                    postFeedbackRaycastMs = $null
                    postFeedbackMissMs = $null
                    postFeedbackBrushMs = $null
                    postFeedbackOwnershipMs = $null
                    postFeedbackPhysicsMs = $null
                    sparseUploadMs = $null
                    sparsePublishMs = $null
                    surfaceExtractMs = $null
                    surfaceStageMs = $null
                    surfacePrePublishMs = $null
                    surfaceReadyPublishMs = $null
                    surfaceTerrainCriticalMs = $null
                    surfaceHiddenExactMs = $null
                    surfaceGeneralPumpMs = $null
                    surfaceBudgetLogMs = $null
                    surfacePruneMs = $null
                    surfaceStatsCommitMs = $null
                    surfacePlanMs = $null
                    surfaceSnapshotMs = $null
                    surfaceEmitMs = $null
                    surfacePrePublishCount = $null
                    surfacePrePublishTerrainCount = $null
                    surfacePrePublishHiddenCriticalCount = $null
                    surfacePrePublishHiddenTrackedCount = $null
                    surfacePrePublishGeneralCount = $null
                    surfacePrePublishOwnershipCriticalMs = $null
                    surfacePrePublishOwnershipNonCriticalMs = $null
                    surfacePrePublishOwnershipCriticalCount = $null
                    surfacePrePublishOwnershipNonCriticalCount = $null
                    surfaceTerrainCriticalCount = $null
                    surfaceHiddenExactCount = $null
                    surfaceGeneralCount = $null
                    surfacePruneScannedCount = $null
                    surfacePruneRemovedCount = $null
                    gpuValid = $null
                    gpuFrameMs = $null
                    gpuSurfaceMs = $null
                    gpuRayMs = $null
                }
                if ($line -match "prep=([0-9.]+)\s+sparsePrep=([0-9.]+)\s+sparseReq=([0-9.]+)\s+sparseGen=([0-9.]+)\s+sparseClip=([0-9.]+)(?:\s+clipInterest=([0-9.]+)\s+clipBudget=([0-9.]+)\s+clipPump=([0-9.]+)(?:\s+clipPumpSplit=height/voxel/genH/genV/qH/qV:([0-9.]+)/([0-9.]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+))?)?\s+wait=([0-9.]+)\s+chunk=([0-9.]+)\s+phys=([0-9.]+)\s+brush=([0-9.]+)\s+render=([0-9.]+)\s+present=([0-9.]+)\s+postGen=([0-9.]+)\s+inputEnd=([0-9.]+)(?:\s+gaps=.*?)?\s+gpuValid=([01])\s+gpuFrame=([0-9.]+)\s+gpuSurface=([0-9.]+)\s+gpuRay=([0-9.]+)") {
                    $row.prepMs = [double]$Matches[1]
                    $row.sparsePrepMs = [double]$Matches[2]
                    $row.sparseReqMs = [double]$Matches[3]
                    $row.sparseGenMs = [double]$Matches[4]
                    $row.sparseClipMs = [double]$Matches[5]
                    if ($Matches[6]) { $row.clipInterestMs = [double]$Matches[6] }
                    if ($Matches[7]) { $row.clipBudgetMs = [double]$Matches[7] }
                    if ($Matches[8]) { $row.clipPumpMs = [double]$Matches[8] }
                    if ($Matches[9]) { $row.clipPumpHeightMs = [double]$Matches[9] }
                    if ($Matches[10]) { $row.clipPumpVoxelMs = [double]$Matches[10] }
                    if ($Matches[11]) { $row.clipPumpGeneratedHeight = [double]$Matches[11] }
                    if ($Matches[12]) { $row.clipPumpGeneratedVoxel = [double]$Matches[12] }
                    if ($Matches[13]) { $row.clipPumpQueuedHeight = [double]$Matches[13] }
                    if ($Matches[14]) { $row.clipPumpQueuedVoxel = [double]$Matches[14] }
                    $row.waitMs = [double]$Matches[15]
                    $row.chunkMs = [double]$Matches[16]
                    $row.physicsMs = [double]$Matches[17]
                    $row.brushMs = [double]$Matches[18]
                    $row.renderSubmitMs = [double]$Matches[19]
                    $row.presentMs = [double]$Matches[20]
                    $row.postGenMs = [double]$Matches[21]
                    $row.inputEndMs = [double]$Matches[22]
                    $gpuValid = [int]$Matches[23]
                    $gpuFrameMs = [double]$Matches[24]
                    $gpuSurfaceMs = [double]$Matches[25]
                    $gpuRayMs = [double]$Matches[26]
                    if ($line -match "gaps=([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)") {
                        $row.postWaitGapMs = [double]$Matches[1]
                        $row.prePhysicsGapMs = [double]$Matches[2]
                        $row.preRenderGapMs = [double]$Matches[3]
                        $row.postRenderGapMs = [double]$Matches[4]
                    }
                    if ($line -match "sparsePost=upload/publish/surfExtract/surfStage:([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)") {
                        $row.sparseUploadMs = [double]$Matches[1]
                        $row.sparsePublishMs = [double]$Matches[2]
                        $row.surfaceExtractMs = [double]$Matches[3]
                        $row.surfaceStageMs = [double]$Matches[4]
                    }
                    if ($line -match "postWaitSplit=feedback/cmd/begin/midSnap/residual:([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)") {
                        $row.postWaitFeedbackMs = [double]$Matches[1]
                        $row.postWaitCommandBeginMs = [double]$Matches[2]
                        $row.postWaitBeginFrameMs = [double]$Matches[3]
                        $row.postWaitMidSnapshotMs = [double]$Matches[4]
                        $row.postWaitResidualMs = [double]$Matches[5]
                    }
                    if ($line -match "feedbackSplit=legacy/raycast/miss/brush/own/phys:([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)") {
                        $row.postFeedbackLegacyMs = [double]$Matches[1]
                        $row.postFeedbackRaycastMs = [double]$Matches[2]
                        $row.postFeedbackMissMs = [double]$Matches[3]
                        $row.postFeedbackBrushMs = [double]$Matches[4]
                        $row.postFeedbackOwnershipMs = [double]$Matches[5]
                        $row.postFeedbackPhysicsMs = [double]$Matches[6]
                    }
                    if ($line -match "surfaceBreak=prePub/ready/terrain/hidden/general/budget/prune/stats:([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)") {
                        $row.surfacePrePublishMs = [double]$Matches[1]
                        $row.surfaceReadyPublishMs = [double]$Matches[2]
                        $row.surfaceTerrainCriticalMs = [double]$Matches[3]
                        $row.surfaceHiddenExactMs = [double]$Matches[4]
                        $row.surfaceGeneralPumpMs = [double]$Matches[5]
                        $row.surfaceBudgetLogMs = [double]$Matches[6]
                        $row.surfacePruneMs = [double]$Matches[7]
                        $row.surfaceStatsCommitMs = [double]$Matches[8]
                    }
                    if ($line -match "surfaceUpload=plan/snapshot/stage/emit:([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)") {
                        $row.surfacePlanMs = [double]$Matches[1]
                        $row.surfaceSnapshotMs = [double]$Matches[2]
                        $row.surfaceStageMs = [double]$Matches[3]
                        $row.surfaceEmitMs = [double]$Matches[4]
                    }
                    if ($line -match "surfaceCounts=prePub/terrain/hidden/general/pruneScan/pruneRemoved:([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)") {
                        $row.surfacePrePublishCount = [double]$Matches[1]
                        $row.surfaceTerrainCriticalCount = [double]$Matches[2]
                        $row.surfaceHiddenExactCount = [double]$Matches[3]
                        $row.surfaceGeneralCount = [double]$Matches[4]
                        $row.surfacePruneScannedCount = [double]$Matches[5]
                        $row.surfacePruneRemovedCount = [double]$Matches[6]
                    }
                    if ($line -match "surfacePrePubCounts=terrain/hiddenCritical/hiddenTracked/general:([0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)") {
                        $row.surfacePrePublishTerrainCount = [double]$Matches[1]
                        $row.surfacePrePublishHiddenCriticalCount = [double]$Matches[2]
                        $row.surfacePrePublishHiddenTrackedCount = [double]$Matches[3]
                        $row.surfacePrePublishGeneralCount = [double]$Matches[4]
                    }
                    if ($line -match "surfacePrePubOwnership=critMs/nonCritMs/critCount/nonCritCount:([0-9.]+)/([0-9.]+)/([0-9]+)/([0-9]+)") {
                        $row.surfacePrePublishOwnershipCriticalMs = [double]$Matches[1]
                        $row.surfacePrePublishOwnershipNonCriticalMs = [double]$Matches[2]
                        $row.surfacePrePublishOwnershipCriticalCount = [double]$Matches[3]
                        $row.surfacePrePublishOwnershipNonCriticalCount = [double]$Matches[4]
                    }
                    $row.gpuValid = $gpuValid
                    $row.gpuFrameMs = $gpuFrameMs
                    $row.gpuSurfaceMs = $gpuSurfaceMs
                    $row.gpuRayMs = $gpuRayMs
                }
                $rows.Add($row)
            }
        }
    }
    return $rows.ToArray()
}

function Parse-PerfSamples([string]$LogPath, [int]$MinFrame) {
    $rows = New-Object System.Collections.Generic.List[object]
    foreach ($line in Get-Content -LiteralPath $LogPath) {
        if ($line -match "PERF frame=([0-9]+).*?ms=([0-9.]+)/([0-9.]+).*?prep=([0-9.]+).*?prepSplit=sched/input/sparse/coll:([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+).*?sparseSplit=req/gen/clip/trim:([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+).*?wait=([0-9.]+).*?chunk=([0-9.]+).*?phys=([0-9.]+).*?brush=([0-9.]+).*?render=([0-9.]+).*?present=([0-9.]+).*?postGenPrev=([0-9.]+).*?inputEndPrev=([0-9.]+).*?bodyPrev=([0-9.]+).*?accounted=([0-9.]+).*?untracked=([0-9.]+).*?gpuValid=([01]).*?gpu=frame/upload/pre/surface/near/mid/ray/overlay/ui:([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)") {
            $frame = [int]$Matches[1]
            if ($frame -gt $MinFrame) {
                $rows.Add([pscustomobject]@{
                    frame = $frame
                    smoothedMs = [double]$Matches[2]
                    rawMs = [double]$Matches[3]
                    prepMs = [double]$Matches[4]
                    sparsePrepMs = [double]$Matches[7]
                    sparseReqMs = [double]$Matches[9]
                    sparseGenMs = [double]$Matches[10]
                    sparseClipMs = [double]$Matches[11]
                    sparseTrimMs = [double]$Matches[12]
                    waitMs = [double]$Matches[13]
                    chunkMs = [double]$Matches[14]
                    physicsMs = [double]$Matches[15]
                    brushMs = [double]$Matches[16]
                    renderSubmitMs = [double]$Matches[17]
                    presentMs = [double]$Matches[18]
                    postGenMs = [double]$Matches[19]
                    inputEndMs = [double]$Matches[20]
                    bodyPrevMs = [double]$Matches[21]
                    accountedMs = [double]$Matches[22]
                    untrackedMs = [double]$Matches[23]
                    gpuValid = [int]$Matches[24]
                    gpuFrameMs = [double]$Matches[25]
                    gpuUploadMs = [double]$Matches[26]
                    gpuPreMs = [double]$Matches[27]
                    gpuSurfaceMs = [double]$Matches[28]
                    gpuNearMs = [double]$Matches[29]
                    gpuMidMs = [double]$Matches[30]
                    gpuRayMs = [double]$Matches[31]
                    gpuOverlayMs = [double]$Matches[32]
                    gpuUiMs = [double]$Matches[33]
                })
            }
        }
    }
    return $rows.ToArray()
}

function Summarize-Field([object[]]$Rows, [string]$Field) {
    $values = @($Rows | Where-Object { $null -ne $_.$Field } | ForEach-Object { [double]($_.$Field) })
    return [pscustomobject]@{
        p50 = Percentile $values 0.50
        p90 = Percentile $values 0.90
        p99 = Percentile $values 0.99
        max = if ($values.Count -gt 0) { ($values | Measure-Object -Maximum).Maximum } else { $null }
    }
}

$envNamesToProtect = @(
    "VENPOD_FRAMETIME_LOG",
    "VENPOD_VSYNC",
    "VENPOD_PERF_FRAME_END_LOG_INTERVAL",
    "VENPOD_PERF_SUMMARY_LOG_INTERVAL",
    "VENPOD_SPARSE_MID_CLIPMAP_INTEREST_DETAIL",
    "VENPOD_REPLAY",
    "VENPOD_REPLAY_BRUSH",
    "VENPOD_CAPTURE_FRAME",
    "VENPOD_CAPTURE_DIR",
    "VENPOD_CAPTURE_INCLUDE_HELD",
    "VENPOD_CAPTURE_HIDE_UI",
    "VENPOD_FAR_MAX_HEIGHT_CACHE",
    "VENPOD_FAR_MAX_HEIGHT_NO_HIT_MASK",
    "VENPOD_FAR_SKY_OWNER",
    "VENPOD_RAYMARCH_FAR_MAX_HEIGHT_DDA",
    "VENPOD_FAR_SKY_OWNER_MIN_Y",
    "VENPOD_FAR_MAX_HEIGHT_SCREEN_MASK_MIP",
    "VENPOD_FAR_MAX_HEIGHT_SCREEN_MASK_DILATION"
)

$savedEnv = @{}
foreach ($name in $envNamesToProtect) {
    Add-SavedEnv $savedEnv $name
}

try {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    Remove-Item -LiteralPath $outLog -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $runtimeLog -Force -ErrorAction SilentlyContinue

    Clear-ProcessEnv $envNamesToProtect
    $env:VENPOD_FRAMETIME_LOG = "1"
    $env:VENPOD_VSYNC = if ($ShippingVSync) { "1" } else { "0" }
    if ($Replay -ne "") {
        $env:VENPOD_REPLAY = Join-Path $binDir $Replay
        if ($ReplayBrush) {
            $env:VENPOD_REPLAY_BRUSH = "1"
        } elseif ([string]::IsNullOrWhiteSpace($savedEnv["VENPOD_REPLAY_BRUSH"])) {
            $env:VENPOD_REPLAY_BRUSH = "0"
        } else {
            $env:VENPOD_REPLAY_BRUSH = [string]$savedEnv["VENPOD_REPLAY_BRUSH"]
        }
    }

    if ($DisableFarOwner) {
        $env:VENPOD_FAR_MAX_HEIGHT_CACHE = "0"
        $env:VENPOD_FAR_MAX_HEIGHT_NO_HIT_MASK = "0"
        $env:VENPOD_FAR_SKY_OWNER = "0"
    }
    if ($FarDda) {
        $env:VENPOD_RAYMARCH_FAR_MAX_HEIGHT_DDA = "1"
    }

    $runDescription = if ($NoBuild) {
        ".\rebrun.ps1 -NoBuild -PerfMode $PerfMode -ExitAfterFrames $Frames"
    } else {
        ".\rebrun.ps1 -PerfMode $PerfMode -ExitAfterFrames $Frames"
    }
    Write-Host ("[prodbench] running: {0}" -f $runDescription)
    Write-Host ("[prodbench] vsync={0} ({1}) replay={2} replayBrush={3} perfMode={4} farDda={5}" -f $env:VENPOD_VSYNC, ($(if ($ShippingVSync) { "shipping pacing" } else { "throughput" })), ($(if ($Replay -ne "") { $Replay } else { "none" })), ($(if ($Replay -ne "") { $env:VENPOD_REPLAY_BRUSH } else { "n/a" })), $PerfMode, ($(if ($FarDda) { "1" } else { "0" })))
    Push-Location $root
    try {
        if ($NoBuild) {
            & $rebrun -NoBuild -PerfMode $PerfMode -ExitAfterFrames $Frames
        } else {
            & $rebrun -PerfMode $PerfMode -ExitAfterFrames $Frames
        }
        $exitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }

    if (Test-Path -LiteralPath $runtimeLog) {
        Copy-Item -LiteralPath $runtimeLog -Destination $outLog -Force
    } else {
        throw "Runtime log was not produced: $runtimeLog"
    }

    Write-Host ("[prodbench] exit={0} log={1}" -f $exitCode, $outLog)
    if ($exitCode -ne 0) {
        exit $exitCode
    }

    $ft = @(Parse-FtLines $outLog $Warmup)
    $perf = @(Parse-PerfSamples $outLog $Warmup)
    $visibleMissingNonzero = (Select-String -LiteralPath $outLog -Pattern "visibleMissing=([1-9][0-9]*)" -AllMatches | Measure-Object).Count
    $residentMissingNonzero = (Select-String -LiteralPath $outLog -Pattern "residentMissingSurface=([1-9][0-9]*)" -AllMatches | Measure-Object).Count

    if ($ft.Count -eq 0) {
        throw "No FT samples found after warmup frame $Warmup"
    }

    $body = Summarize-Field $ft "bodyMs"
    $raw = Summarize-Field $ft "rawMs"
    $over10 = @($ft | Where-Object { $_.bodyMs -gt 10.0 }).Count
    $over1667 = @($ft | Where-Object { $_.bodyMs -gt 16.67 }).Count
    $over10Raw = @($ft | Where-Object { $_.rawMs -gt 10.0 }).Count
    $worst = @($ft | Sort-Object bodyMs -Descending | Select-Object -First 8)

    Write-Host ""
    Write-Host ("PRODBENCH {0} frames={1} warmup>{2} ftSamples={3}" -f $Label, $Frames, $Warmup, $ft.Count)
    Write-Host ("bodyMs p50={0} p90={1} p99={2} max={3}  over10ms={4}/{5} over16.67ms={6}/{5}" -f `
        (Format-Ms $body.p50), (Format-Ms $body.p90), (Format-Ms $body.p99), (Format-Ms $body.max), $over10, $ft.Count, $over1667)
    Write-Host ("rawMs  p50={0} p90={1} p99={2} max={3}  over10ms={4}/{5}" -f `
        (Format-Ms $raw.p50), (Format-Ms $raw.p90), (Format-Ms $raw.p99), (Format-Ms $raw.max), $over10Raw, $ft.Count)
    Write-Host ("missing visibleNonzero={0} residentSurfaceNonzero={1}" -f $visibleMissingNonzero, $residentMissingNonzero)

    if ($perf.Count -gt 0) {
        $fields = @(
            "prepMs", "sparsePrepMs", "sparseReqMs", "sparseGenMs", "sparseClipMs",
            "renderSubmitMs", "presentMs", "postGenMs", "bodyPrevMs",
            "gpuFrameMs", "gpuSurfaceMs", "gpuRayMs"
        )
        Write-Host ""
        Write-Host ("periodic PERF samples after warmup: {0}" -f $perf.Count)
        foreach ($field in $fields) {
            $s = Summarize-Field $perf $field
            Write-Host ("{0,-16} p50={1} p90={2} max={3}" -f $field, (Format-Ms $s.p50), (Format-Ms $s.p90), (Format-Ms $s.max))
        }
    } else {
        Write-Host "periodic PERF samples after warmup: 0"
    }

    $detailedFt = @($ft | Where-Object { $null -ne $_.prepMs })
    if ($detailedFt.Count -gt 0) {
        $fields = @(
            "prepMs", "sparsePrepMs", "sparseReqMs", "sparseGenMs", "sparseClipMs",
            "waitMs", "chunkMs", "physicsMs", "brushMs",
            "clipInterestMs", "clipBudgetMs", "clipPumpMs",
            "clipPumpHeightMs", "clipPumpVoxelMs", "clipPumpGeneratedHeight",
            "clipPumpGeneratedVoxel", "clipPumpQueuedHeight", "clipPumpQueuedVoxel",
            "renderSubmitMs", "presentMs", "postGenMs", "inputEndMs",
            "postWaitGapMs", "prePhysicsGapMs", "preRenderGapMs", "postRenderGapMs",
            "postWaitFeedbackMs", "postWaitCommandBeginMs", "postWaitBeginFrameMs",
            "postWaitMidSnapshotMs", "postWaitResidualMs",
            "postFeedbackLegacyMs", "postFeedbackRaycastMs", "postFeedbackMissMs",
            "postFeedbackBrushMs", "postFeedbackOwnershipMs", "postFeedbackPhysicsMs",
            "sparseUploadMs", "sparsePublishMs", "surfaceExtractMs", "surfaceStageMs",
            "surfacePrePublishMs", "surfaceReadyPublishMs", "surfaceTerrainCriticalMs", "surfaceHiddenExactMs",
            "surfaceGeneralPumpMs", "surfaceBudgetLogMs", "surfacePruneMs", "surfaceStatsCommitMs",
            "surfacePlanMs", "surfaceSnapshotMs", "surfaceEmitMs",
            "surfacePrePublishCount", "surfaceTerrainCriticalCount", "surfaceHiddenExactCount", "surfaceGeneralCount",
            "surfacePrePublishTerrainCount", "surfacePrePublishHiddenCriticalCount",
            "surfacePrePublishHiddenTrackedCount", "surfacePrePublishGeneralCount",
            "surfacePrePublishOwnershipCriticalMs", "surfacePrePublishOwnershipNonCriticalMs",
            "surfacePrePublishOwnershipCriticalCount", "surfacePrePublishOwnershipNonCriticalCount",
            "surfacePruneScannedCount", "surfacePruneRemovedCount",
            "gpuFrameMs", "gpuSurfaceMs", "gpuRayMs"
        )
        $slowFt = @($detailedFt | Where-Object { $_.bodyMs -gt 10.0 })
        Write-Host ""
        Write-Host ("per-frame FT phase samples after warmup: {0} slowBody>10ms={1}" -f $detailedFt.Count, $slowFt.Count)
        foreach ($field in $fields) {
            $all = Summarize-Field $detailedFt $field
            $slow = Summarize-Field $slowFt $field
            Write-Host ("{0,-16} allP50={1} allP90={2} slowP50={3} slowP90={4}" -f `
                $field, (Format-Ms $all.p50), (Format-Ms $all.p90), (Format-Ms $slow.p50), (Format-Ms $slow.p90))
        }
    }

    Write-Host ""
    Write-Host "worst body frames:"
    foreach ($row in $worst) {
        Write-Host ("  frame={0} body={1:N2} raw={2:N2}" -f $row.frame, $row.bodyMs, $row.rawMs)
        if ($null -ne $row.prepMs) {
            Write-Host ("    phases prep={0:N2} sparseReq={1:N2} sparseClip={2:N2} clipInterest={3:N2} clipPump={4:N2} heightPump={5:N2} brush={6:N2} wait={7:N2} render={8:N2} present={9:N2} gpu={10:N2}/{11:N2}/{12:N2}" -f `
                $row.prepMs,
                $row.sparseReqMs,
                $row.sparseClipMs,
                $row.clipInterestMs,
                $row.clipPumpMs,
                $row.clipPumpHeightMs,
                $row.brushMs,
                $row.waitMs,
                $row.renderSubmitMs,
                $row.presentMs,
                $row.gpuFrameMs,
                $row.gpuSurfaceMs,
                $row.gpuRayMs)
        }
        if (($null -ne $row.surfaceExtractMs) -or ($null -ne $row.surfacePrePublishMs)) {
            Write-Host ("    surface post upload={0:N2} publish={1:N2} extract={2:N2} stage={3:N2} prePub={4:N2} ready={5:N2} hidden={6:N2} general={7:N2} prune={8:N2} counts prePub/terrain/hidden/general={9}/{10}/{11}/{12}" -f `
                $row.sparseUploadMs,
                $row.sparsePublishMs,
                $row.surfaceExtractMs,
                $row.surfaceStageMs,
                $row.surfacePrePublishMs,
                $row.surfaceReadyPublishMs,
                $row.surfaceHiddenExactMs,
                $row.surfaceGeneralPumpMs,
                $row.surfacePruneMs,
                $row.surfacePrePublishCount,
                $row.surfaceTerrainCriticalCount,
                $row.surfaceHiddenExactCount,
                $row.surfaceGeneralCount)
        }
    }

    $farParser = Join-Path $PSScriptRoot "parse_farfield_perf.ps1"
    if (Test-Path -LiteralPath $farParser) {
        Write-Host ""
        Write-Host "farfield/composition parser:"
        & $farParser -LogPath $outLog -MinFrame $Warmup -Label $Label
    }
} finally {
    Restore-SavedEnv $savedEnv
}
