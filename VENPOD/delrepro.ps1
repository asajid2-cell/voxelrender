# delrepro.ps1 - F0 repro for the user's edit reports: runs the brush smoke on the
# USER PATH (CPU ApplyBrushEdit, brush feedback OFF - the shipping interactive config)
# aimed at terrain in front of the camera. Captures before/during/after frames so
# deletion visibility can be judged, and splits per-frame timing into pre/edit/post
# windows so the edit cost is measured against an in-run control.
param(
    [int]$Case = 2,            # 0=paint sand, 1=replace glass, 2=erase, 3=replace stone
    [int]$RadiusTenths = 45,
    [int]$Frames = 780,
    [int]$WalkSpeed = 0,       # 0 = stand still (deletion visibility); >0 = perf arm
    [int]$YawDegPerSec = 0,
    [int]$DebugMode = -1,
    [int]$PitchDeg = -45,
    [switch]$RealAim,
    [switch]$Physics,
    [switch]$PaintStone,
    [switch]$DenseCapture,
    [string]$Tag = ""
)
$ErrorActionPreference = "Continue"
Set-Location $PSScriptRoot
[Environment]::CurrentDirectory = $PSScriptRoot

if ($Tag -eq "") { $Tag = "case$Case" }
$cap = Join-Path $PSScriptRoot "build\captures\delrepro_$Tag"
Remove-Item -Recurse -Force $cap -ErrorAction SilentlyContinue

Get-Process VENPOD -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

$env:VENPOD_VSYNC = "0"
$env:VENPOD_PERF_FRAME_END_LOG_INTERVAL = "1"
$env:VENPOD_SPARSE_WALK_TEST = "1"
$env:VENPOD_SPARSE_WALK_TEST_SPEED = "$WalkSpeed"
$env:VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC = "$YawDegPerSec"
if ($env:DELREPRO_YAW_JITTER) { $env:VENPOD_SPARSE_WALK_TEST_YAW_JITTER_DEG_TENTHS = $env:DELREPRO_YAW_JITTER }
$env:VENPOD_SPARSE_WALK_TEST_PITCH_DEG = "$PitchDeg"
$env:VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS = "16"
if ($RealAim) { $env:VENPOD_SPARSE_BRUSH_SMOKE_REAL_AIM = "1" }
if ($PaintStone) { $env:VENPOD_SPARSE_BRUSH_SMOKE_PAINT_STONE = "1" }
$env:VENPOD_SPARSE_BRUSH_SMOKE_FOLLOW_CAMERA = "1"
$env:VENPOD_SPARSE_BRUSH_SMOKE_FOLLOW_DISTANCE = "14"
$env:VENPOD_SPARSE_BRUSH_SMOKE_CASE = "$Case"
$env:VENPOD_SPARSE_BRUSH_SMOKE_RADIUS_TENTHS = "$RadiusTenths"
$env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE_START_FRAME = "300"
$env:VENPOD_SPARSE_BRUSH_PAINT_SMOKE_END_FRAME = "480"
$env:VENPOD_CAPTURE_DIR = $cap
if ($DenseCapture) {
    # Watch the column build: capture every 8 frames across the stroke.
    $env:VENPOD_CAPTURE_START_FRAME = "300"
    $env:VENPOD_CAPTURE_INTERVAL_FRAMES = "8"
    $env:VENPOD_CAPTURE_COUNT = "20"
} else {
    $env:VENPOD_CAPTURE_START_FRAME = "290"
    $env:VENPOD_CAPTURE_INTERVAL_FRAMES = "60"
    $env:VENPOD_CAPTURE_COUNT = "8"
}

$physArg = if ($Physics) { "-SparsePhysics" } else { "" }
if ($DebugMode -ge 0) {
    .\rebrun.ps1 -PerfMode 60fps -NoBuild -SparseBrushSmokeUserPath $physArg -SparseDebugMode $DebugMode -ExitAfterFrames $Frames *> $null
} else {
    .\rebrun.ps1 -PerfMode 60fps -NoBuild -SparseBrushSmokeUserPath $physArg -ExitAfterFrames $Frames *> $null
}

foreach ($v in @(
    "VENPOD_VSYNC","VENPOD_PERF_FRAME_END_LOG_INTERVAL",
    "VENPOD_SPARSE_WALK_TEST","VENPOD_SPARSE_WALK_TEST_SPEED",
    "VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC","VENPOD_SPARSE_WALK_TEST_PITCH_DEG",
    "VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS","VENPOD_SPARSE_WALK_TEST_YAW_JITTER_DEG_TENTHS",
    "VENPOD_SPARSE_BRUSH_SMOKE_FOLLOW_CAMERA","VENPOD_SPARSE_BRUSH_SMOKE_FOLLOW_DISTANCE","VENPOD_SPARSE_BRUSH_SMOKE_REAL_AIM","VENPOD_SPARSE_BRUSH_SMOKE_PAINT_STONE",
    "VENPOD_SPARSE_BRUSH_SMOKE_CASE","VENPOD_SPARSE_BRUSH_SMOKE_RADIUS_TENTHS",
    "VENPOD_SPARSE_BRUSH_PAINT_SMOKE_START_FRAME","VENPOD_SPARSE_BRUSH_PAINT_SMOKE_END_FRAME",
    "VENPOD_CAPTURE_DIR","VENPOD_CAPTURE_START_FRAME",
    "VENPOD_CAPTURE_INTERVAL_FRAMES","VENPOD_CAPTURE_COUNT")) {
    [Environment]::SetEnvironmentVariable($v, $null)
}
Get-Process VENPOD -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

$pre = @(); $edit = @(); $post = @(); $worstEdit = @()
Select-String -Path build\bin\venpod_runtime.log -Pattern "PERF_FRAME_END frame=" | ForEach-Object {
    if ($_.Line -match "frame=(\d+) .*rawMs=([0-9.]+)") {
        $f = [int]$matches[1]; $v = [double]$matches[2]
        if ($f -ge 240 -and $f -lt 300) { $pre += $v }
        elseif ($f -ge 300 -and $f -lt 480) {
            $edit += $v
            if ($v -gt 50) { $worstEdit += ("f{0} {1}ms" -f $f, $v) }
        }
        elseif ($f -ge 480) { $post += $v }
    }
}
function Stat($a, $n) {
    if ($a.Count -eq 0) { "${n}: no samples"; return }
    $s = $a | Sort-Object
    $p50 = $s[[int][Math]::Floor($s.Count * 0.5)]
    $p99 = $s[[int][Math]::Min($s.Count - 1, [Math]::Floor($s.Count * 0.99))]
    $h = @($a | Where-Object { $_ -gt 33.4 }).Count
    "{0}: n={1} p50={2:N1} p99={3:N1} max={4:N1} hitch33={5} ({6:N1}%)" -f $n, $a.Count, $p50, $p99, $s[-1], $h, (100.0 * $h / $a.Count)
}
Stat $pre  "PRE  (240-299, no edits)"
Stat $edit "EDIT (300-479, brush active)"
Stat $post "POST (480+, edits done)"
if ($worstEdit.Count) { "worst edit frames:"; $worstEdit | Select-Object -First 12 }
"captures:"
Get-ChildItem $cap -Filter "engine_frame_*.bmp" -ErrorAction SilentlyContinue | ForEach-Object { "  $($_.Name)" }
Select-String -Path build\bin\venpod_runtime.log -Pattern "BRUSH_PAINT_SMOKE" | Select-Object -Last 4 | ForEach-Object { $_.Line }
