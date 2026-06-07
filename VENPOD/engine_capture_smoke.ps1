# =============================================================================
# VENPOD - Engine Backbuffer Capture Smoke
# Uses VENPOD's in-engine DX12 readback path to capture actual rendered frames.
# =============================================================================

param(
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$NoBuild,
    [int]$ExitAfterFrames = 420,
    [int]$CaptureStartFrame = 200,
    [int]$CaptureIntervalFrames = 20,
    [int]$CaptureCount = 13,
    [int]$SparseDebugMode = 0,
    [switch]$StressCamera,
    [int]$StressCameraRadius = 900,
    [int]$StressCameraHeight = 180,
    [int]$StressCameraBaseHeight = 520,
    [int]$StressCameraSpeed = 50,
    [switch]$SkylineReview,
    [switch]$WaterlineCamera,
    [int]$MinUniqueSampleColors = 120,
    [double]$MinAverageSkyLikePct = -1.0,
    [double]$MaxFrameDarkPct = -1.0,
    [double]$MaxAverageTopTerrainPct = -1.0,
    [double]$MaxFrameTopTerrainPct = -1.0,
    [double]$MaxAverageBrushDomeLikePct = -1.0,
    [double]$MaxFrameBrushDomeLikePct = -1.0,
    [double]$MinOverlayBrushPct = -1.0,
    [double]$MaxUiContaminationPct = -1.0,
    [double]$MaxMaterialSandPct = -1.0,
    [double]$MaxMaterialStonePct = -1.0,
    [double]$MinMaterialDirtPct = -1.0,
    [double]$MaxHeightProxyPct = -1.0,
    [double]$MaxHeightProxyScreenPct = -1.0,
    [double]$MaxValleyAtmosphereScreenPct = -1.0,
    [double]$MaxFarSvoScreenPct = -1.0,
    [double]$MaxFarWaterScreenPct = -1.0,
    [double]$MaxLodParentHeldPct = -1.0,
    [double]$MaxSkyPct = -1.0,
    [double]$MinSurfaceScreenPct = -1.0,
    [double]$MaxSurfaceScreenPct = -1.0,
    [double]$MaxFarSurfacePct = -1.0,
    [double]$MaxOwnershipMissPct = -1.0,
    [double]$MaxBackgroundScreenPct = -1.0,
    [double]$MaxSkylineFlatRunPct = -1.0,
    [double]$MaxSkylineStepPct = -1.0,
    [double]$MaxSkylineInteriorSkyPct = -1.0,
    [double]$MaxSkylineInteriorSkyRunPct = -1.0,
    [double]$MinFarSvoPct = -1.0,
    [double]$MaxFrameMs = -1.0,
    [double]$MaxPrepMs = -1.0,
    [double]$MaxGpuRayMs = -1.0,
    [double]$MaxTemporalChangedPct = -1.0,
    [double]$MaxTemporalLargeChangePct = -1.0,
    [double]$MaxTemporalCenterChangedPct = -1.0,
    [double]$MaxTemporalMeanLumaDelta = -1.0,
    [double]$MaxTemporalP95LumaDelta = -1.0,
    [double]$MaxTemporalOwnershipLayerDeltaPct = -1.0,
    [double]$MaxTemporalOwnershipMissDeltaPct = -1.0,
    [double]$MaxTemporalOwnershipUnsafeDeltaPct = -1.0,
    [int]$SparseOwnershipStabilityReadyFrame = 120,
    [int]$SparseMinTerrainPixelsPct = 35,
    [int]$SparseMaxMissPixelsPct = 15,
    [int]$SparseOwnershipMaxTerrainDeltaPct = 8,
    [int]$SparseOwnershipMaxMissDeltaPct = 4,
    [switch]$WalkTest,
    [int]$WalkTestSpeed = 38,
    [int]$WalkTestYawDegPerSec = 10,
    [int]$WalkTestPitchDeg = -4,
    [int]$WalkTestFixedDtMs = 0,
    [switch]$BoundaryTest,
    [switch]$ShowDiagnostics,
    [switch]$SkipOwnershipDiagnostics,
    [switch]$EnableBrushInput,
    [switch]$SparseBrushFeedback,
    [switch]$SparseBrushPaintSmoke,
    [switch]$SparseBrushPaintMovingSmoke,
    [switch]$SparseBrushPaintNonresidentSmoke,
    [int]$SparseBrushPaintStartFrame = 60,
    [int]$SparseBrushPaintEndFrame = 240,
    [int]$BrushRadiusTenths = 0,
    [switch]$SparsePhysics,
    [switch]$SparseGpuPhysics,
    [switch]$SparseGpuPhysicsStrict,
    [switch]$SparsePhysicsDiagnosticSeed,
    [switch]$SparsePhysicsDiagnosticFluidSeed,
    [switch]$AllowGpuCullDisabled,
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

function Assert-CaptureParameters {
    param(
        [int]$ExitAfterFrames,
        [int]$CaptureStartFrame,
        [int]$CaptureIntervalFrames,
        [int]$CaptureCount
    )

    if ($ExitAfterFrames -lt 1) {
        throw "ExitAfterFrames must be >= 1, got $ExitAfterFrames"
    }
    if ($CaptureStartFrame -lt 0) {
        throw "CaptureStartFrame must be >= 0, got $CaptureStartFrame"
    }
    if ($CaptureIntervalFrames -lt 1) {
        throw "CaptureIntervalFrames must be >= 1, got $CaptureIntervalFrames"
    }
    if ($CaptureCount -lt 1) {
        throw "CaptureCount must be >= 1, got $CaptureCount"
    }

    $lastCaptureFrame64 =
        [int64]$CaptureStartFrame +
        ([int64]$CaptureIntervalFrames * [int64]($CaptureCount - 1))
    $minExitAfterFrames64 = $lastCaptureFrame64 + 5L
    if ($minExitAfterFrames64 -gt [int64][int]::MaxValue) {
        throw "CaptureStartFrame + CaptureIntervalFrames * (CaptureCount - 1) is too large to compute ExitAfterFrames safely"
    }
    return [int]$minExitAfterFrames64
}

function Assert-SafeCaptureOutputDir {
    param(
        [string]$ResolvedOutputDir,
        [string]$ProjectRoot,
        [string]$BuildDir
    )

    function Normalize-GuardPath {
        param([string]$Path)
        return [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    }
    function Test-IsPathUnder {
        param([string]$Path, [string]$Parent)
        $normalizedPath = Normalize-GuardPath $Path
        $normalizedParent = Normalize-GuardPath $Parent
        return $normalizedPath.StartsWith($normalizedParent + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)
    }

    $forbidden = @(
        (Normalize-GuardPath $ProjectRoot),
        (Normalize-GuardPath $BuildDir),
        (Normalize-GuardPath (Get-Location).ProviderPath)
    )
    $projectParent = [System.IO.Directory]::GetParent((Normalize-GuardPath $ProjectRoot))
    if ($projectParent) {
        $forbidden += Normalize-GuardPath $projectParent.FullName
    }
    foreach ($relative in @("captures", "logs", "bin")) {
        $forbidden += Normalize-GuardPath (Join-Path $BuildDir $relative)
    }
    $callerBuildDir = Join-Path (Get-Location).ProviderPath "build"
    $forbidden += Normalize-GuardPath $callerBuildDir
    foreach ($relative in @("captures", "logs", "bin")) {
        $forbidden += Normalize-GuardPath (Join-Path $callerBuildDir $relative)
    }

    $normalizedOutputDir = Normalize-GuardPath $ResolvedOutputDir
    $root = [System.IO.Path]::GetPathRoot($normalizedOutputDir)
    if ($normalizedOutputDir -ieq $root.TrimEnd('\')) {
        throw "Refusing to use filesystem root as engine capture output directory: $ResolvedOutputDir"
    }
    $allowedParents = @(
        (Join-Path $BuildDir "captures"),
        (Join-Path $BuildDir "logs")
    )
    $insideAllowedTree = $false
    foreach ($path in $allowedParents) {
        if (Test-IsPathUnder $normalizedOutputDir $path) {
            $insideAllowedTree = $true
            break
        }
    }
    if (-not $insideAllowedTree) {
        throw "Refusing to use engine capture output outside build/captures or build/logs: $ResolvedOutputDir"
    }
    foreach ($path in @((Join-Path $BuildDir "bin"), (Join-Path $callerBuildDir "bin"))) {
        if (Test-IsPathUnder $normalizedOutputDir $path) {
            throw "Refusing to use runtime binary tree as engine capture output directory: $ResolvedOutputDir"
        }
    }
    foreach ($path in $forbidden) {
        if ($normalizedOutputDir -ieq $path) {
            throw "Refusing to clean broad engine capture output directory: $ResolvedOutputDir"
        }
    }
}

function Clear-EngineCaptureArtifacts {
    param([string]$ResolvedOutputDir)

    Get-ChildItem -LiteralPath $ResolvedOutputDir -Filter "engine_frame_*.bmp" -File |
        Remove-Item -Force
    foreach ($artifact in @("contact_sheet.png", "image_stats.csv", "temporal_stats.csv", "temporal_peaks.png", "ownership_timeline.csv", "composition_timeline.csv", "layer_screen_timeline.csv", "temporal_ownership_review.csv", "venpod_runtime.log")) {
        $path = Join-Path $ResolvedOutputDir $artifact
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Force
        }
    }
}

function Write-ImageArtifacts {
    param(
        [string[]]$Frames,
        [string]$OutDir,
        [switch]$PreciseOverlayStats
    )

    Add-Type -AssemblyName System.Drawing
    $stats = New-Object System.Collections.Generic.List[string]
    $stats.Add("file,width,height,sampled,skyLikePct,darkPct,terrainLikePct,topBandTerrainPct,brushDomeLikePct,centerBrushDomeLikePct,overlayBrushPct,overlayCharacterPct,desktopUiLikePct,bottomUiBandPct,uiContaminationSuspect,uniqueSampleColors,ownerNearPct,ownerMidVoxelPct,ownerMidHeightPct,ownerFarSvoPct,ownerFarHeightPct,ownerFarWaterPct,ownerSkyPct,ownerMissPct,ownerUnsafeNearMissPct,materialWaterPct,materialSandPct,materialDirtPct,materialStonePct,skylineCoveragePct,skylineFlatRunPct,skylineStepPct,skylineInteriorSkyPct,skylineInteriorSkyRunPct")

    foreach ($path in $Frames) {
        $bitmap = [System.Drawing.Bitmap]::FromFile($path)
        try {
            $width = $bitmap.Width
            $height = $bitmap.Height
            $sampled = 0
            $skyLike = 0
            $dark = 0
            $terrainLike = 0
            $topBandSamples = 0
            $topBandTerrain = 0
            $brushDomeLike = 0
            $centerSamples = 0
            $centerBrushDomeLike = 0
            $overlayBrush = 0
            $overlayCharacter = 0
            $preciseOverlayBrush = 0
            $preciseOverlayCharacter = 0
            $desktopUiLike = 0
            $bottomUiBandSamples = 0
            $bottomUiBand = 0
            $ownerNear = 0
            $ownerMidVoxel = 0
            $ownerMidHeight = 0
            $ownerFarSvo = 0
            $ownerFarHeight = 0
            $ownerFarWater = 0
            $ownerSky = 0
            $ownerMiss = 0
            $ownerUnsafeNearMiss = 0
            $materialWater = 0
            $materialSand = 0
            $materialDirt = 0
            $materialStone = 0
            $colors = New-Object 'System.Collections.Generic.HashSet[string]'
            $stepX = [Math]::Max(1, [Math]::Floor($width / 96))
            $stepY = [Math]::Max(1, [Math]::Floor($height / 54))
            $topBandEnd = [int][Math]::Floor($height * 0.30)
            for ($y = 0; $y -lt $height; $y += $stepY) {
                for ($x = 0; $x -lt $width; $x += $stepX) {
                    $c = $bitmap.GetPixel($x, $y)
                    ++$sampled
                    $colors.Add("$($c.R),$($c.G),$($c.B)") | Out-Null
                    $isCoolSky = $c.R -ge 120 -and $c.R -le 190 -and $c.G -ge 155 -and $c.G -le 220 -and $c.B -ge 190
                    $isWarmSky = $c.R -ge 145 -and $c.G -ge 120 -and $c.B -ge 90 -and $c.R -ge ($c.G - 10) -and $c.G -ge ($c.B - 20)
                    $isSky = $isCoolSky -or $isWarmSky
                    $isBrushDomeLike =
                        $c.R -ge 90 -and $c.R -le 195 -and
                        $c.G -ge 145 -and $c.G -le 230 -and
                        $c.B -ge 175 -and
                        ([int]$c.B - [int]$c.R) -ge 35 -and
                        ([int]$c.B - [int]$c.G) -le 55
                    $overlayBrushColor = [Math]::Abs([int]$c.R - 255) -le 6 -and [Math]::Abs([int]$c.G - 51) -le 8 -and [Math]::Abs([int]$c.B - 204) -le 8
                    $overlayCharacterColor = [Math]::Abs([int]$c.R - 51) -le 8 -and [Math]::Abs([int]$c.G - 255) -le 6 -and [Math]::Abs([int]$c.B - 204) -le 8
                    $maxChannel = [Math]::Max([int]$c.R, [Math]::Max([int]$c.G, [int]$c.B))
                    $minChannel = [Math]::Min([int]$c.R, [Math]::Min([int]$c.G, [int]$c.B))
                    $lowSaturation = ($maxChannel - $minChannel) -le 24
                    $isUiDark = $maxChannel -le 55 -and $lowSaturation
                    $isUiText = $minChannel -ge 218 -and $lowSaturation
                    $isUiAccent = $c.B -ge 120 -and $c.R -le 90 -and $c.G -ge 70 -and $c.G -le 180
                    $isDesktopUiLike = $isUiDark -or $isUiText -or $isUiAccent
                    if ($isDesktopUiLike) { ++$desktopUiLike }
                    if ($y -ge [Math]::Floor($height * 0.84)) {
                        ++$bottomUiBandSamples
                        if ($isDesktopUiLike) { ++$bottomUiBand }
                    }
                    if ($isSky) { ++$skyLike }
                    if ($isBrushDomeLike) { ++$brushDomeLike }
                    if ($overlayBrushColor) { ++$overlayBrush }
                    if ($overlayCharacterColor) { ++$overlayCharacter }
                    if (($c.R + $c.G + $c.B) -lt 90) { ++$dark }
                    $isTerrain = (($c.R + $c.G + $c.B) -ge 90 -and -not $isSky)
                    if ($isTerrain) { ++$terrainLike }
                    if ($y -lt $topBandEnd) {
                        ++$topBandSamples
                        if ($isTerrain) { ++$topBandTerrain }
                    }
                    $inCenterField =
                        $x -ge [Math]::Floor($width * 0.18) -and
                        $x -le [Math]::Ceiling($width * 0.82) -and
                        $y -ge [Math]::Floor($height * 0.22) -and
                        $y -le [Math]::Ceiling($height * 0.88)
                    if ($inCenterField) {
                        ++$centerSamples
                        if ($isBrushDomeLike) { ++$centerBrushDomeLike }
                    }
                    $nearColor = [Math]::Abs([int]$c.R - 255) -le 6 -and [Math]::Abs([int]$c.G - 242) -le 8 -and [Math]::Abs([int]$c.B - 13) -le 8
                    $midVoxelColor = [Math]::Abs([int]$c.R - 255) -le 6 -and [Math]::Abs([int]$c.G - 133) -le 8 -and [Math]::Abs([int]$c.B - 26) -le 8
                    $midHeightColor = [Math]::Abs([int]$c.R - 38) -le 8 -and [Math]::Abs([int]$c.G - 191) -le 8 -and [Math]::Abs([int]$c.B - 255) -le 6
                    $farSvoColor = [Math]::Abs([int]$c.R - 217) -le 8 -and [Math]::Abs([int]$c.G - 61) -le 8 -and [Math]::Abs([int]$c.B - 255) -le 6
                    $farHeightColor = [Math]::Abs([int]$c.R - 115) -le 8 -and [Math]::Abs([int]$c.G - 255) -le 6 -and [Math]::Abs([int]$c.B - 77) -le 8
                    $farWaterColor = [Math]::Abs([int]$c.R - 13) -le 8 -and [Math]::Abs([int]$c.G - 82) -le 8 -and [Math]::Abs([int]$c.B - 255) -le 6
                    $ownerSkyColor = [Math]::Abs([int]$c.R - 46) -le 8 -and [Math]::Abs([int]$c.G - 107) -le 8 -and [Math]::Abs([int]$c.B - 242) -le 8
                    $missColor = [Math]::Abs([int]$c.R - 255) -le 6 -and [Math]::Abs([int]$c.G - 13) -le 8 -and [Math]::Abs([int]$c.B - 5) -le 8
                    $unsafeNearMissColor = [Math]::Abs([int]$c.R - 255) -le 6 -and [Math]::Abs([int]$c.G - 5) -le 8 -and [Math]::Abs([int]$c.B - 191) -le 8
                    $materialWaterColor = [Math]::Abs([int]$c.R - 13) -le 8 -and [Math]::Abs([int]$c.G - 97) -le 10 -and [Math]::Abs([int]$c.B - 255) -le 6
                    $materialSandColor = [Math]::Abs([int]$c.R - 255) -le 6 -and [Math]::Abs([int]$c.G - 214) -le 10 -and [Math]::Abs([int]$c.B - 31) -le 10
                    $materialDirtColor = [Math]::Abs([int]$c.R - 46) -le 10 -and [Math]::Abs([int]$c.G - 199) -le 10 -and [Math]::Abs([int]$c.B - 51) -le 10
                    $materialStoneColor = [Math]::Abs([int]$c.R - 140) -le 10 -and [Math]::Abs([int]$c.G - 140) -le 10 -and [Math]::Abs([int]$c.B - 140) -le 10
                    if ($nearColor) { ++$ownerNear }
                    elseif ($midVoxelColor) { ++$ownerMidVoxel }
                    elseif ($midHeightColor) { ++$ownerMidHeight }
                    elseif ($farSvoColor) { ++$ownerFarSvo }
                    elseif ($farHeightColor) { ++$ownerFarHeight }
                    elseif ($farWaterColor) { ++$ownerFarWater }
                    elseif ($ownerSkyColor) { ++$ownerSky }
                    elseif ($missColor) { ++$ownerMiss }
                    elseif ($unsafeNearMissColor) { ++$ownerUnsafeNearMiss }
                    if ($materialWaterColor) { ++$materialWater }
                    elseif ($materialSandColor) { ++$materialSand }
                    elseif ($materialDirtColor) { ++$materialDirt }
                    elseif ($materialStoneColor) { ++$materialStone }
                }
            }
            if ($PreciseOverlayStats) {
                for ($py = 0; $py -lt $height; ++$py) {
                    for ($px = 0; $px -lt $width; ++$px) {
                        $pc = $bitmap.GetPixel($px, $py)
                        $preciseOverlayBrushColor =
                            [Math]::Abs([int]$pc.R - 255) -le 6 -and
                            [Math]::Abs([int]$pc.G - 51) -le 8 -and
                            [Math]::Abs([int]$pc.B - 204) -le 8
                        $preciseOverlayCharacterColor =
                            [Math]::Abs([int]$pc.R - 51) -le 8 -and
                            [Math]::Abs([int]$pc.G - 255) -le 6 -and
                            [Math]::Abs([int]$pc.B - 204) -le 8
                        if ($preciseOverlayBrushColor) { ++$preciseOverlayBrush }
                        if ($preciseOverlayCharacterColor) { ++$preciseOverlayCharacter }
                    }
                }
            }
            $skylineSamples = 0
            $skylineCovered = 0
            $skylineAdjacentPairs = 0
            $skylineLargeSteps = 0
            $skylineInteriorSamples = 0
            $skylineInteriorSky = 0
            $skylineMaxInteriorSkyRun = 0
            $skylineCurrentFlatRun = 0
            $skylineMaxFlatRun = 0
            $prevHorizonY = $null
            $skylineStepX = [Math]::Max(1, [Math]::Floor($width / 192))
            $skylineStepY = [Math]::Max(1, [Math]::Floor($height / 240))
            $skylineYStart = [int][Math]::Floor($height * 0.06)
            $skylineYEnd = [int][Math]::Floor($height * 0.72)
            $skylineInteriorBandSamples = [Math]::Max(1, [Math]::Ceiling(($skylineYEnd - $skylineYStart) / [double]$skylineStepY))
            $flatThresholdPx = [Math]::Max(2, $skylineStepY * 2)
            $largeStepThresholdPx = [Math]::Max(10, [Math]::Floor($height * 0.025))
            for ($sx = 0; $sx -lt $width; $sx += $skylineStepX) {
                ++$skylineSamples
                $horizonY = $null
                for ($sy = $skylineYStart; $sy -lt $skylineYEnd; $sy += $skylineStepY) {
                    $sc = $bitmap.GetPixel($sx, $sy)
                    $isCoolSky =
                        $sc.R -ge 120 -and $sc.R -le 190 -and
                        $sc.G -ge 155 -and $sc.G -le 220 -and
                        $sc.B -ge 190
                    $isWarmSky =
                        $sc.R -ge 145 -and $sc.G -ge 120 -and $sc.B -ge 90 -and
                        $sc.R -ge ($sc.G - 10) -and $sc.G -ge ($sc.B - 20)
                    $smaxChannel = [Math]::Max([int]$sc.R, [Math]::Max([int]$sc.G, [int]$sc.B))
                    $sminChannel = [Math]::Min([int]$sc.R, [Math]::Min([int]$sc.G, [int]$sc.B))
                    $slowSaturation = ($smaxChannel - $sminChannel) -le 24
                    $isUiDark = $smaxChannel -le 55 -and $slowSaturation
                    $isUiText = $sminChannel -ge 218 -and $slowSaturation
                    $isUiAccent = $sc.B -ge 120 -and $sc.R -le 90 -and $sc.G -ge 70 -and $sc.G -le 180
                    $isSkylineTerrain =
                        -not ($isCoolSky -or $isWarmSky) -and
                        -not ($isUiDark -or $isUiText -or $isUiAccent) -and
                        (([int]$sc.R + [int]$sc.G + [int]$sc.B) -ge 90)
                    if ($isSkylineTerrain) {
                        $horizonY = $sy
                        break
                    }
                }
                if ($null -ne $horizonY) {
                    ++$skylineCovered
                    $columnInteriorSkyRun = 0
                    $columnMaxInteriorSkyRun = 0
                    for ($iy = $horizonY + ($skylineStepY * 2); $iy -lt $skylineYEnd; $iy += $skylineStepY) {
                        $ic = $bitmap.GetPixel($sx, $iy)
                        $isInteriorCoolSky =
                            $ic.R -ge 120 -and $ic.R -le 190 -and
                            $ic.G -ge 155 -and $ic.G -le 220 -and
                            $ic.B -ge 190
                        $isInteriorWarmSky =
                            $ic.R -ge 145 -and $ic.G -ge 120 -and $ic.B -ge 90 -and
                            $ic.R -ge ($ic.G - 10) -and $ic.G -ge ($ic.B - 20)
                        $imaxChannel = [Math]::Max([int]$ic.R, [Math]::Max([int]$ic.G, [int]$ic.B))
                        $iminChannel = [Math]::Min([int]$ic.R, [Math]::Min([int]$ic.G, [int]$ic.B))
                        $ilowSaturation = ($imaxChannel - $iminChannel) -le 24
                        $isInteriorUi =
                            ($imaxChannel -le 55 -and $ilowSaturation) -or
                            ($iminChannel -ge 218 -and $ilowSaturation) -or
                            ($ic.B -ge 120 -and $ic.R -le 90 -and $ic.G -ge 70 -and $ic.G -le 180)
                        if (-not $isInteriorUi) {
                            ++$skylineInteriorSamples
                            if ($isInteriorCoolSky -or $isInteriorWarmSky) {
                                ++$skylineInteriorSky
                                ++$columnInteriorSkyRun
                                $columnMaxInteriorSkyRun = [Math]::Max($columnMaxInteriorSkyRun, $columnInteriorSkyRun)
                            } else {
                                $columnInteriorSkyRun = 0
                            }
                        }
                    }
                    $skylineMaxInteriorSkyRun = [Math]::Max($skylineMaxInteriorSkyRun, $columnMaxInteriorSkyRun)
                    if ($null -ne $prevHorizonY) {
                        ++$skylineAdjacentPairs
                        $deltaY = [Math]::Abs([int]$horizonY - [int]$prevHorizonY)
                        if ($deltaY -le $flatThresholdPx) {
                            ++$skylineCurrentFlatRun
                        } else {
                            $skylineMaxFlatRun = [Math]::Max($skylineMaxFlatRun, $skylineCurrentFlatRun)
                            $skylineCurrentFlatRun = 1
                            if ($deltaY -ge $largeStepThresholdPx) {
                                ++$skylineLargeSteps
                            }
                        }
                    } else {
                        $skylineCurrentFlatRun = 1
                    }
                    $prevHorizonY = $horizonY
                } else {
                    $skylineMaxFlatRun = [Math]::Max($skylineMaxFlatRun, $skylineCurrentFlatRun)
                    $skylineCurrentFlatRun = 0
                    $prevHorizonY = $null
                }
            }
            $skylineMaxFlatRun = [Math]::Max($skylineMaxFlatRun, $skylineCurrentFlatRun)
            $skyPct = [Math]::Round(($skyLike * 100.0) / [Math]::Max(1, $sampled), 2)
            $darkPct = [Math]::Round(($dark * 100.0) / [Math]::Max(1, $sampled), 2)
            $terrainPct = [Math]::Round(($terrainLike * 100.0) / [Math]::Max(1, $sampled), 2)
            $topBandTerrainPct = [Math]::Round(($topBandTerrain * 100.0) / [Math]::Max(1, $topBandSamples), 2)
            $brushDomeLikePct = [Math]::Round(($brushDomeLike * 100.0) / [Math]::Max(1, $sampled), 2)
            $centerBrushDomeLikePct = [Math]::Round(($centerBrushDomeLike * 100.0) / [Math]::Max(1, $centerSamples), 2)
            if ($PreciseOverlayStats) {
                $overlayBrushPct = [Math]::Round(($preciseOverlayBrush * 100.0) / [Math]::Max(1, $width * $height), 5)
                $overlayCharacterPct = [Math]::Round(($preciseOverlayCharacter * 100.0) / [Math]::Max(1, $width * $height), 5)
            } else {
                $overlayBrushPct = [Math]::Round(($overlayBrush * 100.0) / [Math]::Max(1, $sampled), 2)
                $overlayCharacterPct = [Math]::Round(($overlayCharacter * 100.0) / [Math]::Max(1, $sampled), 2)
            }
            $desktopUiLikePct = [Math]::Round(($desktopUiLike * 100.0) / [Math]::Max(1, $sampled), 2)
            $bottomUiBandPct = [Math]::Round(($bottomUiBand * 100.0) / [Math]::Max(1, $bottomUiBandSamples), 2)
            $uiContaminationSuspect = if ($bottomUiBandPct -ge 35.0 -or $desktopUiLikePct -ge 22.0) { 1 } else { 0 }
            $ownerNearPct = [Math]::Round(($ownerNear * 100.0) / [Math]::Max(1, $sampled), 2)
            $ownerMidVoxelPct = [Math]::Round(($ownerMidVoxel * 100.0) / [Math]::Max(1, $sampled), 2)
            $ownerMidHeightPct = [Math]::Round(($ownerMidHeight * 100.0) / [Math]::Max(1, $sampled), 2)
            $ownerFarSvoPct = [Math]::Round(($ownerFarSvo * 100.0) / [Math]::Max(1, $sampled), 2)
            $ownerFarHeightPct = [Math]::Round(($ownerFarHeight * 100.0) / [Math]::Max(1, $sampled), 2)
            $ownerFarWaterPct = [Math]::Round(($ownerFarWater * 100.0) / [Math]::Max(1, $sampled), 2)
            $ownerSkyPct = [Math]::Round(($ownerSky * 100.0) / [Math]::Max(1, $sampled), 2)
            $ownerMissPct = [Math]::Round(($ownerMiss * 100.0) / [Math]::Max(1, $sampled), 2)
            $ownerUnsafeNearMissPct = [Math]::Round(($ownerUnsafeNearMiss * 100.0) / [Math]::Max(1, $sampled), 2)
            $materialWaterPct = [Math]::Round(($materialWater * 100.0) / [Math]::Max(1, $sampled), 2)
            $materialSandPct = [Math]::Round(($materialSand * 100.0) / [Math]::Max(1, $sampled), 2)
            $materialDirtPct = [Math]::Round(($materialDirt * 100.0) / [Math]::Max(1, $sampled), 2)
            $materialStonePct = [Math]::Round(($materialStone * 100.0) / [Math]::Max(1, $sampled), 2)
            $skylineCoveragePct = [Math]::Round(($skylineCovered * 100.0) / [Math]::Max(1, $skylineSamples), 2)
            $skylineFlatRunPct = [Math]::Round(($skylineMaxFlatRun * 100.0) / [Math]::Max(1, $skylineSamples), 2)
            $skylineStepPct = [Math]::Round(($skylineLargeSteps * 100.0) / [Math]::Max(1, $skylineAdjacentPairs), 2)
            $skylineInteriorSkyPct = [Math]::Round(($skylineInteriorSky * 100.0) / [Math]::Max(1, $skylineInteriorSamples), 2)
            $skylineInteriorSkyRunPct = [Math]::Round(($skylineMaxInteriorSkyRun * 100.0) / [Math]::Max(1, $skylineInteriorBandSamples), 2)
            $stats.Add((@(
                (Split-Path $path -Leaf), $width, $height, $sampled, $skyPct, $darkPct, $terrainPct,
                $topBandTerrainPct, $brushDomeLikePct, $centerBrushDomeLikePct, $overlayBrushPct,
                $overlayCharacterPct, $desktopUiLikePct, $bottomUiBandPct, $uiContaminationSuspect,
                $colors.Count, $ownerNearPct, $ownerMidVoxelPct, $ownerMidHeightPct, $ownerFarSvoPct,
                $ownerFarHeightPct, $ownerFarWaterPct, $ownerSkyPct, $ownerMissPct,
                $ownerUnsafeNearMissPct, $materialWaterPct, $materialSandPct, $materialDirtPct,
                $materialStonePct, $skylineCoveragePct, $skylineFlatRunPct, $skylineStepPct,
                $skylineInteriorSkyPct, $skylineInteriorSkyRunPct
            ) -join ","))
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

function Write-TemporalArtifacts {
    param(
        [string[]]$Frames,
        [string]$OutDir
    )

    $temporalPath = Join-Path $OutDir "temporal_stats.csv"
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("fromFile,toFile,width,height,sampled,meanLumaDelta,p95LumaDelta,changedPixelPct,largeChangePct,centerChangedPct")
    if ($Frames.Count -lt 2) {
        $lines | Set-Content -Path $temporalPath -Encoding ASCII
        return $temporalPath
    }

    Add-Type -AssemblyName System.Drawing
    $peakPairs = New-Object System.Collections.Generic.List[object]
    for ($i = 1; $i -lt $Frames.Count; ++$i) {
        $prev = [System.Drawing.Bitmap]::FromFile($Frames[$i - 1])
        $next = [System.Drawing.Bitmap]::FromFile($Frames[$i])
        try {
            $width = [Math]::Min($prev.Width, $next.Width)
            $height = [Math]::Min($prev.Height, $next.Height)
            $stepX = [Math]::Max(1, [Math]::Floor($width / 128))
            $stepY = [Math]::Max(1, [Math]::Floor($height / 72))
            $diffs = New-Object 'System.Collections.Generic.List[double]'
            $sampled = 0
            $deltaSum = 0.0
            $changed = 0
            $largeChanged = 0
            $centerSamples = 0
            $centerChanged = 0
            for ($y = 0; $y -lt $height; $y += $stepY) {
                for ($x = 0; $x -lt $width; $x += $stepX) {
                    $a = $prev.GetPixel($x, $y)
                    $b = $next.GetPixel($x, $y)
                    $lumaA = 0.2126 * [double]$a.R + 0.7152 * [double]$a.G + 0.0722 * [double]$a.B
                    $lumaB = 0.2126 * [double]$b.R + 0.7152 * [double]$b.G + 0.0722 * [double]$b.B
                    $delta = [Math]::Abs($lumaB - $lumaA)
                    $diffs.Add($delta) | Out-Null
                    $deltaSum += $delta
                    ++$sampled
                    if ($delta -gt 18.0) { ++$changed }
                    if ($delta -gt 48.0) { ++$largeChanged }
                    $inCenterField =
                        $x -ge [Math]::Floor($width * 0.18) -and
                        $x -le [Math]::Ceiling($width * 0.82) -and
                        $y -ge [Math]::Floor($height * 0.22) -and
                        $y -le [Math]::Ceiling($height * 0.88)
                    if ($inCenterField) {
                        ++$centerSamples
                        if ($delta -gt 18.0) { ++$centerChanged }
                    }
                }
            }
            $diffs.Sort()
            $p95Index = [Math]::Max(0, [Math]::Min($diffs.Count - 1, [Math]::Floor(($diffs.Count - 1) * 0.95)))
            $meanDelta = [Math]::Round($deltaSum / [Math]::Max(1, $sampled), 3)
            $p95Delta = [Math]::Round($diffs[$p95Index], 3)
            $changedPct = [Math]::Round(($changed * 100.0) / [Math]::Max(1, $sampled), 3)
            $largeChangePct = [Math]::Round(($largeChanged * 100.0) / [Math]::Max(1, $sampled), 3)
            $centerChangedPct = [Math]::Round(($centerChanged * 100.0) / [Math]::Max(1, $centerSamples), 3)
            $lines.Add(("{0},{1},{2},{3},{4},{5},{6},{7},{8},{9}" -f (Split-Path $Frames[$i - 1] -Leaf), (Split-Path $Frames[$i] -Leaf), $width, $height, $sampled, $meanDelta, $p95Delta, $changedPct, $largeChangePct, $centerChangedPct))
            $peakPairs.Add([pscustomobject]@{
                From = $Frames[$i - 1]
                To = $Frames[$i]
                Score = [Math]::Max($changedPct, [Math]::Max($largeChangePct, $centerChangedPct))
            }) | Out-Null
        } finally {
            $prev.Dispose()
            $next.Dispose()
        }
    }

    $lines | Set-Content -Path $temporalPath -Encoding ASCII
    $topPairs = $peakPairs | Sort-Object Score -Descending | Select-Object -First ([Math]::Min(4, $peakPairs.Count))
    if ($topPairs -and $topPairs.Count -gt 0) {
        $thumbW = 320
        $thumbH = 180
        $sheet = New-Object System.Drawing.Bitmap($($thumbW * 2), $($thumbH * $topPairs.Count), [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($sheet)
        $g.Clear([System.Drawing.Color]::Black)
        for ($i = 0; $i -lt $topPairs.Count; ++$i) {
            $fromImg = [System.Drawing.Image]::FromFile($topPairs[$i].From)
            $toImg = [System.Drawing.Image]::FromFile($topPairs[$i].To)
            try {
                $y = $i * $thumbH
                $g.DrawImage($fromImg, 0, $y, $thumbW, $thumbH)
                $g.DrawImage($toImg, $thumbW, $y, $thumbW, $thumbH)
            } finally {
                $fromImg.Dispose()
                $toImg.Dispose()
            }
        }
        $g.Dispose()
        $peakSheet = Join-Path $OutDir "temporal_peaks.png"
        $sheet.Save($peakSheet, [System.Drawing.Imaging.ImageFormat]::Png)
        $sheet.Dispose()
    }
    return $temporalPath
}

function Write-OwnershipTimelineArtifacts {
    param(
        [string]$LogPath,
        [string]$OutDir
    )

    $timelinePath = Join-Path $OutDir "ownership_timeline.csv"
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("retireFrame,shaderFrame,total,near,surfaceFragments,farSurface,midVoxel,midVoxelInteriorFallback,midHeight,farSvo,farHeight,farWater,waterContext,valleyAtmosphere,sky,miss,unsafeNearMiss,lodParentHeld,nearPct,surfaceFragmentPct,farSurfacePct,midVoxelPct,midVoxelInteriorFallbackPct,midHeightPct,farSvoPct,farHeightPct,farWaterPct,waterContextPct,valleyAtmospherePct,skyPct,missPct,unsafeNearMissPct,lodParentHeldPct,backgroundTerrainPct,backgroundPct,heightProxyPct")

    if (-not (Test-Path $LogPath)) {
        $lines | Set-Content -Path $timelinePath -Encoding ASCII
        return $timelinePath
    }

    function Format-Pct {
        param([int64]$Value, [int64]$Total)
        return [Math]::Round(([double]$Value * 100.0) / [Math]::Max(1.0, [double]$Total), 4)
    }

    Select-String -Path $LogPath -Pattern "PERF_RENDER_OWNERSHIP" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "PERF_RENDER_OWNERSHIP .*retireFrame=([0-9]+).*shaderFrame=([0-9]+).*total=([0-9]+).*near=([0-9]+).*surfaceFragments=([0-9]+)(?:.*farSurface=([0-9]+))?.*midVoxel=([0-9]+)(?:.*midVoxelInteriorFallback=([0-9]+))?.*midHeight=([0-9]+).*farSvo=([0-9]+).*farHeight=([0-9]+).*farWater=([0-9]+).*waterContext=([0-9]+)(?:.*valleyAtmosphere=([0-9]+))?.*sky=([0-9]+).*miss=([0-9]+).*unsafeNearMiss=([0-9]+)(?:.*lodParentHeld=([0-9]+))?") {
                $retireFrame = [int64]$matches[1]
                $shaderFrame = [int64]$matches[2]
                $total = [int64]$matches[3]
                $near = [int64]$matches[4]
                $surface = [int64]$matches[5]
                $farSurface = 0L
                if ($matches[6]) {
                    $farSurface = [int64]$matches[6]
                }
                $midVoxel = [int64]$matches[7]
                $midVoxelInteriorFallback = 0L
                if ($matches[8]) {
                    $midVoxelInteriorFallback = [int64]$matches[8]
                }
                $midHeight = [int64]$matches[9]
                $farSvo = [int64]$matches[10]
                $farHeight = [int64]$matches[11]
                $farWater = [int64]$matches[12]
                $waterContext = [int64]$matches[13]
                $valleyAtmosphere = 0L
                if ($matches[14]) {
                    $valleyAtmosphere = [int64]$matches[14]
                }
                $sky = [int64]$matches[15]
                $miss = [int64]$matches[16]
                $unsafeNearMiss = [int64]$matches[17]
                $lodParentHeld = 0L
                if ($matches[18]) {
                    $lodParentHeld = [int64]$matches[18]
                }
                $backgroundTerrain = $near + $midVoxel + $midHeight + $farSvo + $farHeight + $farWater + $waterContext
                $background = $midVoxel + $midHeight + $farSvo + $farHeight + $farWater + $waterContext + $valleyAtmosphere + $sky
                $heightProxy = $midHeight + $farHeight
                $lines.Add(("{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13},{14},{15},{16},{17},{18},{19},{20},{21},{22},{23},{24},{25},{26},{27},{28},{29},{30},{31},{32},{33},{34},{35}" -f `
                    $retireFrame,
                    $shaderFrame,
                    $total,
                    $near,
                    $surface,
                    $farSurface,
                    $midVoxel,
                    $midVoxelInteriorFallback,
                    $midHeight,
                    $farSvo,
                    $farHeight,
                    $farWater,
                    $waterContext,
                    $valleyAtmosphere,
                    $sky,
                    $miss,
                    $unsafeNearMiss,
                    $lodParentHeld,
                    (Format-Pct $near $total),
                    (Format-Pct $surface $total),
                    (Format-Pct $farSurface $total),
                    (Format-Pct $midVoxel $total),
                    (Format-Pct $midVoxelInteriorFallback $total),
                    (Format-Pct $midHeight $total),
                    (Format-Pct $farSvo $total),
                    (Format-Pct $farHeight $total),
                    (Format-Pct $farWater $total),
                    (Format-Pct $waterContext $total),
                    (Format-Pct $valleyAtmosphere $total),
                    (Format-Pct $sky $total),
                    (Format-Pct $miss $total),
                    (Format-Pct $unsafeNearMiss $total),
                    (Format-Pct $lodParentHeld $total),
                    (Format-Pct $backgroundTerrain $total),
                    (Format-Pct $background $total),
                    (Format-Pct $heightProxy $total))) | Out-Null
            }
        }

    $lines | Set-Content -Path $timelinePath -Encoding ASCII
    return $timelinePath
}

function Write-RenderCompositionTimelineArtifacts {
    param(
        [string]$LogPath,
        [string]$OutDir
    )

    $timelinePath = Join-Path $OutDir "composition_timeline.csv"
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("frame,screen,backgroundPixels,surfaceOwnedPixels,surfaceFragments,overdrawRatio,backgroundScreenPct,surfaceScreenPct,surfaceFragmentPct")

    if (-not (Test-Path $LogPath)) {
        $lines | Set-Content -Path $timelinePath -Encoding ASCII
        return $timelinePath
    }

    function Format-Pct {
        param([int64]$Value, [int64]$Total)
        return [Math]::Round(([double]$Value * 100.0) / [Math]::Max(1.0, [double]$Total), 4)
    }

    Select-String -Path $LogPath -Pattern "PERF_RENDER_COMPOSITION" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "PERF_RENDER_COMPOSITION frame=([0-9]+).*screen=([0-9]+).*backgroundPixels=([0-9]+).*surfaceOwnedPixels=([0-9]+).*surfaceFragments=([0-9]+).*overdrawRatio=([0-9.]+)") {
                $frame = [int64]$matches[1]
                $screen = [int64]$matches[2]
                $backgroundPixels = [int64]$matches[3]
                $surfaceOwnedPixels = [int64]$matches[4]
                $surfaceFragments = [int64]$matches[5]
                $overdrawRatio = $matches[6]
                $lines.Add(("{0},{1},{2},{3},{4},{5},{6},{7},{8}" -f `
                    $frame,
                    $screen,
                    $backgroundPixels,
                    $surfaceOwnedPixels,
                    $surfaceFragments,
                    $overdrawRatio,
                    (Format-Pct $backgroundPixels $screen),
                    (Format-Pct $surfaceOwnedPixels $screen),
                    (Format-Pct $surfaceFragments $screen))) | Out-Null
            }
        }

    $lines | Set-Content -Path $timelinePath -Encoding ASCII
    return $timelinePath
}

function Write-LayerScreenTimelineArtifacts {
    param(
        [string]$OwnershipTimelinePath,
        [string]$CompositionTimelinePath,
        [string]$OutDir
    )

    $timelinePath = Join-Path $OutDir "layer_screen_timeline.csv"
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("frame,screen,surfaceScreenPct,backgroundScreenPct,nearScreenPct,midVoxelScreenPct,midHeightScreenPct,farSvoScreenPct,farHeightScreenPct,farWaterScreenPct,waterContextScreenPct,valleyAtmosphereScreenPct,skyScreenPct,missScreenPct,unsafeNearMissScreenPct,heightProxyScreenPct,voxelTerrainScreenPct,fragmentOverdrawRatio")

    if (-not (Test-Path $OwnershipTimelinePath) -or -not (Test-Path $CompositionTimelinePath)) {
        $lines | Set-Content -Path $timelinePath -Encoding ASCII
        return $timelinePath
    }

    $compositionByFrame = @{}
    Import-Csv -Path $CompositionTimelinePath | ForEach-Object {
        $compositionByFrame[[int]$_.frame] = $_
    }

    function Format-Pct {
        param([double]$Value, [double]$Total)
        return [Math]::Round(($Value * 100.0) / [Math]::Max(1.0, $Total), 4)
    }

    Import-Csv -Path $OwnershipTimelinePath | ForEach-Object {
        $frame = [int]$_.shaderFrame
        if ($compositionByFrame.ContainsKey($frame)) {
            $composition = $compositionByFrame[$frame]
            $screen = [double]$composition.screen
            $surfaceScreenPct = [double]$composition.surfaceScreenPct
            $backgroundScreenPct = [double]$composition.backgroundScreenPct
            $near = [double]$_.near
            $midVoxel = [double]$_.midVoxel
            $midHeight = [double]$_.midHeight
            $farSvo = [double]$_.farSvo
            $farHeight = [double]$_.farHeight
            $farWater = [double]$_.farWater
            $waterContext = [double]$_.waterContext
            $valleyAtmosphere = 0.0
            if ($_.PSObject.Properties.Name -contains "valleyAtmosphere") {
                $valleyAtmosphere = [double]$_.valleyAtmosphere
            }
            $sky = [double]$_.sky
            $miss = [double]$_.miss
            $unsafeNearMiss = [double]$_.unsafeNearMiss
            $heightProxy = $midHeight + $farHeight
            $voxelTerrain = $surfaceScreenPct + (Format-Pct $near $screen) + (Format-Pct $midVoxel $screen) + (Format-Pct $farSvo $screen)

            $lines.Add(("{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13},{14},{15},{16},{17}" -f `
                $frame,
                [int64]$screen,
                $surfaceScreenPct,
                $backgroundScreenPct,
                (Format-Pct $near $screen),
                (Format-Pct $midVoxel $screen),
                (Format-Pct $midHeight $screen),
                (Format-Pct $farSvo $screen),
                (Format-Pct $farHeight $screen),
                (Format-Pct $farWater $screen),
                (Format-Pct $waterContext $screen),
                (Format-Pct $valleyAtmosphere $screen),
                (Format-Pct $sky $screen),
                (Format-Pct $miss $screen),
                (Format-Pct $unsafeNearMiss $screen),
                (Format-Pct $heightProxy $screen),
                [Math]::Round($voxelTerrain, 4),
                $composition.overdrawRatio)) | Out-Null
        }
    }

    $lines | Set-Content -Path $timelinePath -Encoding ASCII
    return $timelinePath
}

function Write-TemporalOwnershipReviewArtifacts {
    param(
        [string]$TemporalStatsPath,
        [string]$OwnershipTimelinePath,
        [string]$OutDir
    )

    $reviewPath = Join-Path $OutDir "temporal_ownership_review.csv"
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("fromFrame,toFrame,fromFile,toFile,changedPixelPct,largeChangePct,centerChangedPct,fromOwnershipFound,toOwnershipFound,deltaMidVoxelPct,deltaMidHeightPct,deltaFarSvoPct,deltaFarHeightPct,deltaFarWaterPct,deltaWaterContextPct,deltaValleyAtmospherePct,deltaSkyPct,deltaMissPct,deltaUnsafeNearMissPct,deltaBackgroundTerrainPct,deltaHeightProxyPct,maxLayerDeltaPct,maxLayerDeltaName")

    if (-not (Test-Path $TemporalStatsPath) -or -not (Test-Path $OwnershipTimelinePath)) {
        $lines | Set-Content -Path $reviewPath -Encoding ASCII
        return $reviewPath
    }

    $ownershipByFrame = @{}
    Import-Csv -Path $OwnershipTimelinePath | ForEach-Object {
        $ownershipByFrame[[int]$_.shaderFrame] = $_
    }

    function Parse-CaptureFrame {
        param([string]$FileName)
        if ($FileName -match "engine_frame_([0-9]+)\.bmp") {
            return [int]$matches[1]
        }
        return -1
    }

    function Row-Number {
        param($Row, [string]$Name)
        if ($null -eq $Row) {
            return 0.0
        }
        return [double]$Row.$Name
    }

    Import-Csv -Path $TemporalStatsPath | ForEach-Object {
        $fromFrame = Parse-CaptureFrame $_.fromFile
        $toFrame = Parse-CaptureFrame $_.toFile
        $fromOwner = $ownershipByFrame[$fromFrame]
        $toOwner = $ownershipByFrame[$toFrame]
        $fromFound = $null -ne $fromOwner
        $toFound = $null -ne $toOwner

        $deltaNames = @(
            "midVoxelPct",
            "midHeightPct",
            "farSvoPct",
            "farHeightPct",
            "farWaterPct",
            "waterContextPct",
            "valleyAtmospherePct",
            "skyPct",
            "missPct",
            "unsafeNearMissPct",
            "backgroundTerrainPct",
            "heightProxyPct"
        )
        $exclusiveLayerDeltaNames = @(
            "midVoxelPct",
            "midHeightPct",
            "farSvoPct",
            "farHeightPct",
            "farWaterPct",
            "valleyAtmospherePct",
            "skyPct",
            "missPct",
            "unsafeNearMissPct"
        )
        $deltas = @{}
        $maxLayerDelta = 0.0
        $maxLayerName = "none"
        foreach ($name in $deltaNames) {
            $delta = [Math]::Round((Row-Number $toOwner $name) - (Row-Number $fromOwner $name), 4)
            $deltas[$name] = $delta
        }
        foreach ($name in $exclusiveLayerDeltaNames) {
            $delta = $deltas[$name]
            $absDelta = [Math]::Abs($delta)
            if ($absDelta -gt $maxLayerDelta) {
                $maxLayerDelta = $absDelta
                $maxLayerName = $name
            }
        }

        $lines.Add(("{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13},{14},{15},{16},{17},{18},{19},{20},{21},{22}" -f `
            $fromFrame,
            $toFrame,
            $_.fromFile,
            $_.toFile,
            $_.changedPixelPct,
            $_.largeChangePct,
            $_.centerChangedPct,
            ([int]$fromFound),
            ([int]$toFound),
            $deltas["midVoxelPct"],
            $deltas["midHeightPct"],
            $deltas["farSvoPct"],
            $deltas["farHeightPct"],
            $deltas["farWaterPct"],
            $deltas["waterContextPct"],
            $deltas["valleyAtmospherePct"],
            $deltas["skyPct"],
            $deltas["missPct"],
            $deltas["unsafeNearMissPct"],
            $deltas["backgroundTerrainPct"],
            $deltas["heightProxyPct"],
            ([Math]::Round($maxLayerDelta, 4)),
            $maxLayerName)) | Out-Null
    }

    $lines | Set-Content -Path $reviewPath -Encoding ASCII
    return $reviewPath
}

function Test-ImageStats {
    param(
        [string]$StatsPath,
        [switch]$StressCamera,
        [int]$MinUniqueSampleColors = 120,
        [double]$MinAverageSkyLikePct = -1.0,
        [double]$MaxFrameDarkPct = -1.0,
        [double]$MaxAverageTopTerrainPct = -1.0,
        [double]$MaxFrameTopTerrainPct = -1.0,
        [double]$MaxAverageBrushDomeLikePct = -1.0,
        [double]$MaxFrameBrushDomeLikePct = -1.0,
        [double]$MinOverlayBrushPct = -1.0,
        [double]$MaxUiContaminationPct = -1.0,
        [double]$MaxMaterialSandPct = -1.0,
        [double]$MaxMaterialStonePct = -1.0,
        [double]$MinMaterialDirtPct = -1.0,
        [double]$MaxSkylineFlatRunPct = -1.0,
        [double]$MaxSkylineStepPct = -1.0,
        [double]$MaxSkylineInteriorSkyPct = -1.0,
        [double]$MaxSkylineInteriorSkyRunPct = -1.0,
        [int[]]$IgnoreFrameNumbers = @(),
        [switch]$AllowDebugColorTerrainClassifier,
        [switch]$AllowLowSkyCoverage
    )

    if (-not (Test-Path $StatsPath)) {
        Write-Host "[ERROR] Image stats not found at $StatsPath" -ForegroundColor Red
        exit 15
    }

    $rows = Import-Csv -Path $StatsPath
    if ($IgnoreFrameNumbers -and $IgnoreFrameNumbers.Count -gt 0) {
        $ignoreSet = New-Object 'System.Collections.Generic.HashSet[int]'
        foreach ($frame in $IgnoreFrameNumbers) {
            [void]$ignoreSet.Add([int]$frame)
        }
        $rows = @($rows | Where-Object {
            $frameNumber = -1
            if ($_.file -match 'engine_frame_(\d+)\.bmp') {
                $frameNumber = [int]$Matches[1]
            }
            $frameNumber -lt 0 -or -not $ignoreSet.Contains($frameNumber)
        })
    }
    if (-not $rows -or $rows.Count -eq 0) {
        Write-Host "[ERROR] Image stats are empty" -ForegroundColor Red
        exit 15
    }

    $darkSum = 0.0
    $skySum = 0.0
    $terrainSum = 0.0
    $topBandTerrainSum = 0.0
    $brushDomeLikeSum = 0.0
    $centerBrushDomeLikeSum = 0.0
    $maxOverlayBrushObserved = 0.0
    $maxUiContaminationObserved = 0.0
    $maxMaterialSandObserved = 0.0
    $maxMaterialStoneObserved = 0.0
    $minMaterialDirtObserved = [double]::PositiveInfinity
    $maxSkylineFlatRunObserved = 0.0
    $maxSkylineStepObserved = 0.0
    $maxSkylineInteriorSkyObserved = 0.0
    $maxSkylineInteriorSkyRunObserved = 0.0
    $minUnique = [int]::MaxValue
    $badRows = New-Object System.Collections.Generic.List[string]
    foreach ($row in $rows) {
        $dark = [double]$row.darkPct
        $sky = [double]$row.skyLikePct
        $terrain = [double]$row.terrainLikePct
        $topBandTerrain = 0.0
        if ($row.PSObject.Properties.Name -contains "topBandTerrainPct") {
            $topBandTerrain = [double]$row.topBandTerrainPct
        }
        $brushDomeLike = 0.0
        if ($row.PSObject.Properties.Name -contains "brushDomeLikePct") {
            $brushDomeLike = [double]$row.brushDomeLikePct
        }
        $centerBrushDomeLike = 0.0
        if ($row.PSObject.Properties.Name -contains "centerBrushDomeLikePct") {
            $centerBrushDomeLike = [double]$row.centerBrushDomeLikePct
        }
        $overlayBrush = 0.0
        if ($row.PSObject.Properties.Name -contains "overlayBrushPct") {
            $overlayBrush = [double]$row.overlayBrushPct
        }
        $maxOverlayBrushObserved = [Math]::Max($maxOverlayBrushObserved, $overlayBrush)
        $bottomUiBand = 0.0
        if ($row.PSObject.Properties.Name -contains "bottomUiBandPct") {
            $bottomUiBand = [double]$row.bottomUiBandPct
        }
        $desktopUiLike = 0.0
        if ($row.PSObject.Properties.Name -contains "desktopUiLikePct") {
            $desktopUiLike = [double]$row.desktopUiLikePct
        }
        $uiSuspect = 0
        if ($row.PSObject.Properties.Name -contains "uiContaminationSuspect") {
            $uiSuspect = [int]$row.uiContaminationSuspect
        }
        $uiContamination = [Math]::Max($bottomUiBand, $desktopUiLike)
        $maxUiContaminationObserved = [Math]::Max($maxUiContaminationObserved, $uiContamination)
        $materialSand = 0.0
        if ($row.PSObject.Properties.Name -contains "materialSandPct") {
            $materialSand = [double]$row.materialSandPct
        }
        $maxMaterialSandObserved = [Math]::Max($maxMaterialSandObserved, $materialSand)
        $materialDirt = 0.0
        if ($row.PSObject.Properties.Name -contains "materialDirtPct") {
            $materialDirt = [double]$row.materialDirtPct
        }
        $minMaterialDirtObserved = [Math]::Min($minMaterialDirtObserved, $materialDirt)
        $materialStone = 0.0
        if ($row.PSObject.Properties.Name -contains "materialStonePct") {
            $materialStone = [double]$row.materialStonePct
        }
        $maxMaterialStoneObserved = [Math]::Max($maxMaterialStoneObserved, $materialStone)
        $skylineFlatRun = 0.0
        if ($row.PSObject.Properties.Name -contains "skylineFlatRunPct") {
            $skylineFlatRun = [double]$row.skylineFlatRunPct
        }
        $maxSkylineFlatRunObserved = [Math]::Max($maxSkylineFlatRunObserved, $skylineFlatRun)
        $skylineStep = 0.0
        if ($row.PSObject.Properties.Name -contains "skylineStepPct") {
            $skylineStep = [double]$row.skylineStepPct
        }
        $maxSkylineStepObserved = [Math]::Max($maxSkylineStepObserved, $skylineStep)
        $skylineInteriorSky = 0.0
        if ($row.PSObject.Properties.Name -contains "skylineInteriorSkyPct") {
            $skylineInteriorSky = [double]$row.skylineInteriorSkyPct
        }
        $maxSkylineInteriorSkyObserved = [Math]::Max($maxSkylineInteriorSkyObserved, $skylineInteriorSky)
        $skylineInteriorSkyRun = 0.0
        if ($row.PSObject.Properties.Name -contains "skylineInteriorSkyRunPct") {
            $skylineInteriorSkyRun = [double]$row.skylineInteriorSkyRunPct
        }
        $maxSkylineInteriorSkyRunObserved = [Math]::Max($maxSkylineInteriorSkyRunObserved, $skylineInteriorSkyRun)
        $unique = [int]$row.uniqueSampleColors
        $darkSum += $dark
        $skySum += $sky
        $terrainSum += $terrain
        $topBandTerrainSum += $topBandTerrain
        $brushDomeLikeSum += $brushDomeLike
        $centerBrushDomeLikeSum += $centerBrushDomeLike
        $minUnique = [Math]::Min($minUnique, $unique)
        if ($unique -lt $MinUniqueSampleColors) {
            $badRows.Add("$($row.file): uniqueSampleColors=$unique") | Out-Null
        }
        if ($MaxFrameDarkPct -ge 0.0 -and $dark -gt $MaxFrameDarkPct) {
            $badRows.Add("$($row.file): darkPct=$dark max=$MaxFrameDarkPct") | Out-Null
        }
        if (-not $AllowDebugColorTerrainClassifier -and $terrain -lt 25.0) {
            $badRows.Add("$($row.file): terrainLikePct=$terrain") | Out-Null
        }
        if ($MaxFrameTopTerrainPct -ge 0.0 -and $topBandTerrain -gt $MaxFrameTopTerrainPct) {
            $badRows.Add("$($row.file): topBandTerrainPct=$topBandTerrain max=$MaxFrameTopTerrainPct") | Out-Null
        }
        if ($MaxFrameBrushDomeLikePct -ge 0.0 -and $centerBrushDomeLike -gt $MaxFrameBrushDomeLikePct) {
            $badRows.Add("$($row.file): centerBrushDomeLikePct=$centerBrushDomeLike max=$MaxFrameBrushDomeLikePct") | Out-Null
        }
        if ($MaxUiContaminationPct -ge 0.0 -and ($uiContamination -gt $MaxUiContaminationPct -or $uiSuspect -ne 0)) {
            $badRows.Add("$($row.file): uiContamination=$uiContamination max=$MaxUiContaminationPct suspect=$uiSuspect bottomUiBandPct=$bottomUiBand desktopUiLikePct=$desktopUiLike") | Out-Null
        }
        if ($MaxMaterialSandPct -ge 0.0 -and $materialSand -gt $MaxMaterialSandPct) {
            $badRows.Add("$($row.file): materialSandPct=$materialSand max=$MaxMaterialSandPct") | Out-Null
        }
        if ($MaxMaterialStonePct -ge 0.0 -and $materialStone -gt $MaxMaterialStonePct) {
            $badRows.Add("$($row.file): materialStonePct=$materialStone max=$MaxMaterialStonePct") | Out-Null
        }
        if ($MinMaterialDirtPct -ge 0.0 -and $materialDirt -lt $MinMaterialDirtPct) {
            $badRows.Add("$($row.file): materialDirtPct=$materialDirt min=$MinMaterialDirtPct") | Out-Null
        }
        if ($MaxSkylineFlatRunPct -ge 0.0 -and $skylineFlatRun -gt $MaxSkylineFlatRunPct) {
            $badRows.Add("$($row.file): skylineFlatRunPct=$skylineFlatRun max=$MaxSkylineFlatRunPct") | Out-Null
        }
        if ($MaxSkylineStepPct -ge 0.0 -and $skylineStep -gt $MaxSkylineStepPct) {
            $badRows.Add("$($row.file): skylineStepPct=$skylineStep max=$MaxSkylineStepPct") | Out-Null
        }
        if ($MaxSkylineInteriorSkyPct -ge 0.0 -and $skylineInteriorSky -gt $MaxSkylineInteriorSkyPct) {
            $badRows.Add("$($row.file): skylineInteriorSkyPct=$skylineInteriorSky max=$MaxSkylineInteriorSkyPct") | Out-Null
        }
        if ($MaxSkylineInteriorSkyRunPct -ge 0.0 -and $skylineInteriorSkyRun -gt $MaxSkylineInteriorSkyRunPct) {
            $badRows.Add("$($row.file): skylineInteriorSkyRunPct=$skylineInteriorSkyRun max=$MaxSkylineInteriorSkyRunPct") | Out-Null
        }
    }

    $avgDark = $darkSum / [Math]::Max(1, $rows.Count)
    $avgSky = $skySum / [Math]::Max(1, $rows.Count)
    $avgTerrain = $terrainSum / [Math]::Max(1, $rows.Count)
    $avgTopBandTerrain = $topBandTerrainSum / [Math]::Max(1, $rows.Count)
    $avgBrushDomeLike = $brushDomeLikeSum / [Math]::Max(1, $rows.Count)
    $avgCenterBrushDomeLike = $centerBrushDomeLikeSum / [Math]::Max(1, $rows.Count)
    $maxAvgDark = 48.0
    if ($StressCamera) {
        $maxAvgDark = 12.0
    }
    if ($avgDark -gt $maxAvgDark) {
        $badRows.Add("average darkPct=$([Math]::Round($avgDark, 2)) max=$maxAvgDark") | Out-Null
    }
    if (-not $AllowDebugColorTerrainClassifier -and $avgTerrain -lt 35.0) {
        $badRows.Add("average terrainLikePct=$([Math]::Round($avgTerrain, 2)) min=35") | Out-Null
    }
    if (-not $StressCamera -and -not $AllowLowSkyCoverage -and $avgSky -lt 20.0 -and $avgTerrain -lt 55.0) {
        $badRows.Add("average skyLikePct=$([Math]::Round($avgSky, 2)) min=20 unless terrainLikePct>=55") | Out-Null
    }
    if ($MinAverageSkyLikePct -ge 0.0 -and $avgSky -lt $MinAverageSkyLikePct) {
        $badRows.Add("average skyLikePct=$([Math]::Round($avgSky, 2)) min=$MinAverageSkyLikePct") | Out-Null
    }
    if ($MaxAverageTopTerrainPct -ge 0.0 -and $avgTopBandTerrain -gt $MaxAverageTopTerrainPct) {
        $badRows.Add("average topBandTerrainPct=$([Math]::Round($avgTopBandTerrain, 2)) max=$MaxAverageTopTerrainPct") | Out-Null
    }
    if ($MaxAverageBrushDomeLikePct -ge 0.0 -and $avgCenterBrushDomeLike -gt $MaxAverageBrushDomeLikePct) {
        $badRows.Add("average centerBrushDomeLikePct=$([Math]::Round($avgCenterBrushDomeLike, 2)) max=$MaxAverageBrushDomeLikePct wholeFrameAvg=$([Math]::Round($avgBrushDomeLike, 2))") | Out-Null
    }
    if ($MinOverlayBrushPct -ge 0.0 -and $maxOverlayBrushObserved -lt $MinOverlayBrushPct) {
        $badRows.Add("max overlayBrushPct=$([Math]::Round($maxOverlayBrushObserved, 2)) min=$MinOverlayBrushPct") | Out-Null
    }

    if ($badRows.Count -gt 0) {
        Write-Host "[ERROR] Engine capture smoke found visual regression markers:" -ForegroundColor Red
        $badRows | Select-Object -First 20 | ForEach-Object {
            Write-Host "  $_" -ForegroundColor Red
        }
        exit 15
    }
    if ($MaxUiContaminationPct -ge 0.0) {
        Write-Info "UI contamination check observed: maxUiContaminationPct=$('{0:F2}' -f $maxUiContaminationObserved) threshold=$MaxUiContaminationPct"
    }
    if ($MinOverlayBrushPct -ge 0.0) {
        Write-Info "Overlay brush check observed: maxOverlayBrushPct=$('{0:F5}' -f $maxOverlayBrushObserved) threshold=$MinOverlayBrushPct"
    }
    if ($MaxMaterialSandPct -ge 0.0) {
        Write-Info "Material sand check observed: maxMaterialSandPct=$('{0:F2}' -f $maxMaterialSandObserved) threshold=$MaxMaterialSandPct"
    }
    if ($MaxMaterialStonePct -ge 0.0) {
        Write-Info "Material stone check observed: maxMaterialStonePct=$('{0:F2}' -f $maxMaterialStoneObserved) threshold=$MaxMaterialStonePct"
    }
    if ($MinMaterialDirtPct -ge 0.0) {
        if ([double]::IsPositiveInfinity($minMaterialDirtObserved)) {
            $minMaterialDirtObserved = 0.0
        }
        Write-Info "Material dirt check observed: minMaterialDirtPct=$('{0:F2}' -f $minMaterialDirtObserved) threshold=$MinMaterialDirtPct"
    }
    if ($MaxSkylineFlatRunPct -ge 0.0) {
        Write-Info "Skyline flat-run check observed: maxSkylineFlatRunPct=$('{0:F2}' -f $maxSkylineFlatRunObserved) threshold=$MaxSkylineFlatRunPct"
    }
    if ($MaxSkylineStepPct -ge 0.0) {
        Write-Info "Skyline step check observed: maxSkylineStepPct=$('{0:F2}' -f $maxSkylineStepObserved) threshold=$MaxSkylineStepPct"
    }
    if ($MaxSkylineInteriorSkyPct -ge 0.0) {
        Write-Info "Skyline interior-sky check observed: maxSkylineInteriorSkyPct=$('{0:F2}' -f $maxSkylineInteriorSkyObserved) threshold=$MaxSkylineInteriorSkyPct"
    }
    if ($MaxSkylineInteriorSkyRunPct -ge 0.0) {
        Write-Info "Skyline interior-sky-run check observed: maxSkylineInteriorSkyRunPct=$('{0:F2}' -f $maxSkylineInteriorSkyRunObserved) threshold=$MaxSkylineInteriorSkyRunPct"
    }
}

function Test-TemporalStats {
    param(
        [string]$StatsPath,
        [double]$MaxTemporalChangedPct = -1.0,
        [double]$MaxTemporalLargeChangePct = -1.0,
        [double]$MaxTemporalCenterChangedPct = -1.0,
        [double]$MaxTemporalMeanLumaDelta = -1.0,
        [double]$MaxTemporalP95LumaDelta = -1.0
    )

    if ($MaxTemporalChangedPct -lt 0.0 -and
        $MaxTemporalLargeChangePct -lt 0.0 -and
        $MaxTemporalCenterChangedPct -lt 0.0 -and
        $MaxTemporalMeanLumaDelta -lt 0.0 -and
        $MaxTemporalP95LumaDelta -lt 0.0) {
        return
    }
    if (-not (Test-Path $StatsPath)) {
        Write-Host "[ERROR] Temporal stats not found at $StatsPath" -ForegroundColor Red
        exit 21
    }

    $rows = Import-Csv -Path $StatsPath
    if (-not $rows -or $rows.Count -eq 0) {
        Write-Host "[ERROR] Temporal stats are empty but temporal thresholds were requested" -ForegroundColor Red
        exit 21
    }

    $badRows = New-Object System.Collections.Generic.List[string]
    $maxChangedObserved = 0.0
    $maxLargeChangedObserved = 0.0
    $maxCenterChangedObserved = 0.0
    $maxMeanObserved = 0.0
    $maxP95Observed = 0.0
    foreach ($row in $rows) {
        $changed = [double]$row.changedPixelPct
        $largeChanged = [double]$row.largeChangePct
        $centerChanged = [double]$row.centerChangedPct
        $meanDelta = [double]$row.meanLumaDelta
        $p95Delta = [double]$row.p95LumaDelta
        $maxChangedObserved = [Math]::Max($maxChangedObserved, $changed)
        $maxLargeChangedObserved = [Math]::Max($maxLargeChangedObserved, $largeChanged)
        $maxCenterChangedObserved = [Math]::Max($maxCenterChangedObserved, $centerChanged)
        $maxMeanObserved = [Math]::Max($maxMeanObserved, $meanDelta)
        $maxP95Observed = [Math]::Max($maxP95Observed, $p95Delta)
        $label = "$($row.fromFile)->$($row.toFile)"
        if ($MaxTemporalChangedPct -ge 0.0 -and $changed -gt $MaxTemporalChangedPct) {
            $badRows.Add("${label}: changedPixelPct=$changed max=$MaxTemporalChangedPct") | Out-Null
        }
        if ($MaxTemporalLargeChangePct -ge 0.0 -and $largeChanged -gt $MaxTemporalLargeChangePct) {
            $badRows.Add("${label}: largeChangePct=$largeChanged max=$MaxTemporalLargeChangePct") | Out-Null
        }
        if ($MaxTemporalCenterChangedPct -ge 0.0 -and $centerChanged -gt $MaxTemporalCenterChangedPct) {
            $badRows.Add("${label}: centerChangedPct=$centerChanged max=$MaxTemporalCenterChangedPct") | Out-Null
        }
        if ($MaxTemporalMeanLumaDelta -ge 0.0 -and $meanDelta -gt $MaxTemporalMeanLumaDelta) {
            $badRows.Add("${label}: meanLumaDelta=$meanDelta max=$MaxTemporalMeanLumaDelta") | Out-Null
        }
        if ($MaxTemporalP95LumaDelta -ge 0.0 -and $p95Delta -gt $MaxTemporalP95LumaDelta) {
            $badRows.Add("${label}: p95LumaDelta=$p95Delta max=$MaxTemporalP95LumaDelta") | Out-Null
        }
    }

    if ($badRows.Count -gt 0) {
        Write-Host "[ERROR] Engine capture smoke found temporal regression markers:" -ForegroundColor Red
        $badRows | Select-Object -First 20 | ForEach-Object {
            Write-Host "  $_" -ForegroundColor Red
        }
        exit 21
    }

    Write-Info "Temporal check observed: samples=$($rows.Count) maxChangedPct=$('{0:F3}' -f $maxChangedObserved) maxLargeChangePct=$('{0:F3}' -f $maxLargeChangedObserved) maxCenterChangedPct=$('{0:F3}' -f $maxCenterChangedObserved) maxMeanLumaDelta=$('{0:F3}' -f $maxMeanObserved) maxP95LumaDelta=$('{0:F3}' -f $maxP95Observed)"
}

function Test-TemporalOwnershipReview {
    param(
        [string]$ReviewPath,
        [double]$MaxTemporalOwnershipLayerDeltaPct = -1.0,
        [double]$MaxTemporalOwnershipMissDeltaPct = -1.0,
        [double]$MaxTemporalOwnershipUnsafeDeltaPct = -1.0
    )

    if ($MaxTemporalOwnershipLayerDeltaPct -lt 0.0 -and
        $MaxTemporalOwnershipMissDeltaPct -lt 0.0 -and
        $MaxTemporalOwnershipUnsafeDeltaPct -lt 0.0) {
        return
    }
    if (-not (Test-Path $ReviewPath)) {
        Write-Host "[ERROR] Temporal ownership review not found at $ReviewPath" -ForegroundColor Red
        exit 22
    }

    $rows = Import-Csv -Path $ReviewPath
    if (-not $rows -or $rows.Count -eq 0) {
        Write-Host "[ERROR] Temporal ownership review is empty but ownership thresholds were requested" -ForegroundColor Red
        exit 22
    }

    $badRows = New-Object System.Collections.Generic.List[string]
    $maxLayerObserved = 0.0
    $maxMissObserved = 0.0
    $maxUnsafeObserved = 0.0
    foreach ($row in $rows) {
        $label = "$($row.fromFile)->$($row.toFile)"
        $layerDelta = [double]$row.maxLayerDeltaPct
        $missDelta = [Math]::Abs([double]$row.deltaMissPct)
        $unsafeDelta = [Math]::Abs([double]$row.deltaUnsafeNearMissPct)
        $maxLayerObserved = [Math]::Max($maxLayerObserved, $layerDelta)
        $maxMissObserved = [Math]::Max($maxMissObserved, $missDelta)
        $maxUnsafeObserved = [Math]::Max($maxUnsafeObserved, $unsafeDelta)
        if ($MaxTemporalOwnershipLayerDeltaPct -ge 0.0 -and $layerDelta -gt $MaxTemporalOwnershipLayerDeltaPct) {
            $badRows.Add("${label}: maxLayerDeltaPct=$layerDelta layer=$($row.maxLayerDeltaName) max=$MaxTemporalOwnershipLayerDeltaPct") | Out-Null
        }
        if ($MaxTemporalOwnershipMissDeltaPct -ge 0.0 -and $missDelta -gt $MaxTemporalOwnershipMissDeltaPct) {
            $badRows.Add("${label}: abs(deltaMissPct)=$missDelta max=$MaxTemporalOwnershipMissDeltaPct") | Out-Null
        }
        if ($MaxTemporalOwnershipUnsafeDeltaPct -ge 0.0 -and $unsafeDelta -gt $MaxTemporalOwnershipUnsafeDeltaPct) {
            $badRows.Add("${label}: abs(deltaUnsafeNearMissPct)=$unsafeDelta max=$MaxTemporalOwnershipUnsafeDeltaPct") | Out-Null
        }
    }

    if ($badRows.Count -gt 0) {
        Write-Host "[ERROR] Engine capture smoke found temporal ownership regression markers:" -ForegroundColor Red
        $badRows | Select-Object -First 20 | ForEach-Object {
            Write-Host "  $_" -ForegroundColor Red
        }
        exit 22
    }

    Write-Info "Temporal ownership check observed: samples=$($rows.Count) maxLayerDeltaPct=$('{0:F4}' -f $maxLayerObserved) maxMissDeltaPct=$('{0:F4}' -f $maxMissObserved) maxUnsafeDeltaPct=$('{0:F4}' -f $maxUnsafeObserved)"
}

function Test-RenderOwnershipStats {
    param(
        [string]$LogPath,
        [string]$StatsPath,
        [int]$ReadyFrame,
        [double]$MaxHeightProxyPct,
        [double]$MaxSkyPct,
        [double]$MinFarSvoPct,
        [int]$MinSamples = 1
    )

    if ($MaxHeightProxyPct -lt 0.0 -and $MaxSkyPct -lt 0.0 -and $MinFarSvoPct -lt 0.0) {
        return
    }
    if (-not (Test-Path $LogPath)) {
        Write-Host "[ERROR] Runtime log not found at $LogPath" -ForegroundColor Red
        exit 16
    }

    $screenPixels = 0L
    if (Test-Path $StatsPath) {
        Import-Csv -Path $StatsPath |
            Select-Object -First 1 |
            ForEach-Object {
                $width = [int64]$_.width
                $height = [int64]$_.height
                if ($width -gt 0 -and $height -gt 0) {
                    $screenPixels = $width * $height
                }
            }
    }

    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    $sampleCount = 0
    $maxHeightProxyObserved = 0.0
    $maxSkyObserved = 0.0
    $maxFarSvoObserved = 0.0
    $maxMissPixels = 0L
    $maxUnsafeNearMissPixels = 0L

    Select-String -Path $LogPath -Pattern "PERF_RENDER_OWNERSHIP" |
        ForEach-Object {
            $line = $_.Line
            if ($line -match "PERF_RENDER_OWNERSHIP .*retireFrame=([0-9]+).*total=([0-9]+).*midHeight=([0-9]+).*farSvo=([0-9]+).*farHeight=([0-9]+)(?:.*farWater=([0-9]+))?.*sky=([0-9]+).*miss=([0-9]+).*unsafeNearMiss=([0-9]+)") {
                $m = $Matches.Clone()
                $frame = [int]$m[1]
                if ($frame -lt $ReadyFrame) {
                    return
                }
                $total = [double]::Parse($m[2], $culture)
                if ($total -le 0.0) {
                    return
                }
                $midHeight = [int64]$m[3]
                $farSvo = [int64]$m[4]
                $farHeight = [int64]$m[5]
                $sky = [double]::Parse($m[7], $culture)
                $miss = [int64]$m[8]
                $unsafeNearMiss = [int64]$m[9]
                $screenTotal = $total
                if ($screenPixels -gt 0) {
                    $screenTotal = [double]$screenPixels
                }
                $skyPct = ($sky * 100.0) / [Math]::Max(1.0, $screenTotal)
                $ownershipTestPixels = [Math]::Max(1.0, $screenTotal - $sky)
                $heightProxyPct = ([double]($midHeight + $farHeight) * 100.0) / $ownershipTestPixels
                $farSvoPct = ([double]$farSvo * 100.0) / $ownershipTestPixels
                ++$sampleCount
                $maxHeightProxyObserved = [Math]::Max($maxHeightProxyObserved, $heightProxyPct)
                $maxSkyObserved = [Math]::Max($maxSkyObserved, $skyPct)
                $maxFarSvoObserved = [Math]::Max($maxFarSvoObserved, $farSvoPct)
                $maxMissPixels = [Math]::Max($maxMissPixels, $miss)
                $maxUnsafeNearMissPixels = [Math]::Max($maxUnsafeNearMissPixels, $unsafeNearMiss)
            }
        }

    if ($sampleCount -lt $MinSamples) {
        Write-Host "[ERROR] Engine capture ownership check saw too few post-ready samples (samples=$sampleCount required=$MinSamples readyFrame=$ReadyFrame)" -ForegroundColor Red
        exit 16
    }
    if ($maxMissPixels -gt 512 -or $maxUnsafeNearMissPixels -gt 0) {
        Write-Host "[ERROR] Engine capture ownership found miss/unsafe-near-miss pixels (miss=$maxMissPixels unsafeNearMiss=$maxUnsafeNearMissPixels)" -ForegroundColor Red
        exit 16
    }
    if ($MaxHeightProxyPct -ge 0.0 -and $maxHeightProxyObserved -gt $MaxHeightProxyPct) {
        Write-Host "[ERROR] Engine capture ownership is too height-proxy dominated (maxHeightProxy=$('{0:F2}' -f $maxHeightProxyObserved)% threshold=$MaxHeightProxyPct%)" -ForegroundColor Red
        exit 16
    }
    if ($MaxSkyPct -ge 0.0 -and $maxSkyObserved -gt $MaxSkyPct) {
        Write-Host "[ERROR] Engine capture ownership is too sky-dominated (maxSky=$('{0:F2}' -f $maxSkyObserved)% threshold=$MaxSkyPct%)" -ForegroundColor Red
        exit 16
    }
    if ($MinFarSvoPct -ge 0.0 -and $maxFarSvoObserved -lt $MinFarSvoPct) {
        Write-Host "[ERROR] Engine capture ownership did not observe enough far-SVO terrain (maxFarSvo=$('{0:F2}' -f $maxFarSvoObserved)% threshold=$MinFarSvoPct%)" -ForegroundColor Red
        exit 16
    }

    Write-Info "Ownership proxy check observed: samples=$sampleCount maxHeightProxy=$('{0:F2}' -f $maxHeightProxyObserved)% maxSky=$('{0:F2}' -f $maxSkyObserved)% maxFarSvo=$('{0:F2}' -f $maxFarSvoObserved)% miss=$maxMissPixels unsafeNearMiss=$maxUnsafeNearMissPixels heightProxyThreshold=$MaxHeightProxyPct skyThreshold=$MaxSkyPct farSvoThreshold=$MinFarSvoPct"
}

function Test-RenderCompositionStats {
    param(
        [string]$CompositionPath,
        [int]$ReadyFrame,
        [double]$MinSurfaceScreenPct,
        [double]$MaxSurfaceScreenPct,
        [double]$MaxBackgroundScreenPct,
        [int]$MinSamples = 1
    )

    if ($MinSurfaceScreenPct -lt 0.0 -and $MaxSurfaceScreenPct -lt 0.0 -and $MaxBackgroundScreenPct -lt 0.0) {
        return
    }
    if (-not (Test-Path $CompositionPath)) {
        Write-Host "[ERROR] Render composition timeline not found at $CompositionPath" -ForegroundColor Red
        exit 19
    }

    $rows = Import-Csv -Path $CompositionPath | Where-Object { [int]$_.frame -ge $ReadyFrame }
    if ($rows.Count -lt $MinSamples) {
        Write-Host "[ERROR] Render composition check saw too few post-ready samples (samples=$($rows.Count) required=$MinSamples readyFrame=$ReadyFrame)" -ForegroundColor Red
        exit 19
    }

    $minSurfaceObserved = 100.0
    $maxSurfaceObserved = 0.0
    $maxBackgroundObserved = 0.0
    foreach ($row in $rows) {
        $surface = [double]$row.surfaceScreenPct
        $background = [double]$row.backgroundScreenPct
        $minSurfaceObserved = [Math]::Min($minSurfaceObserved, $surface)
        $maxSurfaceObserved = [Math]::Max($maxSurfaceObserved, $surface)
        $maxBackgroundObserved = [Math]::Max($maxBackgroundObserved, $background)
    }

    if ($MinSurfaceScreenPct -ge 0.0 -and $minSurfaceObserved -lt $MinSurfaceScreenPct) {
        Write-Host "[ERROR] Sparse surface screen coverage is too low (minSurfaceScreen=$('{0:F2}' -f $minSurfaceObserved)% threshold=$MinSurfaceScreenPct%)" -ForegroundColor Red
        exit 19
    }
    if ($MaxSurfaceScreenPct -ge 0.0 -and $maxSurfaceObserved -gt $MaxSurfaceScreenPct) {
        Write-Host "[ERROR] Sparse surface screen coverage is too high for a near-only surface contract (maxSurfaceScreen=$('{0:F2}' -f $maxSurfaceObserved)% threshold=$MaxSurfaceScreenPct%)" -ForegroundColor Red
        exit 19
    }
    if ($MaxBackgroundScreenPct -ge 0.0 -and $maxBackgroundObserved -gt $MaxBackgroundScreenPct) {
        Write-Host "[ERROR] Background/proxy screen coverage is too high (maxBackgroundScreen=$('{0:F2}' -f $maxBackgroundObserved)% threshold=$MaxBackgroundScreenPct%)" -ForegroundColor Red
        exit 19
    }

    Write-Info "Render composition check observed: samples=$($rows.Count) minSurfaceScreen=$('{0:F2}' -f $minSurfaceObserved)% maxSurfaceScreen=$('{0:F2}' -f $maxSurfaceObserved)% maxBackgroundScreen=$('{0:F2}' -f $maxBackgroundObserved)% thresholds=$MinSurfaceScreenPct/$MaxSurfaceScreenPct/$MaxBackgroundScreenPct"
}

function Test-LodParentHeldStats {
    param(
        [string]$OwnershipTimelinePath,
        [int]$ReadyFrame,
        [double]$MaxLodParentHeldPct,
        [int]$MinSamples = 1
    )

    if ($MaxLodParentHeldPct -lt 0.0) {
        return
    }
    if (-not (Test-Path $OwnershipTimelinePath)) {
        Write-Host "[ERROR] Ownership timeline not found at $OwnershipTimelinePath" -ForegroundColor Red
        exit 23
    }

    $rows = Import-Csv -Path $OwnershipTimelinePath | Where-Object { [int]$_.retireFrame -ge $ReadyFrame }
    if ($rows.Count -lt $MinSamples) {
        Write-Host "[ERROR] LOD parent-held check saw too few post-ready samples (samples=$($rows.Count) required=$MinSamples readyFrame=$ReadyFrame)" -ForegroundColor Red
        exit 23
    }

    $maxParentHeldObserved = 0.0
    $maxParentHeldPixels = 0L
    foreach ($row in $rows) {
        $parentHeldPct = [double]$row.lodParentHeldPct
        $parentHeldPixels = [int64]$row.lodParentHeld
        if ($parentHeldPct -gt $maxParentHeldObserved) {
            $maxParentHeldObserved = $parentHeldPct
            $maxParentHeldPixels = $parentHeldPixels
        }
    }

    if ($maxParentHeldObserved -gt $MaxLodParentHeldPct) {
        Write-Host "[ERROR] LOD parent-held fallback is too high (maxLodParentHeld=$('{0:F4}' -f $maxParentHeldObserved)% pixels=$maxParentHeldPixels threshold=$MaxLodParentHeldPct%)" -ForegroundColor Red
        exit 23
    }

    Write-Info "LOD parent-held check observed: samples=$($rows.Count) maxLodParentHeld=$('{0:F4}' -f $maxParentHeldObserved)% pixels=$maxParentHeldPixels threshold=$MaxLodParentHeldPct"
}

function Test-FarSurfaceStats {
    param(
        [string]$OwnershipTimelinePath,
        [int]$ReadyFrame,
        [double]$MaxFarSurfacePct,
        [int]$MinSamples = 1
    )

    if ($MaxFarSurfacePct -lt 0.0) {
        return
    }
    if (-not (Test-Path $OwnershipTimelinePath)) {
        Write-Host "[ERROR] Ownership timeline not found at $OwnershipTimelinePath" -ForegroundColor Red
        exit 24
    }

    $rows = Import-Csv -Path $OwnershipTimelinePath | Where-Object { [int]$_.retireFrame -ge $ReadyFrame }
    if ($rows.Count -lt $MinSamples) {
        Write-Host "[ERROR] Far sparse-surface check saw too few post-ready samples (samples=$($rows.Count) required=$MinSamples readyFrame=$ReadyFrame)" -ForegroundColor Red
        exit 24
    }

    $maxFarSurfaceObserved = 0.0
    $maxFarSurfaceFragments = 0L
    foreach ($row in $rows) {
        $farSurfacePct = 0.0
        $farSurfaceFragments = 0L
        if ($row.PSObject.Properties.Name -contains "farSurfacePct") {
            $farSurfacePct = [double]$row.farSurfacePct
        }
        if ($row.PSObject.Properties.Name -contains "farSurface") {
            $farSurfaceFragments = [int64]$row.farSurface
        }
        if ($farSurfacePct -gt $maxFarSurfaceObserved) {
            $maxFarSurfaceObserved = $farSurfacePct
            $maxFarSurfaceFragments = $farSurfaceFragments
        }
    }

    if ($maxFarSurfaceObserved -gt $MaxFarSurfacePct) {
        Write-Host "[ERROR] Far sparse-surface leakage is too high (maxFarSurface=$('{0:F4}' -f $maxFarSurfaceObserved)% fragments=$maxFarSurfaceFragments threshold=$MaxFarSurfacePct%)" -ForegroundColor Red
        exit 24
    }

    Write-Info "Far sparse-surface check observed: samples=$($rows.Count) maxFarSurface=$('{0:F4}' -f $maxFarSurfaceObserved)% fragments=$maxFarSurfaceFragments threshold=$MaxFarSurfacePct"
}

function Test-OwnershipMissStats {
    param(
        [string]$OwnershipTimelinePath,
        [int]$ReadyFrame,
        [double]$MaxOwnershipMissPct,
        [int]$MinSamples = 1
    )

    if ($MaxOwnershipMissPct -lt 0.0) {
        return
    }
    if (-not (Test-Path $OwnershipTimelinePath)) {
        Write-Host "[ERROR] Ownership timeline not found at $OwnershipTimelinePath" -ForegroundColor Red
        exit 25
    }

    $rows = Import-Csv -Path $OwnershipTimelinePath | Where-Object { [int]$_.retireFrame -ge $ReadyFrame }
    if ($rows.Count -lt $MinSamples) {
        Write-Host "[ERROR] Ownership miss check saw too few post-ready samples (samples=$($rows.Count) required=$MinSamples readyFrame=$ReadyFrame)" -ForegroundColor Red
        exit 25
    }

    $maxMissObserved = 0.0
    $maxMissPixels = 0L
    foreach ($row in $rows) {
        $missPct = 0.0
        $missPixels = 0L
        if ($row.PSObject.Properties.Name -contains "missPct") {
            $missPct = [double]$row.missPct
        }
        if ($row.PSObject.Properties.Name -contains "miss") {
            $missPixels = [int64]$row.miss
        }
        if ($missPct -gt $maxMissObserved) {
            $maxMissObserved = $missPct
            $maxMissPixels = $missPixels
        }
    }

    if ($maxMissObserved -gt $MaxOwnershipMissPct) {
        Write-Host "[ERROR] Ownership miss coverage is too high (maxMiss=$('{0:F4}' -f $maxMissObserved)% pixels=$maxMissPixels threshold=$MaxOwnershipMissPct%)" -ForegroundColor Red
        exit 25
    }

    Write-Info "Ownership miss check observed: samples=$($rows.Count) maxMiss=$('{0:F4}' -f $maxMissObserved)% pixels=$maxMissPixels threshold=$MaxOwnershipMissPct"
}

function Test-LayerScreenStats {
    param(
        [string]$LayerScreenPath,
        [int]$ReadyFrame,
        [double]$MaxHeightProxyScreenPct,
        [double]$MaxValleyAtmosphereScreenPct,
        [double]$MaxFarSvoScreenPct,
        [double]$MaxFarWaterScreenPct,
        [int]$MinSamples = 1
    )

    if ($MaxHeightProxyScreenPct -lt 0.0 -and
        $MaxValleyAtmosphereScreenPct -lt 0.0 -and
        $MaxFarSvoScreenPct -lt 0.0 -and
        $MaxFarWaterScreenPct -lt 0.0) {
        return
    }
    if (-not (Test-Path $LayerScreenPath)) {
        Write-Host "[ERROR] Layer screen timeline not found at $LayerScreenPath" -ForegroundColor Red
        exit 20
    }

    $rows = Import-Csv -Path $LayerScreenPath | Where-Object { [int]$_.frame -ge $ReadyFrame }
    if ($rows.Count -lt $MinSamples) {
        Write-Host "[ERROR] Layer screen check saw too few post-ready samples (samples=$($rows.Count) required=$MinSamples readyFrame=$ReadyFrame)" -ForegroundColor Red
        exit 20
    }

    $maxHeightProxyObserved = 0.0
    $maxValleyAtmosphereObserved = 0.0
    $maxFarSvoObserved = 0.0
    $maxFarWaterObserved = 0.0
    $minVoxelTerrainObserved = 100.0
    foreach ($row in $rows) {
        $heightProxy = [double]$row.heightProxyScreenPct
        $valleyAtmosphere = 0.0
        if ($row.PSObject.Properties.Name -contains "valleyAtmosphereScreenPct") {
            $valleyAtmosphere = [double]$row.valleyAtmosphereScreenPct
        }
        $farSvo = 0.0
        if ($row.PSObject.Properties.Name -contains "farSvoScreenPct") {
            $farSvo = [double]$row.farSvoScreenPct
        }
        $farWater = 0.0
        if ($row.PSObject.Properties.Name -contains "farWaterScreenPct") {
            $farWater = [double]$row.farWaterScreenPct
        }
        $voxelTerrain = [double]$row.voxelTerrainScreenPct
        $maxHeightProxyObserved = [Math]::Max($maxHeightProxyObserved, $heightProxy)
        $maxValleyAtmosphereObserved = [Math]::Max($maxValleyAtmosphereObserved, $valleyAtmosphere)
        $maxFarSvoObserved = [Math]::Max($maxFarSvoObserved, $farSvo)
        $maxFarWaterObserved = [Math]::Max($maxFarWaterObserved, $farWater)
        $minVoxelTerrainObserved = [Math]::Min($minVoxelTerrainObserved, $voxelTerrain)
    }

    if ($MaxHeightProxyScreenPct -ge 0.0 -and $maxHeightProxyObserved -gt $MaxHeightProxyScreenPct) {
        Write-Host "[ERROR] Height-proxy screen coverage is too high (maxHeightProxyScreen=$('{0:F2}' -f $maxHeightProxyObserved)% threshold=$MaxHeightProxyScreenPct%)" -ForegroundColor Red
        exit 20
    }
    if ($MaxValleyAtmosphereScreenPct -ge 0.0 -and $maxValleyAtmosphereObserved -gt $MaxValleyAtmosphereScreenPct) {
        Write-Host "[ERROR] Valley-atmosphere screen coverage is too high (maxValleyAtmosphereScreen=$('{0:F2}' -f $maxValleyAtmosphereObserved)% threshold=$MaxValleyAtmosphereScreenPct%)" -ForegroundColor Red
        exit 20
    }
    if ($MaxFarSvoScreenPct -ge 0.0 -and $maxFarSvoObserved -gt $MaxFarSvoScreenPct) {
        Write-Host "[ERROR] Far-SVO screen coverage is too high for this low-altitude ownership contract (maxFarSvoScreen=$('{0:F2}' -f $maxFarSvoObserved)% threshold=$MaxFarSvoScreenPct%)" -ForegroundColor Red
        exit 20
    }
    if ($MaxFarWaterScreenPct -ge 0.0 -and $maxFarWaterObserved -gt $MaxFarWaterScreenPct) {
        Write-Host "[ERROR] Far-water screen coverage is too high for this low-altitude voxel terrain contract (maxFarWaterScreen=$('{0:F2}' -f $maxFarWaterObserved)% threshold=$MaxFarWaterScreenPct%)" -ForegroundColor Red
        exit 20
    }

    Write-Info "Layer screen check observed: samples=$($rows.Count) maxHeightProxyScreen=$('{0:F2}' -f $maxHeightProxyObserved)% maxValleyAtmosphereScreen=$('{0:F2}' -f $maxValleyAtmosphereObserved)% maxFarSvoScreen=$('{0:F2}' -f $maxFarSvoObserved)% maxFarWaterScreen=$('{0:F2}' -f $maxFarWaterObserved)% minVoxelTerrainScreen=$('{0:F2}' -f $minVoxelTerrainObserved)% thresholds=$MaxHeightProxyScreenPct/$MaxValleyAtmosphereScreenPct/$MaxFarSvoScreenPct/$MaxFarWaterScreenPct"
}

function Test-RenderPerformanceStats {
    param(
        [string]$LogPath,
        [int]$ReadyFrame,
        [double]$MaxFrameMs,
        [double]$MaxPrepMs,
        [double]$MaxGpuRayMs,
        [int]$CaptureStartFrame,
        [int]$CaptureIntervalFrames,
        [int]$CaptureCount,
        [int]$MinSamples = 1
    )

    if ($MaxFrameMs -lt 0.0 -and $MaxPrepMs -lt 0.0 -and $MaxGpuRayMs -lt 0.0) {
        return
    }
    if (-not (Test-Path $LogPath)) {
        Write-Host "[ERROR] Runtime log not found at $LogPath" -ForegroundColor Red
        exit 18
    }

    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    $sampleCount = 0
    $maxObservedFrameMs = 0.0
    $maxObservedSmoothedFrameMs = 0.0
    $maxObservedPrepMs = 0.0
    $maxObservedGpuRayMs = 0.0
    $badRows = New-Object System.Collections.Generic.List[string]

    Select-String -Path $LogPath -Pattern "PERF frame=" | ForEach-Object {
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

        if ($MaxFrameMs -ge 0.0 -and $frameMs -gt $MaxFrameMs) {
            $badRows.Add("frame ${frame}: frameMs=$('{0:F2}' -f $frameMs) max=$MaxFrameMs") | Out-Null
        }
        if ($MaxPrepMs -ge 0.0 -and $prepMs -gt $MaxPrepMs) {
            $badRows.Add("frame ${frame}: prepMs=$('{0:F2}' -f $prepMs) max=$MaxPrepMs") | Out-Null
        }
        if ($MaxGpuRayMs -ge 0.0 -and $gpuRayMs -gt $MaxGpuRayMs) {
            $badRows.Add("frame ${frame}: gpuRayMs=$('{0:F2}' -f $gpuRayMs) max=$MaxGpuRayMs") | Out-Null
        }
    }

    if ($sampleCount -lt $MinSamples) {
        Write-Host "[ERROR] Engine capture performance check saw too few post-ready samples (samples=$sampleCount required=$MinSamples readyFrame=$ReadyFrame)" -ForegroundColor Red
        exit 18
    }
    if ($badRows.Count -gt 0) {
        Write-Host "[ERROR] Engine capture performance gate failed:" -ForegroundColor Red
        $badRows | Select-Object -First 20 | ForEach-Object {
            Write-Host "  $_" -ForegroundColor Red
        }
        exit 18
    }

    Write-Info "Performance check observed: samples=$sampleCount maxFrameMs=$('{0:F2}' -f $maxObservedFrameMs) maxSmoothedFrameMs=$('{0:F2}' -f $maxObservedSmoothedFrameMs) maxPrepMs=$('{0:F2}' -f $maxObservedPrepMs) maxGpuRayMs=$('{0:F2}' -f $maxObservedGpuRayMs) thresholds=$MaxFrameMs/$MaxPrepMs/$MaxGpuRayMs"
}

function Test-SparseSurfaceRuntimeStats {
    param(
        [string]$LogPath,
        [switch]$AllowGpuCullDisabled
    )

    if (-not (Test-Path $LogPath)) {
        Write-Host "[ERROR] Runtime log not found at $LogPath" -ForegroundColor Red
        exit 16
    }

    $initLine = Select-String -Path $LogPath -Pattern "Sparse surface GPU buffers initialized" | Select-Object -Last 1
    if (-not $initLine -or
        $initLine.Line -notlike "*rangeAllocator=enabled*" -or
        (-not $AllowGpuCullDisabled -and $initLine.Line -notlike "*gpuCull=enabled*") -or
        $initLine.Line -notlike "*stableDrawSlots=enabled*" -or
        $initLine.Line -notlike "*compactStableDraws=enabled*") {
        if ($AllowGpuCullDisabled) {
            Write-Host "[ERROR] Sparse surface GPU buffers did not initialize with required non-cull optimized components" -ForegroundColor Red
        } else {
            Write-Host "[ERROR] Sparse surface GPU buffers did not initialize with the default optimized path" -ForegroundColor Red
        }
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
        $requiredFields = @("stableDraw=1", "compactDraw=1")
        if (-not $AllowGpuCullDisabled) {
            $requiredFields += @("gpuCull=1")
            $rasterFacesMatch = [regex]::Match($line, "rasterFaces=(\d+)")
            $rasterFaces = 0
            if ($rasterFacesMatch.Success) {
                $rasterFaces = [int]$rasterFacesMatch.Groups[1].Value
            }
            if ($rasterFaces -gt 0) {
                $requiredFields += @("gpuCullDispatch=1")
            }
        }
        foreach ($required in $requiredFields) {
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

function Test-TerrainCriticalReadinessStats {
    param(
        [string]$LogPath,
        [int]$ReadyFrame
    )

    if (-not (Test-Path $LogPath)) {
        Write-Host "[ERROR] Runtime log not found at $LogPath" -ForegroundColor Red
        exit 16
    }

    $criticalLines = Select-String -Path $LogPath -Pattern "PERF_SPARSE_TERRAIN_CRITICAL frame="
    if (-not $criticalLines) {
        Write-Info "Terrain-critical readiness check skipped: no PERF_SPARSE_TERRAIN_CRITICAL rows."
        return
    }

    $effectiveReadyFrame = $ReadyFrame
    $heldLines = Select-String -Path $LogPath -Pattern "SPARSE_STARTUP_PUBLIC_RENDER_HELD frame="
    foreach ($held in $heldLines) {
        $heldFrameMatch = [regex]::Match($held.Line, "SPARSE_STARTUP_PUBLIC_RENDER_HELD frame=(\d+)")
        if ($heldFrameMatch.Success) {
            $effectiveReadyFrame = [Math]::Max(
                $effectiveReadyFrame,
                [int]$heldFrameMatch.Groups[1].Value + 1)
        }
    }

    $validated = 0
    $badRows = New-Object System.Collections.Generic.List[string]
    foreach ($match in $criticalLines) {
        $line = $match.Line
        $frameMatch = [regex]::Match($line, "frame=(\d+)")
        if (-not $frameMatch.Success) {
            continue
        }
        $frame = [int]$frameMatch.Groups[1].Value
        if ($frame -lt $effectiveReadyFrame) {
            continue
        }

        $postFields = @(
            "postMissing",
            "postRequested",
            "postGenerating",
            "postUploadQueued",
            "postUploading",
            "postResidentMissingSurface"
        )
        $rowBad = New-Object System.Collections.Generic.List[string]
        foreach ($field in $postFields) {
            $fieldMatch = [regex]::Match($line, "$field=(\d+)")
            if (-not $fieldMatch.Success) {
                $rowBad.Add("$field=missing") | Out-Null
                continue
            }
            $value = [int]$fieldMatch.Groups[1].Value
            if ($value -ne 0) {
                $rowBad.Add("$field=$value") | Out-Null
            }
        }

        ++$validated
        if ($rowBad.Count -gt 0) {
            $sampleLine = Select-String `
                -Path $LogPath `
                -Pattern "PERF_SPARSE_TERRAIN_CRITICAL_NONREADY frame=$frame " |
                Select-Object -First 1
            $sampleText = if ($sampleLine) { " | $($sampleLine.Line)" } else { "" }
            $badRows.Add("frame ${frame}: $($rowBad -join ', ')$sampleText") | Out-Null
        }
    }

    if ($validated -eq 0) {
        Write-Info "Terrain-critical readiness check skipped: no rows at or after ready frame $effectiveReadyFrame."
        return
    }
    if ($badRows.Count -gt 0) {
        Write-Host "[ERROR] Terrain-critical readiness contract failed after protected publish:" -ForegroundColor Red
        $badRows | Select-Object -First 20 | ForEach-Object {
            Write-Host "  $_" -ForegroundColor Red
        }
        exit 16
    }

    Write-Info "Terrain-critical readiness check observed: samples=$validated readyFrame=$effectiveReadyFrame postNonReady=0"
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
    $totalGpuApplyCompleted = 0
    $totalGpuApplySubmitted = 0
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
        $gpuApplyMatch = [regex]::Match($line, "gpuApply=(\d+)/(\d+)")
        if ($gpuApplyMatch.Success) {
            $totalGpuApplyCompleted += [int]$gpuApplyMatch.Groups[1].Value
            $totalGpuApplySubmitted += [int]$gpuApplyMatch.Groups[2].Value
        }
        if ([int]$budgetMatch.Groups[1].Value -le 0 -or [int]$budgetMatch.Groups[2].Value -le 0) {
            $badRows.Add("frame $($frameMatch.Groups[1].Value): invalid physics budget") | Out-Null
        }
    }

    if ($validated -eq 0) {
        Write-Host "[ERROR] No valid PERF_SPARSE_PHYSICS rows found" -ForegroundColor Red
        exit 17
    }
    if ($RequireMovement -and $totalMoved -eq 0 -and $totalGpuApplyCompleted -eq 0) {
        Write-Host "[ERROR] Sparse physics diagnostic seed produced no CPU or GPU-applied voxel movement" -ForegroundColor Red
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

$minExitAfterFrames = Assert-CaptureParameters `
    -ExitAfterFrames $ExitAfterFrames `
    -CaptureStartFrame $CaptureStartFrame `
    -CaptureIntervalFrames $CaptureIntervalFrames `
    -CaptureCount $CaptureCount

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss-fff"
    $OutputDir = Join-Path $buildDir "captures\engine_sparse_$stamp"
}
Assert-SafeCaptureOutputDir -ResolvedOutputDir $OutputDir -ProjectRoot $projectRoot -BuildDir $buildDir
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).ProviderPath
Clear-EngineCaptureArtifacts -ResolvedOutputDir $OutputDir

if (-not $NoBuild) {
    Write-Step "Building latest code..."
    if ($Clean) { & $buildScript -Config $Config -Clean } else { & $buildScript -Config $Config }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Build"
} else {
    Write-Info "Build step: skipped (-NoBuild)"
}

if ($ExitAfterFrames -lt $minExitAfterFrames) {
    Write-Info "ExitAfterFrames=$ExitAfterFrames is too short for $CaptureCount captures; raising to $minExitAfterFrames"
    $ExitAfterFrames = $minExitAfterFrames
}
if ($SparseBrushPaintSmoke) {
    $brushSmokeMinExit = $SparseBrushPaintEndFrame + 185
    if ($ExitAfterFrames -lt $brushSmokeMinExit) {
        Write-Info "ExitAfterFrames=$ExitAfterFrames is too short for sparse brush paint smoke settle; raising to $brushSmokeMinExit"
        $ExitAfterFrames = $brushSmokeMinExit
    }
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
    VENPOD_SPARSE_DEBUG_MODE = $env:VENPOD_SPARSE_DEBUG_MODE
    VENPOD_ENABLE_TEST_MODES = $env:VENPOD_ENABLE_TEST_MODES
    VENPOD_BOUNDARY_TEST = $env:VENPOD_BOUNDARY_TEST
    VENPOD_SPARSE_STRESS_REQUESTS = $env:VENPOD_SPARSE_STRESS_REQUESTS
    VENPOD_SPARSE_STRESS_CAMERA = $env:VENPOD_SPARSE_STRESS_CAMERA
    VENPOD_SPARSE_STRESS_CAMERA_RADIUS = $env:VENPOD_SPARSE_STRESS_CAMERA_RADIUS
    VENPOD_SPARSE_STRESS_CAMERA_HEIGHT = $env:VENPOD_SPARSE_STRESS_CAMERA_HEIGHT
    VENPOD_SPARSE_STRESS_CAMERA_BASE_HEIGHT = $env:VENPOD_SPARSE_STRESS_CAMERA_BASE_HEIGHT
    VENPOD_SPARSE_STRESS_CAMERA_SPEED = $env:VENPOD_SPARSE_STRESS_CAMERA_SPEED
    VENPOD_SPARSE_STRESS_CAMERA_WATER_ANCHOR = $env:VENPOD_SPARSE_STRESS_CAMERA_WATER_ANCHOR
    VENPOD_SPARSE_WALK_TEST = $env:VENPOD_SPARSE_WALK_TEST
    VENPOD_SPARSE_WALK_TEST_SPEED = $env:VENPOD_SPARSE_WALK_TEST_SPEED
    VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC = $env:VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC
    VENPOD_SPARSE_WALK_TEST_PITCH_DEG = $env:VENPOD_SPARSE_WALK_TEST_PITCH_DEG
    VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS = $env:VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS
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
    VENPOD_SPARSE_PHYSICS_GPU_APPLY = $env:VENPOD_SPARSE_PHYSICS_GPU_APPLY
    VENPOD_SPARSE_PHYSICS_GPU_STRICT = $env:VENPOD_SPARSE_PHYSICS_GPU_STRICT
    VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_SEED = $env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_SEED
    VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_MATERIAL_SEED = $env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_MATERIAL_SEED
    VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_FLUID_SEED = $env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_FLUID_SEED
    VENPOD_SPARSE_BRUSH_FEEDBACK = $env:VENPOD_SPARSE_BRUSH_FEEDBACK
    VENPOD_SPARSE_BRUSH_FEEDBACK_APPLY = $env:VENPOD_SPARSE_BRUSH_FEEDBACK_APPLY
    VENPOD_SPARSE_BRUSH_FEEDBACK_AUTHORITATIVE = $env:VENPOD_SPARSE_BRUSH_FEEDBACK_AUTHORITATIVE
    VENPOD_SPARSE_BRUSH_FEEDBACK_STRICT_RESIDENT_ONLY = $env:VENPOD_SPARSE_BRUSH_FEEDBACK_STRICT_RESIDENT_ONLY
    VENPOD_SPARSE_BRUSH_PAINT_SMOKE = $env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE
    VENPOD_SPARSE_BRUSH_PAINT_MOVING_SMOKE = $env:VENPOD_SPARSE_BRUSH_PAINT_MOVING_SMOKE
    VENPOD_SPARSE_BRUSH_PAINT_NONRESIDENT_SMOKE = $env:VENPOD_SPARSE_BRUSH_PAINT_NONRESIDENT_SMOKE
    VENPOD_SPARSE_BRUSH_PAINT_SMOKE_START_FRAME = $env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE_START_FRAME
    VENPOD_SPARSE_BRUSH_PAINT_SMOKE_END_FRAME = $env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE_END_FRAME
    VENPOD_BRUSH_RADIUS_TENTHS = $env:VENPOD_BRUSH_RADIUS_TENTHS
    VENPOD_DISABLE_BRUSH_INPUT = $env:VENPOD_DISABLE_BRUSH_INPUT
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
    $stressCameraCapture = $StressCamera -or $SkylineReview
    if ($SkylineReview) {
        if (-not $PSBoundParameters.ContainsKey("StressCameraRadius")) { $StressCameraRadius = 900 }
        if (-not $PSBoundParameters.ContainsKey("StressCameraHeight")) { $StressCameraHeight = 24 }
        if (-not $PSBoundParameters.ContainsKey("StressCameraBaseHeight")) { $StressCameraBaseHeight = 160 }
        if (-not $PSBoundParameters.ContainsKey("StressCameraSpeed")) { $StressCameraSpeed = 18 }
    }
    if ($SkipOwnershipDiagnostics -and (
        $MaxHeightProxyPct -ge 0.0 -or
        $MaxHeightProxyScreenPct -ge 0.0 -or
        $MaxValleyAtmosphereScreenPct -ge 0.0 -or
        $MaxFarSvoScreenPct -ge 0.0 -or
        $MaxFarWaterScreenPct -ge 0.0 -or
        $MaxLodParentHeldPct -ge 0.0 -or
        $MaxSkyPct -ge 0.0 -or
        $MinFarSvoPct -ge 0.0 -or
        $MaxFarSurfacePct -ge 0.0 -or
        $MaxOwnershipMissPct -ge 0.0 -or
        $MaxTemporalOwnershipLayerDeltaPct -ge 0.0 -or
        $MaxTemporalOwnershipMissDeltaPct -ge 0.0 -or
        $MaxTemporalOwnershipUnsafeDeltaPct -ge 0.0)) {
        throw "-SkipOwnershipDiagnostics cannot be combined with ownership/layer gates"
    }
    $env:VENPOD_LOG_FILE = "1"
    if ($ShowDiagnostics) {
        $env:VENPOD_DIAGNOSTICS = "1"
    } else {
        Remove-Item "env:VENPOD_DIAGNOSTICS" -ErrorAction SilentlyContinue
    }
    $env:VENPOD_MODE = "sandbox"
    if ($SparsePhysics -or $SparseGpuPhysics -or $SparseGpuPhysicsStrict -or $SparsePhysicsDiagnosticSeed -or $SparsePhysicsDiagnosticFluidSeed) {
        Remove-Item "env:VENPOD_DISABLE_PHYSICS" -ErrorAction SilentlyContinue
        $env:VENPOD_ENABLE_SPARSE_PHYSICS = "1"
    } else {
        $env:VENPOD_DISABLE_PHYSICS = "1"
        Remove-Item "env:VENPOD_ENABLE_SPARSE_PHYSICS" -ErrorAction SilentlyContinue
    }
    if ($SparseGpuPhysics -or $SparseGpuPhysicsStrict) {
        $env:VENPOD_SPARSE_PHYSICS_GPU = "1"
    } else {
        Remove-Item "env:VENPOD_SPARSE_PHYSICS_GPU" -ErrorAction SilentlyContinue
    }
    if ($SparseGpuPhysicsStrict) {
        $env:VENPOD_SPARSE_PHYSICS_GPU_APPLY = "1"
        $env:VENPOD_SPARSE_PHYSICS_GPU_STRICT = "1"
    } else {
        Remove-Item "env:VENPOD_SPARSE_PHYSICS_GPU_APPLY" -ErrorAction SilentlyContinue
        Remove-Item "env:VENPOD_SPARSE_PHYSICS_GPU_STRICT" -ErrorAction SilentlyContinue
    }
    $env:VENPOD_ENABLE_EXPERIMENTAL_SPARSE = "1"
    $env:VENPOD_RENDER_BACKEND = "sparse"
    $env:VENPOD_SPARSE_RAYMARCH = "1"
    $env:VENPOD_SPARSE_ONLY = "1"
    $env:VENPOD_SPARSE_SURFACE_AUTHORITATIVE = "1"
    Remove-Item "env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_GATE" -ErrorAction SilentlyContinue
    Remove-Item "env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_FRAME" -ErrorAction SilentlyContinue
    Remove-Item "env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MAX_FRAME" -ErrorAction SilentlyContinue
    Remove-Item "env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_FAIL_OPEN_AT_MAX_FRAME" -ErrorAction SilentlyContinue
    Remove-Item "env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_READY_BRICKS" -ErrorAction SilentlyContinue
    Remove-Item "env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_TERRAIN_CRITICAL_BLOCKS" -ErrorAction SilentlyContinue
    Remove-Item "env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_SURFACE_PROOF" -ErrorAction SilentlyContinue
    Remove-Item "env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_SURFACE_FRAGMENTS" -ErrorAction SilentlyContinue
    Remove-Item "env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_FAR_SVO_PROOF" -ErrorAction SilentlyContinue
    Remove-Item "env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_PROOF" -ErrorAction SilentlyContinue
    Remove-Item "env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_MID_VOXEL_COVERAGE_PCT" -ErrorAction SilentlyContinue
    Remove-Item "env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_MID_VOXEL_WORST_RING_PCT" -ErrorAction SilentlyContinue
    Remove-Item "env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_HIDDEN_EXACT_PROOF" -ErrorAction SilentlyContinue
    Remove-Item "env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_HIDDEN_EXACT_BLOCKS" -ErrorAction SilentlyContinue
    Remove-Item "env:VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_HIDDEN_EXACT_CLEAN_FRAMES" -ErrorAction SilentlyContinue
    if ([string]::IsNullOrWhiteSpace($savedEnv.VENPOD_SPARSE_HIDDEN_EXACT_MISS_DURING_STARTUP)) {
        $env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_DURING_STARTUP = "1"
    } else {
        $env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_DURING_STARTUP =
            $savedEnv.VENPOD_SPARSE_HIDDEN_EXACT_MISS_DURING_STARTUP
    }
    if ([string]::IsNullOrWhiteSpace($savedEnv.VENPOD_SPARSE_STARTUP_HIDDEN_EXACT_REPAIR_BLOCKS)) {
        # Keep hidden-exact repair as startup prewarm by default. Strict repair
        # blocking is useful for probes but can stall public open while new
        # hidden-exact candidates keep streaming in.
        $env:VENPOD_SPARSE_STARTUP_HIDDEN_EXACT_REPAIR_BLOCKS = "0"
    } else {
        $env:VENPOD_SPARSE_STARTUP_HIDDEN_EXACT_REPAIR_BLOCKS =
            $savedEnv.VENPOD_SPARSE_STARTUP_HIDDEN_EXACT_REPAIR_BLOCKS
    }
    Remove-Item "env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_WARMUP_FRAME" -ErrorAction SilentlyContinue
    Remove-Item "env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_CLEAN_IDLE_FRAMES" -ErrorAction SilentlyContinue
    $env:VENPOD_SPARSE_DEBUG_MODE = "$SparseDebugMode"
    if ($EnableBrushInput) {
        Remove-Item "env:VENPOD_DISABLE_BRUSH_INPUT" -ErrorAction SilentlyContinue
    } else {
        $env:VENPOD_DISABLE_BRUSH_INPUT = "1"
    }
    if ($SparseBrushFeedback -or $SparseBrushPaintSmoke) {
        $env:VENPOD_SPARSE_BRUSH_FEEDBACK = "1"
    } else {
        Remove-Item "env:VENPOD_SPARSE_BRUSH_FEEDBACK" -ErrorAction SilentlyContinue
    }
    if ($SparseBrushPaintSmoke) {
        $env:VENPOD_SPARSE_BRUSH_FEEDBACK_APPLY = "1"
        $env:VENPOD_SPARSE_BRUSH_FEEDBACK_AUTHORITATIVE = "1"
        $env:VENPOD_SPARSE_BRUSH_FEEDBACK_STRICT_RESIDENT_ONLY = "1"
        $env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE = "1"
        $env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE_START_FRAME = "$SparseBrushPaintStartFrame"
        $env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE_END_FRAME = "$SparseBrushPaintEndFrame"
    } else {
        Remove-Item "env:VENPOD_SPARSE_BRUSH_FEEDBACK_APPLY" -ErrorAction SilentlyContinue
        Remove-Item "env:VENPOD_SPARSE_BRUSH_FEEDBACK_AUTHORITATIVE" -ErrorAction SilentlyContinue
        Remove-Item "env:VENPOD_SPARSE_BRUSH_FEEDBACK_STRICT_RESIDENT_ONLY" -ErrorAction SilentlyContinue
        Remove-Item "env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE" -ErrorAction SilentlyContinue
        Remove-Item "env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE_START_FRAME" -ErrorAction SilentlyContinue
        Remove-Item "env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE_END_FRAME" -ErrorAction SilentlyContinue
    }
    if ($SparseBrushPaintSmoke -and $SparseBrushPaintMovingSmoke) {
        $env:VENPOD_SPARSE_BRUSH_PAINT_MOVING_SMOKE = "1"
    } else {
        Remove-Item "env:VENPOD_SPARSE_BRUSH_PAINT_MOVING_SMOKE" -ErrorAction SilentlyContinue
    }
    if ($SparseBrushPaintSmoke -and $SparseBrushPaintNonresidentSmoke) {
        $env:VENPOD_SPARSE_BRUSH_PAINT_NONRESIDENT_SMOKE = "1"
    } else {
        Remove-Item "env:VENPOD_SPARSE_BRUSH_PAINT_NONRESIDENT_SMOKE" -ErrorAction SilentlyContinue
    }
    if ($BrushRadiusTenths -gt 0) {
        $env:VENPOD_BRUSH_RADIUS_TENTHS = "$BrushRadiusTenths"
    } else {
        Remove-Item "env:VENPOD_BRUSH_RADIUS_TENTHS" -ErrorAction SilentlyContinue
    }
    if ($stressCameraCapture) {
        $env:VENPOD_SPARSE_STRESS_REQUESTS = "1"
        $env:VENPOD_SPARSE_STRESS_CAMERA = "1"
        $env:VENPOD_SPARSE_STRESS_CAMERA_RADIUS = "$StressCameraRadius"
        $env:VENPOD_SPARSE_STRESS_CAMERA_HEIGHT = "$StressCameraHeight"
        $env:VENPOD_SPARSE_STRESS_CAMERA_BASE_HEIGHT = "$StressCameraBaseHeight"
        $env:VENPOD_SPARSE_STRESS_CAMERA_SPEED = "$StressCameraSpeed"
        $env:VENPOD_SPARSE_STRESS_CAMERA_ABSOLUTE_FRAME = "1"
        if ($WaterlineCamera) {
            $env:VENPOD_SPARSE_STRESS_CAMERA_WATER_ANCHOR = "1"
        } else {
            Remove-Item "env:VENPOD_SPARSE_STRESS_CAMERA_WATER_ANCHOR" -ErrorAction SilentlyContinue
        }
    }
    if ($WalkTest) {
        $env:VENPOD_SPARSE_WALK_TEST = "1"
        $env:VENPOD_SPARSE_WALK_TEST_SPEED = "$WalkTestSpeed"
        $env:VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC = "$WalkTestYawDegPerSec"
        $env:VENPOD_SPARSE_WALK_TEST_PITCH_DEG = "$WalkTestPitchDeg"
        if ($WalkTestFixedDtMs -gt 0) {
            $env:VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS = "$WalkTestFixedDtMs"
        } else {
            Remove-Item "env:VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS" -ErrorAction SilentlyContinue
        }
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
    if ($SparsePhysicsDiagnosticSeed) {
        $env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_MATERIAL_SEED = "1"
    } else {
        Remove-Item "env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_MATERIAL_SEED" -ErrorAction SilentlyContinue
    }
    if ($SparsePhysicsDiagnosticFluidSeed) {
        $env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_FLUID_SEED = "1"
    } else {
        Remove-Item "env:VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_FLUID_SEED" -ErrorAction SilentlyContinue
    }
    $env:VENPOD_SPARSE_REQUIRE_PIPE_READY = "1"
    $env:VENPOD_SPARSE_PIPE_READY_FRAME = "120"
    if ($SkipOwnershipDiagnostics) {
        Remove-Item "env:VENPOD_SPARSE_RENDER_OWNERSHIP" -ErrorAction SilentlyContinue
        Remove-Item "env:VENPOD_SPARSE_RENDER_OWNERSHIP_INTERVAL" -ErrorAction SilentlyContinue
        Remove-Item "env:VENPOD_SPARSE_REQUIRE_OWNERSHIP_QUALITY" -ErrorAction SilentlyContinue
        Remove-Item "env:VENPOD_SPARSE_OWNERSHIP_QUALITY_READY_FRAME" -ErrorAction SilentlyContinue
        Remove-Item "env:VENPOD_SPARSE_MIN_TERRAIN_PIXELS_PCT" -ErrorAction SilentlyContinue
        Remove-Item "env:VENPOD_SPARSE_MAX_MISS_PIXELS_PCT" -ErrorAction SilentlyContinue
        Remove-Item "env:VENPOD_SPARSE_REQUIRE_OWNERSHIP_STABILITY" -ErrorAction SilentlyContinue
        Remove-Item "env:VENPOD_SPARSE_OWNERSHIP_STABILITY_READY_FRAME" -ErrorAction SilentlyContinue
        Remove-Item "env:VENPOD_SPARSE_MAX_TERRAIN_DELTA_PCT" -ErrorAction SilentlyContinue
        Remove-Item "env:VENPOD_SPARSE_MAX_MISS_DELTA_PCT" -ErrorAction SilentlyContinue
    } else {
        $env:VENPOD_SPARSE_RENDER_OWNERSHIP = "1"
        # Ownership readback uses full-screen pixel-shader atomics. Sampling it
        # every frame can become the validation bottleneck once public sparse
        # raymarching opens; every 10 frames still gates the same ownership
        # quality/stability contract without forcing a diagnostic TDR.
        $env:VENPOD_SPARSE_RENDER_OWNERSHIP_INTERVAL = "10"
        $env:VENPOD_SPARSE_REQUIRE_OWNERSHIP_QUALITY = "1"
        $env:VENPOD_SPARSE_OWNERSHIP_QUALITY_READY_FRAME = "120"
        $env:VENPOD_SPARSE_MIN_TERRAIN_PIXELS_PCT = "$SparseMinTerrainPixelsPct"
        $env:VENPOD_SPARSE_MAX_MISS_PIXELS_PCT = "$SparseMaxMissPixelsPct"
        $env:VENPOD_SPARSE_REQUIRE_OWNERSHIP_STABILITY = "1"
        $env:VENPOD_SPARSE_OWNERSHIP_STABILITY_READY_FRAME = "$SparseOwnershipStabilityReadyFrame"
        $env:VENPOD_SPARSE_MAX_TERRAIN_DELTA_PCT = "$SparseOwnershipMaxTerrainDeltaPct"
        $env:VENPOD_SPARSE_MAX_MISS_DELTA_PCT = "$SparseOwnershipMaxMissDeltaPct"
    }
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
    Write-Info "Visual mode: debug=$SparseDebugMode stressCamera=$([int]$StressCamera.IsPresent) skylineReview=$([int]$SkylineReview.IsPresent) waterlineCamera=$([int]$WaterlineCamera.IsPresent) walkTest=$([int]$WalkTest.IsPresent) boundaryTest=$([int]$BoundaryTest.IsPresent) diagnostics=$([int]$ShowDiagnostics.IsPresent) ownershipDiagnostics=$([int](-not $SkipOwnershipDiagnostics.IsPresent)) brushInput=$([int]$EnableBrushInput.IsPresent) brushPaint=$([int]$SparseBrushPaintSmoke.IsPresent) brushRadiusTenths=$BrushRadiusTenths"
    if ($stressCameraCapture) {
        Write-Info "Stress camera: radius=$StressCameraRadius height=$StressCameraHeight baseHeight=$StressCameraBaseHeight speed=$StressCameraSpeed"
    }
    if ($WalkTest) {
        Write-Info "Walk test: speed=$WalkTestSpeed yawDegPerSec=$WalkTestYawDegPerSec pitchDeg=$WalkTestPitchDeg fixedDtMs=$WalkTestFixedDtMs"
    }

    & $runScript -Config $Config
    Stop-OnFailure -Code $LASTEXITCODE -Stage "VENPOD engine capture run"

    $frames = Get-ChildItem -LiteralPath $OutputDir -Filter "engine_frame_*.bmp" -File | Sort-Object Name | ForEach-Object { $_.FullName }
    if ($frames.Count -lt $CaptureCount) {
        Write-Host "[ERROR] Expected $CaptureCount engine captures but found $($frames.Count) in $OutputDir" -ForegroundColor Red
        exit 13
    }

    $artifacts = Write-ImageArtifacts `
        -Frames $frames `
        -OutDir $OutputDir `
        -PreciseOverlayStats:($MinOverlayBrushPct -ge 0.0)
    $temporalStats = Write-TemporalArtifacts -Frames $frames -OutDir $OutputDir
    $runtimeLog = Join-Path $projectRoot "build\bin\venpod_runtime.log"
    if (Test-Path $runtimeLog) {
        Copy-Item -Path $runtimeLog -Destination (Join-Path $OutputDir "venpod_runtime.log") -Force
    }
    $ownershipTimeline = Write-OwnershipTimelineArtifacts `
        -LogPath (Join-Path $OutputDir "venpod_runtime.log") `
        -OutDir $OutputDir
    $compositionTimeline = Write-RenderCompositionTimelineArtifacts `
        -LogPath (Join-Path $OutputDir "venpod_runtime.log") `
        -OutDir $OutputDir
    $layerScreenTimeline = Write-LayerScreenTimelineArtifacts `
        -OwnershipTimelinePath $ownershipTimeline `
        -CompositionTimelinePath $compositionTimeline `
        -OutDir $OutputDir
    $temporalOwnershipReview = Write-TemporalOwnershipReviewArtifacts `
        -TemporalStatsPath $temporalStats `
        -OwnershipTimelinePath $ownershipTimeline `
        -OutDir $OutputDir
    $startupHeldVisualFrames = @()
    $runtimeLogForStartupHeld = Join-Path $OutputDir "venpod_runtime.log"
    if (Test-Path -LiteralPath $runtimeLogForStartupHeld) {
        $startupHeldLastFrame = -1
        Select-String -Path $runtimeLogForStartupHeld -Pattern "SPARSE_STARTUP_PUBLIC_RENDER_HELD frame=(\d+)" |
            ForEach-Object {
                if ($_.Line -match "SPARSE_STARTUP_PUBLIC_RENDER_HELD frame=(\d+)") {
                    $startupHeldLastFrame = [Math]::Max($startupHeldLastFrame, [int]$Matches[1])
                }
            }
        if ($startupHeldLastFrame -ge 0) {
            foreach ($framePath in $frames) {
                $leaf = Split-Path $framePath -Leaf
                if ($leaf -match "engine_frame_(\d+)\.bmp") {
                    $frameNumber = [int]$Matches[1]
                    if ($frameNumber -le ($startupHeldLastFrame + 1)) {
                        $startupHeldVisualFrames += $frameNumber
                    }
                }
            }
            if ($startupHeldVisualFrames.Count -gt 0) {
                Write-Info ("Visual checks: ignoring startup-held loading frames {0}" -f (($startupHeldVisualFrames | Sort-Object -Unique) -join ","))
            }
        }
    }
    Test-ImageStats `
        -StatsPath $artifacts.Stats `
        -StressCamera:$stressCameraCapture `
        -MinUniqueSampleColors $MinUniqueSampleColors `
        -MinAverageSkyLikePct $MinAverageSkyLikePct `
        -MaxFrameDarkPct $MaxFrameDarkPct `
        -MaxAverageTopTerrainPct $MaxAverageTopTerrainPct `
        -MaxFrameTopTerrainPct $MaxFrameTopTerrainPct `
        -MaxAverageBrushDomeLikePct $MaxAverageBrushDomeLikePct `
        -MaxFrameBrushDomeLikePct $MaxFrameBrushDomeLikePct `
        -MinOverlayBrushPct $MinOverlayBrushPct `
        -MaxUiContaminationPct $MaxUiContaminationPct `
        -MaxMaterialSandPct $MaxMaterialSandPct `
        -MaxMaterialStonePct $MaxMaterialStonePct `
        -MinMaterialDirtPct $MinMaterialDirtPct `
        -MaxSkylineFlatRunPct $MaxSkylineFlatRunPct `
        -MaxSkylineStepPct $MaxSkylineStepPct `
        -MaxSkylineInteriorSkyPct $MaxSkylineInteriorSkyPct `
        -MaxSkylineInteriorSkyRunPct $MaxSkylineInteriorSkyRunPct `
        -IgnoreFrameNumbers $startupHeldVisualFrames `
        -AllowDebugColorTerrainClassifier:($SparseDebugMode -ne 0) `
        -AllowLowSkyCoverage:($ShowDiagnostics -or $WalkTest -or $SkylineReview -or $SparseDebugMode -ne 0)
    Test-TemporalStats `
        -StatsPath $temporalStats `
        -MaxTemporalChangedPct $MaxTemporalChangedPct `
        -MaxTemporalLargeChangePct $MaxTemporalLargeChangePct `
        -MaxTemporalCenterChangedPct $MaxTemporalCenterChangedPct `
        -MaxTemporalMeanLumaDelta $MaxTemporalMeanLumaDelta `
        -MaxTemporalP95LumaDelta $MaxTemporalP95LumaDelta
    if (-not $SkipOwnershipDiagnostics) {
        Test-TemporalOwnershipReview `
            -ReviewPath $temporalOwnershipReview `
            -MaxTemporalOwnershipLayerDeltaPct $MaxTemporalOwnershipLayerDeltaPct `
            -MaxTemporalOwnershipMissDeltaPct $MaxTemporalOwnershipMissDeltaPct `
            -MaxTemporalOwnershipUnsafeDeltaPct $MaxTemporalOwnershipUnsafeDeltaPct
    }

    $badLogLines = Select-String `
        -Path (Join-Path $OutputDir "venpod_runtime.log") `
        -Pattern "\] \[(critical|error)\]|device removed|device-removed|timeout|SPARSE_BACKEND_PIPE readiness failed|SPARSE_RENDER_OWNERSHIP quality failed|SPARSE_RENDER_OWNERSHIP stability failed" `
        -CaseSensitive:$false
    if ($badLogLines) {
        Write-Host "[ERROR] Engine capture smoke found runtime failure markers:" -ForegroundColor Red
        $badLogLines | Select-Object -First 20 | ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor Red }
        exit 14
    }
    if (-not $SkipOwnershipDiagnostics) {
        Test-RenderOwnershipStats `
            -LogPath (Join-Path $OutputDir "venpod_runtime.log") `
            -StatsPath $artifacts.Stats `
            -ReadyFrame $CaptureStartFrame `
            -MaxHeightProxyPct $MaxHeightProxyPct `
            -MaxSkyPct $MaxSkyPct `
            -MinFarSvoPct $MinFarSvoPct
        Test-LodParentHeldStats `
            -OwnershipTimelinePath $ownershipTimeline `
            -ReadyFrame $CaptureStartFrame `
            -MaxLodParentHeldPct $MaxLodParentHeldPct
        Test-FarSurfaceStats `
            -OwnershipTimelinePath $ownershipTimeline `
            -ReadyFrame $CaptureStartFrame `
            -MaxFarSurfacePct $MaxFarSurfacePct
        Test-OwnershipMissStats `
            -OwnershipTimelinePath $ownershipTimeline `
            -ReadyFrame $CaptureStartFrame `
            -MaxOwnershipMissPct $MaxOwnershipMissPct
    } else {
        Write-Info "Ownership checks: skipped (-SkipOwnershipDiagnostics)"
    }
    Test-RenderCompositionStats `
        -CompositionPath $compositionTimeline `
        -ReadyFrame $CaptureStartFrame `
        -MinSurfaceScreenPct $MinSurfaceScreenPct `
        -MaxSurfaceScreenPct $MaxSurfaceScreenPct `
        -MaxBackgroundScreenPct $MaxBackgroundScreenPct
    if (-not $SkipOwnershipDiagnostics) {
        Test-LayerScreenStats `
            -LayerScreenPath $layerScreenTimeline `
            -ReadyFrame $CaptureStartFrame `
            -MaxHeightProxyScreenPct $MaxHeightProxyScreenPct `
            -MaxValleyAtmosphereScreenPct $MaxValleyAtmosphereScreenPct `
            -MaxFarSvoScreenPct $MaxFarSvoScreenPct `
            -MaxFarWaterScreenPct $MaxFarWaterScreenPct
    }
    Test-RenderPerformanceStats `
        -LogPath (Join-Path $OutputDir "venpod_runtime.log") `
        -ReadyFrame $CaptureStartFrame `
        -MaxFrameMs $MaxFrameMs `
        -MaxPrepMs $MaxPrepMs `
        -MaxGpuRayMs $MaxGpuRayMs `
        -CaptureStartFrame $CaptureStartFrame `
        -CaptureIntervalFrames $CaptureIntervalFrames `
        -CaptureCount $CaptureCount
    Test-TerrainCriticalReadinessStats `
        -LogPath (Join-Path $OutputDir "venpod_runtime.log") `
        -ReadyFrame $CaptureStartFrame
    Test-SparseSurfaceRuntimeStats `
        -LogPath (Join-Path $OutputDir "venpod_runtime.log") `
        -AllowGpuCullDisabled:$AllowGpuCullDisabled
    if ($SparsePhysics -or $SparseGpuPhysics -or $SparseGpuPhysicsStrict -or $SparsePhysicsDiagnosticSeed -or $SparsePhysicsDiagnosticFluidSeed) {
        Test-SparsePhysicsRuntimeStats `
            -LogPath (Join-Path $OutputDir "venpod_runtime.log") `
            -RequireMovement:($SparsePhysicsDiagnosticSeed -or $SparsePhysicsDiagnosticFluidSeed) `
            -RequireGpuPackets:($SparseGpuPhysics -or $SparseGpuPhysicsStrict)
    }

    Write-Success "Engine capture smoke passed."
    Write-Info "Contact sheet: $($artifacts.ContactSheet)"
    Write-Info "Stats: $($artifacts.Stats)"
    Write-Info "Temporal stats: $temporalStats"
    Write-Info "Ownership timeline: $ownershipTimeline"
    Write-Info "Composition timeline: $compositionTimeline"
    Write-Info "Layer screen timeline: $layerScreenTimeline"
    Write-Info "Temporal ownership review: $temporalOwnershipReview"
    $temporalPeakSheet = Join-Path $OutputDir "temporal_peaks.png"
    if (Test-Path -LiteralPath $temporalPeakSheet) {
        Write-Info "Temporal peaks: $temporalPeakSheet"
    }
    exit 0
} finally {
    Restore-Env
}
