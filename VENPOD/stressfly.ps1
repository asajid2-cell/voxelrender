# stressfly.ps1 - random-ish stress flights with dense capture + contact sheets.
# Each run flies a different speed/heading-rate/pitch/altitude to stress streaming
# in varied directions, dumps frames densely, and builds a labeled contact sheet.
param(
    [int]$RunSet = 1   # 1 = runs A-C, 2 = runs D-E (split to fit tool timeouts)
)
$ErrorActionPreference = "Continue"
Set-Location $PSScriptRoot
# .NET APIs (Bitmap.Save / Image.FromFile) resolve relative paths against the
# PROCESS cwd, which Set-Location does not change - keep them in sync.
[Environment]::CurrentDirectory = $PSScriptRoot

$runs = @(
    @{ Tag="fly_A"; Speed=45; Yaw=35;   Pitch=-30; AltTenths=3000;  Perf=$false; Start=120; Interval=12; Count=24; Exit=420 },
    @{ Tag="fly_B"; Speed=70; Yaw=-80;  Pitch=-55; AltTenths=8000;  Perf=$false; Start=120; Interval=12; Count=24; Exit=420 },
    @{ Tag="fly_C"; Speed=25; Yaw=120;  Pitch=-10; AltTenths=1500;  Perf=$false; Start=120; Interval=12; Count=24; Exit=420 },
    # long-range: fly OUT of the spawn band (slight yaw so the path stays mostly straight),
    # capture the fade annulus (~2-6k u) and the natural band=0 far field (~7-13k u).
    @{ Tag="fly_F_annulus"; Speed=400; Yaw=6;  Pitch=-35; AltTenths=6000;  Perf=$false; Start=400; Interval=20; Count=24; Exit=900 },
    @{ Tag="fly_G_far";     Speed=600; Yaw=-4; Pitch=-45; AltTenths=9000;  Perf=$true;  Start=700; Interval=24; Count=24; Exit=1300 }
)
$selected = switch ($RunSet) { 1 { $runs[0..2] } 2 { $runs[3..4] } default { $runs } }

Add-Type -AssemblyName System.Drawing

foreach ($r in $selected) {
    Get-Process VENPOD -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
    $env:VENPOD_VSYNC = "0"
    # Without this, the walk-support rule resets x/z every frame at altitude and
    # the "flight" silently stands still (all captures identical).
    $env:VENPOD_SPARSE_REQUIRE_WALK_SUPPORT = "0"
    $env:VENPOD_SPARSE_WALK_MIN_CAMERA_ABOVE_TERRAIN_TENTHS = "$($r.AltTenths)"
    $env:VENPOD_SPARSE_WALK_TEST_PITCH_DEG = "$($r.Pitch)"
    $env:VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC = "$($r.Yaw)"
    if ($r.Perf) { $env:VENPOD_PERF_SUMMARY_LOG_INTERVAL = "1" }

    .\capvis.ps1 -Mode 60fps -Tag $r.Tag -StartFrame $r.Start -Interval $r.Interval -Count $r.Count -ExitFrame $r.Exit -Speed $r.Speed | Out-Null

    if ($r.Perf) {
        Copy-Item build\bin\venpod_runtime.log "build\captures\$($r.Tag)\perf_run.log" -Force -EA SilentlyContinue
    }
    foreach ($v in "VENPOD_SPARSE_REQUIRE_WALK_SUPPORT","VENPOD_SPARSE_WALK_MIN_CAMERA_ABOVE_TERRAIN_TENTHS","VENPOD_SPARSE_WALK_TEST_PITCH_DEG","VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC","VENPOD_PERF_SUMMARY_LOG_INTERVAL","VENPOD_VSYNC") {
        [Environment]::SetEnvironmentVariable($v, $null)
    }

    # BMP -> thumbnails -> contact sheet
    $dir = "build\captures\$($r.Tag)"
    $bmps = @(Get-ChildItem "$dir\*.bmp" -EA SilentlyContinue | Sort-Object Name)
    if ($bmps.Count -eq 0) { Write-Output "$($r.Tag): NO FRAMES CAPTURED"; continue }
    $tw = 320; $th = 180; $cols = 4
    $rows = [math]::Ceiling($bmps.Count / $cols)
    $sheet = New-Object System.Drawing.Bitmap ($tw * $cols), (($th + 16) * $rows)
    $g = [System.Drawing.Graphics]::FromImage($sheet)
    $g.Clear([System.Drawing.Color]::Black)
    $font = New-Object System.Drawing.Font "Consolas", 9
    $brush = [System.Drawing.Brushes]::Yellow
    for ($i = 0; $i -lt $bmps.Count; $i++) {
        $img = [System.Drawing.Image]::FromFile($bmps[$i].FullName)
        $x = ($i % $cols) * $tw; $y = [math]::Floor($i / $cols) * ($th + 16)
        $g.DrawImage($img, $x, $y, $tw, $th)
        $g.DrawString($bmps[$i].BaseName, $font, $brush, $x + 2, $y + $th)
        $img.Dispose()
    }
    $g.Dispose()
    $sheetPath = "$dir\contact_sheet.png"
    $sheet.Save($sheetPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $sheet.Dispose()
    Write-Output ("$($r.Tag): " + $bmps.Count + " frames, sheet=$sheetPath  (speed=$($r.Speed) yaw=$($r.Yaw) pitch=$($r.Pitch) alt=$($r.AltTenths/10)u)")
}
Get-Process VENPOD -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
Write-Output ("errors-in-last-log: " + (Select-String -Path build\bin\venpod_runtime.log -Pattern "0x-7FF8FFF2|Failed to create graphics|Map failed|DEVICE_HUNG" -EA SilentlyContinue | Measure-Object).Count)
