# lodpolicy.ps1 - non-greedy mid-voxel LOD admission sweep (cost-center C).
# VENPOD_SPARSE_MID_VOXEL_INTEREST_PCT caps the voxel interest set UPSTREAM (admission),
# not a post-hoc drain. Sweep it down and see whether lower mid-voxel residency actually
# reduces the warm-yaw frame, or just shrinks the cache with no win (the trap we already
# caught with the drain). Same-session, count/ms based, warm window (frame>=700).
param([int[]]$Pcts = @(75, 50, 35, 25), [int]$Frames = 1100)
$ErrorActionPreference = "Continue"
$log = "z:\328\CMPUT328-A2\codexworks\301\3d\VENPOD\build\bin\venpod_runtime.log"
function A($pat,$fld,$lo){ $s=0.0;$n=0; Select-String -Path $log -Pattern $pat | ForEach-Object { if($_.Line -match "frame=(\d+)"){$f=[int]$matches[1]; if($f -ge $lo -and $_.Line -match "$fld=([0-9.]+)"){$s+=[double]$matches[1];$n++}}}; if($n){[math]::Round($s/$n,1)}else{"-"} }
"### MID-VOXEL LOD ADMISSION SWEEP (pool 32768) - warm window frame>=700 ###"
"pct  resid  gpu  genPrep surfExt trim reqPrep | rawMs  lodHeld%  midVox%  miss"
foreach ($pct in $Pcts) {
    Get-Process VENPOD -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 600
    $env:VENPOD_SPARSE_MID_VOXEL_INTEREST_PCT="$pct"
    $env:VENPOD_VSYNC="0"; $env:VENPOD_PERF_FRAME_END_LOG_INTERVAL="1"; $env:VENPOD_EDIT_TELEMETRY="1"
    $env:VENPOD_SPARSE_WALK_TEST="1"; $env:VENPOD_SPARSE_WALK_TEST_SPEED="0"
    $env:VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC="90"; $env:VENPOD_SPARSE_WALK_TEST_PITCH_DEG="-12"
    $env:VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS="16"
    & "z:\328\CMPUT328-A2\codexworks\301\3d\VENPOD\rebrun.ps1" -PerfMode 60fps -NoBuild -ExitAfterFrames $Frames *> $null
    foreach($v in @("VENPOD_SPARSE_MID_VOXEL_INTEREST_PCT","VENPOD_VSYNC","VENPOD_PERF_FRAME_END_LOG_INTERVAL","VENPOD_EDIT_TELEMETRY","VENPOD_SPARSE_WALK_TEST","VENPOD_SPARSE_WALK_TEST_SPEED","VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC","VENPOD_SPARSE_WALK_TEST_PITCH_DEG","VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS")){[Environment]::SetEnvironmentVariable($v,$null)}
    Get-Process VENPOD -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    $resid=0; Select-String -Path $log -Pattern "EDIT_TELEM frame=" | ForEach-Object { if($_.Line -match "frame=(\d+).*residentBricks=(\d+)"){ if([int]$matches[1] -ge 700){$resid=[int]$matches[2]} } }
    $miss=0; $held=0; $mv=0; $tot=46656
    Select-String -Path $log -Pattern "PERF_RENDER_OWNERSHIP" | ForEach-Object {
        if($_.Line -match "retireFrame=(\d+)" -and [int]$matches[1] -ge 700){
            if($_.Line -match " miss=(\d+)"){if([int]$matches[1] -gt $miss){$miss=[int]$matches[1]}}
            if($_.Line -match "lodParentHeld=(\d+)"){$held=[int]$matches[1]}
            if($_.Line -match "midVoxel=(\d+)"){$mv=[int]$matches[1]}
            if($_.Line -match "total=(\d+)"){$tot=[int]$matches[1]}
        }
    }
    $heldPct = if($tot){[math]::Round(100.0*$held/$tot,0)}else{0}
    $mvPct = if($tot){[math]::Round(100.0*$mv/$tot,0)}else{0}
    "{0,3}  {1,5}  {2,4} {3,7} {4,6} {5,4} {6,7} | {7,5}  {8,7}  {9,6}  {10}" -f `
        $pct, $resid, (A 'PERF_SPARSE_STEPS frame=' 'gpuMs' 700), (A 'PERF_SPARSE_REQ frame=' 'genPrep' 700), `
        (A 'PERF_SPARSE_STEPS frame=' 'surfExtract' 700), (A 'PERF_SPARSE_STEPS frame=' 'trim' 700), `
        (A 'PERF_SPARSE_REQ frame=' 'reqPrep' 700), (A 'PERF_FRAME_END frame=' 'rawMs' 700), $heldPct, $mvPct, $miss
}
"GATE: pass only if rawMs materially DOWN + miss=0 + lodHeld% not exploding + resident off the 99.8% ceiling."