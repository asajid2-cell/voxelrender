param(
    [string]$OutputDir = "build\captures\far_svo_background_gpu_reduction_20260602",
    [switch]$NoBuild,
    [switch]$SkipRuns
)

$ErrorActionPreference = "Stop"
$Culture = [Globalization.CultureInfo]::InvariantCulture

function Write-Info {
    param([string]$Message)
    Write-Host "[far-svo-gpu] $Message" -ForegroundColor Cyan
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

function Invoke-Scenario {
    param([object]$Scenario, [string]$ScenarioDir)

    New-Item -ItemType Directory -Force -Path $ScenarioDir | Out-Null
    $runtimeLog = Join-Path $PSScriptRoot "build\bin\venpod_runtime.log"
    if (Test-Path $runtimeLog) {
        Remove-Item -LiteralPath $runtimeLog -Force
    }

    $env:VENPOD_RAYMARCH_RENDER_SCALE = $Scenario.scale.ToString("0.###", $Culture)
    $env:VENPOD_RAYMARCH_FAR_SVO_QUALITY_SCALE = $Scenario.farSvoQualityScale.ToString("0.###", $Culture)
    $env:VENPOD_RAYMARCH_MAX_STEPS_SCALE = "1"
    $env:VENPOD_RAYMARCH_MAX_DISTANCE_SCALE = "1"
    $env:VENPOD_VSYNC = "0"
    $env:VENPOD_SPARSE_SURFACE_PROMOTION_POLICY = $Scenario.promotionPolicy
    if ($Scenario.promotionPolicy -eq "bounded_repair") {
        $env:VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_DEMOTE_BOUND = "64"
        $env:VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE = "12"
    } else {
        Remove-Item env:VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_DEMOTE_BOUND -ErrorAction SilentlyContinue
        Remove-Item env:VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE -ErrorAction SilentlyContinue
    }

    Remove-Item env:VENPOD_SPARSE_WALK_TEST -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_WALK_AUTO_STEER -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_ENABLE_FAR_SVO -ErrorAction SilentlyContinue
    Remove-Item env:VENPOD_SPARSE_MID_VOXEL_RENDER -ErrorAction SilentlyContinue
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

    Write-Info "Run $($Scenario.name): scale=$($Scenario.scale) farSvoQualityScale=$($Scenario.farSvoQualityScale) policy=$($Scenario.promotionPolicy) highAlt=$($Scenario.highAlt) walk=$($Scenario.walk)"
    & .\rebrun.ps1 @runArgs *> (Join-Path $ScenarioDir "run_stdout.txt")
    $exitCode = $LASTEXITCODE
    if (Test-Path $runtimeLog) {
        Copy-Item -LiteralPath $runtimeLog -Destination (Join-Path $ScenarioDir "venpod_runtime.log") -Force
    }
    Set-Content -Path (Join-Path $ScenarioDir "exit_code.txt") -Value "$exitCode" -Encoding UTF8
    if ($exitCode -ne 0) {
        Write-Host "[far-svo-gpu] scenario $($Scenario.name) exited with $exitCode" -ForegroundColor Yellow
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
    $backgroundConfigLine = Get-LogLine $lines "RAYMARCH_BACKGROUND_CONFIG"
    $perfLine = Get-LogLine $lines "PERF frame=$frame\b"
    $frameEndLine = Get-LogLine $lines "PERF_FRAME_END frame=$frame\b"
    $cameraLine = Get-LogLine $lines "PERF_CAMERA_EXPOSURE frame=$frame\b"
    $policyLine = Get-LogLine $lines "PERF_SPARSE_SURFACE_PROMOTION_POLICY frame=$frame\b"
    $ownershipLine = Get-LogLine $lines "PERF_RENDER_OWNERSHIP retireFrame=\d+ shaderFrame=$frame\b"
    $compositionLine = Get-LogLine $lines "PERF_RENDER_COMPOSITION frame=$frame\b"

    $gpu = Get-SlashNumbers $perfLine "gpu=frame/upload/pre/surface/ray/overlay/ui:" 7
    $outputWidth = Get-Int $configLine "output=(\d+)x\d+"
    $outputHeight = Get-Int $configLine "output=\d+x(\d+)"
    $screen = Get-Number $compositionLine "screen=([0-9.]+)"
    if ($screen -eq $null -and $outputWidth -and $outputHeight) {
        $screen = [double]($outputWidth * $outputHeight)
    }

    $totalBackgroundPixels = Get-Number $ownershipLine "total=([0-9.]+)"
    $midPixels = Get-Number $ownershipLine "midVoxel=([0-9.]+)"
    $farSvoPixels = Get-Number $ownershipLine "farSvo=([0-9.]+)"
    $skyPixels = Get-Number $ownershipLine "sky=([0-9.]+)"
    $missPixels = Get-Number $ownershipLine "miss=([0-9.]+)"
    $unsafePixels = Get-Number $ownershipLine "unsafeNearMiss=([0-9.]+)"
    $surfacePixels = Get-Number $compositionLine "surfaceOwnedPixels=([0-9.]+)"

    $gpuRayMs = if ($gpu.Count -ge 5) { $gpu[4] } else { $null }
    $present = Get-Number $frameEndLine "present=([0-9.]+)"
    $body = Get-Number $frameEndLine "body=([0-9.]+)"
    $raw = Get-Number $frameEndLine "rawMs=([0-9.]+)"
    $cpu = Get-Number $perfLine "prep=([0-9.]+)"

    [pscustomobject]@{
        group = $Scenario.group
        scenario = $Scenario.name
        candidate = if ([double]$Scenario.farSvoQualityScale -lt 0.999) { "far_svo_quality_scale_$($Scenario.farSvoQualityScale)" } else { "baseline" }
        envFlags = "farSvoQualityScale=$($Scenario.farSvoQualityScale);policy=$($Scenario.promotionPolicy)"
        frame = $frame
        outputWidth = $outputWidth
        outputHeight = $outputHeight
        scale = [double]$Scenario.scale
        backgroundScale = ""
        promoted = Get-Int $cameraLine "surfacePromoted=(\d+)"
        surfaceRasterMax = Get-Number $cameraLine "surfaceRasterMax=([0-9.]+)"
        exactSurfacePct = if ($screen -gt 0 -and $surfacePixels -ne $null) { $surfacePixels * 100.0 / $screen } else { $null }
        backgroundScreenPct = if ($screen -gt 0 -and $totalBackgroundPixels -ne $null) { $totalBackgroundPixels * 100.0 / $screen } else { $null }
        midVoxelScreenPct = if ($screen -gt 0 -and $midPixels -ne $null) { $midPixels * 100.0 / $screen } else { $null }
        farSvoScreenPct = if ($screen -gt 0 -and $farSvoPixels -ne $null) { $farSvoPixels * 100.0 / $screen } else { $null }
        skyScreenPct = if ($screen -gt 0 -and $skyPixels -ne $null) { $skyPixels * 100.0 / $screen } else { $null }
        missScreenPct = if ($screen -gt 0 -and $missPixels -ne $null) { $missPixels * 100.0 / $screen } else { $null }
        unsafeNearMissScreenPct = if ($screen -gt 0 -and $unsafePixels -ne $null) { $unsafePixels * 100.0 / $screen } else { $null }
        gpuRayMs = $gpuRayMs
        bodyMs = $body
        rawMs = $raw
        cpuUpdateMs = $cpu
        presentOrWaitMs = $present
        farSvoOwned = $farSvoPixels
        backgroundConfig = $backgroundConfigLine
        promotionReason = Get-Text $policyLine "reason=([^ ]+)"
        notes = $Scenario.notes
    }
}

function Add-Speedups {
    param([object[]]$Rows)
    $out = @()
    foreach ($group in ($Rows | Group-Object group)) {
        $baseline = $group.Group | Where-Object { $_.candidate -eq "baseline" } | Select-Object -First 1
        foreach ($row in $group.Group) {
            $speedup = $null
            $delta = $null
            if ($baseline -and $baseline.gpuRayMs -ne $null -and $row.gpuRayMs -ne $null -and [double]$row.gpuRayMs -gt 0.0) {
                $speedup = [double]$baseline.gpuRayMs / [double]$row.gpuRayMs
                $delta = [double]$row.gpuRayMs - [double]$baseline.gpuRayMs
            }
            $out += $row | Select-Object *, @{Name="gpuRaySpeedupVsBaseline"; Expression={ $speedup }}, @{Name="gpuRayDeltaVsBaseline"; Expression={ $delta }}
        }
    }
    return $out
}

function ConvertTo-MarkdownTable {
    param([object[]]$Rows)
    $columns = @(
        "group","scenario","candidate","frame","outputWidth","outputHeight","scale","promoted","surfaceRasterMax",
        "exactSurfacePct","backgroundScreenPct","midVoxelScreenPct","farSvoScreenPct","skyScreenPct","missScreenPct","unsafeNearMissScreenPct",
        "gpuRayMs","gpuRayDeltaVsBaseline","gpuRaySpeedupVsBaseline","bodyMs","rawMs","cpuUpdateMs","presentOrWaitMs",
        "farSvoOwned","promotionReason","notes"
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
    @{ name="strict_native_baseline"; group="strict_native"; scale=1.00; farSvoQualityScale=1.00; promotionPolicy="strict"; highAlt=$false; walk=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="strict fixed native baseline" },
    @{ name="strict_native_far_svo_quality50"; group="strict_native"; scale=1.00; farSvoQualityScale=0.50; promotionPolicy="strict"; highAlt=$false; walk=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="default-off Far-SVO quality scale candidate" },
    @{ name="strict_half_baseline"; group="strict_half"; scale=0.50; farSvoQualityScale=1.00; promotionPolicy="strict"; highAlt=$false; walk=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="strict fixed half-res baseline" },
    @{ name="strict_half_far_svo_quality50"; group="strict_half"; scale=0.50; farSvoQualityScale=0.50; promotionPolicy="strict"; highAlt=$false; walk=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="half-res plus Far-SVO quality scale candidate" },
    @{ name="bounded64_native_baseline"; group="bounded64_native"; scale=1.00; farSvoQualityScale=1.00; promotionPolicy="bounded_repair"; highAlt=$false; walk=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="bounded64 comparison only" },
    @{ name="bounded64_native_far_svo_quality50"; group="bounded64_native"; scale=1.00; farSvoQualityScale=0.50; promotionPolicy="bounded_repair"; highAlt=$false; walk=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="bounded64 comparison only; candidate still default-off" },
    @{ name="highalt_native_baseline"; group="highalt_native"; scale=1.00; farSvoQualityScale=1.00; promotionPolicy="strict"; highAlt=$true; walk=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="high-alt excluded path native baseline" },
    @{ name="highalt_native_far_svo_quality50"; group="highalt_native"; scale=1.00; farSvoQualityScale=0.50; promotionPolicy="strict"; highAlt=$true; walk=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="high-alt native with Far-SVO quality scale candidate" },
    @{ name="highalt_half_baseline"; group="highalt_half"; scale=0.50; farSvoQualityScale=1.00; promotionPolicy="strict"; highAlt=$true; walk=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="high-alt half-res baseline" },
    @{ name="highalt_half_far_svo_quality50"; group="highalt_half"; scale=0.50; farSvoQualityScale=0.50; promotionPolicy="strict"; highAlt=$true; walk=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="high-alt half-res with Far-SVO quality scale candidate" },
    @{ name="walk_native_baseline"; group="walk_native"; scale=1.00; farSvoQualityScale=1.00; promotionPolicy="strict"; highAlt=$false; walk=$true; exitFrame=500; sampleFrame=480; env=@{ VENPOD_SPARSE_WALK_TEST="1"; VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS="33"; VENPOD_SPARSE_WALK_AUTO_STEER="1" }; notes="short low-alt movement sample" },
    @{ name="walk_native_far_svo_quality50"; group="walk_native"; scale=1.00; farSvoQualityScale=0.50; promotionPolicy="strict"; highAlt=$false; walk=$true; exitFrame=500; sampleFrame=480; env=@{ VENPOD_SPARSE_WALK_TEST="1"; VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS="33"; VENPOD_SPARSE_WALK_AUTO_STEER="1" }; notes="short low-alt movement with Far-SVO quality scale candidate" }
) | ForEach-Object { [pscustomobject]$_ }

$envNames = @(
    "VENPOD_RAYMARCH_RENDER_SCALE",
    "VENPOD_RAYMARCH_FAR_SVO_QUALITY_SCALE",
    "VENPOD_RAYMARCH_MAX_STEPS_SCALE",
    "VENPOD_RAYMARCH_MAX_DISTANCE_SCALE",
    "VENPOD_VSYNC",
    "VENPOD_SPARSE_SURFACE_PROMOTION_POLICY",
    "VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_DEMOTE_BOUND",
    "VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE",
    "VENPOD_SPARSE_WALK_TEST",
    "VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS",
    "VENPOD_SPARSE_WALK_AUTO_STEER",
    "VENPOD_ENABLE_FAR_SVO",
    "VENPOD_SPARSE_MID_VOXEL_RENDER",
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
            Invoke-Scenario -Scenario $scenario -ScenarioDir (Join-Path $OutputDir $scenario.name)
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

$csvPath = Join-Path $OutputDir "far_svo_gpu_summary.csv"
$mdPath = Join-Path $OutputDir "far_svo_gpu_table.md"
$rows | Export-Csv -NoTypeInformation -Path $csvPath

$md = New-Object System.Collections.Generic.List[string]
$md.Add("# Far-SVO / Background GPU Reduction")
$md.Add("")
$md.Add("Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
$md.Add("")
$md.Add("Audit directory: ``build/captures/far_svo_background_gpu_reduction_20260602``")
$md.Add("")
$md.Add("Candidate: ``VENPOD_RAYMARCH_FAR_SVO_QUALITY_SCALE=0.5``. This is a default-off C++-side budget lever that feeds the existing Far-SVO quality scalar used by the shader page-step schedule; it does not disable Far-SVO.")
$md.Add("")
$md.Add("Stats: this report uses the existing render-ownership UAV counters for final background ownership, plus GPU timing. A per-attempt Far-SVO atomic stats variant was tested but removed because it turned the pixel shader into a multi-minute runtime compile path, so it was not low-overhead enough for this pass.")
$md.Add("")
$md.Add("## Summary Table")
$md.Add("")
foreach ($line in (ConvertTo-MarkdownTable $rows)) {
    $md.Add($line)
}
$md.Add("")
$md.Add("## Notes")
$md.Add("")
$md.Add("- ``bounded64`` rows are comparison-only; the bounded policy remains default-off.")
$md.Add("- ``missScreenPct`` and ``unsafeNearMissScreenPct`` must remain zero for the candidate to stay viable.")
$md.Add("- Per-attempt Far-SVO counters are intentionally absent in this run; use final owner percentages and paired timing deltas for this candidate decision.")
$md | Set-Content -Path $mdPath -Encoding UTF8

Write-Host "Wrote $csvPath"
Write-Host "Wrote $mdPath"
