# democapture.ps1 - capture demo stills + smooth video of VENPOD for the public README.
# Smooth-motion trick: the scripted walk/fly camera advances by FIXED_DT per frame, so we
# dump EVERY frame (interval 1) and assemble at -VideoFps; real-time capture speed is irrelevant.
# UI is hidden in captured frames (VENPOD_CAPTURE_HIDE_UI=1).
param(
    [Parameter(Mandatory=$true)][string]$Tag,
    [string]$Mode = "60fps",          # 60fps = smooth motion video; quality = full-res stills
    [int]$StartCapture = 150,         # let terrain converge first
    [int]$Frames = 360,
    [int]$Speed = 120,
    [int]$Head = 0,                   # initial heading deg
    [int]$YawRate = 0,                # deg/sec turn
    [int]$PitchDeg = -18,
    [int]$AltTenths = 0,              # 0 = ground/walk-support; >0 = fly at this altitude (tenths of a unit)
    [switch]$Stills,                  # capture a few converged stills instead of video
    [int]$VideoFps = 60,
    [int]$StillEvery = 60,            # stills mode: capture interval
    [int]$StillCount = 6
)
$ErrorActionPreference = "Continue"
Set-Location $PSScriptRoot
[Environment]::CurrentDirectory = $PSScriptRoot
$ff = "C:\Users\Ahmed\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg.Essentials_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-8.0-essentials_build\bin\ffmpeg.exe"

$cap = Join-Path $PSScriptRoot "build\captures\demo_$Tag"
if (Test-Path $cap) { Remove-Item -Recurse -Force $cap }
New-Item -ItemType Directory -Force $cap | Out-Null

Get-Process VENPOD -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue

$interval = if ($Stills) { $StillEvery } else { 1 }
$count = if ($Stills) { $StillCount } else { $Frames }
$exit = $StartCapture + ($interval * $count) + 40

$env:VENPOD_VSYNC = "0"
$env:VENPOD_CAPTURE_DIR = $cap
$env:VENPOD_CAPTURE_START_FRAME = "$StartCapture"
$env:VENPOD_CAPTURE_INTERVAL_FRAMES = "$interval"
$env:VENPOD_CAPTURE_COUNT = "$count"
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

.\rebrun.ps1 -PerfMode $Mode -NoBuild -ExitAfterFrames $exit | Out-Null

foreach ($v in "VENPOD_VSYNC","VENPOD_CAPTURE_DIR","VENPOD_CAPTURE_START_FRAME","VENPOD_CAPTURE_INTERVAL_FRAMES","VENPOD_CAPTURE_COUNT","VENPOD_CAPTURE_HIDE_UI","VENPOD_CAMERA_INITIAL_YAW_DEG","VENPOD_SPARSE_WALK_TEST","VENPOD_SPARSE_WALK_TEST_SPEED","VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC","VENPOD_SPARSE_WALK_TEST_PITCH_DEG","VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS","VENPOD_SPARSE_REQUIRE_WALK_SUPPORT","VENPOD_SPARSE_WALK_MIN_CAMERA_ABOVE_TERRAIN_TENTHS") {
    [Environment]::SetEnvironmentVariable($v, $null)
}
Get-Process VENPOD -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue

$bmps = @(Get-ChildItem "$cap\*.bmp" -EA SilentlyContinue | Sort-Object Name)
Write-Output "$Tag captured $($bmps.Count) frames"
if ($bmps.Count -eq 0) { return }

Add-Type -AssemblyName System.Drawing
if ($Stills) {
    foreach ($b in $bmps) {
        $img = [System.Drawing.Image]::FromFile($b.FullName)
        $img.Save(($b.FullName -replace '\.bmp$','.png'), [System.Drawing.Imaging.ImageFormat]::Png)
        $img.Dispose()
    }
    Write-Output "$Tag stills -> $cap (*.png)"
} else {
    $first = [int]($bmps[0].BaseName -replace 'engine_frame_','')
    $out = Join-Path $PSScriptRoot "docs\assets\$Tag.mp4"
    New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
    & $ff -y -framerate $VideoFps -start_number $first -i "$cap\engine_frame_%04d.bmp" -c:v libx264 -pix_fmt yuv420p -crf 20 -movflags +faststart $out 2>$null
    if (Test-Path $out) {
        Write-Output ("$Tag VIDEO -> $out  (" + [math]::Round((Get-Item $out).Length/1MB,2) + " MB, " + [math]::Round($bmps.Count/$VideoFps,1) + "s)")
    } else { Write-Output "$Tag ffmpeg FAILED" }
}