param(
    [int]$ExitAfterFrames = 900
)

$ErrorActionPreference = "Stop"

$RepoRoot = "Z:\328\CMPUT328-A2\codexworks\301\3d\VENPOD"
$BuildDir = if (-not [string]::IsNullOrWhiteSpace($env:VENPOD_BUILD_DIR)) {
    $env:VENPOD_BUILD_DIR
} else {
    Join-Path $RepoRoot "build"
}
$LogPath = Join-Path $BuildDir "bin\venpod_runtime.log"

$env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE = "1"
$env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE_START_FRAME = "70"
$env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE_END_FRAME = "260"
$env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE_MIN_FRAMES = "30"

& (Join-Path $RepoRoot "rebrun.ps1") -NoBuild -SparseBrushSmokeUserPath -ExitAfterFrames $ExitAfterFrames
$runExit = $LASTEXITCODE

if (-not (Test-Path $LogPath)) {
    Write-Host "EDIT_PUBLISH_GUARD status=FAIL reason=missing_log"
    exit 20
}

$log = Get-Content -Raw -LiteralPath $LogPath
$smokeMatch = [regex]::Match($log, "SPARSE_BRUSH_PAINT_SMOKE passed .*?deltas=([0-9]+).*?cases=([0-9]+)/4")
$hasSmokePass = $smokeMatch.Success
$editDeltas = if ($hasSmokePass) { [int]$smokeMatch.Groups[1].Value } else { 0 }
$editCases = if ($hasSmokePass) { [int]$smokeMatch.Groups[2].Value } else { 0 }

$criticalCount = ([regex]::Matches($log, "\[critical\]")).Count
$shutdownClean = ($runExit -eq 0) -or $log.Contains("VENPOD shut down cleanly")

$maxFrameSeen = 0
foreach ($m in [regex]::Matches($log, "\bframe=([0-9]+)")) {
    $frame = [int]$m.Groups[1].Value
    if ($frame -gt $maxFrameSeen) { $maxFrameSeen = $frame }
}
$exitAfterReached =
    $log.Contains("VENPOD_EXIT_AFTER_FRAMES reached") -or
    ($runExit -eq 0 -and $maxFrameSeen -ge [Math]::Max(0, $ExitAfterFrames - 60))

$missingNonzero = 0
$residentMissingSurfaceNonzero = 0
foreach ($m in [regex]::Matches($log, "PERF_SPARSE_READINESS frame=([0-9]+).*?missing=([0-9]+).*?residentMissingSurface=([0-9]+)")) {
    $frame = [int]$m.Groups[1].Value
    if ($frame -lt 200) { continue }
    if ([int]$m.Groups[2].Value -ne 0) { ++$missingNonzero }
    if ([int]$m.Groups[3].Value -ne 0) { ++$residentMissingSurfaceNonzero }
}

$maxPublishLag = 0
$finalPublishLag = 0
$maxPublishReady = 0
$finalPublishReady = 0
$maxSurfaceGateReady = 0
foreach ($m in [regex]::Matches($log, "PERF_SPARSE frame=([0-9]+).*?publishPending=([0-9]+) publishReady=([0-9]+).*?publishLag=([0-9]+).*?publishSurfGate=([0-9]+)/([0-9]+)")) {
    $lag = [int]$m.Groups[4].Value
    $ready = [int]$m.Groups[3].Value
    $gateReady = [int]$m.Groups[5].Value
    if ($lag -gt $maxPublishLag) { $maxPublishLag = $lag }
    if ($ready -gt $maxPublishReady) { $maxPublishReady = $ready }
    if ($gateReady -gt $maxSurfaceGateReady) { $maxSurfaceGateReady = $gateReady }
    $finalPublishLag = $lag
    $finalPublishReady = $ready
}

$maxPrePublishMs = 0.0
$maxPrePublishGeneral = 0
foreach ($m in [regex]::Matches($log, "PERF_SPARSE_PRE_PUBLISH_SURFACE frame=([0-9]+).*?general=([0-9]+).*?elapsedMs=([0-9.]+)")) {
    $frame = [int]$m.Groups[1].Value
    if ($frame -lt 200) { continue }
    $general = [int]$m.Groups[2].Value
    $elapsed = [double]$m.Groups[3].Value
    if ($general -gt $maxPrePublishGeneral) { $maxPrePublishGeneral = $general }
    if ($elapsed -gt $maxPrePublishMs) { $maxPrePublishMs = $elapsed }
}

Write-Host ("EDIT_PUBLISH_GUARD smoke_pass={0} edit_deltas={1} edit_cases={2} critical_count={3} missing_nonzero={4} resident_missing_surface_nonzero={5} max_publish_lag={6} final_publish_lag={7} max_publish_ready={8} final_publish_ready={9} max_surface_gate_ready={10} max_pre_publish_ms={11:F2} max_pre_publish_general={12} shutdown_clean={13} exit_after_reached={14}" -f `
    ([int]$hasSmokePass), `
    $editDeltas, `
    $editCases, `
    $criticalCount, `
    $missingNonzero, `
    $residentMissingSurfaceNonzero, `
    $maxPublishLag, `
    $finalPublishLag, `
    $maxPublishReady, `
    $finalPublishReady, `
    $maxSurfaceGateReady, `
    $maxPrePublishMs, `
    $maxPrePublishGeneral, `
    ([int]$shutdownClean), `
    ([int]$exitAfterReached))

$failed =
    $runExit -ne 0 -or
    -not $hasSmokePass -or
    $editDeltas -le 0 -or
    $editCases -lt 4 -or
    $criticalCount -ne 0 -or
    $missingNonzero -ne 0 -or
    $residentMissingSurfaceNonzero -ne 0 -or
    $maxPublishLag -gt 8 -or
    $finalPublishLag -gt 2 -or
    $maxPrePublishMs -gt 20.0 -or
    -not $shutdownClean -or
    -not $exitAfterReached

if ($failed) {
    Write-Host "EDIT_PUBLISH_GUARD FAIL"
    exit 21
}

Write-Host "EDIT_PUBLISH_GUARD PASS"
exit 0
