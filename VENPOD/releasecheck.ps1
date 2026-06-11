# releasecheck.ps1 - stitched release verification: capture the user's situations
# (fly at multiple headings/altitudes + ground) as motion grids + pixel counts,
# so every camera mode can be judged frame-by-frame for rendering mistakes.
# Assumes shaders already compiled (run after a build that passed the crash gate).
$ErrorActionPreference = "Continue"
Set-Location $PSScriptRoot
[Environment]::CurrentDirectory = $PSScriptRoot
$ff = "C:\Users\Ahmed\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg.Essentials_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-8.0-essentials_build\bin\ffmpeg.exe"

# Each: tag, altTenths (0=ground), pitch, head, yawRate, speed, frames
$runs = @(
  @{ Tag="rc_fly0_h0";    Alt=2500;  Pitch=-16; Head=0;   Yaw=0; Speed=80;  Frames=300 },
  @{ Tag="rc_fly0_h90";   Alt=2500;  Pitch=-16; Head=90;  Yaw=0; Speed=80;  Frames=300 },
  @{ Tag="rc_fly0_h200";  Alt=3000;  Pitch=-18; Head=200; Yaw=0; Speed=90;  Frames=300 },
  @{ Tag="rc_aerial";     Alt=15000; Pitch=-42; Head=30;  Yaw=6; Speed=200; Frames=300 },
  @{ Tag="rc_ground";     Alt=0;     Pitch=-10; Head=200; Yaw=0; Speed=38;  Frames=280 }
)

$summary = @()
foreach ($r in $runs) {
  Get-Process VENPOD -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
  $env:VENPOD_VSYNC="0"; $env:VENPOD_CAMERA_INITIAL_YAW_DEG="$($r.Head)"
  $env:VENPOD_SPARSE_WALK_TEST_PITCH_DEG="$($r.Pitch)"; $env:VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC="$($r.Yaw)"
  if ($r.Alt -gt 0) { $env:VENPOD_SPARSE_REQUIRE_WALK_SUPPORT="0"; $env:VENPOD_SPARSE_WALK_MIN_CAMERA_ABOVE_TERRAIN_TENTHS="$($r.Alt)" }
  .\democapture.ps1 -Tag $r.Tag -Mode 60fps -StartCapture 200 -Frames $r.Frames -Speed $r.Speed -Head $r.Head -PitchDeg $r.Pitch -YawRate $r.Yaw -AltTenths $r.Alt -VideoFps 60 | Out-Null
  # pixel counts from this run's log
  $px = ""
  $line = Select-String -Path build\bin\venpod_runtime.log -Pattern "PERF_RENDER_OWNERSHIP" -EA SilentlyContinue | Select-Object -Last 1
  if ($line) {
    $m = [regex]::Match($line.Line, "total=(\d+).*?farSvo=(\d+) farHeight=(\d+) farWater=(\d+)")
    if ($m.Success) {
      $tot=[double]$m.Groups[1].Value; $fw=[double]$m.Groups[4].Value
      $px = "total=$($m.Groups[1].Value) farHeight=$($m.Groups[3].Value) farWater=$($m.Groups[4].Value) (water " + [math]::Round(100*$fw/[math]::Max(1,$tot),0) + "%)"
    }
  }
  foreach ($v in "VENPOD_SPARSE_REQUIRE_WALK_SUPPORT","VENPOD_SPARSE_WALK_MIN_CAMERA_ABOVE_TERRAIN_TENTHS") { [Environment]::SetEnvironmentVariable($v,$null) }
  # grid
  & $ff -y -i "docs\assets\$($r.Tag).mp4" -vf "select='not(mod(n,40))',scale=476:268,tile=4x3:padding=3:color=gray" -frames:v 1 -fps_mode vfr "build\captures\$($r.Tag)_grid.png" 2>$null
  $summary += "$($r.Tag): $px"
}
foreach ($v in "VENPOD_VSYNC","VENPOD_CAMERA_INITIAL_YAW_DEG","VENPOD_SPARSE_WALK_TEST_PITCH_DEG","VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC") { [Environment]::SetEnvironmentVariable($v,$null) }
Get-Process VENPOD -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
Write-Output "=== RELEASE CHECK PIXEL COUNTS ==="
$summary | ForEach-Object { Write-Output $_ }
Write-Output ("errors: " + (Select-String -Path build\bin\venpod_runtime.log -Pattern "0x887A0005|DEVICE_HUNG|Failed to create graphics" -EA SilentlyContinue | Measure-Object).Count)
Write-Output ("grids: " + ((Get-ChildItem build\captures\rc_*_grid.png -EA SilentlyContinue | Measure-Object).Count))
