# =============================================================================
# VENPOD - Engine Backbuffer Capture Smoke
# Uses VENPOD's in-engine DX12 readback path to capture actual rendered frames.
# =============================================================================

param(
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$NoBuild,
    [int]$ExitAfterFrames = 240,
    [int]$CaptureStartFrame = 120,
    [int]$CaptureIntervalFrames = 20,
    [int]$CaptureCount = 8,
    [int]$SparseDebugMode = 0,
    [switch]$StressCamera,
    [int]$StressCameraRadius = 900,
    [int]$StressCameraHeight = 180,
    [int]$StressCameraBaseHeight = 520,
    [int]$StressCameraSpeed = 50,
    [switch]$BoundaryTest,
    [switch]$ShowDiagnostics,
    [switch]$SparsePhysics,
    [switch]$SparseGpuPhysics,
    [switch]$SparsePhysicsDiagnosticSeed,
    [switch]$SparsePhysicsDiagnosticFluidSeed,
    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"

function Write-Step { Write-Host "`n==> $args" -ForegroundColor Cyan }
function Write-Success { Write-Host "[OK] $args" -ForegroundColor Green }
function Write-Info { Write-Host "  $args" -ForegroundColor Gray }
function Stop-OnFailure {
    param([int]$Code, [string]$Stage)
    if ($Code -ne 0) {
        Write-Host "[ERROR] $Stage failed with exit code $Code" -ForegroundColor Red
        exit $Code
    }
}

function Write-ImageArtifacts {
    param(
        [string[]]$Frames,
        [string]$OutDir
    )

    Add-Type -AssemblyName System.Drawing
    $stats = New-Object System.Collections.Generic.List[string]
    $stats.Add("file,width,height,sampled,skyLikePct,darkPct,terrainLikePct,uniqueSampleColors")

    foreach ($path in $Frames) {
        $bitmap = [System.Drawing.Bitmap]::FromFile($path)
        try {
            $width = $bitmap.Width
            $height = $bitmap.Height
            $sampled = 0
            $skyLike = 0
            $dark = 0
            $terrainLike = 0
            $colors = New-Object 'System.Collections.Generic.HashSet[string]'
            $stepX = [Math]::Max(1, [Math]::Floor($width / 96))
            $stepY = [Math]::Max(1, [Math]::Floor($height / 54))
            for ($y = 0; $y -lt $height; $y += $stepY) {
                for ($x = 0; $x -lt $width; $x += $stepX) {
                    $c = $bitmap.GetPixel($x, $y)
                    ++$sampled
                    $colors.Add("$($c.R),$($c.G),$($c.B)") | Out-Null
                    $isSky = $c.R -ge 120 -and $c.R -le 190 -and $c.G -ge 155 -and $c.G -le 220 -and $c.B -ge 190
                    if ($isSky) { ++$skyLike }
                    if (($c.R + $c.G + $c.B) -lt 90) { ++$dark }
                    if (($c.R + $c.G + $c.B) -ge 90 -and -not $isSky) { ++$terrainLike }
                }
            }
            $skyPct = [Math]::Round(($skyLike * 100.0) / [Math]::Max(1, $sampled), 2)
            $darkPct = [Math]::Round(($dark * 100.0) / [Math]::Max(1, $sampled), 2)
            $terrainPct = [Math]::Round(($terrainLike * 100.0) / [Math]::Max(1, $sampled), 2)
            $stats.Add(("{0},{1},{2},{3},{4},{5},{6},{7}" -f (Split-Path $path -Leaf), $width, $height, $sampled, $skyPct, $darkPct, $terrainPct, $colors.Count))
        } finally {
            $bitmap.Dispose()
        }
    }

    $thumbW = 320
    $thumbH = 180
    $columns = 1
    $rows = $Frames.Count
    $bestWaste = [int]::MaxValue
    for ($candidateColumns = 1; $candidateColumns -le [Math]::Min(4, [Math]::Max(1, $Frames.Count)); ++$candidateColumns) {
        $candidateRows = [Math]::Ceiling($Frames.Count / [double]$candidateColumns)
        $waste = ($candidateRows * $candidateColumns) - $Frames.Count
        if ($waste -lt $bestWaste -or ($waste -eq $bestWaste -and $candidateColumns -gt $columns)) {
            $columns = $candidateColumns
            $rows = $candidateRows
            $bestWaste = $waste
        }
    }
    $sheet = New-Object System.Drawing.Bitmap($($thumbW * $columns), $($thumbH * $rows), [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($sheet)
    $g.Clear([System.Drawing.Color]::Black)
    for ($i = 0; $i -lt $Frames.Count; ++$i) {
        $img = [System.Drawing.Image]::FromFile($Frames[$i])
        try {
            $x = ($i % $columns) * $thumbW
            $y = [Math]::Floor($i / $columns) * $thumbH
            $g.DrawImage($img, $x, $y, $thumbW, $thumbH)
        } finally {
            $img.Dispose()
        }
    }
    $g.Dispose()
    $contactSheet = Join-Path $OutDir "contact_sheet.png"
    $sheet.Save($contactSheet, [System.Drawing.Imaging.ImageFormat]::Png)
    $sheet.Dispose()

    $statsPath = Join-Path $OutDir "image_stats.csv"
    $stats | Set-Content -Path $statsPath -Encoding ASCII
    return @{ ContactSheet = $contactSheet; Stats = $statsPath }
}

function Test-ImageStats {
    param(
        [string]$StatsPath,
        [switch]$StressCamera
    )

    if (-not (Test-Path $StatsPath)) {
        Write-Host "[ERROR] Image stats not found at $StatsPath" -ForegroundColor Red
        exit 15
    }

    $rows = Import-Csv -Path $StatsPath
    if (-not $rows -or $rows.Count -eq 0) {
        Write-Host "[ERROR] Image stats are empty" -ForegroundColor Red
        exit 15
    }

    $darkSum = 0.0
    $terrainSum = 0.0
    $minUnique = [int]::MaxValue
    $badRows = New-Object System.Collections.Generic.List[string]
    foreach ($row in $rows) {
        $dark = [double]$row.darkPct
        $terrain = [double]$row.terrainLikePct
        $unique = [int]$row.uniqueSampleColors
        $darkSum += $dark
        $terrainSum += $terrain
        $minUnique = [Math]::Min($minUnique, $unique)
        if ($unique -lt 120) {
            $badRows.Add("$($row.file): uniqueSampleColors=$unique") | Out-Null
        }
        if ($terrain -lt 25.0) {
            $badRows.Add("$($row.file): terrainLikePct=$terrain") | Out-Null
        }
    }

    $avgDark = $darkSum / [Math]::Max(1, $rows.Count)
    $avgTerrain = $terrainSum / [Math]::Max(1, $rows.Count)
    $maxAvgDark = 48.0
    if ($StressCamera) {
        $maxAvgDark = 12.0
    }
    if ($avgDark -gt $maxAvgDark) {
        $badRows.Add("average darkPct=$([Math]::Round($avgDark, 2)) max=$maxAvgDark") | Out-Null
    }
    if ($avgTerrain -lt 35.0) {
        $badRows.Add("average terrainLikePct=$([Math]::Round($avgTerrain, 2)) min=35") | Out-Null
    }

    if ($badRows.Count -gt 0) {
        Write-Host "[ERROR] Engine capture smoke found visual regression markers:" -ForegroundColor Red
        $badRows | Select-Object -First 20 | ForEach-Object {
            Write-Host "  $_" -ForegroundColor Red
        }
        exit 15
    }
}

function Test-SparseSurfaceRuntimeStats {
    param(
        [string]$LogPath
    )

    if (-not (Test-Path $LogPath)) {
        Write-Host "[ERROR] Runtime log not found at $LogPath" -ForegroundColor Red
        exit 16
    }

    $initLine = Select-String -Path $LogPath -Pattern "Sparse surface GPU buffers initialized" | Select-Object -Last 1
    if (-not $initLine -or
        $initLine.Line -notlike "*rangeAllocator=enabled*" -or
        $initLine.Line -notlike "*gpuCull=enabled*" -or
        $initLine.Line -notlike "*stableDrawSlots=enabled*" -or
        $initLine.Line -notlike "*compactStableDraws=enabled*") {
        Write-Host "[ERROR] Sparse surface GPU buffers did not initialize with the default optimized path" -ForegroundColor Red
        if ($initLine) {
            Write-Host "  $($initLine.Line)" -ForegroundColor Red
        }
        exit 16
    }

    $surfaceLines = Select-String -Path $LogPath -Pattern "PERF_SPARSE_SURFACE frame="
    if (-not $surfaceLines) {
        Write-Host "[ERROR] No PERF_SPARSE_SURFACE lines found in runtime log" -ForegroundColor Red
        exit 16
    }

    $validated = 0
    $badRows = New-Object System.Collections.Generic.List[string]
    foreach ($match in $surfaceLines) {
        $line = $match.Line
        $frameMatch = [regex]::Match($line, "frame=(\d+)")
        if (-not $frameMatch.Success) {
            continue
        }
        $frame = [int]$frameMatch.Groups[1].Value
        if ($frame -lt 120) {
            continue
        }
        ++$validated
        foreach ($required in @("gpuCull=1", "gpuCullDispatch=1", "stableDraw=1", "compactDraw=1")) {
            if ($line -notlike "*$required*") {
                $badRows.Add("frame ${frame}: missing $required") | Out-Null
            }
        }
        $overflowMatch = [regex]::Match($line, "gpuCullOverflow=(\d+)")
        if ($overflowMatch.Success -and [int]$overflowMatch.Groups[1].Value -ne 0) {
            $badRows.Add("frame ${frame}: gpuCullOverflow=$($overflowMatch.Groups[1].Value)") | Out-Null
        }
        $allocFailMatch = [regex]::Match($line, "allocFail=(\d+)")
        if ($allocFailMatch.Success -and [int]$allocFailMatch.Groups[1].Value -ne 0) {
            $badRows.Add("frame ${frame}: allocFail=$($allocFailMatch.Groups[1].Value)") | Out-Null
        }
        $retryMatch = [regex]::Match($line, "retry=(\d+)")
        if ($retryMatch.Success -and [int]$retryMatch.Groups[1].Value -gt 2) {
            $badRows.Add("frame ${frame}: retry=$($retryMatch.Groups[1].Value)") | Out-Null
        }
        $overflowFlagMatch = [regex]::Match($line, "overflow=(\d+)")
        if ($overflowFlagMatch.Success -and [int]$overflowFlagMatch.Groups[1].Value -ne 0) {
            $badRows.Add("frame ${frame}: upload overflow=$($overflowFlagMatch.Groups[1].Value)") | Out-Null
        }
    }

    if ($validated -eq 0) {
        Write-Host "[ERROR] No PERF_SPARSE_SURFACE lines at or after frame 120" -ForegroundColor Red
        exit 16
    }
    if ($badRows.Count -gt 0) {
        Write-Host "[ERROR] Sparse surface runtime contract failed:" -ForegroundColor Red
        $badRows | Select-Object -First 20 | ForEach-Object {
            Write-Host "  $_" -ForegroundColor Red
        }
        exit 16
    }

    $sparseLines = Select-String -Path $LogPath -Pattern "PERF_SPARSE frame="
    if (-not $sparseLines) {
        Write-Host "[ERROR] No PERF_SPARSE lines found in runtime log" -ForegroundColor Red
        exit 16
    }

    $validatedSparse = 0
    $badSparseRows = New-Object System.Collections.Generic.List[string]
    foreach ($match in $sparseLines) {
        $line = $match.Line
        $frameMatch = [regex]::Match($line, "frame=(\d+)")
        if (-not $frameMatch.Success) {
            continue
        }
        $frame = [int]$frameMatch.Groups[1].Value
        if ($frame -lt 120) {
            continue
        }
        ++$validatedSparse
        if ($line -notlike "*midClip=1*") {
            $badSparseRows.Add("frame ${frame}: midClip not enabled") | Out-Null
        }
        $midSerialMatch = [regex]::Match($line, "midSerial=(\d+)")
        if (-not $midSerialMatch.Success -or [int]$midSerialMatch.Groups[1].Value -le 0) {
            $badSparseRows.Add("frame ${frame}: midSerial missing or zero") | Out-Null
        }
        $midVoxelMatch = [regex]::Match($line, "midVoxels=(\d+)/(\d+)")
        if (-not $midVoxelMatch.Success -or [int]$midVoxelMatch.Groups[1].Value -le 0) {
            $badSparseRows.Add("frame ${frame}: mid voxel clipmap has no resident bricks") | Out-Null
        }
        $midRetryMatch = [regex]::Match($line, "midRetry=(\d+)")
        if ($midRetryMatch.Success -and [int]$midRetryMatch.Groups[1].Value -gt 2) {
            $badSparseRows.Add("frame ${frame}: midRetry=$($midRetryMatch.Groups[1].Value)") | Out-Null
        }
        $uploadDefersMatch = [regex]::Match($line, "uploadByteDefers=(\d+)")
        if ($uploadDefersMatch.Success -and [int]$uploadDefersMatch.Groups[1].Value -gt 2) {
            $badSparseRows.Add("frame ${frame}: uploadByteDefers=$($uploadDefersMatch.Groups[1].Value)") | Out-Null
        }
    }

    if ($validatedSparse -eq 0) {
        Write-Host "[ERROR] No PERF_SPARSE lines at or after frame 120" -ForegroundColor Red
        exit 16
    }
    if ($badSparseRows.Count -gt 0) {
        Write-Host "[ERROR] Sparse hierarchy runtime contract failed:" -ForegroundColor Red
        $badSparseRows | Select-Object -First 20 | ForEach-Object {
            Write-Host "  $_" -ForegroundColor Red
        }
        exit 16
    }
}

function Test-SparsePhysicsRuntimeStats {
    param(
        [string]$LogPath,
        [switch]$RequireMovement,
        [switch]$RequireGpuPackets
    )

    if (-not (Test-Path $LogPath)) {
        Write-Host "[ERROR] Runtime log not found at $LogPath" -ForegroundColor Red
        exit 17
    }

    $initLine = Select-String -Path $LogPath -Pattern "Sparse local physics: enabled" | Select-Object -Last 1
    if (-not $initLine) {
        Write-Host "[ERROR] Sparse local physics did not enable" -ForegroundColor Red
        exit 17
    }

    $physicsLines = Select-String -Path $LogPath -Pattern "PERF_SPARSE_PHYSICS frame="
    if (-not $physicsLines) {
        Write-Host "[ERROR] No PERF_SPARSE_PHYSICS lines found in runtime log" -ForegroundColor Red
        exit 17
    }

    $validated = 0
    $totalMoved = 0
    $totalGpuPackets = 0
    $badRows = New-Object System.Collections.Generic.List[string]
    foreach ($match in $physicsLines) {
        $line = $match.Line
        $enabledMatch = [regex]::Match($line, "enabled=(\d+)")
        if (-not $enabledMatch.Success -or [int]$enabledMatch.Groups[1].Value -ne 1) {
            $badRows.Add("physics log line is not enabled: $line") | Out-Null
            continue
        }

        $frameMatch = [regex]::Match($line, "frame=(\d+)")
        $packetMatch = [regex]::Match($line, "packets=(\d+)")
        $gpuPacketMatch = [regex]::Match($line, "gpuPackets=(\d+)")
        $movedMatch = [regex]::Match($line, " moved=(\d+)")
        $budgetMatch = [regex]::Match($line, "budget=(\d+)/(\d+)")
        if (-not $frameMatch.Success -or -not $packetMatch.Success -or -not $movedMatch.Success -or -not $budgetMatch.Success) {
            $badRows.Add("malformed physics log line: $line") | Out-Null
            continue
        }

        ++$validated
        $totalMoved += [int]$movedMatch.Groups[1].Value
        if ($gpuPacketMatch.Success) {
            $totalGpuPackets += [int]$gpuPacketMatch.Groups[1].Value
        }
        if ([int]$budgetMatch.Groups[1].Value -le 0 -or [int]$budgetMatch.Groups[2].Value -le 0) {
            $badRows.Add("frame $($frameMatch.Groups[1].Value): invalid physics budget") | Out-Null
        }
    }

    if ($validated -eq 0) {
        Write-Host "[ERROR] No valid PERF_SPARSE_PHYSICS rows found" -ForegroundColor Red
        exit 17
    }
    if ($RequireMovement -and $totalMoved -eq 0) {
        Write-Host "[ERROR] Sparse physics diagnostic seed produced no voxel movement" -ForegroundColor Red
        exit 17
    }
    if ($RequireGpuPackets -and $totalGpuPackets -eq 0) {
        Write-Host "[ERROR] Sparse GPU physics packet path produced no packet uploads" -ForegroundColor Red
        exit 17
    }
    if ($badRows.Count -gt 0) {
        Write-Host "[ERROR] Sparse physics runtime contract failed:" -ForegroundColor Red
        $badRows | Select-Object -First 20 | ForEach-Object {
            Write-Host "  $_" -ForegroundColor Red
        }
        exit 17
    }
}

$projectRoot = $PSScriptRoot
$buildScript = Join-Path $projectRoot "build.ps1"
$runScript = Join-Path $projectRoot "run.ps1"
$buildDir = Join-Path $projectRoot "build"

if (-not (Test-Path $buildScript)) { throw "build.ps1 not found at $buildScript" }
if (-not (Test-Path $runScript)) { throw "run.ps1 not found at $runScript" }

if (-not $NoBuild) {
    Write-Step "Building latest code..."
    if ($Clean) { & $buildScript -Config $Config -Clean } else { & $buildScript -Config $Config }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Build"
} else {
    Write-Info "Build step: skipped (-NoBuild)"
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss-fff"
    $OutputDir = Join-Path $buildDir "captures\engine_sparse_$stamp"
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).ProviderPath

$minExitAfterFrames = $CaptureStartFrame + ($CaptureIntervalFrames * [Math]::Max(0, $CaptureCount - 1)) + 5
if ($ExitAfterFrames -lt $minExitAfterFrames) {
    Write-Info "ExitAfterFrames=$ExitAfterFrames is too short for $CaptureCount captures; raising to $minExitAfterFrames"
    $ExitAfterFrames = $minExitAfterFrames
}

$savedEnv = @{
    VENPOD_LOG_FILE = $env:VENPOD_LOG_FILE
    VENPOD_DIAGNOSTICS = $env:VENPOD_DIAGNOSTICS
    VENPOD_MODE = $env:VENPOD_MODE
    VENPOD_DISABLE_PHYSICS = $env:VENPOD_DISABLE_PHYSICS
    VENPOD_ENABLE_EXPERIMENTAL_SPARSE = $env:VENPOD_ENABLE_EXPERIMENTAL_SPARSE
    VENPOD_RENDER_BACKEND = $env:VENPOD_RENDER_BACKEND
    VENPOD_SPARSE_RAYMARCH = $env:VENPOD_SPARSE_RAYMARCH
    VENPOD_SPARSE_ONLY = $env:VENPOD_SPARSE_ONLY
    VENPOD_SPARSE_SURFACE_AUTHORITATIVE = $env:VENPOD_SPARSE_SURFACE_AUTHORITATIVE
    VENPOD_SPARSE_DEBUG_MODE = $env:VENPOD_SPARSE_DEBUG_MODE
    VENPOD_ENABLE_TEST_MODES = $env:VENPOD_ENABLE_TEST_MODES
    VENPOD_BOUNDARY_TEST = $env:VENPOD_BOUNDARY_TEST
    VENPOD_SPARSE_STRESS_REQUESTS = $env:VENPOD_SPARSE_STRESS_REQUESTS
    VENPOD_SPARSE_STRESS_CAMERA = $env:VENPOD_SPARSE_STRESS_CAMERA
    VENPOD_SPARSE_STRESS_CAMERA_RADIUS = $env:VENPOD_SPARSE_STRESS_CAMERA_RADIUS
    VENPOD_SPARSE_STRESS_CAMERA_HEIGHT = $env:VENPOD_SPARSE_STRESS_CAMERA_HEIGHT
    VENPOD_SPARSE_STRESS_CAMERA_BASE_HEIGHT = $env:VENPOD_SPARSE_STRESS_CAMERA_BASE_HEIGHT
    VENPOD_SPARSE_STRESS_CAMERA_SPEED = $env:VENPOD_SPARSE_STRESS_CAMERA_SPEED
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
    VENPOD_ENABLE_SPARSE_PHYSICS = $env:VENPOD_ENABLE_SPARSE_PHYSICS
    VENPOD_SPARSE_PHYSICS_GPU = $env:VENPOD_SPARSE_PHYSICS_GPU
    VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_SEED = $env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_SEED
    VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_FLUID_SEED = $env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_FLUID_SEED
    VENPOD_CAPTURE_DIR = $env:VENPOD_CAPTURE_DIR
    VENPOD_CAPTURE_START_FRAME = $env:VENPOD_CAPTURE_START_FRAME
    VENPOD_CAPTURE_INTERVAL_FRAMES = $env:VENPOD_CAPTURE_INTERVAL_FRAMES
    VENPOD_CAPTURE_COUNT = $env:VENPOD_CAPTURE_COUNT
    VENPOD_CAPTURE_HIDE_UI = $env:VENPOD_CAPTURE_HIDE_UI
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

try {
    $env:VENPOD_LOG_FILE = "1"
    if ($ShowDiagnostics) {
        $env:VENPOD_DIAGNOSTICS = "1"
    } else {
        Remove-Item "env:VENPOD_DIAGNOSTICS" -ErrorAction SilentlyContinue
    }
    $env:VENPOD_MODE = "sandbox"
    if ($SparsePhysics -or $SparseGpuPhysics -or $SparsePhysicsDiagnosticSeed -or $SparsePhysicsDiagnosticFluidSeed) {
        Remove-Item "env:VENPOD_DISABLE_PHYSICS" -ErrorAction SilentlyContinue
        $env:VENPOD_ENABLE_SPARSE_PHYSICS = "1"
    } else {
        $env:VENPOD_DISABLE_PHYSICS = "1"
        Remove-Item "env:VENPOD_ENABLE_SPARSE_PHYSICS" -ErrorAction SilentlyContinue
    }
    if ($SparseGpuPhysics) {
        $env:VENPOD_SPARSE_PHYSICS_GPU = "1"
    } else {
        Remove-Item "env:VENPOD_SPARSE_PHYSICS_GPU" -ErrorAction SilentlyContinue
    }
    $env:VENPOD_ENABLE_EXPERIMENTAL_SPARSE = "1"
    $env:VENPOD_RENDER_BACKEND = "sparse"
    $env:VENPOD_SPARSE_RAYMARCH = "1"
    $env:VENPOD_SPARSE_ONLY = "1"
    $env:VENPOD_SPARSE_SURFACE_AUTHORITATIVE = "1"
    $env:VENPOD_SPARSE_DEBUG_MODE = "$SparseDebugMode"
    if ($StressCamera) {
        $env:VENPOD_SPARSE_STRESS_REQUESTS = "1"
        $env:VENPOD_SPARSE_STRESS_CAMERA = "1"
        $env:VENPOD_SPARSE_STRESS_CAMERA_RADIUS = "$StressCameraRadius"
        $env:VENPOD_SPARSE_STRESS_CAMERA_HEIGHT = "$StressCameraHeight"
        $env:VENPOD_SPARSE_STRESS_CAMERA_BASE_HEIGHT = "$StressCameraBaseHeight"
        $env:VENPOD_SPARSE_STRESS_CAMERA_SPEED = "$StressCameraSpeed"
    }
    if ($BoundaryTest) {
        $env:VENPOD_ENABLE_TEST_MODES = "1"
        $env:VENPOD_BOUNDARY_TEST = "1"
    }
    if ($SparsePhysicsDiagnosticSeed -or $SparsePhysicsDiagnosticFluidSeed) {
        $env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_SEED = "1"
    } else {
        Remove-Item "env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_SEED" -ErrorAction SilentlyContinue
    }
    if ($SparsePhysicsDiagnosticFluidSeed) {
        $env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_FLUID_SEED = "1"
    } else {
        Remove-Item "env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_FLUID_SEED" -ErrorAction SilentlyContinue
    }
    $env:VENPOD_SPARSE_RENDER_OWNERSHIP = "1"
    $env:VENPOD_SPARSE_RENDER_OWNERSHIP_INTERVAL = "1"
    $env:VENPOD_SPARSE_REQUIRE_PIPE_READY = "1"
    $env:VENPOD_SPARSE_PIPE_READY_FRAME = "120"
    $env:VENPOD_SPARSE_REQUIRE_OWNERSHIP_QUALITY = "1"
    $env:VENPOD_SPARSE_OWNERSHIP_QUALITY_READY_FRAME = "120"
    $env:VENPOD_SPARSE_MIN_TERRAIN_PIXELS_PCT = "35"
    $env:VENPOD_SPARSE_MAX_MISS_PIXELS_PCT = "15"
    $env:VENPOD_SPARSE_REQUIRE_OWNERSHIP_STABILITY = "1"
    $env:VENPOD_SPARSE_OWNERSHIP_STABILITY_READY_FRAME = "120"
    $env:VENPOD_SPARSE_MAX_TERRAIN_DELTA_PCT = "8"
    $env:VENPOD_SPARSE_MAX_MISS_DELTA_PCT = "4"
    $env:VENPOD_CAPTURE_DIR = $OutputDir
    $env:VENPOD_CAPTURE_START_FRAME = "$CaptureStartFrame"
    $env:VENPOD_CAPTURE_INTERVAL_FRAMES = "$CaptureIntervalFrames"
    $env:VENPOD_CAPTURE_COUNT = "$CaptureCount"
    if ($ShowDiagnostics) {
        Remove-Item "env:VENPOD_CAPTURE_HIDE_UI" -ErrorAction SilentlyContinue
    } else {
        $env:VENPOD_CAPTURE_HIDE_UI = "1"
    }
    $env:VENPOD_EXIT_AFTER_FRAMES = "$ExitAfterFrames"

    Write-Host "VENPOD - Engine Backbuffer Capture Smoke" -ForegroundColor Magenta
    Write-Info "Output: $OutputDir"
    Write-Info "Frames: count=$CaptureCount start=$CaptureStartFrame interval=$CaptureIntervalFrames exit=$ExitAfterFrames"
    Write-Info "Visual mode: debug=$SparseDebugMode stressCamera=$([int]$StressCamera.IsPresent) boundaryTest=$([int]$BoundaryTest.IsPresent) diagnostics=$([int]$ShowDiagnostics.IsPresent)"
    if ($StressCamera) {
        Write-Info "Stress camera: radius=$StressCameraRadius height=$StressCameraHeight baseHeight=$StressCameraBaseHeight speed=$StressCameraSpeed"
    }

    & $runScript -Config $Config
    Stop-OnFailure -Code $LASTEXITCODE -Stage "VENPOD engine capture run"

    $frames = Get-ChildItem -Path $OutputDir -Filter "engine_frame_*.bmp" | Sort-Object Name | ForEach-Object { $_.FullName }
    if ($frames.Count -lt $CaptureCount) {
        Write-Host "[ERROR] Expected $CaptureCount engine captures but found $($frames.Count) in $OutputDir" -ForegroundColor Red
        exit 13
    }

    $artifacts = Write-ImageArtifacts -Frames $frames -OutDir $OutputDir
    $runtimeLog = Join-Path $projectRoot "build\bin\venpod_runtime.log"
    if (Test-Path $runtimeLog) {
        Copy-Item -Path $runtimeLog -Destination (Join-Path $OutputDir "venpod_runtime.log") -Force
    }
    Test-ImageStats -StatsPath $artifacts.Stats -StressCamera:$StressCamera

    $badLogLines = Select-String `
        -Path (Join-Path $OutputDir "venpod_runtime.log") `
        -Pattern "\] \[(critical|error)\]|device removed|device-removed|timeout|SPARSE_BACKEND_PIPE readiness failed|SPARSE_RENDER_OWNERSHIP quality failed|SPARSE_RENDER_OWNERSHIP stability failed" `
        -CaseSensitive:$false
    if ($badLogLines) {
        Write-Host "[ERROR] Engine capture smoke found runtime failure markers:" -ForegroundColor Red
        $badLogLines | Select-Object -First 20 | ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor Red }
        exit 14
    }
    Test-SparseSurfaceRuntimeStats -LogPath (Join-Path $OutputDir "venpod_runtime.log")
    if ($SparsePhysics -or $SparseGpuPhysics -or $SparsePhysicsDiagnosticSeed -or $SparsePhysicsDiagnosticFluidSeed) {
        Test-SparsePhysicsRuntimeStats `
            -LogPath (Join-Path $OutputDir "venpod_runtime.log") `
            -RequireMovement:($SparsePhysicsDiagnosticSeed -or $SparsePhysicsDiagnosticFluidSeed) `
            -RequireGpuPackets:$SparseGpuPhysics
    }

    Write-Success "Engine capture smoke passed."
    Write-Info "Contact sheet: $($artifacts.ContactSheet)"
    Write-Info "Stats: $($artifacts.Stats)"
    exit 0
} finally {
    Restore-Env
}
