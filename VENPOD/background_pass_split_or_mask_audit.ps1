param(
    [string]$OutputDir = "build\captures\background_pass_split_or_mask_20260602",
    [switch]$NoBuild,
    [switch]$SkipRuns
)

$ErrorActionPreference = "Stop"
$Culture = [Globalization.CultureInfo]::InvariantCulture

function Write-Info {
    param([string]$Message)
    Write-Host "[background-split] $Message" -ForegroundColor Cyan
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
    $value = Get-Number $Line $Pattern
    if ($value -eq $null) { return $null }
    return [int]$value
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
    Get-ChildItem -LiteralPath $ScenarioDir -Filter "engine_frame_*.bmp" -File -ErrorAction SilentlyContinue |
        Remove-Item -Force

    $runtimeLog = Join-Path $PSScriptRoot "build\bin\venpod_runtime.log"
    if (Test-Path $runtimeLog) {
        Remove-Item -LiteralPath $runtimeLog -Force
    }

    $env:VENPOD_RAYMARCH_RENDER_SCALE = "1"
    Remove-Item env:VENPOD_RENDER_QUALITY -ErrorAction SilentlyContinue
    if (-not [string]::IsNullOrWhiteSpace($Scenario.renderQuality)) {
        $env:VENPOD_RENDER_QUALITY = $Scenario.renderQuality
        Remove-Item env:VENPOD_RAYMARCH_RENDER_SCALE -ErrorAction SilentlyContinue
    }

    $env:VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE = $Scenario.backgroundEnable.ToString($Culture)
    $env:VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE = $Scenario.backgroundScale.ToString("0.###", $Culture)
    $env:VENPOD_RAYMARCH_FAR_SVO_QUALITY_SCALE = "1"
    $env:VENPOD_RAYMARCH_MAX_STEPS_SCALE = "1"
    $env:VENPOD_RAYMARCH_MAX_DISTANCE_SCALE = "1"
    if ($Scenario.walk) {
        Remove-Item env:VENPOD_RAYMARCH_FIXED_CAMERA -ErrorAction SilentlyContinue
    } else {
        $env:VENPOD_RAYMARCH_FIXED_CAMERA = "1"
    }
    $env:VENPOD_VSYNC = "0"
    $env:VENPOD_SPARSE_SURFACE_PROMOTION_POLICY = $Scenario.promotionPolicy
    if ($Scenario.promotionPolicy -eq "bounded_repair") {
        $env:VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_DEMOTE_BOUND = "64"
        $env:VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_BOUND = "64"
        $env:VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE = "12"
    } else {
        Remove-Item env:VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_DEMOTE_BOUND -ErrorAction SilentlyContinue
        Remove-Item env:VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_BOUND -ErrorAction SilentlyContinue
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

    $env:VENPOD_CAPTURE_DIR = $ScenarioDir
    $env:VENPOD_CAPTURE_START_FRAME = $Scenario.sampleFrame.ToString($Culture)
    $env:VENPOD_CAPTURE_INTERVAL_FRAMES = "1"
    $env:VENPOD_CAPTURE_COUNT = "1"
    $env:VENPOD_CAPTURE_HIDE_UI = "1"

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

    Write-Info "Run $($Scenario.name): bgEnable=$($Scenario.backgroundEnable) bgScale=$($Scenario.backgroundScale) policy=$($Scenario.promotionPolicy) quality=$($Scenario.renderQuality) highAlt=$($Scenario.highAlt) walk=$($Scenario.walk)"
    & .\rebrun.ps1 @runArgs *> (Join-Path $ScenarioDir "run_stdout.txt")
    $exitCode = $LASTEXITCODE
    if (Test-Path $runtimeLog) {
        Copy-Item -LiteralPath $runtimeLog -Destination (Join-Path $ScenarioDir "venpod_runtime.log") -Force
    }
    Set-Content -Path (Join-Path $ScenarioDir "exit_code.txt") -Value "$exitCode" -Encoding UTF8
    if ($exitCode -ne 0) {
        Write-Host "[background-split] scenario $($Scenario.name) exited with $exitCode" -ForegroundColor Yellow
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
    $backgroundPassLine = Get-LogLine $lines "RAYMARCH_BACKGROUND_PASS_CONFIG"
    $perfLine = Get-LogLine $lines "PERF frame=$frame\b"
    $frameEndLine = Get-LogLine $lines "PERF_FRAME_END frame=$frame\b"
    $cameraLine = Get-LogLine $lines "PERF_CAMERA_EXPOSURE frame=$frame\b"
    $policyLine = Get-LogLine $lines "PERF_SPARSE_SURFACE_PROMOTION_POLICY frame=$frame\b"
    $ownershipLine = Get-LogLine $lines "PERF_RENDER_OWNERSHIP retireFrame=\d+ shaderFrame=$frame\b"
    $compositionLine = Get-LogLine $lines "PERF_RENDER_COMPOSITION frame=$frame\b"
    $splitEnabled = ([int]$Scenario.backgroundEnable -ne 0)

    $gpu = Get-SlashNumbers $perfLine "gpu=frame/upload/pre/surface/ray/overlay/ui:" 7
    $outputWidth = Get-Int $configLine "output=(\d+)x\d+"
    $outputHeight = Get-Int $configLine "output=\d+x(\d+)"
    $foregroundWidth = Get-Int $backgroundPassLine "foreground=(\d+)x\d+"
    $foregroundHeight = Get-Int $backgroundPassLine "foreground=\d+x(\d+)"
    $backgroundWidth = Get-Int $backgroundPassLine "background=(\d+)x\d+"
    $backgroundHeight = Get-Int $backgroundPassLine "background=\d+x(\d+)"
    $screen = Get-Number $compositionLine "screen=([0-9.]+)"
    if ($screen -eq $null -and $outputWidth -and $outputHeight) {
        $screen = [double]($outputWidth * $outputHeight)
    }

    $raymarchPixels = Get-Number $ownershipLine "total=([0-9.]+)"
    $nearPixels = Get-Number $ownershipLine "near=([0-9.]+)"
    $midPixels = Get-Number $ownershipLine "midVoxel=([0-9.]+)"
    $farSvoPixels = Get-Number $ownershipLine "farSvo=([0-9.]+)"
    $farWaterPixels = Get-Number $ownershipLine "farWater=([0-9.]+)"
    $skyPixels = Get-Number $ownershipLine "sky=([0-9.]+)"
    $missPixels = Get-Number $ownershipLine "miss=([0-9.]+)"
    $unsafePixels = Get-Number $ownershipLine "unsafeNearMiss=([0-9.]+)"
    $surfacePixels = Get-Number $compositionLine "surfaceOwnedPixels=([0-9.]+)"
    $backgroundPixels = Get-Number $compositionLine "backgroundPixels=([0-9.]+)"
    if ($backgroundPixels -eq $null -and $screen -ne $null -and $surfacePixels -ne $null) {
        $backgroundPixels = $screen - $surfacePixels
    }
    if ($splitEnabled) {
        # The split composite is stencil-gated at full resolution, but the current
        # PERF_RENDER_COMPOSITION counters are emitted from the lower-resolution
        # raymarch pass. Do not report a fake unique full-res surface coverage.
        $surfacePixels = $null
        $backgroundPixels = $raymarchPixels
    }

    $scaleLayer = {
        param($pixels)
        if ($screen -gt 0 -and $raymarchPixels -gt 0 -and $backgroundPixels -ne $null -and $pixels -ne $null) {
            return ([double]$pixels / [double]$raymarchPixels) * [double]$backgroundPixels * 100.0 / [double]$screen
        }
        return $null
    }

    $candidateName = if ([int]$Scenario.backgroundEnable -ne 0) {
        "background_pass_scale_$($Scenario.backgroundScale)"
    } else {
        "baseline"
    }
    if (-not [string]::IsNullOrWhiteSpace($Scenario.renderQuality)) {
        $candidateName = "$candidateName+$($Scenario.renderQuality)"
    }

    [pscustomobject]@{
        group = $Scenario.group
        scenario = $Scenario.name
        candidate = $candidateName
        envFlags = "backgroundEnable=$($Scenario.backgroundEnable);backgroundScale=$($Scenario.backgroundScale);renderQuality=$($Scenario.renderQuality);policy=$($Scenario.promotionPolicy)"
        frame = $frame
        candidateEnabled = [int]$Scenario.backgroundEnable
        backgroundPassScale = [double]$Scenario.backgroundScale
        outputWidth = $outputWidth
        outputHeight = $outputHeight
        foregroundWidth = $foregroundWidth
        foregroundHeight = $foregroundHeight
        backgroundWidth = $backgroundWidth
        backgroundHeight = $backgroundHeight
        promoted = Get-Int $cameraLine "surfacePromoted=(\d+)"
        surfaceRasterMax = Get-Number $cameraLine "surfaceRasterMax=([0-9.]+)"
        surfaceScreenPct = if ($screen -gt 0 -and $surfacePixels -ne $null) { $surfacePixels * 100.0 / $screen } else { $null }
        backgroundPixelsPct = if ($screen -gt 0 -and $backgroundPixels -ne $null) { $backgroundPixels * 100.0 / $screen } else { $null }
        nearRaymarchScreenPct = & $scaleLayer $nearPixels
        midVoxelScreenPct = & $scaleLayer $midPixels
        farSvoScreenPct = & $scaleLayer $farSvoPixels
        farWaterScreenPct = & $scaleLayer $farWaterPixels
        skyScreenPct = & $scaleLayer $skyPixels
        missScreenPct = & $scaleLayer $missPixels
        unsafeNearMissScreenPct = & $scaleLayer $unsafePixels
        bodyMs = Get-Number $frameEndLine "body=([0-9.]+)"
        rawMs = Get-Number $frameEndLine "rawMs=([0-9.]+)"
        gpuRayMs = if ($gpu.Count -ge 5) { $gpu[4] } else { $null }
        cpuUpdateMs = Get-Number $perfLine "prep=([0-9.]+)"
        presentOrWaitMs = Get-Number $frameEndLine "present=([0-9.]+)"
        raymarchOwnershipPixels = $raymarchPixels
        farSvoOwnedPixels = $farSvoPixels
        backgroundPixels = $backgroundPixels
        backgroundSamplePctOfFullRes = if ($screen -gt 0 -and $raymarchPixels -ne $null) { $raymarchPixels * 100.0 / $screen } else { $null }
        rejectedTilesPct = ""
        promotionReason = Get-Text $policyLine "reason=([^ ]+)"
        visualNotes = if ($splitEnabled) { "$($Scenario.notes); split ownership stats are low-res raymarch samples" } else { $Scenario.notes }
        backgroundPassConfig = $backgroundPassLine
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
            $out += $row | Select-Object *, @{Name="gpuRayDeltaVsBaseline"; Expression={ $delta }}, @{Name="gpuRaySpeedupVsBaseline"; Expression={ $speedup }}
        }
    }
    return $out
}

function ConvertTo-MarkdownTable {
    param([object[]]$Rows)
    $columns = @(
        "group","scenario","candidate","frame","candidateEnabled","backgroundPassScale",
        "outputWidth","outputHeight","foregroundWidth","foregroundHeight","backgroundWidth","backgroundHeight",
        "promoted","surfaceRasterMax","surfaceScreenPct","backgroundPixelsPct","nearRaymarchScreenPct",
        "midVoxelScreenPct","farSvoScreenPct","farWaterScreenPct","skyScreenPct","missScreenPct","unsafeNearMissScreenPct",
        "bodyMs","rawMs","gpuRayMs","gpuRayDeltaVsBaseline","gpuRaySpeedupVsBaseline","cpuUpdateMs","presentOrWaitMs",
        "raymarchOwnershipPixels","backgroundSamplePctOfFullRes","farSvoOwnedPixels","promotionReason","visualNotes"
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

function New-ContactSheet {
    param([object[]]$Rows, [string]$OutputDir)

    Add-Type -AssemblyName System.Drawing
    $items = @()
    foreach ($row in $Rows) {
        $scenarioDir = Join-Path $OutputDir $row.scenario
        $bmp = Get-ChildItem -LiteralPath $scenarioDir -Filter "engine_frame_*.bmp" -File -ErrorAction SilentlyContinue |
            Sort-Object Name |
            Select-Object -First 1
        if ($bmp) {
            $items += [pscustomobject]@{ Row = $row; Path = $bmp.FullName }
        }
    }
    if ($items.Count -eq 0) {
        return $null
    }

    $thumbWidth = 480
    $labelHeight = 44
    $padding = 12
    $columns = 2
    $thumbHeight = 270
    $gridRows = [int][Math]::Ceiling($items.Count / [double]$columns)
    $sheetPixelWidth = [int](($columns * $thumbWidth) + (($columns + 1) * $padding))
    $sheetPixelHeight = [int](($gridRows * ($thumbHeight + $labelHeight + $padding)) + $padding)
    $sheet = [System.Drawing.Bitmap]::new(
        $sheetPixelWidth,
        $sheetPixelHeight,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($sheet)
    try {
        $g.Clear([System.Drawing.Color]::FromArgb(28, 30, 34))
        $font = New-Object System.Drawing.Font("Segoe UI", 10)
        $brush = [System.Drawing.Brushes]::White
        for ($i = 0; $i -lt $items.Count; $i++) {
            $item = $items[$i]
            $col = $i % $columns
            $rowIndex = [int][Math]::Floor($i / $columns)
            $x = $padding + $col * ($thumbWidth + $padding)
            $y = $padding + $rowIndex * ($thumbHeight + $labelHeight + $padding)
            $image = [System.Drawing.Bitmap]::FromFile($item.Path)
            try {
                $g.DrawImage($image, $x, $y + $labelHeight, $thumbWidth, $thumbHeight)
            } finally {
                $image.Dispose()
            }
            $label = "$($item.Row.scenario)  gpuRay=$($item.Row.gpuRayMs)ms  farSvo=$($item.Row.farSvoScreenPct)%"
            $g.DrawString($label, $font, $brush, [System.Drawing.RectangleF]::new($x, $y, $thumbWidth, $labelHeight))
        }
    } finally {
        $g.Dispose()
    }
    $sheetPath = Join-Path $OutputDir "background_split_contact_sheet.png"
    $sheet.Save($sheetPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $sheet.Dispose()
    return $sheetPath
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
    @{ name="strict_native_baseline"; group="strict_native"; backgroundEnable=0; backgroundScale=1.0; renderQuality=""; promotionPolicy="strict"; highAlt=$false; walk=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="strict fixed native baseline" },
    @{ name="strict_native_background375"; group="strict_native"; backgroundEnable=1; backgroundScale=0.375; renderQuality=""; promotionPolicy="strict"; highAlt=$false; walk=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="VISUAL_FAIL: lower-res background pass produced white sampled background/sky" },
    @{ name="strict_playable_background375"; group="strict_playable"; backgroundEnable=1; backgroundScale=0.375; renderQuality="playable"; promotionPolicy="strict"; highAlt=$false; walk=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="VISUAL_FAIL: playable plus split still produced white sampled background/sky" },
    @{ name="bounded64_native_background375"; group="bounded64_native"; backgroundEnable=1; backgroundScale=0.375; renderQuality=""; promotionPolicy="bounded_repair"; highAlt=$false; walk=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="VISUAL_FAIL: bounded64 comparison only; bounded policy remains default-off" },
    @{ name="highalt_native_baseline"; group="highalt_native"; backgroundEnable=0; backgroundScale=1.0; renderQuality=""; promotionPolicy="strict"; highAlt=$true; walk=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="high-alt native baseline" },
    @{ name="highalt_native_background375"; group="highalt_native"; backgroundEnable=1; backgroundScale=0.375; renderQuality=""; promotionPolicy="strict"; highAlt=$true; walk=$false; exitFrame=260; sampleFrame=240; env=@{}; notes="VISUAL_FAIL: high-alt split produced white sampled background/sky; high-alt camera was not matched" },
    @{ name="walk_native_background375"; group="walk_native"; backgroundEnable=1; backgroundScale=0.375; renderQuality=""; promotionPolicy="strict"; highAlt=$false; walk=$true; exitFrame=500; sampleFrame=480; env=@{ VENPOD_SPARSE_WALK_TEST="1"; VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS="33"; VENPOD_SPARSE_WALK_AUTO_STEER="1" }; notes="VISUAL_FAIL: movement sample is GPU-diagnostic only; split sampled background failed visual validation" }
) | ForEach-Object { [pscustomobject]$_ }

$envNames = @(
    "VENPOD_RAYMARCH_RENDER_SCALE",
    "VENPOD_RENDER_QUALITY",
    "VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE",
    "VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE",
    "VENPOD_RAYMARCH_FAR_SVO_QUALITY_SCALE",
    "VENPOD_RAYMARCH_MAX_STEPS_SCALE",
    "VENPOD_RAYMARCH_MAX_DISTANCE_SCALE",
    "VENPOD_RAYMARCH_FIXED_CAMERA",
    "VENPOD_VSYNC",
    "VENPOD_SPARSE_SURFACE_PROMOTION_POLICY",
    "VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_DEMOTE_BOUND",
    "VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_BOUND",
    "VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE",
    "VENPOD_SPARSE_WALK_TEST",
    "VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS",
    "VENPOD_SPARSE_WALK_AUTO_STEER",
    "VENPOD_ENABLE_FAR_SVO",
    "VENPOD_SPARSE_MID_VOXEL_RENDER",
    "VENPOD_SPARSE_RAYMARCH_MAX_SCALE_PERCENT",
    "VENPOD_CAPTURE_DIR",
    "VENPOD_CAPTURE_START_FRAME",
    "VENPOD_CAPTURE_INTERVAL_FRAMES",
    "VENPOD_CAPTURE_COUNT",
    "VENPOD_CAPTURE_HIDE_UI"
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

$csvPath = Join-Path $OutputDir "background_split_summary.csv"
$mdPath = Join-Path $OutputDir "background_split_table.md"
$rows | Export-Csv -NoTypeInformation -Path $csvPath
$contactSheet = New-ContactSheet -Rows $rows -OutputDir $OutputDir

$md = New-Object System.Collections.Generic.List[string]
$md.Add("# Background Pass Split / Mask Prototype")
$md.Add("")
$md.Add("Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
$md.Add("")
$md.Add("Audit directory: ``build/captures/background_pass_split_or_mask_20260602``")
$md.Add("")
$md.Add("Candidate: ``VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE=1`` with ``VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE=0.375``. The exact sparse surface raster pass remains full-resolution on the swapchain back buffer; the raymarch background is rendered into a lower-resolution offscreen target and composited only where the full-resolution stencil is still zero.")
$md.Add("")
$md.Add("## Structure Answers")
$md.Add("")
$md.Add("1. ``PS_Raymarch`` is the combined near/background ray shader for pixels that survive the existing surface-stencil reject; sparse surface rasterization is a separate pass.")
$md.Add("2. Exact sparse surface is not resolved in the same shader as Far-SVO/background when raster surfaces are enabled; it is drawn first by ``PS_SparseSurface``/``VS_SparseSurface`` and marks depth/stencil.")
$md.Add("3. The renderer already has a full-resolution swapchain color target plus a full-resolution depth/stencil target. It does not have a reusable owner texture, but stencil is already a foreground mask.")
$md.Add("4. Exact foreground pixels are known cheaply through stencil after the surface pass. Mid/Far/Sky ownership is only known inside ``PS_Raymarch`` unless a new prepass/tile classifier is added.")
$md.Add("5. A two-pass split is practical as a small renderer change: offscreen lower-res raymarch target plus stencil-gated composite.")
$md.Add("6. A tile mask would need new per-tile classification data or shader-side counters. The lower-res target is smaller for this pass.")
$md.Add("7. The smallest default-off change is renderer-side offscreen background target/composite plumbing plus a tiny composite shader; no ``PS_Raymarch`` source churn is needed.")
$md.Add("")
$md.Add("For split rows, ``surfaceScreenPct`` is intentionally blank because the current logs do not count unique full-resolution stencil/composite surface pixels. ``backgroundPixelsPct`` and layer percentages are low-resolution raymarch-sample percentages relative to the full-resolution output. Miss and unsafe counters remain exact zero/nonzero checks for the sampled raymarch pass; the contact sheet is the visual guard for full-resolution composite correctness.")
$md.Add("")
$md.Add("Visual result: the prototype is not correctness-preserving yet. The strict split rows reduce GPU ray time, but contact-sheet pixels show white sampled background/sky in unstenciled regions. A temporary composite constant-color probe confirmed the full-resolution stencil composite executes; the bad output is in the sampled lower-resolution background target or its SRV contents. Treat all split-row timing as diagnostic-only until the background target content path is fixed.")
$md.Add("")
$md.Add("High-alt note: the high-alt stress-camera baseline and candidate did not land on the same camera pose in this run, so the high-alt timing row is directional only.")
$md.Add("")
if ($contactSheet) {
    $md.Add("Contact sheet: ``$([System.IO.Path]::GetFileName($contactSheet))``")
    $md.Add("")
}
$md.Add("## Summary Table")
$md.Add("")
foreach ($line in (ConvertTo-MarkdownTable $rows)) {
    $md.Add($line)
}
$md.Add("")
$md.Add("## Notes")
$md.Add("")
$md.Add("- ``bounded64`` is comparison-only; ``bounded_repair`` remains default-off.")
$md.Add("- ``VENPOD_RAYMARCH_FAR_SVO_QUALITY_SCALE`` is pinned to ``1`` for this audit.")
$md.Add("- The candidate fails if ``missScreenPct`` or ``unsafeNearMissScreenPct`` becomes nonzero, if Far-SVO ownership collapses in fixed/high-alt scenes, or if contact sheets show terrain holes/sky leaks.")
$md | Set-Content -Path $mdPath -Encoding UTF8

Write-Host "Wrote $csvPath"
Write-Host "Wrote $mdPath"
if ($contactSheet) {
    Write-Host "Wrote $contactSheet"
}
