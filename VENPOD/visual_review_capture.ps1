# =============================================================================
# VENPOD - Visual Review Capture
# Regenerates the canonical public-review visual scenarios from in-engine DX12
# backbuffer readbacks and writes a manual review checklist.
# =============================================================================

param(
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$NoBuild,
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

function Assert-SafeOutputDir {
    param(
        [string]$ResolvedOutputDir,
        [string]$ProjectRoot,
        [string]$BuildDir
    )

    $normalizedOutputDir = Normalize-GuardPath $ResolvedOutputDir
    $root = [System.IO.Path]::GetPathRoot($normalizedOutputDir)
    if ($normalizedOutputDir -ieq $root.TrimEnd('\')) {
        throw "Refusing to use filesystem root as visual review output directory: $ResolvedOutputDir"
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
        throw "Refusing to use visual review output outside build/captures or build/logs: $ResolvedOutputDir"
    }

    $forbidden = @(
        (Normalize-GuardPath $ProjectRoot),
        (Normalize-GuardPath $BuildDir),
        (Normalize-GuardPath (Join-Path $BuildDir "logs")),
        (Normalize-GuardPath (Join-Path $BuildDir "captures")),
        (Normalize-GuardPath (Join-Path $BuildDir "bin"))
    )
    foreach ($path in $forbidden) {
        if ($normalizedOutputDir -ieq $path) {
            throw "Refusing to clean broad visual review output directory: $ResolvedOutputDir"
        }
    }
}

function Invoke-CaptureScenario {
    param(
        [string]$Name,
        [string[]]$CaptureArgs,
        [string]$ScenarioDir
    )

    if (Test-Path -LiteralPath $ScenarioDir) {
        Remove-Item -LiteralPath $ScenarioDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $ScenarioDir -Force | Out-Null

    Write-Step "Capturing $Name..."
    $allArgs = $CaptureArgs + @("-OutputDir", $ScenarioDir)
    & powershell @allArgs
    Stop-OnFailure -Code $LASTEXITCODE -Stage "$Name capture"

    $contactSheet = Join-Path $ScenarioDir "contact_sheet.png"
    $stats = Join-Path $ScenarioDir "image_stats.csv"
    $runtimeLog = Join-Path $ScenarioDir "venpod_runtime.log"
    if (-not (Test-Path -LiteralPath $contactSheet)) {
        throw "$Name did not produce contact_sheet.png"
    }
    if (-not (Test-Path -LiteralPath $stats)) {
        throw "$Name did not produce image_stats.csv"
    }
    if (-not (Test-Path -LiteralPath $runtimeLog)) {
        throw "$Name did not produce venpod_runtime.log"
    }

    return [pscustomobject]@{
        Name = $Name
        Directory = $ScenarioDir
        ContactSheet = $contactSheet
        Stats = $stats
        RuntimeLog = $runtimeLog
        CaptureArgs = $CaptureArgs
    }
}

function Get-CaptureArgValue {
    param(
        [string[]]$CaptureArgs,
        [string]$Name,
        [string]$DefaultValue = ""
    )

    for ($i = 0; $i -lt ($CaptureArgs.Count - 1); ++$i) {
        if ($CaptureArgs[$i] -eq $Name) {
            return $CaptureArgs[$i + 1]
        }
    }
    return $DefaultValue
}

function Get-CsvColumnMetric {
    param(
        [object[]]$Rows,
        [string]$Column,
        [string]$Metric
    )

    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    $values = New-Object System.Collections.Generic.List[double]
    foreach ($row in $Rows) {
        $property = $row.PSObject.Properties[$Column]
        if ($null -ne $property -and -not [string]::IsNullOrWhiteSpace([string]$property.Value)) {
            $values.Add([double]::Parse([string]$property.Value, $culture)) | Out-Null
        }
    }
    if ($values.Count -eq 0) {
        return $null
    }

    $measure = $values | Measure-Object -Average -Maximum -Minimum
    switch ($Metric) {
        "Average" { return $measure.Average }
        "Maximum" { return $measure.Maximum }
        "Minimum" { return $measure.Minimum }
        default { throw "Unsupported CSV metric: $Metric" }
    }
}

function Format-OptionalNumber {
    param(
        $Value,
        [int]$Decimals = 2
    )

    if ($null -eq $Value) {
        return ""
    }
    return ([double]$Value).ToString(("F{0}" -f $Decimals), [System.Globalization.CultureInfo]::InvariantCulture)
}

function Get-RenderOwnershipSummary {
    param(
        [string]$LogPath,
        [string]$StatsPath,
        [int]$ReadyFrame
    )

    $screenPixels = 0L
    if (Test-Path -LiteralPath $StatsPath) {
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
                $ownershipTestPixels = [Math]::Max(1.0, $screenTotal - $sky)
                $heightProxyPct = ([double]($midHeight + $farHeight) * 100.0) / $ownershipTestPixels
                $farSvoPct = ([double]$farSvo * 100.0) / $ownershipTestPixels

                ++$sampleCount
                $maxHeightProxyObserved = [Math]::Max($maxHeightProxyObserved, $heightProxyPct)
                $maxFarSvoObserved = [Math]::Max($maxFarSvoObserved, $farSvoPct)
                $maxMissPixels = [Math]::Max($maxMissPixels, $miss)
                $maxUnsafeNearMissPixels = [Math]::Max($maxUnsafeNearMissPixels, $unsafeNearMiss)
            }
        }

    return [pscustomobject]@{
        Samples = $sampleCount
        MaxHeightProxyPct = $maxHeightProxyObserved
        MaxFarSvoPct = $maxFarSvoObserved
        MaxMissPixels = $maxMissPixels
        MaxUnsafeNearMissPixels = $maxUnsafeNearMissPixels
    }
}

function Get-RenderPerformanceSummary {
    param(
        [string]$LogPath,
        [int]$ReadyFrame
    )

    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    $sampleCount = 0
    $maxObservedFrameMs = 0.0
    $maxObservedSmoothedFrameMs = 0.0
    $maxObservedPrepMs = 0.0
    $maxObservedGpuRayMs = 0.0

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
    }

    return [pscustomobject]@{
        Samples = $sampleCount
        MaxFrameMs = $maxObservedFrameMs
        MaxSmoothedFrameMs = $maxObservedSmoothedFrameMs
        MaxPrepMs = $maxObservedPrepMs
        MaxGpuRayMs = $maxObservedGpuRayMs
    }
}

function Write-VisualReviewSummary {
    param(
        [object[]]$Scenarios,
        [string]$SummaryPath
    )

    $summaryRows = New-Object System.Collections.Generic.List[object]
    foreach ($scenario in $Scenarios) {
        $statsRows = @(Import-Csv -Path $scenario.Stats)
        $readyFrame = [int](Get-CaptureArgValue -CaptureArgs $scenario.CaptureArgs -Name "-CaptureStartFrame" -DefaultValue "0")
        $ownership = Get-RenderOwnershipSummary -LogPath $scenario.RuntimeLog -StatsPath $scenario.Stats -ReadyFrame $readyFrame
        $performance = Get-RenderPerformanceSummary -LogPath $scenario.RuntimeLog -ReadyFrame $readyFrame
        $relativeContactSheet = Resolve-Path -LiteralPath $scenario.ContactSheet -Relative
        $relativeStats = Resolve-Path -LiteralPath $scenario.Stats -Relative
        $relativeRuntimeLog = Resolve-Path -LiteralPath $scenario.RuntimeLog -Relative

        $summaryRows.Add([pscustomobject]@{
            Scenario = $scenario.Name
            Frames = $statsRows.Count
            ContactSheet = $relativeContactSheet
            Stats = $relativeStats
            RuntimeLog = $relativeRuntimeLog
            CaptureStartFrame = $readyFrame
            MaxHeightProxyGatePct = (Get-CaptureArgValue -CaptureArgs $scenario.CaptureArgs -Name "-MaxHeightProxyPct")
            MinFarSvoGatePct = (Get-CaptureArgValue -CaptureArgs $scenario.CaptureArgs -Name "-MinFarSvoPct")
            MaxFrameGateMs = (Get-CaptureArgValue -CaptureArgs $scenario.CaptureArgs -Name "-MaxFrameMs")
            MaxPrepGateMs = (Get-CaptureArgValue -CaptureArgs $scenario.CaptureArgs -Name "-MaxPrepMs")
            MaxGpuRayGateMs = (Get-CaptureArgValue -CaptureArgs $scenario.CaptureArgs -Name "-MaxGpuRayMs")
            AvgTopBandTerrainPct = (Format-OptionalNumber -Value (Get-CsvColumnMetric -Rows $statsRows -Column "topBandTerrainPct" -Metric "Average"))
            MaxTopBandTerrainPct = (Format-OptionalNumber -Value (Get-CsvColumnMetric -Rows $statsRows -Column "topBandTerrainPct" -Metric "Maximum"))
            MinUniqueSampleColors = (Format-OptionalNumber -Value (Get-CsvColumnMetric -Rows $statsRows -Column "uniqueSampleColors" -Metric "Minimum") -Decimals 0)
            OwnershipSamples = $ownership.Samples
            MaxHeightProxyObservedPct = (Format-OptionalNumber -Value $ownership.MaxHeightProxyPct)
            MaxFarSvoObservedPct = (Format-OptionalNumber -Value $ownership.MaxFarSvoPct)
            MaxMissPixels = $ownership.MaxMissPixels
            MaxUnsafeNearMissPixels = $ownership.MaxUnsafeNearMissPixels
            PerformanceSamples = $performance.Samples
            MaxFrameObservedMs = (Format-OptionalNumber -Value $performance.MaxFrameMs)
            MaxSmoothedFrameObservedMs = (Format-OptionalNumber -Value $performance.MaxSmoothedFrameMs)
            MaxPrepObservedMs = (Format-OptionalNumber -Value $performance.MaxPrepMs)
            MaxGpuRayObservedMs = (Format-OptionalNumber -Value $performance.MaxGpuRayMs)
        }) | Out-Null
    }

    $summaryRows | Export-Csv -Path $SummaryPath -NoTypeInformation -Encoding ASCII
}

$projectRoot = $PSScriptRoot
$buildDir = Join-Path $projectRoot "build"
$engineCaptureScript = Join-Path $projectRoot "engine_capture_smoke.ps1"
$buildScript = Join-Path $projectRoot "build.ps1"

if (-not (Test-Path -LiteralPath $engineCaptureScript)) {
    throw "engine_capture_smoke.ps1 not found at $engineCaptureScript"
}
if (-not (Test-Path -LiteralPath $buildScript)) {
    throw "build.ps1 not found at $buildScript"
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $buildDir "logs\visual_review_capture"
} elseif (-not [System.IO.Path]::IsPathRooted($OutputDir)) {
    $OutputDir = Join-Path $projectRoot $OutputDir
}

Assert-SafeOutputDir -ResolvedOutputDir $OutputDir -ProjectRoot $projectRoot -BuildDir $buildDir
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).ProviderPath

if ($Clean) {
    Get-ChildItem -LiteralPath $OutputDir -Force | Remove-Item -Recurse -Force
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

Write-Host "VENPOD - Visual Review Capture" -ForegroundColor Magenta
Write-Info "Output: $OutputDir"

if (-not $NoBuild) {
    Write-Step "Building latest code..."
    & powershell -ExecutionPolicy Bypass -File $buildScript -Config $Config
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Build"
} else {
    Write-Info "Build step: skipped (-NoBuild)"
}

$baseArgs = @(
    "-ExecutionPolicy", "Bypass",
    "-File", $engineCaptureScript,
    "-Config", $Config,
    "-NoBuild",
    "-MaxFrameMs", "110",
    "-MaxPrepMs", "70",
    "-MaxGpuRayMs", "65"
)

$scenarios = New-Object System.Collections.Generic.List[object]

$scenarios.Add((Invoke-CaptureScenario `
    -Name "normal" `
    -ScenarioDir (Join-Path $OutputDir "normal") `
    -CaptureArgs ($baseArgs + @(
        "-ExitAfterFrames", "245",
        "-CaptureStartFrame", "120",
        "-CaptureIntervalFrames", "20",
        "-CaptureCount", "6",
        "-MaxHeightProxyPct", "65"
    )))) | Out-Null

$scenarios.Add((Invoke-CaptureScenario `
    -Name "walk" `
    -ScenarioDir (Join-Path $OutputDir "walk") `
    -CaptureArgs ($baseArgs + @(
        "-WalkTest",
        "-WalkTestSpeed", "8",
        "-WalkTestYawDegPerSec", "30",
        "-ExitAfterFrames", "410",
        "-CaptureStartFrame", "220",
        "-CaptureIntervalFrames", "50",
        "-CaptureCount", "4",
        "-MaxAverageTopTerrainPct", "52",
        "-MaxFrameTopTerrainPct", "70",
        "-MaxHeightProxyPct", "55"
    )))) | Out-Null

$scenarios.Add((Invoke-CaptureScenario `
    -Name "long-walk" `
    -ScenarioDir (Join-Path $OutputDir "long-walk") `
    -CaptureArgs ($baseArgs + @(
        "-WalkTest",
        "-WalkTestSpeed", "8",
        "-WalkTestYawDegPerSec", "30",
        "-ExitAfterFrames", "1900",
        "-CaptureStartFrame", "220",
        "-CaptureIntervalFrames", "30",
        "-CaptureCount", "56",
        "-MinUniqueSampleColors", "50",
        "-MinAverageSkyLikePct", "12",
        "-MaxAverageTopTerrainPct", "55",
        "-MaxFrameTopTerrainPct", "98",
        "-MaxHeightProxyPct", "55"
    )))) | Out-Null

$scenarios.Add((Invoke-CaptureScenario `
    -Name "fast-flight" `
    -ScenarioDir (Join-Path $OutputDir "fast-flight") `
    -CaptureArgs ($baseArgs + @(
        "-StressCamera",
        "-StressCameraRadius", "1400",
        "-StressCameraHeight", "260",
        "-StressCameraBaseHeight", "620",
        "-StressCameraSpeed", "160",
        "-ExitAfterFrames", "720",
        "-CaptureStartFrame", "240",
        "-CaptureIntervalFrames", "30",
        "-CaptureCount", "12",
        "-MinUniqueSampleColors", "50",
        "-MinFarSvoPct", "35",
        "-MaxHeightProxyPct", "60"
    )))) | Out-Null

$scenarios.Add((Invoke-CaptureScenario `
    -Name "long-fast-flight" `
    -ScenarioDir (Join-Path $OutputDir "long-fast-flight") `
    -CaptureArgs ($baseArgs + @(
        "-StressCamera",
        "-StressCameraRadius", "1900",
        "-StressCameraHeight", "340",
        "-StressCameraBaseHeight", "760",
        "-StressCameraSpeed", "230",
        "-ExitAfterFrames", "1320",
        "-CaptureStartFrame", "390",
        "-CaptureIntervalFrames", "60",
        "-CaptureCount", "16",
        "-MinUniqueSampleColors", "50",
        "-MinFarSvoPct", "35",
        "-MaxHeightProxyPct", "45"
    )))) | Out-Null

$scenarios.Add((Invoke-CaptureScenario `
    -Name "fast-water-transition" `
    -ScenarioDir (Join-Path $OutputDir "fast-water-transition") `
    -CaptureArgs ($baseArgs + @(
        "-StressCamera",
        "-WaterlineCamera",
        "-StressCameraRadius", "220",
        "-StressCameraHeight", "25",
        "-StressCameraBaseHeight", "70",
        "-StressCameraSpeed", "120",
        "-ExitAfterFrames", "760",
        "-CaptureStartFrame", "220",
        "-CaptureIntervalFrames", "40",
        "-CaptureCount", "12",
        "-MinUniqueSampleColors", "50",
        "-MaxFrameTopTerrainPct", "92",
        "-MaxHeightProxyPct", "60"
    )))) | Out-Null

$scenarios.Add((Invoke-CaptureScenario `
    -Name "long-fast-water-transition" `
    -ScenarioDir (Join-Path $OutputDir "long-fast-water-transition") `
    -CaptureArgs ($baseArgs + @(
        "-StressCamera",
        "-WaterlineCamera",
        "-StressCameraRadius", "220",
        "-StressCameraHeight", "25",
        "-StressCameraBaseHeight", "70",
        "-StressCameraSpeed", "120",
        "-ExitAfterFrames", "1320",
        "-CaptureStartFrame", "360",
        "-CaptureIntervalFrames", "60",
        "-CaptureCount", "16",
        "-MinUniqueSampleColors", "50",
        "-MaxAverageTopTerrainPct", "55",
        "-MaxFrameTopTerrainPct", "92",
        "-MaxHeightProxyPct", "60"
    )))) | Out-Null

$scenarios.Add((Invoke-CaptureScenario `
    -Name "waterline" `
    -ScenarioDir (Join-Path $OutputDir "waterline") `
    -CaptureArgs ($baseArgs + @(
        "-StressCamera",
        "-WaterlineCamera",
        "-StressCameraRadius", "28",
        "-StressCameraHeight", "6",
        "-StressCameraBaseHeight", "-22",
        "-StressCameraSpeed", "36",
        "-ExitAfterFrames", "660",
        "-CaptureStartFrame", "200",
        "-CaptureIntervalFrames", "35",
        "-CaptureCount", "10",
        "-MinUniqueSampleColors", "50",
        "-MaxHeightProxyPct", "25"
    )))) | Out-Null

$scenarios.Add((Invoke-CaptureScenario `
    -Name "long-waterline" `
    -ScenarioDir (Join-Path $OutputDir "long-waterline") `
    -CaptureArgs ($baseArgs + @(
        "-StressCamera",
        "-WaterlineCamera",
        "-StressCameraRadius", "28",
        "-StressCameraHeight", "6",
        "-StressCameraBaseHeight", "-22",
        "-StressCameraSpeed", "36",
        "-ExitAfterFrames", "1900",
        "-CaptureStartFrame", "220",
        "-CaptureIntervalFrames", "30",
        "-CaptureCount", "56",
        "-MinUniqueSampleColors", "50",
        "-MaxHeightProxyPct", "25"
    )))) | Out-Null

$checklistPath = Join-Path $OutputDir "VISUAL_REVIEW_CHECKLIST.md"
$summaryPath = Join-Path $OutputDir "VISUAL_REVIEW_SUMMARY.csv"
Write-VisualReviewSummary -Scenarios $scenarios -SummaryPath $summaryPath

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("# VENPOD Visual Review Checklist")
$lines.Add("")
$lines.Add("Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')")
$lines.Add("")
$lines.Add("This artifact is evidence for manual public-readiness review. It is not, by itself, a completion certificate.")
$lines.Add("")
$relativeSummary = Resolve-Path -LiteralPath $summaryPath -Relative
$lines.Add(("Machine-readable scenario summary: {0}" -f $relativeSummary))
$lines.Add("")
$lines.Add("## Scenarios")
$lines.Add("")
foreach ($scenario in $scenarios) {
    $relativeDir = Resolve-Path -LiteralPath $scenario.Directory -Relative
    $relativeContactSheet = Resolve-Path -LiteralPath $scenario.ContactSheet -Relative
    $relativeStats = Resolve-Path -LiteralPath $scenario.Stats -Relative
    $relativeRuntimeLog = Resolve-Path -LiteralPath $scenario.RuntimeLog -Relative
    $lines.Add(('- {0}: `{1}`' -f $scenario.Name, $relativeDir))
    $lines.Add(('  - Contact sheet: `{0}`' -f $relativeContactSheet))
    $lines.Add(('  - Stats: `{0}`' -f $relativeStats))
    $lines.Add(('  - Runtime log: `{0}`' -f $relativeRuntimeLog))
}
$lines.Add("")
$lines.Add("## Manual Acceptance Criteria")
$lines.Add("")
$lines.Add("- [ ] Normal, walk, and long-walk views are coherent sparse voxel terrain, not voids, fallback masks, or wall-stuck route artifacts.")
$lines.Add("- [ ] Short and long fast-flight views are continuous and do not show finite dense cubes, holes, flashing, or noisy proxy dominance.")
$lines.Add("- [ ] Fast water-transition and long fast-water transition views remain coherent while crossing near water level without reverting to all-blue water/sky or sparse holes.")
$lines.Add("- [ ] Waterline, long-waterline, and long fast-water transition views do not turn into blue sky/far-water fill and do not read as disconnected shell slabs during extended traversal.")
$lines.Add('- [ ] Ownership logs for reviewed frames have `miss=0` and `unsafeNearMiss=0`.')
$lines.Add("- [ ] Proxy ownership gates passed for captured review frames: normal `MaxHeightProxyPct<=65`, walk `<=55`, long-walk `<=55`, fast-flight `<=60`, long-fast-flight `<=45`, fast water-transition `<=60`, long fast-water transition `<=60`, waterline `<=25`, long-waterline `<=25`.")
$lines.Add("- [ ] High-flight ownership gates also observed far-SVO terrain in captured review frames: fast-flight and long-fast-flight `MinFarSvoPct>=35`.")
$lines.Add('- [ ] Runtime performance gates passed for captured review frames: `MaxFrameMs<=110`, `MaxPrepMs<=70`, `MaxGpuRayMs<=65`.')
$lines.Add('- [ ] Any remaining proxy/fog/LOD artifacts are explicitly accepted or tracked in `docs/COMPLETION_LEDGER.md`.')
$lines.Add("")
$lines.Add("## Completion Note")
$lines.Add("")
$lines.Add("Only mark ledger items complete after this checklist is manually accepted and the ledger statuses are updated with exact evidence.")
$lines | Set-Content -Path $checklistPath -Encoding ASCII

Write-Success "Visual review capture passed."
Write-Info "Checklist: $checklistPath"
Write-Info "Summary: $summaryPath"
foreach ($scenario in $scenarios) {
    Write-Info "$($scenario.Name): $($scenario.ContactSheet)"
}
