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
    [switch]$SkipEngineCaptureSmoke,
    [int]$RenderExitAfterFrames = 240,
    [int]$PhysicsExitAfterFrames = 240,
    [int]$FlickerExitAfterFrames = 180,
    [int]$SurfaceExitAfterFrames = 240,
    [int]$GpuRaycastExitAfterFrames = 300,
    [int]$MissFeedbackExitAfterFrames = 240,
    [int]$BrushFeedbackExitAfterFrames = 360,
    [int]$BrushFeedbackApplyExitAfterFrames = 360,
    [int]$BrushFeedbackAuthoritativeExitAfterFrames = 390,
    [int]$EngineCaptureExitAfterFrames = 245,
    [int]$EngineCaptureStartFrame = 120,
    [int]$EngineCaptureIntervalFrames = 20,
    [int]$EngineCaptureCount = 6
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
        -Pattern "PERF_BACKEND_PIPE|PERF_RENDER_OWNERSHIP|PERF_SPARSE_OWNERSHIP_PRESSURE|PERF_SPARSE_SURFACE|PERF_SPARSE_PHYSICS_GPU_RESULT|PERF_SPARSE_GPU_RAYCAST|farCov=|look=|missPending=|missRetired=|missConsumed=|brushGpuFb=|brushGpuFbMiss=|brushGpuFbFallback=|Sparse brush feedback diagnostic queued|SPARSE_BRUSH_FEEDBACK parity observed|SPARSE_BRUSH_FEEDBACK parity failed|SPARSE_BRUSH_FEEDBACK diagnostic suite passed|SPARSE_BRUSH_FEEDBACK GPU apply|SPARSE_BRUSH_FEEDBACK CPU fallback|Sparse surface diagnostic seed queued|SPARSE_SURFACE_FRAGMENTS failed|SPARSE_GPU_RAYCAST health observed|SPARSE_GPU_RAYCAST health failed|SPARSE_BACKEND_PIPE readiness failed|SPARSE_RENDER_OWNERSHIP quality failed|SPARSE_RENDER_OWNERSHIP stability failed|\] \[(critical|error)\]|device removed|device-removed|timeout" `
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

function Assert-MissFeedbackPressureFromLog {
    param(
        [string]$Label,
        [string]$SavedLog
    )

    if (-not $SavedLog -or -not (Test-Path $SavedLog)) {
        throw "$Label miss-feedback pressure check has no runtime log"
    }

    $observed = $false
    Select-String -Path $SavedLog -Pattern "PERF_SPARSE_OWNERSHIP_PRESSURE.*effectiveLevel=([0-9]+).*pendingMiss=([0-9]+)" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "effectiveLevel=([0-9]+).*pendingMiss=([0-9]+)") {
                $effectiveLevel = [int]$Matches[1]
                $pendingMiss = [int]$Matches[2]
                if ($effectiveLevel -gt 0 -and $pendingMiss -gt 0) {
                    $observed = $true
                }
            }
        }

    if (-not $observed) {
        throw "$Label did not show pending miss feedback producing nonzero effective ownership pressure"
    }

    Write-Info "$Label miss-feedback pressure response observed"
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

    Select-String -Path $SavedLog -Pattern "PERF_BACKEND_PIPE.*far=1|farCov=([0-9.]+)/([0-9.]+)" |
        ForEach-Object {
            if ($_.Line -match "PERF_BACKEND_PIPE.*far=1") {
                $farActive = $true
            }
            if ($_.Line -match "farCov=([0-9.]+)/([0-9.]+)") {
                $uploadCoverage = [double]::Parse($Matches[1], $culture)
                $pageCoverage = [double]::Parse($Matches[2], $culture)
                $maxUploadCoverage = [Math]::Max($maxUploadCoverage, $uploadCoverage)
                $maxPageCoverage = [Math]::Max($maxPageCoverage, $pageCoverage)
                if ($uploadCoverage -ge 0.999 -and $pageCoverage -gt 0.0) {
                    $farCoverageReady = $true
                }
            }
        }

    if (-not $farActive -or -not $farCoverageReady) {
        throw "$Label far SVO did not become resident/active (active=$farActive maxCov=$('{0:F2}/{1:F2}' -f $maxUploadCoverage, $maxPageCoverage))"
    }

    Write-Info "$Label far SVO readiness observed: active=$farActive maxCov=$('{0:F2}/{1:F2}' -f $maxUploadCoverage, $maxPageCoverage)"
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

$projectRoot = $PSScriptRoot
$buildScript = Join-Path $projectRoot "build.ps1"
$rebrunScript = Join-Path $projectRoot "rebrun.ps1"
$engineCaptureScript = Join-Path $projectRoot "engine_capture_smoke.ps1"
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

Write-Host "VENPOD - Sparse Regression Gate" -ForegroundColor Magenta
Write-Info "Config: $Config"
Write-Info "Render smoke frames: $RenderExitAfterFrames"
Write-Info "Physics smoke frames: $PhysicsExitAfterFrames"
if (-not $SkipFlickerSmoke) {
    Write-Info "Flicker smoke frames: $FlickerExitAfterFrames"
}
if (-not $SkipSurfaceSmoke) {
    Write-Info "Surface smoke frames: $SurfaceExitAfterFrames"
    Write-Info "Surface smoke sparse edit persistence: enabled"
}
if (-not $SkipGpuRaycastSmoke) {
    Write-Info "GPU raycast smoke frames: $GpuRaycastExitAfterFrames"
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
if (-not $SkipEngineCaptureSmoke) {
    Write-Info "Engine capture smoke frames: $EngineCaptureExitAfterFrames"
    Write-Info "Engine capture samples: count=$EngineCaptureCount start=$EngineCaptureStartFrame interval=$EngineCaptureIntervalFrames"
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

Write-Step "Running sparse render/backend smoke..."
if ($ForceSync) {
    & $rebrunScript -Config $Config -NoBuild -SparseSmoke -ExitAfterFrames $RenderExitAfterFrames -ForceSync
} else {
    & $rebrunScript -Config $Config -NoBuild -SparseSmoke -ExitAfterFrames $RenderExitAfterFrames
}
Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse render smoke"
$renderSmokeLog = Save-AndSummarizeRuntimeLog -Label "Sparse render smoke" -FileStem "sparse_render_smoke"
Assert-FarSvoReadyFromLog -Label "Sparse render smoke" -SavedLog $renderSmokeLog
Assert-SurfaceLookaheadTelemetryFromLog -Label "Sparse render smoke" -SavedLog $renderSmokeLog
Assert-OwnershipPressureTelemetryFromLog -Label "Sparse render smoke" -SavedLog $renderSmokeLog

if (-not $SkipFlickerSmoke) {
    Write-Step "Running sparse every-frame flicker smoke..."
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparseFlickerSmoke -ExitAfterFrames $FlickerExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparseFlickerSmoke -ExitAfterFrames $FlickerExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse flicker smoke"
    $null = Save-AndSummarizeRuntimeLog -Label "Sparse flicker smoke" -FileStem "sparse_flicker_smoke"
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
    $null = Save-AndSummarizeRuntimeLog -Label "Sparse seeded-surface smoke" -FileStem "sparse_surface_smoke"
    Assert-SparseEditPersistenceFile -Path $editPersistencePath
} else {
    Write-Info "Sparse seeded-surface smoke: skipped (-SkipSurfaceSmoke)"
}

if (-not $SkipGpuRaycastSmoke) {
    Write-Step "Running sparse GPU-raycast smoke..."
    if ($ForceSync) {
        & $rebrunScript -Config $Config -NoBuild -SparseGpuRaycastSmoke -ExitAfterFrames $GpuRaycastExitAfterFrames -ForceSync
    } else {
        & $rebrunScript -Config $Config -NoBuild -SparseGpuRaycastSmoke -ExitAfterFrames $GpuRaycastExitAfterFrames
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse GPU-raycast smoke"
    $null = Save-AndSummarizeRuntimeLog -Label "Sparse GPU-raycast smoke" -FileStem "sparse_gpu_raycast_smoke"
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
    $null = Save-AndSummarizeRuntimeLog -Label "Sparse brush-feedback smoke" -FileStem "sparse_brush_feedback_smoke"
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
    $null = Save-AndSummarizeRuntimeLog -Label "Sparse brush-feedback apply smoke" -FileStem "sparse_brush_feedback_apply_smoke"
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
    $null = Save-AndSummarizeRuntimeLog -Label "Sparse brush-feedback authoritative smoke" -FileStem "sparse_brush_feedback_authoritative_smoke"
} else {
    Write-Info "Sparse brush-feedback authoritative smoke: skipped (-SkipBrushFeedbackAuthoritativeSmoke)"
}

Write-Step "Running sparse GPU-physics smoke..."
if ($ForceSync) {
    & $rebrunScript -Config $Config -NoBuild -SparsePhysicsSmoke -ExitAfterFrames $PhysicsExitAfterFrames -ForceSync
} else {
    & $rebrunScript -Config $Config -NoBuild -SparsePhysicsSmoke -ExitAfterFrames $PhysicsExitAfterFrames
}
Stop-OnFailure -Code $LASTEXITCODE -Stage "Sparse GPU-physics smoke"
$null = Save-AndSummarizeRuntimeLog -Label "Sparse GPU-physics smoke" -FileStem "sparse_physics_smoke"

if (-not $SkipEngineCaptureSmoke) {
    Write-Step "Running sparse engine backbuffer capture smoke..."
    $captureOut = Join-Path $buildDir "logs\sparse_engine_capture"
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
} else {
    Write-Info "Sparse engine capture smoke: skipped (-SkipEngineCaptureSmoke)"
}

Write-Success "Sparse regression gate passed."
