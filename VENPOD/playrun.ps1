# Replay a recorded VENPOD session deterministically (for profiling + A/B).
#
# Drives the camera from <Path> frame-by-frame (live input disabled), reproducing
# the exact recorded scenario every run -- so the only variable between two
# replays is the code under test. Edit telemetry is on; the run auto-stops when
# the recording is exhausted. Uses the current build (no rebuild). Pair with
# VENPOD_* A/B toggles, e.g.:
#   $env:VENPOD_SPARSE_VIEW_FOLLOW_TRIM="0"; .\playrun.ps1
#
# Usage:
#   .\playrun.ps1                          # replays build\bin\run.rec
#   .\playrun.ps1 -Path build\bin\mtns.rec
#   .\playrun.ps1 -Tag fixA                 # tags the log build\bin\replay_fixA.log
param(
    [string]$Path = "$PSScriptRoot\build\bin\run.rec",
    [string]$Tag = ""
)

$resolved = $Path
if (-not [System.IO.Path]::IsPathRooted($resolved)) {
    $resolved = Join-Path (Get-Location) $resolved
}
if (-not (Test-Path $resolved)) {
    Write-Host "[playrun] No recording at $resolved -- record one first with .\recrun.ps1"
    exit 1
}
$frames = [int](((Get-Item $resolved).Length - 8) / 32)
$env:VENPOD_REPLAY = $resolved
$env:VENPOD_EDIT_TELEMETRY = "1"
# Profiling harness: measure WORK, not vblank waits. vsync=1 makes present block on
# the refresh cap (and, if the window is occluded/headless, DXGI throttles present to
# ~10fps), which inflates body/rawMs and drags the replay out. vsync=0 lets every frame
# run at its true CPU+GPU cost so A/B frame-times are meaningful.
$env:VENPOD_VSYNC = "0"
if ($Tag -ne "") { $env:VENPOD_LOG_FILE = "$PSScriptRoot\build\bin\replay_$Tag.log" }
Write-Host "[playrun] Replaying $resolved ($frames frames, auto-stops at end)."

# Hashtable splat so the -NoBuild switch binds correctly.
$rebrunArgs = @{ PerfMode = '60fps'; NoBuild = $true }
try {
    & "$PSScriptRoot\rebrun.ps1" @rebrunArgs
} finally {
    Remove-Item Env:\VENPOD_REPLAY, Env:\VENPOD_EDIT_TELEMETRY -ErrorAction SilentlyContinue
    if ($Tag -ne "") { Remove-Item Env:\VENPOD_LOG_FILE -ErrorAction SilentlyContinue }
}
