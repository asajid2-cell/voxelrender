# yawverify.ps1 - the loop verifier for Goal 2 (yaw perf / hidden-exact).
# Runs ONE scenario+config and reports the contract metrics:
#   C1 warm rawMs (frame>=$Warm), C2 missMax (holes), C4 resident+VIS, C3 midMeshUploads.
# Same-session A/B discipline: call twice (fix off/on) back-to-back, compare deltas.
param(
    [int]$Speed = 0,
    [int]$Yaw = 90,
    [int]$Pitch = -12,
    [int]$Fly = 0,            # eye Y offset (fly height); 0 = ground
    [int]$HiddenExact = 1,    # VENPOD_SPARSE_HIDDEN_EXACT_MISS_FEEDBACK
    [int]$Frames = 1000,
    [int]$Warm = 600,
    [string]$Tag = "run",
    [string]$ExtraEnv = ""    # "NAME=VAL;NAME2=VAL2" applied verbatim (for the fix toggle)
)
$ErrorActionPreference = "Continue"
$log = "z:\328\CMPUT328-A2\codexworks\301\3d\VENPOD\build\bin\venpod_runtime.log"
Get-Process VENPOD -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 600
$env:VENPOD_SPARSE_HIDDEN_EXACT_MISS_FEEDBACK="$HiddenExact"
$env:VENPOD_VSYNC="0"; $env:VENPOD_PERF_FRAME_END_LOG_INTERVAL="1"; $env:VENPOD_EDIT_TELEMETRY="1"
$env:VENPOD_SPARSE_WALK_TEST="1"; $env:VENPOD_SPARSE_WALK_TEST_SPEED="$Speed"
$env:VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC="$Yaw"; $env:VENPOD_SPARSE_WALK_TEST_PITCH_DEG="$Pitch"
$env:VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS="16"
if ($Fly -ne 0) { $env:VENPOD_SPARSE_WALK_TEST_EYE_OFFSET_Y="$Fly"; $env:VENPOD_SPARSE_WALK_TEST_FLY="1" }
$extraNames = @()
if ($ExtraEnv -ne "") { foreach ($kv in $ExtraEnv.Split(";")) { if ($kv -match "^([^=]+)=(.*)$") { Set-Item "env:$($matches[1])" $matches[2]; $extraNames += $matches[1] } } }
& "z:\328\CMPUT328-A2\codexworks\301\3d\VENPOD\rebrun.ps1" -PerfMode 60fps -NoBuild -ExitAfterFrames $Frames *> $null
foreach($v in (@("VENPOD_SPARSE_HIDDEN_EXACT_MISS_FEEDBACK","VENPOD_VSYNC","VENPOD_PERF_FRAME_END_LOG_INTERVAL","VENPOD_EDIT_TELEMETRY","VENPOD_SPARSE_WALK_TEST","VENPOD_SPARSE_WALK_TEST_SPEED","VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC","VENPOD_SPARSE_WALK_TEST_PITCH_DEG","VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS","VENPOD_SPARSE_WALK_TEST_EYE_OFFSET_Y","VENPOD_SPARSE_WALK_TEST_FLY") + $extraNames)) { [Environment]::SetEnvironmentVariable($v,$null) }
Get-Process VENPOD -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

$ran=(Select-String -Path $log -Pattern 'PERF_FRAME_END frame=(\d+)' | Select-Object -Last 1).Matches.Groups[1].Value
$ft=@(); Select-String -Path $log -Pattern "PERF_FRAME_END frame=" | ForEach-Object { if($_.Line -match "frame=(\d+) .*rawMs=([0-9.]+)"){ if([int]$matches[1] -ge $Warm){ $ft+=[double]$matches[2] } } }
$p50="-";$p90="-"; if($ft.Count){ $s=$ft|Sort-Object; $p50=[math]::Round($s[[int][math]::Floor($s.Count*0.5)],1); $p90=[math]::Round($s[[int][math]::Min($s.Count-1,[math]::Floor($s.Count*0.9))],1) }
$res=0;$vis=0; Select-String -Path $log -Pattern "res: poolMB" | Select-Object -Last 1 | ForEach-Object { if($_.Line -match "residentBricks=(\d+) freePages=\d+ spec=\d+ vis=(\d+)"){$res=$matches[1];$vis=$matches[2]} }
$miss=0; Select-String -Path $log -Pattern "PERF_RENDER_OWNERSHIP" | ForEach-Object { if($_.Line -match "retireFrame=(\d+)" -and [int]$matches[1] -ge $Warm -and $_.Line -match " miss=(\d+)"){ if([int]$matches[1] -gt $miss){$miss=[int]$matches[1]} } }
$ups=(Select-String -Path $log -Pattern "Sparse mid mesh upload:").Count
"[$Tag] he=$HiddenExact spd=$Speed yaw=$Yaw fly=$Fly extra='$ExtraEnv' ran=$ran | warm_rawMs p50=$p50 p90=$p90 | resident=$res VIS=$vis | MISS=$miss | midUploads=$ups"