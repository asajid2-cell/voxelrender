# turnperf.ps1 - reproduce the USER'S reported lag: stand still, no editing, just
# look around (pure yaw turn) and watch FPS. Drives the walk-test camera with
# speed=0 + continuous yaw, NO brush smoke (zero edits), telemetry on. Parses the
# sustained-turn window so we measure the streaming cost a turn provokes, including
# the spikes that drop 70fps -> 20fps. A/B the look-bias-moving-only gate.
param(
    [int]$LookBiasMovingOnly = 0,   # 0 = old behavior (bias always), 1 = bias only when moving
    [int]$YawDegPerSec = 70,
    [int]$PitchDeg = -12,
    [int]$Frames = 760,
    [string]$Tag = ""
)
$ErrorActionPreference = "Continue"
Set-Location $PSScriptRoot
[Environment]::CurrentDirectory = $PSScriptRoot
if ($Tag -eq "") { $Tag = "lb$LookBiasMovingOnly" }

Get-Process VENPOD -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

$env:VENPOD_VSYNC = "0"
$env:VENPOD_PERF_FRAME_END_LOG_INTERVAL = "1"
$env:VENPOD_EDIT_TELEMETRY = "1"
$env:VENPOD_SPARSE_INTEREST_LOOK_BIAS_MOVING_ONLY = "$LookBiasMovingOnly"
# Pure stationary turn: walk test on, speed 0, continuous yaw, near-horizon pitch
# so the sweep crosses the most distant streamed terrain (worst-case streaming).
$env:VENPOD_SPARSE_WALK_TEST = "1"
$env:VENPOD_SPARSE_WALK_TEST_SPEED = "0"
$env:VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC = "$YawDegPerSec"
$env:VENPOD_SPARSE_WALK_TEST_PITCH_DEG = "$PitchDeg"
$env:VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS = "16"

.\rebrun.ps1 -PerfMode 60fps -NoBuild -ExitAfterFrames $Frames *> $null

foreach ($v in @(
    "VENPOD_VSYNC","VENPOD_PERF_FRAME_END_LOG_INTERVAL","VENPOD_EDIT_TELEMETRY",
    "VENPOD_SPARSE_INTEREST_LOOK_BIAS_MOVING_ONLY",
    "VENPOD_SPARSE_WALK_TEST","VENPOD_SPARSE_WALK_TEST_SPEED",
    "VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC","VENPOD_SPARSE_WALK_TEST_PITCH_DEG",
    "VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS")) {
    [Environment]::SetEnvironmentVariable($v, $null)
}
Get-Process VENPOD -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

# Measure the SUSTAINED turn window (frame 220+, after the public-render gate opens
# and the initial load settles). This is "the world is loaded, I'm just turning".
$turn = @()
Select-String -Path build\bin\venpod_runtime.log -Pattern "PERF_FRAME_END frame=" | ForEach-Object {
    if ($_.Line -match "frame=(\d+) .*rawMs=([0-9.]+)") {
        $f = [int]$matches[1]; $v = [double]$matches[2]
        if ($f -ge 220) { $turn += $v }
    }
}
function Stat($a, $n) {
    if ($a.Count -eq 0) { "${n}: no samples"; return }
    $s = $a | Sort-Object
    $p50 = $s[[int][Math]::Floor($s.Count * 0.5)]
    $p90 = $s[[int][Math]::Min($s.Count - 1, [Math]::Floor($s.Count * 0.90))]
    $p99 = $s[[int][Math]::Min($s.Count - 1, [Math]::Floor($s.Count * 0.99))]
    $fps50 = if ($p50 -gt 0) { 1000.0 / $p50 } else { 0 }
    $h = @($a | Where-Object { $_ -gt 33.4 }).Count
    "{0}: n={1} p50={2:N1}ms ({7:N0}fps) p90={3:N1} p99={4:N1} max={5:N1} hitch33={6} ({8:N1}%)" -f `
        $n, $a.Count, $p50, $p90, $p99, $s[-1], $h, $fps50, (100.0 * $h / $a.Count)
}
"== turnperf Tag=$Tag LookBiasMovingOnly=$LookBiasMovingOnly Yaw=$YawDegPerSec Pitch=$PitchDeg =="
Stat $turn "TURN (220+, stationary look-around)"
# Surface the streaming breakdown on the worst frames so we see WHAT the turn costs.
"--- PERF_SPARSE_STEPS on slow frames (>30ms) ---"
Select-String -Path build\bin\venpod_runtime.log -Pattern "PERF_SPARSE_STEPS" | ForEach-Object {
    if ($_.Line -match "total=([0-9.]+)") { if ([double]$matches[1] -gt 30) { $_.Line } }
} | Select-Object -Last 6
