param(
    [string]$Label = "omni_prod_motion_guard",
    [int]$Frames = 900,
    [int]$Warmup = 300,
    [string]$Replay = "mtns.rec",
    [ValidateSet("none", "60fps", "30fps", "detail", "quality")]
    [string]$PerfMode = "quality",
    [double]$MaxBodyP90Ms = 10.0,
    [double]$MaxBodyMaxMs = 10.0,
    [int]$MaxOver16 = 0,
    [int]$MaxVisibleMissingNonzero = 0,
    [int]$MaxResidentMissingNonzero = 0,
    [switch]$ReplayBrush
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$buildDir = if (-not [string]::IsNullOrWhiteSpace($env:VENPOD_BUILD_DIR)) {
    $env:VENPOD_BUILD_DIR
} else {
    Join-Path $root "build"
}
$metricsPath = Join-Path $buildDir "bin/prodbench/$Label/omni_metrics.json"

function Fail-Guard([string]$message) {
    Write-Host "OMNI_GUARD_FAIL $message"
    exit 1
}

Push-Location $root
try {
    $vsDevCmd = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
    if (-not (Test-Path -LiteralPath $vsDevCmd)) {
        Fail-Guard "missing VS dev environment: $vsDevCmd"
    }

    $buildCmd = "call ""$vsDevCmd"" -arch=amd64 -host_arch=amd64 >nul && cmake --build ""$buildDir"" --config Release --parallel"
    & cmd.exe /d /s /c $buildCmd
    if ($LASTEXITCODE -ne 0) {
        Fail-Guard "Release build failed exit=$LASTEXITCODE"
    }

    $prodbenchArgs = @(
        "-ExecutionPolicy", "Bypass",
        "-File", "$root/scripts/prodbench.ps1",
        "-NoBuild",
        "-Frames", "$Frames",
        "-Warmup", "$Warmup",
        "-Label", "$Label",
        "-Replay", "$Replay",
        "-PerfMode", "$PerfMode"
    )
    if ($ReplayBrush) {
        $prodbenchArgs += "-ReplayBrush"
    }
    $output = & powershell @prodbenchArgs 2>&1
    $exitCode = $LASTEXITCODE
    $text = ($output | Out-String)
    Write-Host $text
    if ($exitCode -ne 0) {
        Fail-Guard "prodbench failed exit=$exitCode"
    }

    $body = [regex]::Match(
        $text,
        "bodyMs\s+p50=([0-9.]+)\s+p90=([0-9.]+)\s+p99=([0-9.]+)\s+max=([0-9.]+)\s+over10ms=(\d+)/(\d+)\s+over16\.67ms=(\d+)/(\d+)")
    if (-not $body.Success) {
        Fail-Guard "could not parse bodyMs summary"
    }

    $missing = [regex]::Match(
        $text,
        "missing\s+visibleNonzero=(\d+)\s+residentSurfaceNonzero=(\d+)")
    if (-not $missing.Success) {
        Fail-Guard "could not parse missing summary"
    }

    $gpu = [regex]::Match(
        $text,
        "gpuFrameMs\s+allP50=([0-9.]+)\s+allP90=([0-9.]+)")
    $background = [regex]::Match(
        $text,
        "backgroundPixelsP50\s+:\s+([0-9.]+)")

    $metrics = [ordered]@{
        label = $Label
        frames = $Frames
        warmup = $Warmup
        replay = $Replay
        replayBrush = [bool]$ReplayBrush
        perfMode = $PerfMode
        bodyMsP50 = [double]$body.Groups[1].Value
        bodyMsP90 = [double]$body.Groups[2].Value
        bodyMsP99 = [double]$body.Groups[3].Value
        bodyMsMax = [double]$body.Groups[4].Value
        over10Count = [int]$body.Groups[5].Value
        sampledFrames = [int]$body.Groups[6].Value
        over16Count = [int]$body.Groups[7].Value
        visibleMissingNonzero = [int]$missing.Groups[1].Value
        residentMissingSurfaceNonzero = [int]$missing.Groups[2].Value
        gpuFrameMsP50 = if ($gpu.Success) { [double]$gpu.Groups[1].Value } else { $null }
        gpuFrameMsP90 = if ($gpu.Success) { [double]$gpu.Groups[2].Value } else { $null }
        backgroundPixelsP50 = if ($background.Success) { [double]$background.Groups[1].Value } else { $null }
        thresholdBodyMsP90 = $MaxBodyP90Ms
        thresholdBodyMsMax = $MaxBodyMaxMs
        thresholdOver16Count = $MaxOver16
        thresholdVisibleMissingNonzero = $MaxVisibleMissingNonzero
        thresholdResidentMissingNonzero = $MaxResidentMissingNonzero
    }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $metricsPath) | Out-Null
    $metrics | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $metricsPath -Encoding UTF8
    Write-Host "OMNI_METRICS $metricsPath"
    Write-Host ("OMNI_METRIC bodyMsP90={0} bodyMsMax={1} over16={2} visibleMissing={3} residentMissingSurface={4}" -f `
        $metrics.bodyMsP90,
        $metrics.bodyMsMax,
        $metrics.over16Count,
        $metrics.visibleMissingNonzero,
        $metrics.residentMissingSurfaceNonzero)

    $failures = @()
    if ($metrics.visibleMissingNonzero -gt $MaxVisibleMissingNonzero) {
        $failures += "visibleMissingNonzero=$($metrics.visibleMissingNonzero) > $MaxVisibleMissingNonzero"
    }
    if ($metrics.residentMissingSurfaceNonzero -gt $MaxResidentMissingNonzero) {
        $failures += "residentMissingSurfaceNonzero=$($metrics.residentMissingSurfaceNonzero) > $MaxResidentMissingNonzero"
    }
    if ($metrics.over16Count -gt $MaxOver16) {
        $failures += "over16Count=$($metrics.over16Count) > $MaxOver16"
    }
    if ($metrics.bodyMsP90 -gt $MaxBodyP90Ms) {
        $failures += "bodyMsP90=$($metrics.bodyMsP90) > $MaxBodyP90Ms"
    }
    if ($metrics.bodyMsMax -gt $MaxBodyMaxMs) {
        $failures += "bodyMsMax=$($metrics.bodyMsMax) > $MaxBodyMaxMs"
    }

    if ($failures.Count -gt 0) {
        Fail-Guard ($failures -join "; ")
    }

    Write-Host "OMNI_GUARD_PASS"
}
finally {
    Pop-Location
}
