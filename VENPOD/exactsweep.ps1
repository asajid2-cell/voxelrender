# exactsweep.ps1 - objective-gated exact-surface FOOTPRINT sweep (cost-center C, option B).
# Decisive test: does a smaller outer exact-surface footprint let a full stationary
# 360 fit in cache, so the WARM (later) rotations hit resident bricks instead of
# regenerating? Continuous yaw (90 deg/s, fixed dt 16ms => 1.44 deg/frame => 360 ~ 250
# frames). COLD = first rotation after gate-open (bucket 300-499); WARM = later
# rotations (bucket 900-1099). Env-only (no rebuild). Same-session, count/ms based.
param(
    [int[]]$Distances = @(3072, 1536, 768, 384),   # SURFACE_PREFETCH_DISTANCE (voxels); 3072 = baseline
    [int]$Frames = 1160
)
$ErrorActionPreference = "Continue"
$log = "z:\328\CMPUT328-A2\codexworks\301\3d\VENPOD\build\bin\venpod_runtime.log"

function Avg($pattern, $field, $lo, $hi) {
    $s = 0.0; $n = 0
    Select-String -Path $log -Pattern $pattern | ForEach-Object {
        if ($_.Line -match "frame=(\d+)" ) {
            $f = [int]$matches[1]
            if ($f -ge $lo -and $f -lt $hi -and $_.Line -match "$field=([0-9.]+)") { $s += [double]$matches[1]; $n++ }
        }
    }
    if ($n -gt 0) { return [math]::Round($s/$n, 2) } else { return "-" }
}
function ResidentAt($lo, $hi) {
    $last = 0; $peak = 0
    Select-String -Path $log -Pattern "EDIT_TELEM frame=" | ForEach-Object {
        if ($_.Line -match "frame=(\d+).*residentBricks=(\d+)") {
            $f = [int]$matches[1]; $r = [int]$matches[2]
            if ($f -ge $lo -and $f -lt $hi) { $last = $r; if ($r -gt $peak) { $peak = $r } }
        }
    }
    return @($last, $peak)
}
function MissMax() {
    $m = 0
    Select-String -Path $log -Pattern "PERF_RENDER_OWNERSHIP" | ForEach-Object {
        if ($_.Line -match " miss=(\d+)") { if ([int]$matches[1] -gt $m) { $m = [int]$matches[1] } }
    }
    return $m
}

"###### EXACT-SURFACE FOOTPRINT SWEEP (pool cap 32768) ######"
"dist  ran  resid(warm/peak)  cold_genPrep cold_surfExt | warm_genPrep warm_surfExt warm_reqPrep warm_rawMs  missMax  cacheFit"
foreach ($dist in $Distances) {
    Get-Process VENPOD -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 600
    $env:VENPOD_SPARSE_MID_VOXEL_RADIUS_XZ = "$dist"
    $env:VENPOD_VSYNC="0"; $env:VENPOD_PERF_FRAME_END_LOG_INTERVAL="1"; $env:VENPOD_EDIT_TELEMETRY="1"
    $env:VENPOD_SPARSE_WALK_TEST="1"; $env:VENPOD_SPARSE_WALK_TEST_SPEED="0"
    $env:VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC="90"; $env:VENPOD_SPARSE_WALK_TEST_PITCH_DEG="-12"
    $env:VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS="16"
    & "z:\328\CMPUT328-A2\codexworks\301\3d\VENPOD\rebrun.ps1" -PerfMode 60fps -NoBuild -ExitAfterFrames $Frames *> $null
    foreach($v in @("VENPOD_SPARSE_MID_VOXEL_RADIUS_XZ","VENPOD_VSYNC","VENPOD_PERF_FRAME_END_LOG_INTERVAL","VENPOD_EDIT_TELEMETRY","VENPOD_SPARSE_WALK_TEST","VENPOD_SPARSE_WALK_TEST_SPEED","VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC","VENPOD_SPARSE_WALK_TEST_PITCH_DEG","VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS")){[Environment]::SetEnvironmentVariable($v,$null)}
    Get-Process VENPOD -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

    $ranTo = (Select-String -Path $log -Pattern 'PERF_FRAME_END frame=(\d+)' | Select-Object -Last 1).Matches.Groups[1].Value
    $coldG = Avg "PERF_SPARSE_REQ frame=" "genPrep" 300 500
    $coldS = Avg "PERF_SPARSE_STEPS frame=" "surfExtract" 300 500
    $warmG = Avg "PERF_SPARSE_REQ frame=" "genPrep" 900 1100
    $warmS = Avg "PERF_SPARSE_STEPS frame=" "surfExtract" 900 1100
    $warmR = Avg "PERF_SPARSE_REQ frame=" "reqPrep" 900 1100
    $warmMs = Avg "PERF_FRAME_END frame=" "rawMs" 900 1100
    $res = ResidentAt 900 1100; $peak2 = (ResidentAt 0 99999)[1]
    $miss = MissMax
    $fit = [math]::Round($peak2 / 32768.0, 2)
    "{0,4}  {1,4}  {2,6}/{3,-6}  {4,11} {5,11} | {6,11} {7,11} {8,11} {9,9}  {10,6}  {11}" -f `
        $dist, $ranTo, $res[0], $peak2, $coldG, $coldS, $warmG, $warmS, $warmR, $warmMs, $miss, $fit
}
"GATE: pass if warm_genPrep ~0 + warm_surfExt <1-2 + warm_rawMs materially lower + missMax=0 + cacheFit <= 0.7-0.8"