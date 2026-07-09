param(
    [switch]$Record,
    [switch]$Replay,
    [switch]$Summary,
    [switch]$ReplayMidMeshOff,
    [string]$Recording = "wave_repro.vnrd",
    [string]$LogDir = "logs"
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$recordingPath = if ([System.IO.Path]::IsPathRooted($Recording)) {
    $Recording
} else {
    Join-Path $scriptDir $Recording
}
$logDirPath = if ([System.IO.Path]::IsPathRooted($LogDir)) {
    $LogDir
} else {
    Join-Path $scriptDir $LogDir
}

if (-not (Test-Path $logDirPath)) {
    New-Item -ItemType Directory -Force -Path $logDirPath | Out-Null
}

$selected = @($Record.IsPresent, $Replay.IsPresent, $Summary.IsPresent, $ReplayMidMeshOff.IsPresent) |
    Where-Object { $_ }
if ($selected.Count -ne 1) {
    throw "Choose exactly one: -Record, -Replay, -ReplayMidMeshOff, or -Summary"
}

if ($Record) {
    & (Join-Path $scriptDir "rebrun_env.ps1") `
        -NoBuild `
        -Env "VENPOD_RECORD=$recordingPath" `
        -SaveLog (Join-Path $logDirPath "wave_record.log")
    return
}

if ($Replay) {
    & (Join-Path $scriptDir "rebrun_env.ps1") `
        -NoBuild `
        -Env "VENPOD_REPLAY=$recordingPath" `
        -Env "VENPOD_DEBUG_MIDMESH_WAVE_TRACE=1" `
        -Env "VENPOD_DEBUG_MIDMESH_WAVE_TRACE_BIN=512" `
        -SaveLog (Join-Path $logDirPath "wave_midmesh_on.log")
    return
}

if ($ReplayMidMeshOff) {
    & (Join-Path $scriptDir "rebrun_env.ps1") `
        -NoBuild `
        -Env "VENPOD_REPLAY=$recordingPath" `
        -Env "VENPOD_DEBUG_MIDMESH_WAVE_TRACE=1" `
        -Env "VENPOD_DEBUG_MIDMESH_WAVE_TRACE_BIN=512" `
        -Env "VENPOD_SPARSE_MID_MESH=0" `
        -SaveLog (Join-Path $logDirPath "wave_midmesh_off.log")
    return
}

if ($Summary) {
    & (Join-Path $scriptDir "midmesh_wave_trace_summary.ps1") `
        -Log (Join-Path $logDirPath "wave_midmesh_on.log") `
        -Bins
    return
}
