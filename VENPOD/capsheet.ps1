# capsheet.ps1 - REAL capture->contact-sheet harness for honest full-motion verification.
# Captures a representative motion (walk + yaw scan) in the user's run mode, dumps full-res
# frames, and assembles labeled contact sheets sized to actually be readable, plus keeps the
# full-res frames so specific failures can be inspected at native 1:1. NO tiny cherry crops.
param(
    [Parameter(Mandatory=$true)][string]$Tag,
    [string]$Mode = "60fps",      # the user's actual run mode
    [int]$StartCapture = 420,     # let startup converge
    [int]$Every = 24,             # frames between captures (~0.4s at 60fps)
    [int]$Count = 12,             # number of frames
    [int]$Speed = 90,             # walk speed (motion exposes streaming/coverage failures)
    [int]$YawRate = 28,           # deg/sec turn (scan the world)
    [int]$Head = 0,
    [int]$PitchDeg = -10,
    [int]$AltTenths = 0,          # >0 = fly at altitude
    [int]$ThumbW = 480,           # contact-sheet thumb size (readable)
    [int]$Cols = 3,
    [switch]$DagOn,
    [switch]$MidPass,             # enable the mid-only DDA/form pass (off by default)
    [int]$DebugMode = -1,         # >=0 -> rebrun -SparseDebugMode (58 = owner-layer map)
    [int]$MidStart = 0,           # >0 -> VENPOD_SPARSE_MID_START (push coarse mid back)
    [int]$OwnRadius = 0,          # >0 -> VENPOD_SPARSE_SURFACE_OWNERSHIP_RADIUS (near owns further)
    [int]$MidCell = 0             # >0 -> VENPOD_SPARSE_MID_CELL (finer mid voxels)
)
$ErrorActionPreference = "Continue"
Set-Location $PSScriptRoot
[Environment]::CurrentDirectory = $PSScriptRoot
Get-Process VENPOD -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue

$cap = "build\captures\demo_$Tag"
if (Test-Path $cap) { Remove-Item -Recurse -Force $cap }
New-Item -ItemType Directory -Force $cap | Out-Null
$exit = $StartCapture + ($Every * $Count) + 60

$env:VENPOD_VSYNC = "0"
$env:VENPOD_CAPTURE_DIR = (Join-Path $PSScriptRoot $cap)
$env:VENPOD_CAPTURE_START_FRAME = "$StartCapture"
$env:VENPOD_CAPTURE_INTERVAL_FRAMES = "$Every"
$env:VENPOD_CAPTURE_COUNT = "$Count"
$env:VENPOD_CAPTURE_HIDE_UI = "1"
$env:VENPOD_CAMERA_INITIAL_YAW_DEG = "$Head"
$env:VENPOD_SPARSE_WALK_TEST = "1"
$env:VENPOD_SPARSE_WALK_TEST_SPEED = "$Speed"
$env:VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC = "$YawRate"
$env:VENPOD_SPARSE_WALK_TEST_PITCH_DEG = "$PitchDeg"
$env:VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS = "16"
if ($AltTenths -gt 0) {
    $env:VENPOD_SPARSE_REQUIRE_WALK_SUPPORT = "0"
    $env:VENPOD_SPARSE_WALK_MIN_CAMERA_ABOVE_TERRAIN_TENTHS = "$AltTenths"
}
if ($DagOn) { $env:VENPOD_FARVOXEL_DAG = "1" } else { Remove-Item Env:\VENPOD_FARVOXEL_DAG -EA SilentlyContinue }
# -MidPass forces the overlay on; otherwise LEAVE the caller's env alone (an explicit
# VENPOD_RAYMARCH_MID_PASS_ENABLE=0 must survive -- rebrun only defaults it when UNSET;
# the old Remove-Item here silently turned overlay-off experiments back ON).
if ($MidPass) { $env:VENPOD_RAYMARCH_MID_PASS_ENABLE = "1" }
if ($MidStart -gt 0) { $env:VENPOD_SPARSE_MID_START = "$MidStart" } else { Remove-Item Env:\VENPOD_SPARSE_MID_START -EA SilentlyContinue }
if ($OwnRadius -gt 0) { $env:VENPOD_SPARSE_SURFACE_OWNERSHIP_RADIUS = "$OwnRadius" } else { Remove-Item Env:\VENPOD_SPARSE_SURFACE_OWNERSHIP_RADIUS -EA SilentlyContinue }
if ($MidCell -gt 0) { $env:VENPOD_SPARSE_MID_CELL = "$MidCell" } else { Remove-Item Env:\VENPOD_SPARSE_MID_CELL -EA SilentlyContinue }

Write-Host "[capsheet] capturing $Count frames ($Mode, speed=$Speed yaw=$YawRate/s) ..."
if ($DebugMode -ge 0) {
    .\rebrun.ps1 -PerfMode $Mode -NoBuild -ExitAfterFrames $exit -SparseDebugMode $DebugMode *> "$cap\_run.log"
} else {
    .\rebrun.ps1 -PerfMode $Mode -NoBuild -ExitAfterFrames $exit *> "$cap\_run.log"
}

foreach ($v in "VENPOD_VSYNC","VENPOD_CAPTURE_DIR","VENPOD_CAPTURE_START_FRAME","VENPOD_CAPTURE_INTERVAL_FRAMES","VENPOD_CAPTURE_COUNT","VENPOD_CAPTURE_HIDE_UI","VENPOD_CAMERA_INITIAL_YAW_DEG","VENPOD_SPARSE_WALK_TEST","VENPOD_SPARSE_WALK_TEST_SPEED","VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC","VENPOD_SPARSE_WALK_TEST_PITCH_DEG","VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS","VENPOD_SPARSE_REQUIRE_WALK_SUPPORT","VENPOD_SPARSE_WALK_MIN_CAMERA_ABOVE_TERRAIN_TENTHS") {
    [Environment]::SetEnvironmentVariable($v, $null)
}
Get-Process VENPOD -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue

$bmps = @(Get-ChildItem "$cap\*.bmp" -EA SilentlyContinue | Sort-Object Name)
if ($bmps.Count -eq 0) { Write-Output "${Tag}: NO FRAMES (see $cap\_run.log)"; return }

Add-Type -AssemblyName System.Drawing
# convert bmp->png (full res, kept for native inspection)
foreach ($b in $bmps) {
    $img = [System.Drawing.Image]::FromFile($b.FullName)
    $img.Save(($b.FullName -replace '\.bmp$','.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    $img.Dispose()
}
$pngs = @(Get-ChildItem "$cap\*.png" -EA SilentlyContinue | Sort-Object Name)
$srcW = 0; $srcH = 0
$first = [System.Drawing.Image]::FromFile($pngs[0].FullName); $srcW=$first.Width; $srcH=$first.Height; $first.Dispose()
$thumbH = [int]($ThumbW * $srcH / $srcW)
# split into sheets of at most (Cols * 3) frames so each sheet stays readable when viewed
$perSheet = $Cols * 3
$sheetIdx = 0
for ($start = 0; $start -lt $pngs.Count; $start += $perSheet) {
    $batch = $pngs[$start..([Math]::Min($start+$perSheet-1, $pngs.Count-1))]
    $rows = [math]::Ceiling($batch.Count / $Cols)
    $sheet = New-Object System.Drawing.Bitmap ($ThumbW * $Cols), (($thumbH + 18) * $rows)
    $g = [System.Drawing.Graphics]::FromImage($sheet)
    $g.Clear([System.Drawing.Color]::Black)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $font = New-Object System.Drawing.Font "Consolas", 10
    for ($i = 0; $i -lt $batch.Count; $i++) {
        $img = [System.Drawing.Image]::FromFile($batch[$i].FullName)
        $x = ($i % $Cols) * $ThumbW; $y = [math]::Floor($i / $Cols) * ($thumbH + 18)
        $g.DrawImage($img, $x, $y, $ThumbW, $thumbH)
        $g.DrawString($batch[$i].BaseName, $font, [System.Drawing.Brushes]::Yellow, $x + 3, $y + $thumbH + 1)
        $img.Dispose()
    }
    $g.Dispose()
    $sheetPath = Join-Path $PSScriptRoot "$cap\contact_$sheetIdx.png"
    $sheet.Save($sheetPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $sheet.Dispose()
    Write-Output "sheet -> $sheetPath ($($batch.Count) frames @ ${ThumbW}x${thumbH})"
    $sheetIdx++
}
Write-Output "${Tag}: $($pngs.Count) full-res frames ($srcW x $srcH) in $cap (native PNGs kept)"
