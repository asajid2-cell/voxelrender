# Record a VENPOD session for deterministic replay.
#
# Runs the engine (rebrun) with recording on, using the CURRENT build -- the
# camera path + brush intent are captured every frame and flushed to <Path>
# when you quit. Fly your exact scenario (e.g. out to the mountains + edit),
# then close the window / press Esc so the recording flushes (do NOT force-kill).
#
# Replay it identically with:  .\playrun.ps1 -Path <Path>
#
# Usage:
#   .\recrun.ps1                          # records to build\bin\run.rec
#   .\recrun.ps1 -Path build\bin\mtns.rec
#   .\recrun.ps1 -Build                   # rebuild first (default: use current exe)
param(
    [string]$Path = "$PSScriptRoot\build\bin\run.rec",
    [switch]$Build,
    [int]$ExitAfterFrames = 0
)

$resolved = $Path
if (-not [System.IO.Path]::IsPathRooted($resolved)) {
    $resolved = Join-Path (Get-Location) $resolved
}
$env:VENPOD_RECORD = $resolved
# Capture full per-frame + per-edit telemetry into venpod_runtime.log so the recorded
# session can be analyzed directly (PERF_SPARSE_STEPS / EDIT_TELEM are gated behind this).
$env:VENPOD_EDIT_TELEMETRY = "1"
Write-Host "[recrun] Recording to: $resolved (current build, telemetry ON)"
Write-Host "[recrun] Fly/edit your scenario, then QUIT normally (Esc / close window) to flush."

# Hashtable splat so switches/named params bind correctly (array splat would pass
# '-NoBuild' as a positional string and break rebrun's -Config).
$rebrunArgs = @{ PerfMode = '60fps' }
if (-not $Build) { $rebrunArgs['NoBuild'] = $true }
if ($ExitAfterFrames -gt 0) { $rebrunArgs['ExitAfterFrames'] = $ExitAfterFrames }
try {
    & "$PSScriptRoot\rebrun.ps1" @rebrunArgs
} finally {
    Remove-Item Env:\VENPOD_RECORD, Env:\VENPOD_EDIT_TELEMETRY -ErrorAction SilentlyContinue
}

if (Test-Path $resolved) {
    $frames = [int](((Get-Item $resolved).Length - 8) / 32)
    Write-Host "[recrun] Saved $resolved ($frames frames). Replay: .\playrun.ps1 -Path `"$resolved`""
} else {
    Write-Host "[recrun] WARNING: no recording written (was the session force-killed?)."
}
