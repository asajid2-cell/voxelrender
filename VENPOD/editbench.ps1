# editbench.ps1 - MACHINE verifier for edit-path hitching (E0).
# Per-frame timing via VENPOD_PERF_FRAME_END_LOG_INTERVAL=1 (rawMs + per-phase splits on
# every frame). Control and edit arms run the IDENTICAL walk scenario; the edit arm adds
# the moving brush-paint smoke. Hitches live in the tail: p50/p95/p99/max + counts.
param(
    [int]$Frames = 900,
    [int]$WarmupFrame = 240,
    [int]$Runs = 2,
    [switch]$NoEdit
)
$ErrorActionPreference = "Continue"
Set-Location $PSScriptRoot
[Environment]::CurrentDirectory = $PSScriptRoot

$allMs = @(); $worst = @()
for ($r = 1; $r -le $Runs; $r++) {
    Get-Process VENPOD -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
    Start-Sleep -Milliseconds 800
    $env:VENPOD_VSYNC = "0"
    $env:VENPOD_PERF_FRAME_END_LOG_INTERVAL = "1"
    $env:VENPOD_SPARSE_WALK_TEST = "1"
    $env:VENPOD_SPARSE_WALK_TEST_SPEED = "25"
    $env:VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC = "10"
    $env:VENPOD_SPARSE_WALK_TEST_PITCH_DEG = "-12"
    $env:VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS = "16"
    if ($NoEdit) {
        .\rebrun.ps1 -PerfMode 60fps -NoBuild -ExitAfterFrames $Frames *> $null
    } else {
        .\rebrun.ps1 -PerfMode 60fps -NoBuild -SparseBrushPaintMovingSmoke -ExitAfterFrames $Frames *> $null
    }
    foreach ($v in "VENPOD_VSYNC","VENPOD_PERF_FRAME_END_LOG_INTERVAL","VENPOD_SPARSE_WALK_TEST","VENPOD_SPARSE_WALK_TEST_SPEED","VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC","VENPOD_SPARSE_WALK_TEST_PITCH_DEG","VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS") {
        [Environment]::SetEnvironmentVariable($v, $null)
    }
    Get-Process VENPOD -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue

    $runMs = @()
    Select-String -Path build\bin\venpod_runtime.log -Pattern "PERF_FRAME_END frame=" | ForEach-Object {
        if ($_.Line -match "frame=(\d+) .*rawMs=([0-9.]+)") {
            $f = [int]$matches[1]
            if ($f -ge $WarmupFrame) {
                $v = [double]$matches[2]
                $runMs += $v
                if ($v -gt 40 -and $_.Line -match "sparsePost=[a-zA-Z/]+:([0-9./]+)") {
                    $script:worst += ("f{0} {1}ms sparsePost={2}" -f $f, $v, $matches[1])
                }
            }
        }
    }
    $allMs += $runMs
    "run${r}: samples=$($runMs.Count)"
}

if ($allMs.Count -lt 200) { Write-Output "EDITBENCH: TOO FEW SAMPLES ($($allMs.Count))"; exit 2 }
$sorted = $allMs | Sort-Object
$p = { param($q) $sorted[[int][Math]::Min($sorted.Count-1, [Math]::Floor($sorted.Count*$q))] }
$h33 = @($allMs | Where-Object { $_ -gt 33.4 }).Count
$h50 = @($allMs | Where-Object { $_ -gt 50.0 }).Count
$mode = if ($NoEdit) { "NOEDIT-BASELINE" } else { "EDITING" }
Write-Output ("EDITBENCH[{0}] n={1} p50={2:N1} p95={3:N1} p99={4:N1} max={5:N1} hitch33={6} hitch50={7} hitchPct={8:N2}%" -f `
    $mode, $allMs.Count, (& $p 0.50), (& $p 0.95), (& $p 0.99), $sorted[-1], $h33, $h50, (100.0*$h33/$allMs.Count))
if ($worst.Count) { "worst frames (sparsePost=feedback/cmd/begin/midSnap/plan/upload/publish/midUpload/stats/surfExtract/surfPlan/surfSnap/surfStage/surfEmit):"; $worst | Select-Object -First 10 }
