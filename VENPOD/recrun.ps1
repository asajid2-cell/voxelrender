# Record a VENPOD session for deterministic replay.
#
# Runs the engine normally (rebrun) with recording on: the camera path + brush
# intent are captured every frame and flushed to <Path> when you quit. Fly your
# exact scenario (e.g. out to the mountains + edit), then close the window /
# press Esc so the recording flushes cleanly (do NOT force-kill).
#
# Then replay it identically with:  .\playrun.ps1 -Path <Path>
#
# Usage:
#   .\recrun.ps1                       # records to build\bin\run.rec, builds first
#   .\recrun.ps1 -NoBuild              # skip the build (use current exe)
#   .\recrun.ps1 -Path build\bin\mtns.rec -NoBuild
param(
    [string]$Path = "$PSScriptRoot\build\bin\run.rec",
    [Parameter(ValueFromRemainingArguments = $true)]
    $RebrunArgs
)

$resolved = $Path
if (-not [System.IO.Path]::IsPathRooted($resolved)) {
    $resolved = Join-Path (Get-Location) $resolved
}
$env:VENPOD_RECORD = $resolved
Write-Host "[recrun] Recording to: $resolved"
Write-Host "[recrun] Fly/edit your scenario, then QUIT normally (Esc / close window) to flush."
try {
    & "$PSScriptRoot\rebrun.ps1" @RebrunArgs
} finally {
    Remove-Item Env:\VENPOD_RECORD -ErrorAction SilentlyContinue
}
if (Test-Path $resolved) {
    $frames = [int](((Get-Item $resolved).Length - 8) / 32)
    Write-Host "[recrun] Saved $resolved ($frames frames). Replay: .\playrun.ps1 -Path `"$resolved`""
} else {
    Write-Host "[recrun] WARNING: no recording written (was the session force-killed?)."
}
