# Replay a recorded VENPOD session deterministically (for profiling + A/B).
#
# Drives the camera from <Path> frame-by-frame (live input is disabled), so the
# exact recorded scenario is reproduced every run -- the only variable between
# two replays is the code under test. Edit telemetry is on; the run auto-stops
# when the recording is exhausted. Pair with VENPOD_* A/B toggles, e.g.:
#   $env:VENPOD_SPARSE_VIEW_FOLLOW_TRIM="0"; .\playrun.ps1 ...
#
# Usage:
#   .\playrun.ps1                                  # replays build\bin\run.rec
#   .\playrun.ps1 -Path build\bin\mtns.rec
#   .\playrun.ps1 -Tag fixA                         # tags the log for A/B
param(
    [string]$Path = "$PSScriptRoot\build\bin\run.rec",
    [string]$Tag = "",
    [Parameter(ValueFromRemainingArguments = $true)]
    $RebrunArgs
)

if ($null -eq $RebrunArgs) { $RebrunArgs = @() }  # never splat $null (leaks an empty positional arg)
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
if ($Tag -ne "") { $env:VENPOD_LOG_FILE = "$PSScriptRoot\build\bin\replay_$Tag.log" }
Write-Host "[playrun] Replaying $resolved ($frames frames, auto-stops at end)."
try {
    & "$PSScriptRoot\rebrun.ps1" -PerfMode 60fps -NoBuild @RebrunArgs
} finally {
    Remove-Item Env:\VENPOD_REPLAY, Env:\VENPOD_EDIT_TELEMETRY -ErrorAction SilentlyContinue
    if ($Tag -ne "") { Remove-Item Env:\VENPOD_LOG_FILE -ErrorAction SilentlyContinue }
}
