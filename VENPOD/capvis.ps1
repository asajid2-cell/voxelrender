# Visual self-verification: run the walk scenario in quality mode, capture a few frames,
# convert to half-res PNG for reading. Usage: .\capvis.ps1 [-Mode quality] [-Tag baseline]
param(
    [string]$Mode = "quality",
    [string]$Tag = "vis",
    [int]$StartFrame = 520,
    [int]$Interval = 60,
    [int]$Count = 3,
    [int]$ExitFrame = 660,
    [int]$Speed = 26
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$cap = Join-Path $root "build\captures\$Tag"
if (Test-Path $cap) { Remove-Item -Recurse -Force $cap }
$env:VENPOD_CAPTURE_DIR = $cap
$env:VENPOD_CAPTURE_START_FRAME = "$StartFrame"
$env:VENPOD_CAPTURE_INTERVAL_FRAMES = "$Interval"
$env:VENPOD_CAPTURE_COUNT = "$Count"
$env:VENPOD_CAPTURE_HIDE_UI = "1"
$env:VENPOD_SPARSE_WALK_TEST = "1"
$env:VENPOD_SPARSE_WALK_TEST_SPEED = "$Speed"
$env:VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS = "16"
try {
    & (Join-Path $root "rebrun.ps1") -PerfMode $Mode -NoBuild -ExitAfterFrames $ExitFrame | Out-Null
} finally {
    foreach ($v in "VENPOD_CAPTURE_DIR","VENPOD_CAPTURE_START_FRAME","VENPOD_CAPTURE_INTERVAL_FRAMES","VENPOD_CAPTURE_COUNT","VENPOD_CAPTURE_HIDE_UI","VENPOD_SPARSE_WALK_TEST","VENPOD_SPARSE_WALK_TEST_SPEED","VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS") {
        Remove-Item "env:$v" -ErrorAction SilentlyContinue
    }
}
Add-Type -AssemblyName System.Drawing
Get-ChildItem $cap -Filter *.bmp | ForEach-Object {
    $img = [System.Drawing.Image]::FromFile($_.FullName)
    $out = $_.FullName -replace '\.bmp$','.png'
    $w = [int]($img.Width/2); $h = [int]($img.Height/2)
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.DrawImage($img, 0, 0, $w, $h)
    $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose(); $img.Dispose()
    Write-Output $out
}
