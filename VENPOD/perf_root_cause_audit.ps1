param(
    [string]$OutputDir = "build\captures\perf_root_cause_20260602"
)

$ErrorActionPreference = "Stop"

$Culture = [Globalization.CultureInfo]::InvariantCulture

function Get-LogLine {
    param(
        [string[]]$Lines,
        [string]$Pattern
    )
    foreach ($line in $Lines) {
        if ($line -match $Pattern) {
            return $line
        }
    }
    return $null
}

function Get-Number {
    param(
        [string]$Line,
        [string]$Pattern
    )
    if ([string]::IsNullOrWhiteSpace($Line)) {
        return $null
    }
    $match = [regex]::Match($Line, $Pattern)
    if (-not $match.Success) {
        return $null
    }
    return [double]::Parse($match.Groups[1].Value, $Culture)
}

function Get-Int {
    param(
        [string]$Line,
        [string]$Pattern
    )
    if ([string]::IsNullOrWhiteSpace($Line)) {
        return $null
    }
    $match = [regex]::Match($Line, $Pattern)
    if (-not $match.Success) {
        return $null
    }
    return [int]::Parse($match.Groups[1].Value, $Culture)
}

function Get-Text {
    param(
        [string]$Line,
        [string]$Pattern
    )
    if ([string]::IsNullOrWhiteSpace($Line)) {
        return $null
    }
    $match = [regex]::Match($Line, $Pattern)
    if (-not $match.Success) {
        return $null
    }
    return $match.Groups[1].Value
}

function Get-SlashNumbers {
    param(
        [string]$Line,
        [string]$Prefix,
        [int]$Count
    )
    if ([string]::IsNullOrWhiteSpace($Line)) {
        return @()
    }
    $pattern = [regex]::Escape($Prefix) + "([0-9.\/]+)"
    $match = [regex]::Match($Line, $pattern)
    if (-not $match.Success) {
        return @()
    }
    $parts = $match.Groups[1].Value.Split("/")
    $values = @()
    for ($i = 0; $i -lt [Math]::Min($Count, $parts.Count); $i++) {
        $values += [double]::Parse($parts[$i], $Culture)
    }
    return $values
}

function Get-LayerRow {
    param(
        [string]$CaptureDir,
        [int]$Frame
    )
    $csv = Join-Path $CaptureDir "layer_screen_timeline.csv"
    if (-not (Test-Path $csv)) {
        return $null
    }
    $rows = Import-Csv $csv
    $exact = $rows | Where-Object { [int]$_.frame -eq $Frame } | Select-Object -First 1
    if ($exact) {
        return $exact
    }
    return $rows |
        Sort-Object { [Math]::Abs(([int]$_.frame) - $Frame) } |
        Select-Object -First 1
}

function New-ScenarioResult {
    param(
        [string]$Scenario,
        [string]$CaptureDir,
        [int]$Frame,
        [string]$Notes
    )

    $logPath = Join-Path $CaptureDir "venpod_runtime.log"
    if (-not (Test-Path $logPath)) {
        throw "Missing log for scenario '$Scenario': $logPath"
    }

    $lines = Get-Content $logPath
    $policyLine = Get-LogLine $lines "PERF_SPARSE_SURFACE_PROMOTION_POLICY frame=$Frame\b"
    $cameraLine = Get-LogLine $lines "PERF_CAMERA_EXPOSURE frame=$Frame\b"
    $perfLine = Get-LogLine $lines "PERF frame=$Frame\b"
    $frameEndLine = Get-LogLine $lines "PERF_FRAME_END frame=$Frame\b"
    $clipLine = Get-LogLine $lines "PERF_SPARSE_CLIPMAP frame=$Frame\b"
    $sparseLine = Get-LogLine $lines "PERF_SPARSE frame=$Frame\b"
    $surfaceLine = Get-LogLine $lines "PERF_SPARSE_SURFACE frame=$Frame\b"
    $hiddenLine = Get-LogLine $lines "PERF_SPARSE_HIDDEN_EXACT_MISS frame=$Frame\b"
    $ownershipLine = Get-LogLine $lines "PERF_RENDER_OWNERSHIP retireFrame=\d+ shaderFrame=$Frame\b"
    $compositionLine = Get-LogLine $lines "PERF_RENDER_COMPOSITION frame=$Frame\b"
    $layer = Get-LayerRow $CaptureDir $Frame

    $gpu = Get-SlashNumbers $perfLine "gpu=frame/upload/pre/surface/ray/overlay/ui:" 7
    $prepSplit = Get-SlashNumbers $perfLine "prepSplit=sched/input/sparse/coll:" 4
    $sparseSplit = Get-SlashNumbers $perfLine "sparseSplit=req/gen/clip/trim:" 4
    $gaps = Get-SlashNumbers $frameEndLine "gaps=postWait/prePhys/preRender/postRender:" 4
    $sparsePost = Get-SlashNumbers $frameEndLine "sparsePost=feedback/cmd/begin/midSnap/plan/upload/publish/midUpload/stats/surfExtract/surfPlan/surfSnap/surfStage/surfEmit:" 14
    $feedbackSplit = Get-SlashNumbers $frameEndLine "feedbackSplit=legacy/raycast/miss/brush/own/phys:" 6

    $bodyMs = Get-Number $frameEndLine "body=([0-9.]+)"
    $rawMs = Get-Number $frameEndLine "rawMs=([0-9.]+)"
    $presentMs = Get-Number $frameEndLine "present=([0-9.]+)"
    $prepMs = Get-Number $perfLine "prep=([0-9.]+)"
    $hiddenMs = Get-Number $hiddenLine " ms=([0-9.]+)"
    $clipPrepMs = Get-Number $clipLine "prep=([0-9.]+)"

    $sparseFeedbackMs = if ($sparsePost.Count -ge 1) { $sparsePost[0] } else { $null }
    $sparseUploadMs = if ($sparsePost.Count -ge 8) { $sparsePost[5] + $sparsePost[7] } else { $null }
    $surfaceExtractMs = if ($sparsePost.Count -ge 10) { $sparsePost[9] } else { $null }
    $surfaceStageMs = if ($sparsePost.Count -ge 13) { $sparsePost[12] } else { $null }
    $surfaceEmitMs = if ($sparsePost.Count -ge 14) { $sparsePost[13] } else { $null }
    $pageGenMs = if ($sparseSplit.Count -ge 2) { $sparseSplit[1] } else { $null }
    $sparseReqMs = if ($sparseSplit.Count -ge 1) { $sparseSplit[0] } else { $null }
    $sparseClipMs = if ($sparseSplit.Count -ge 3) { $sparseSplit[2] } else { $null }
    $gpuRayMs = if ($gpu.Count -ge 5) { $gpu[4] } else { $null }
    $gpuUploadMs = if ($gpu.Count -ge 2) { $gpu[1] } else { $null }
    $gpuSurfaceMs = if ($gpu.Count -ge 4) { $gpu[3] } else { $null }
    $gpuFrameMs = if ($gpu.Count -ge 1) { $gpu[0] } else { $null }
    $postWaitMs = if ($gaps.Count -ge 1) { $gaps[0] } else { $null }
    $presentOrWaitMs = $null
    if ($presentMs -ne $null -and $gaps.Count -ge 4) {
        $presentOrWaitMs = $presentMs + $gaps[0] + $gaps[1] + $gaps[2] + $gaps[3]
    }
    $readbackMs = $null
    if ($feedbackSplit.Count -ge 6 -and $sparseFeedbackMs -ne $null) {
        $readbackMs = $sparseFeedbackMs
        foreach ($value in $feedbackSplit) {
            $readbackMs += $value
        }
    }
    $uploadMs = $null
    if ($sparseUploadMs -ne $null -and $gpuUploadMs -ne $null) {
        $uploadMs = $sparseUploadMs + $gpuUploadMs
    }

    $midPct = $null
    if ($layer) {
        $midPct = [double]::Parse($layer.midVoxelScreenPct, $Culture)
    } elseif ($ownershipLine -and $compositionLine) {
        $midPixels = Get-Number $ownershipLine "midVoxel=([0-9.]+)"
        $screenPixels = Get-Number $compositionLine "screen=([0-9.]+)"
        if ($midPixels -ne $null -and $screenPixels -ne $null -and $screenPixels -gt 0.0) {
            $midPct = ($midPixels * 100.0) / $screenPixels
        }
    }

    $scenarioNotes = @($Notes)
    $policyReason = Get-Text $policyLine "reason=([^ ]+)"
    if ($policyReason) {
        $scenarioNotes += "promotionReason=$policyReason"
    }
    $demoteReason = Get-Text $policyLine "demoteReason=([^ ]+)"
    if ($demoteReason -and $demoteReason -ne "none") {
        $scenarioNotes += "demoteReason=$demoteReason"
    }
    if (Test-Path (Join-Path $CaptureDir "engine_frame_*.bmp")) {
        $scenarioNotes += "captureBmp"
    }
    if ($hiddenLine) {
        $scenarioNotes += ("hiddenExactMs={0}" -f $hiddenMs)
    }

    [pscustomobject]@{
        scenario = $Scenario
        frame = $Frame
        promoted = Get-Int $cameraLine "surfacePromoted=(\d+)"
        surfaceRasterMax = Get-Number $cameraLine "surfaceRasterMax=([0-9.]+)"
        midVoxelScreenPct = $midPct
        bodyMs = $bodyMs
        rawMs = $rawMs
        gpuRayMs = $gpuRayMs
        cpuUpdateMs = $prepMs
        sparseRepairMs = $hiddenMs
        surfaceExtractMs = $surfaceExtractMs
        pageGenMs = $pageGenMs
        uploadMs = $uploadMs
        readbackMs = $readbackMs
        presentOrWaitMs = $presentOrWaitMs
        hiddenExactAccepted = Get-Int $cameraLine "hiddenExactAccepted=(\d+)"
        hiddenExactMissing = Get-Int $cameraLine "hiddenExactMissing=(\d+)/"
        notes = ($scenarioNotes -join "; ")
        gpuFrameMs = $gpuFrameMs
        gpuUploadMs = $gpuUploadMs
        gpuSurfaceMs = $gpuSurfaceMs
        sparseReqMs = $sparseReqMs
        sparseClipMs = $sparseClipMs
        sparseFeedbackMs = $sparseFeedbackMs
        surfaceStageMs = $surfaceStageMs
        surfaceEmitMs = $surfaceEmitMs
        postWaitMs = $postWaitMs
        clipPrepMs = $clipPrepMs
        clipPumpVoxelMs = Get-Number $clipLine "pumpVoxel=([0-9.]+)"
        genQueued = Get-Int $sparseLine "genQueued=(\d+)"
        pgen = Get-Text $sparseLine "pgen=([^ ]+)"
        uploadMB = Get-Number $sparseLine "uploadMB=([0-9.]+)"
        midVoxelUpload = Get-Int $sparseLine "midVoxelUpload=(\d+)"
        brushEval = Get-Int $sparseLine "brushEval=(\d+)"
        brushEdit = Get-Int $sparseLine "brushEdit=(\d+)"
        brushUploads = Get-Int $sparseLine "brushUploads=(\d+)"
        brushGpuFb = Get-Text $sparseLine "brushGpuFb=([^ ]+)"
        surfacePendingDirty = Get-Int $surfaceLine "pendingDirty=(\d+)"
        surfaceCopyRegions = Get-Int $surfaceLine "copyRegions=(\d+)"
        surfaceStagedMB = Get-Number $surfaceLine "stagedMB=([0-9.]+)"
        surfaceRasterFaces = Get-Int $surfaceLine "rasterFaces=(\d+)"
        untrackedMs = Get-Number $perfLine "untracked=([0-9.]+)"
    }
}

function ConvertTo-MarkdownTable {
    param([object[]]$Rows)
    $columns = @(
        "scenario",
        "frame",
        "promoted",
        "surfaceRasterMax",
        "midVoxelScreenPct",
        "bodyMs",
        "rawMs",
        "gpuRayMs",
        "cpuUpdateMs",
        "sparseRepairMs",
        "surfaceExtractMs",
        "pageGenMs",
        "uploadMs",
        "readbackMs",
        "presentOrWaitMs",
        "hiddenExactAccepted",
        "hiddenExactMissing",
        "notes"
    )
    $lines = @()
    $lines += "|" + ($columns -join "|") + "|"
    $lines += "|" + (($columns | ForEach-Object { "---" }) -join "|") + "|"
    foreach ($row in $Rows) {
        $values = foreach ($column in $columns) {
            $value = $row.$column
            if ($value -eq $null) {
                ""
            } elseif ($value -is [double]) {
                $value.ToString("0.##", $Culture)
            } else {
                ($value.ToString() -replace "\|", "/")
            }
        }
        $lines += "|" + ($values -join "|") + "|"
    }
    return $lines
}

$scenarios = @(
    @{ Scenario = "A default strict fixed"; Dir = "build\captures\contract_policy_default_off_20260602"; Frame = 240; Notes = "strict default-off capture" },
    @{ Scenario = "B bounded64 fixed"; Dir = "build\captures\contract_policy_bounded64_fixed_final_20260602"; Frame = 240; Notes = "bounded_repair bound64 capture" },
    @{ Scenario = "C bounded64 walk"; Dir = "build\captures\contract_policy_bounded64_walk_20260602"; Frame = 480; Notes = "movement capture" },
    @{ Scenario = "D high-alt excluded"; Dir = "build\captures\contract_policy_bounded64_highalt_20260602"; Frame = 240; Notes = "high_alt_excluded capture" }
)

$editDir = "build\captures\perf_edit_brush_paint_20260602"
if (Test-Path (Join-Path $editDir "venpod_runtime.log")) {
    $scenarios += @{ Scenario = "E edit brush paint"; Dir = $editDir; Frame = 240; Notes = "brush paint edit capture" }
}
$nonCaptureDir = "build\captures\perf_non_capture_default_strict_nophys_20260602"
if (Test-Path (Join-Path $nonCaptureDir "venpod_runtime.log")) {
    $scenarios += @{ Scenario = "F non-capture strict"; Dir = $nonCaptureDir; Frame = 240; Notes = "no capture, physics disabled" }
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$rows = foreach ($scenario in $scenarios) {
    New-ScenarioResult -Scenario $scenario.Scenario -CaptureDir $scenario.Dir -Frame $scenario.Frame -Notes $scenario.Notes
}

$csvPath = Join-Path $OutputDir "perf_cost_table.csv"
$mdPath = Join-Path $OutputDir "perf_cost_table.md"
$rows | Export-Csv -NoTypeInformation -Path $csvPath

$mdLines = @(
    "# VENPOD Performance Root-Cause Cost Table",
    "",
    "Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')",
    "",
    "Mapping notes:",
    "",
    '- `cpuUpdateMs` is `PERF prep`.',
    '- `gpuRayMs` is the `PERF gpu=.../ray` slot.',
    '- `sparseRepairMs` is `PERF_SPARSE_HIDDEN_EXACT_MISS ms`; repair generation/upload/publish work also appears in generation, upload, surface extraction, and publish counters.',
    '- `surfaceExtractMs` is `PERF_FRAME_END sparsePost.surfExtract`.',
    '- `pageGenMs` is `PERF sparseSplit.gen`.',
    '- `uploadMs` is CPU sparse upload plus GPU upload timing where both are available.',
    '- `readbackMs` is sparse feedback processing plus feedback split subfields; this is a lower-bound for sync/readback cost.',
    '- `presentOrWaitMs` is present plus post-wait and frame-end gaps.',
    "",
    (ConvertTo-MarkdownTable $rows)
)
$mdLines | Set-Content -Path $mdPath -Encoding UTF8

$editLatencyPath = Join-Path $OutputDir "edit_latency_events.txt"
$editLog = "build\captures\perf_edit_brush_paint_20260602\venpod_runtime.log"
if (Test-Path $editLog) {
    Select-String -Path $editLog -Pattern "PERF_SPARSE_EDIT_LATENCY|SPARSE_BRUSH_FEEDBACK GPU apply|PERF_SPARSE_EDIT_PUBLISH|SPARSE_BRUSH_PAINT_SMOKE" |
        ForEach-Object { $_.Line } |
        Set-Content -Path $editLatencyPath -Encoding UTF8
}

Write-Host "Wrote $csvPath"
Write-Host "Wrote $mdPath"
if (Test-Path $editLatencyPath) {
    Write-Host "Wrote $editLatencyPath"
}
