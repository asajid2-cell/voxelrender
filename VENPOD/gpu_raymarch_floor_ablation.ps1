param(
    [string]$OutputDir = "build\captures\gpu_raymarch_floor_ablation_20260602",
    [switch]$NoBuild,
    [switch]$SkipRuns
)

$ErrorActionPreference = "Stop"
$Culture = [Globalization.CultureInfo]::InvariantCulture

function Write-Info {
    param([string]$Message)
    Write-Host "[gpu-ray] $Message" -ForegroundColor Cyan
}

function Get-LogLine {
    param([string[]]$Lines, [string]$Pattern)
    foreach ($line in $Lines) {
        if ($line -match $Pattern) { return $line }
    }
    return $null
}

function Get-Number {
    param([string]$Line, [string]$Pattern)
    if ([string]::IsNullOrWhiteSpace($Line)) { return $null }
    $m = [regex]::Match($Line, $Pattern)
    if (-not $m.Success) { return $null }
    return [double]::Parse($m.Groups[1].Value, $Culture)
}

function Get-Int {
    param([string]$Line, [string]$Pattern)
    if ([string]::IsNullOrWhiteSpace($Line)) { return $null }
    $m = [regex]::Match($Line, $Pattern)
    if (-not $m.Success) { return $null }
    return [int]::Parse($m.Groups[1].Value, $Culture)
}

function Get-Text {
    param([string]$Line, [string]$Pattern)
    if ([string]::IsNullOrWhiteSpace($Line)) { return $null }
    $m = [regex]::Match($Line, $Pattern)
    if (-not $m.Success) { return $null }
    return $m.Groups[1].Value
}

function Get-SlashNumbers {
    param([string]$Line, [string]$Prefix, [int]$Count)
    if ([string]::IsNullOrWhiteSpace($Line)) { return @() }
    $pattern = [regex]::Escape($Prefix) + "([0-9.\/]+)"
    $m = [regex]::Match($Line, $pattern)
    if (-not $m.Success) { return @() }
    $parts = $m.Groups[1].Value.Split("/")
    $values = @()
    for ($i = 0; $i -lt [Math]::Min($Count, $parts.Count); $i++) {
        $values += [double]::Parse($parts[$i], $Culture)
    }
    return $values
}

function Format-Nullable {
    param($Value)
    if ($Value -eq $null) { return "" }
    if ($Value -is [double]) { return $Value.ToString("0.##", $Culture) }
    return ($Value.ToString() -replace "\|", "/")
}

function Invoke-AblationRun {
    param(
        [object]$Scenario,
        [string]$ScenarioDir
    )

    New-Item -ItemType Directory -Force -Path $ScenarioDir | Out-Null
    $runtimeLog = Join-Path $PSScriptRoot "build\bin\venpod_runtime.log"
    if (Test-Path $runtimeLog) {
        Remove-Item -LiteralPath $runtimeLog -Force
    }

    $env:VENPOD_RAYMARCH_RENDER_SCALE = $Scenario.scale.ToString("0.###", $Culture)
    $env:VENPOD_RAYMARCH_MAX_STEPS_SCALE = $Scenario.stepScale.ToString("0.###", $Culture)
    $env:VENPOD_RAYMARCH_MAX_DISTANCE_SCALE = $Scenario.distanceScale.ToString("0.###", $Culture)
    $env:VENPOD_VSYNC = "0"
    $env:VENPOD_SPARSE_SURFACE_PROMOTION_POLICY = $Scenario.promotionPolicy
    if ($Scenario.promotionPolicy -eq "bounded_repair") {
        $env:VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_DEMOTE_BOUND = "64"
        $env:VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE = "12"
    } else {
        Remove-Item env:VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_DEMOTE_BOUND -ErrorAction SilentlyContinue
        Remove-Item env:VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE -ErrorAction SilentlyContinue
    }

    Remove-Item env:VENPOD_ENABLE_FAR_SVO -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_MID_VOXEL_RENDER -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_MID_CLIPMAP -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_RAYMARCH_MAX_SCALE_PERCENT -ErrorAction SilentlyContinue
    foreach ($kvp in $Scenario.env.GetEnumerator()) {
        Set-Item -Path "env:$($kvp.Key)" -Value $kvp.Value
    }

    $runArgs = @{
        Config = "Release"
        NoBuild = $true
        DisablePhysics = $true
        SparseOwnershipInterval = 1
        ExitAfterFrames = [int]$Scenario.exitFrame
    }
    if ($Scenario.highAlt) {
        $runArgs.SparseStressCamera = $true
    }

    Write-Info "Run $($Scenario.name): scale=$($Scenario.scale) step=$($Scenario.stepScale) policy=$($Scenario.promotionPolicy) highAlt=$($Scenario.highAlt)"
    & .\rebrun.ps1 @runArgs *> (Join-Path $ScenarioDir "run_stdout.txt")
    $exitCode = $LASTEXITCODE
    if (Test-Path $runtimeLog) {
        Copy-Item -LiteralPath $runtimeLog -Destination (Join-Path $ScenarioDir "venpod_runtime.log") -Force
    }
    Set-Content -Path (Join-Path $ScenarioDir "exit_code.txt") -Value "$exitCode" -Encoding UTF8
    if ($exitCode -ne 0) {
        Write-Host "[gpu-ray] scenario $($Scenario.name) exited with $exitCode" -ForegroundColor Yellow
    }
}

function Convert-ScenarioLog {
    param([object]$Scenario, [string]$ScenarioDir)

    $logPath = Join-Path $ScenarioDir "venpod_runtime.log"
    if (-not (Test-Path $logPath)) {
        throw "Missing runtime log for $($Scenario.name): $logPath"
    }
    $lines = Get-Content $logPath
    $frame = $Scenario.sampleFrame
    $configLine = Get-LogLine $lines "RAYMARCH_FLOOR_CONFIG renderScale="
    $budgetLine = Get-LogLine $lines "RAYMARCH_FLOOR_CONFIG maxDistanceScale="
    $perfLine = Get-LogLine $lines "PERF frame=$frame\b"
    $frameEndLine = Get-LogLine $lines "PERF_FRAME_END frame=$frame\b"
    $cameraLine = Get-LogLine $lines "PERF_CAMERA_EXPOSURE frame=$frame\b"
    $policyLine = Get-LogLine $lines "PERF_SPARSE_SURFACE_PROMOTION_POLICY frame=$frame\b"
    $sparseLine = Get-LogLine $lines "PERF_SPARSE frame=$frame\b"
    $ownershipLine = Get-LogLine $lines "PERF_RENDER_OWNERSHIP retireFrame=\d+ shaderFrame=$frame\b"
    $compositionLine = Get-LogLine $lines "PERF_RENDER_COMPOSITION frame=$frame\b"

    $gpu = Get-SlashNumbers $perfLine "gpu=frame/upload/pre/surface/ray/overlay/ui:" 7
    $sparsePost = Get-SlashNumbers $frameEndLine "sparsePost=feedback/cmd/begin/midSnap/plan/upload/publish/midUpload/stats/surfExtract/surfPlan/surfSnap/surfStage/surfEmit:" 14
    $feedbackSplit = Get-SlashNumbers $frameEndLine "feedbackSplit=legacy/raycast/miss/brush/own/phys:" 6

    $outputWidth = Get-Int $configLine "output=(\d+)x\d+"
    $outputHeight = Get-Int $configLine "output=\d+x(\d+)"
    $screen = Get-Number $compositionLine "screen=([0-9.]+)"
    if ($screen -eq $null -and $outputWidth -and $outputHeight) {
        $screen = [double]($outputWidth * $outputHeight)
    }
    $rayPixels = if ($outputWidth -and $outputHeight) { [int64]$outputWidth * [int64]$outputHeight } else { $null }

    $midPixels = Get-Number $ownershipLine "midVoxel=([0-9.]+)"
    $farSvoPixels = Get-Number $ownershipLine "farSvo=([0-9.]+)"
    $missPixels = Get-Number $ownershipLine "miss=([0-9.]+)"
    $unsafePixels = Get-Number $ownershipLine "unsafeNearMiss=([0-9.]+)"
    $surfacePixels = Get-Number $compositionLine "surfaceOwnedPixels=([0-9.]+)"
    $backgroundPixels = Get-Number $compositionLine "backgroundPixels=([0-9.]+)"

    $gpuRayMs = if ($gpu.Count -ge 5) { $gpu[4] } else { $null }
    $gpuUploadMs = if ($gpu.Count -ge 2) { $gpu[1] } else { $null }
    $sparseUploadMs = if ($sparsePost.Count -ge 8) { $sparsePost[5] + $sparsePost[7] } else { $null }
    $uploadMs = if ($gpuUploadMs -ne $null -and $sparseUploadMs -ne $null) { $gpuUploadMs + $sparseUploadMs } else { $null }
    $readbackMs = if ($sparsePost.Count -ge 1 -and $feedbackSplit.Count -ge 6) {
        $v = $sparsePost[0]
        foreach ($x in $feedbackSplit) { $v += $x }
        $v
    } else {
        $null
    }
    $rayMpix = if ($rayPixels) { [double]$rayPixels / 1000000.0 } else { $null }
    $gpuRayPerMpix = if ($gpuRayMs -ne $null -and $rayMpix -gt 0.0) { $gpuRayMs / $rayMpix } else { $null }

    [pscustomobject]@{
        group = $Scenario.group
        scenario = $Scenario.name
        frame = $frame
        scale = [double]$Scenario.scale
        stepScale = [double]$Scenario.stepScale
        distanceScale = [double]$Scenario.distanceScale
        featureDisabled = $Scenario.featureDisabled
        outputWidth = $outputWidth
        outputHeight = $outputHeight
        rayPixels = $rayPixels
        promoted = Get-Int $cameraLine "surfacePromoted=(\d+)"
        surfaceRasterMax = Get-Number $cameraLine "surfaceRasterMax=([0-9.]+)"
        midVoxelScreenPct = if ($screen -gt 0 -and $midPixels -ne $null) { $midPixels * 100.0 / $screen } else { $null }
        farSvoScreenPct = if ($screen -gt 0 -and $farSvoPixels -ne $null) { $farSvoPixels * 100.0 / $screen } else { $null }
        exactSurfacePct = if ($screen -gt 0 -and $surfacePixels -ne $null) { $surfacePixels * 100.0 / $screen } else { $null }
        backgroundScreenPct = if ($screen -gt 0 -and $backgroundPixels -ne $null) { $backgroundPixels * 100.0 / $screen } else { $null }
        missScreenPct = if ($screen -gt 0 -and $missPixels -ne $null) { $missPixels * 100.0 / $screen } else { $null }
        unsafeNearMissScreenPct = if ($screen -gt 0 -and $unsafePixels -ne $null) { $unsafePixels * 100.0 / $screen } else { $null }
        bodyMs = Get-Number $frameEndLine "body=([0-9.]+)"
        rawMs = Get-Number $frameEndLine "rawMs=([0-9.]+)"
        gpuRayMs = $gpuRayMs
        gpuRayMsPerMpix = $gpuRayPerMpix
        cpuUpdateMs = Get-Number $perfLine "prep=([0-9.]+)"
        uploadMs = $uploadMs
        readbackMs = $readbackMs
        rayBudgetDistance = Get-Number $sparseLine "rayBudget=([0-9.]+)/"
        rayBudgetSteps = Get-Int $sparseLine "rayBudget=[0-9.]+/([0-9]+)"
        rayScaleLogged = Get-Number $sparseLine "rayScale=([0-9.]+)"
        bgQuality = Get-Number $sparseLine "bgQuality=([0-9.]+)"
        bgTier = Get-Int $sparseLine "bgTier=(\d+)"
        hiddenExactAccepted = Get-Int $cameraLine "hiddenExactAccepted=(\d+)"
        hiddenExactMissing = Get-Int $cameraLine "hiddenExactMissing=(\d+)/"
        promotionReason = Get-Text $policyLine "reason=([^ ]+)"
        notes = $Scenario.notes
    }
}

function Add-Speedups {
    param([object[]]$Rows)
    $groups = $Rows | Group-Object group
    $out = @()
    foreach ($group in $groups) {
        $baseline = $group.Group |
            Where-Object { [double]$_.scale -eq 1.0 -and [double]$_.stepScale -eq 1.0 -and [double]$_.distanceScale -eq 1.0 } |
            Select-Object -First 1
        if (-not $baseline) { $baseline = $group.Group | Select-Object -First 1 }
        foreach ($row in $group.Group) {
            $speedup = $null
            if ($baseline.gpuRayMs -ne $null -and $row.gpuRayMs -ne $null -and $row.gpuRayMs -gt 0.0) {
                $speedup = [double]$baseline.gpuRayMs / [double]$row.gpuRayMs
            }
            $out += $row | Select-Object *, @{Name="relativeGpuRaySpeedup"; Expression={ $speedup }}
        }
    }
    return $out
}

function ConvertTo-MarkdownTable {
    param([object[]]$Rows)
    $columns = @(
        "group","scenario","frame","scale","stepScale","featureDisabled",
        "outputWidth","outputHeight","rayPixels","promoted","surfaceRasterMax",
        "midVoxelScreenPct","farSvoScreenPct","exactSurfacePct","missScreenPct","unsafeNearMissScreenPct",
        "bodyMs","rawMs","gpuRayMs","gpuRayMsPerMpix","relativeGpuRaySpeedup","cpuUpdateMs","uploadMs","readbackMs",
        "rayBudgetDistance","rayBudgetSteps","rayScaleLogged","bgQuality","bgTier","hiddenExactAccepted","hiddenExactMissing","promotionReason","notes"
    )
    $lines = @()
    $lines += "|" + ($columns -join "|") + "|"
    $lines += "|" + (($columns | ForEach-Object { "---" }) -join "|") + "|"
    foreach ($row in $Rows) {
        $values = foreach ($column in $columns) { Format-Nullable $row.$column }
        $lines += "|" + ($values -join "|") + "|"
    }
    return $lines
}

$projectRoot = $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($OutputDir)) {
    $OutputDir = Join-Path $projectRoot $OutputDir
}
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
$buildCaptures = [System.IO.Path]::GetFullPath((Join-Path $projectRoot "build\captures"))
if (-not $OutputDir.StartsWith($buildCaptures, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing output outside build/captures: $OutputDir"
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$scenarioData = @(
    @{ name="res_strict_1_00"; group="resolution_strict"; scale=1.00; stepScale=1.00; distanceScale=1.00; featureDisabled="none"; promotionPolicy="strict"; highAlt=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="strict fixed native" },
    @{ name="res_strict_0_75"; group="resolution_strict"; scale=0.75; stepScale=1.00; distanceScale=1.00; featureDisabled="none"; promotionPolicy="strict"; highAlt=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="strict fixed resolution scale" },
    @{ name="res_strict_0_50"; group="resolution_strict"; scale=0.50; stepScale=1.00; distanceScale=1.00; featureDisabled="none"; promotionPolicy="strict"; highAlt=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="strict fixed resolution scale" },
    @{ name="res_strict_0_33"; group="resolution_strict"; scale=0.33; stepScale=1.00; distanceScale=1.00; featureDisabled="none"; promotionPolicy="strict"; highAlt=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="strict fixed resolution scale" },
    @{ name="res_bounded64_1_00"; group="resolution_bounded64"; scale=1.00; stepScale=1.00; distanceScale=1.00; featureDisabled="none"; promotionPolicy="bounded_repair"; highAlt=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="bounded64 comparison only" },
    @{ name="res_bounded64_0_50"; group="resolution_bounded64"; scale=0.50; stepScale=1.00; distanceScale=1.00; featureDisabled="none"; promotionPolicy="bounded_repair"; highAlt=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="bounded64 comparison only" },
    @{ name="step_strict_1_00"; group="step_strict"; scale=1.00; stepScale=1.00; distanceScale=1.00; featureDisabled="none"; promotionPolicy="strict"; highAlt=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="strict native max-step baseline" },
    @{ name="step_strict_0_75"; group="step_strict"; scale=1.00; stepScale=0.75; distanceScale=1.00; featureDisabled="max_steps_0_75"; promotionPolicy="strict"; highAlt=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="max steps scaled only" },
    @{ name="step_strict_0_50"; group="step_strict"; scale=1.00; stepScale=0.50; distanceScale=1.00; featureDisabled="max_steps_0_50"; promotionPolicy="strict"; highAlt=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="max steps scaled only" },
    @{ name="step_strict_0_25"; group="step_strict"; scale=1.00; stepScale=0.25; distanceScale=1.00; featureDisabled="max_steps_0_25"; promotionPolicy="strict"; highAlt=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="max steps scaled only" },
    @{ name="feature_far_svo_off"; group="feature_strict"; scale=1.00; stepScale=1.00; distanceScale=1.00; featureDisabled="far_svo"; promotionPolicy="strict"; highAlt=$false; exitFrame=260; sampleFrame=240; env=@{ VENPOD_ENABLE_FAR_SVO="0" }; notes="diagnostic only; Far-SVO disabled" },
    @{ name="feature_mid_voxel_off"; group="feature_strict"; scale=1.00; stepScale=1.00; distanceScale=1.00; featureDisabled="mid_voxel_render"; promotionPolicy="strict"; highAlt=$false; exitFrame=260; sampleFrame=240; env=@{ VENPOD_SPARSE_MID_VOXEL_RENDER="0" }; notes="diagnostic only; mid voxel render disabled" },
    @{ name="feature_quality_ceiling_50"; group="feature_strict"; scale=1.00; stepScale=1.00; distanceScale=1.00; featureDisabled="runtime_quality_ceiling_50"; promotionPolicy="strict"; highAlt=$false; exitFrame=260; sampleFrame=240; env=@{ VENPOD_SPARSE_RAYMARCH_MAX_SCALE_PERCENT="50" }; notes="diagnostic quality/step ceiling" },
    @{ name="highalt_res_1_00"; group="high_alt"; scale=1.00; stepScale=1.00; distanceScale=1.00; featureDisabled="none"; promotionPolicy="strict"; highAlt=$true; exitFrame=260; sampleFrame=240; env=@{}; notes="high-alt excluded path native" },
    @{ name="highalt_res_0_50"; group="high_alt"; scale=0.50; stepScale=1.00; distanceScale=1.00; featureDisabled="none"; promotionPolicy="strict"; highAlt=$true; exitFrame=260; sampleFrame=240; env=@{}; notes="high-alt excluded path half res" },
    @{ name="highalt_step_0_50"; group="high_alt"; scale=1.00; stepScale=0.50; distanceScale=1.00; featureDisabled="max_steps_0_50"; promotionPolicy="strict"; highAlt=$true; exitFrame=260; sampleFrame=240; env=@{}; notes="high-alt native res half steps" }
) | ForEach-Object { [pscustomobject]$_ }

$envNames = @(
    "VENPOD_RAYMARCH_RENDER_SCALE",
    "VENPOD_RAYMARCH_MAX_STEPS_SCALE",
    "VENPOD_RAYMARCH_MAX_DISTANCE_SCALE",
    "VENPOD_RENDER_QUALITY",
    "VENPOD_VSYNC",
    "VENPOD_SPARSE_SURFACE_PROMOTION_POLICY",
    "VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_DEMOTE_BOUND",
    "VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE",
    "VENPOD_ENABLE_FAR_SVO",
    "VENPOD_SPARSE_MID_VOXEL_RENDER",
    "VENPOD_SPARSE_MID_CLIPMAP",
    "VENPOD_SPARSE_RAYMARCH_MAX_SCALE_PERCENT"
)
$savedEnv = @{}
foreach ($name in $envNames) {
    $savedEnv[$name] = (Get-Item "env:$name" -ErrorAction SilentlyContinue).Value
}

try {
    if (-not $NoBuild -and -not $SkipRuns) {
        & .\build.ps1 -Config Release
        if ($LASTEXITCODE -ne 0) { throw "Build failed" }
    }
    if (-not $SkipRuns) {
        foreach ($scenario in $scenarioData) {
            $scenarioDir = Join-Path $OutputDir $scenario.name
            Invoke-AblationRun -Scenario $scenario -ScenarioDir $scenarioDir
        }
    }
} finally {
    foreach ($name in $envNames) {
        if ($savedEnv[$name] -eq $null) {
            Remove-Item "env:$name" -ErrorAction SilentlyContinue
        } else {
            Set-Item -Path "env:$name" -Value $savedEnv[$name]
        }
    }
}

$rows = foreach ($scenario in $scenarioData) {
    Convert-ScenarioLog -Scenario $scenario -ScenarioDir (Join-Path $OutputDir $scenario.name)
}
$rows = Add-Speedups $rows

$csvPath = Join-Path $OutputDir "raymarch_floor_summary.csv"
$mdPath = Join-Path $OutputDir "raymarch_floor_table.md"
$rows | Export-Csv -NoTypeInformation -Path $csvPath

$existingKnobs = @(
    'VENPOD_RAYMARCH_RENDER_SCALE default 1.0 (new; startup backbuffer/window ray-pixel scale)',
    'VENPOD_RENDER_QUALITY=playable (new alias for render scale 0.5 unless explicit scale is set)',
    'VENPOD_RAYMARCH_MAX_STEPS_SCALE default 1.0 (new; scales dense/sparse max-step budgets)',
    'VENPOD_RAYMARCH_MAX_DISTANCE_SCALE default 1.0 (new; scales dense/sparse max distance budgets)',
    'VENPOD_RAYMARCH_MAX_DISTANCE and VENPOD_RAYMARCH_MAX_STEPS (existing dense path)',
    'VENPOD_SPARSE_RAYMARCH_MAX_DISTANCE and VENPOD_SPARSE_RAYMARCH_MAX_STEPS (existing sparse path)',
    'VENPOD_SPARSE_RAYMARCH_MAX_SCALE_PERCENT (existing runtime quality ceiling)',
    'VENPOD_ENABLE_FAR_SVO, VENPOD_SPARSE_MID_VOXEL_RENDER, VENPOD_SPARSE_MID_CLIPMAP (existing diagnostic path toggles)'
)

$md = New-Object System.Collections.Generic.List[string]
$md.Add("# GPU Raymarch Floor Ablation")
$md.Add("")
$md.Add("Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
$md.Add("")
$md.Add("Audit name: ``gpu_raymarch_floor_ablation_20260602``")
$md.Add("")
$md.Add("## Existing / Added Knobs")
$md.Add("")
foreach ($knob in $existingKnobs) {
    $md.Add("- ``$knob``")
}
$md.Add("")
$md.Add("## Summary Table")
$md.Add("")
foreach ($line in (ConvertTo-MarkdownTable $rows)) {
    $md.Add($line)
}
$md.Add("")
$md.Add("## Notes")
$md.Add("")
$md.Add("- All runs keep ``VENPOD_SPARSE_SURFACE_PROMOTION_POLICY=strict`` except the two explicit ``resolution_bounded64`` comparison rows.")
$md.Add("- ``VENPOD_VSYNC=0`` is set for the ablation to keep present wait from hiding ray timing.")
$md.Add("- Feature-disabled rows are diagnostic attribution rows, not correctness passes.")
$md.Add("- ``gpuRayMsPerMpix`` is ``gpuRayMs / (outputWidth * outputHeight / 1e6)``.")
$md.Add("- Existing logs do not expose average or percentile ray-step counts; this pass uses max-step budget sweeps plus GPU timing and ownership as the iteration-bound proxy.")
$md | Set-Content -Path $mdPath -Encoding UTF8

Write-Host "Wrote $csvPath"
Write-Host "Wrote $mdPath"
