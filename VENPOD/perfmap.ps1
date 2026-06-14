# perfmap.ps1 - holistic per-scenario performance characterization. Runs the walk
# test in several REAL movement modes (idle / look / move / move+look) and, for each,
# reports frame-time stats AND the average PERF_SPARSE_STEPS cost breakdown over the
# steady window, so we can see WHERE the frame time goes in each mode instead of
# guessing. One launch per scenario (this box accumulates GPU-memory pressure across
# launches - prefer a FRESH BOOT before a trusted run, and compare modes within ONE
# session only).
param(
    [string[]]$Scenarios = @("idle","look","move","movelook"),
    [int]$Frames = 700,
    [int]$SteadyStart = 240   # ignore startup/gate-open fill before this frame
)
$ErrorActionPreference = "Continue"
Set-Location $PSScriptRoot
[Environment]::CurrentDirectory = $PSScriptRoot

function Run-Scenario($name, $speed, $yaw) {
    Get-Process VENPOD -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 600
    $env:VENPOD_VSYNC = "0"
    $env:VENPOD_PERF_FRAME_END_LOG_INTERVAL = "1"
    $env:VENPOD_EDIT_TELEMETRY = "1"
    $env:VENPOD_SPARSE_WALK_TEST = "1"
    $env:VENPOD_SPARSE_WALK_TEST_SPEED = "$speed"
    $env:VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC = "$yaw"
    $env:VENPOD_SPARSE_WALK_TEST_PITCH_DEG = "-12"
    $env:VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS = "16"
    .\rebrun.ps1 -PerfMode 60fps -NoBuild -ExitAfterFrames $Frames *> $null
    foreach ($v in @("VENPOD_VSYNC","VENPOD_PERF_FRAME_END_LOG_INTERVAL","VENPOD_EDIT_TELEMETRY",
        "VENPOD_SPARSE_WALK_TEST","VENPOD_SPARSE_WALK_TEST_SPEED","VENPOD_SPARSE_WALK_TEST_YAW_DEG_PER_SEC",
        "VENPOD_SPARSE_WALK_TEST_PITCH_DEG","VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS")) {
        [Environment]::SetEnvironmentVariable($v, $null)
    }
    Get-Process VENPOD -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

    $log = "build\bin\venpod_runtime.log"
    # Frame times
    $ft = @()
    Select-String -Path $log -Pattern "PERF_FRAME_END frame=" | ForEach-Object {
        if ($_.Line -match "frame=(\d+) .*rawMs=([0-9.]+)") {
            if ([int]$matches[1] -ge $SteadyStart) { $ft += [double]$matches[2] }
        }
    }
    # Average key step costs over the steady window
    $fields = "body","gpuMs","reqPrep","genPrep","clipInterest","clipPump","surfExtract","midMeshUpload","trim"
    $sum = @{}; foreach ($f in $fields) { $sum[$f] = 0.0 }
    $cnt = 0
    Select-String -Path $log -Pattern "PERF_SPARSE_STEPS frame=" | ForEach-Object {
        if ($_.Line -match "frame=(\d+) " -and [int]$matches[1] -ge $SteadyStart) {
            $cnt++
            foreach ($f in $fields) {
                if ($_.Line -match "$f=([0-9.]+)") { $sum[$f] += [double]$matches[1] }
            }
        }
    }
    # Last residency snapshot
    $resident = "?"; $free = "?"
    Select-String -Path $log -Pattern "residentBricks=\d+ freePages=\d+" | Select-Object -Last 1 | ForEach-Object {
        if ($_.Line -match "residentBricks=(\d+) freePages=(\d+)") { $resident = $matches[1]; $free = $matches[2] }
    }
    $lastFrame = 0
    Select-String -Path $log -Pattern "PERF_FRAME_END frame=(\d+)" | Select-Object -Last 1 | ForEach-Object {
        if ($_.Line -match "frame=(\d+)") { $lastFrame = [int]$matches[1] }
    }

    if ($ft.Count -eq 0) { "  [$name] no frame samples (last frame=$lastFrame)"; return }
    $s = $ft | Sort-Object
    $p50 = $s[[int][Math]::Floor($s.Count*0.5)]
    $p90 = $s[[int][Math]::Min($s.Count-1,[Math]::Floor($s.Count*0.9))]
    $h = @($ft | Where-Object { $_ -gt 33.4 }).Count
    $fps = if ($p50 -gt 0) { 1000.0/$p50 } else { 0 }
    "== [$name] speed=$speed yaw=$yaw  ranTo=$lastFrame  resident=$resident free=$free =="
    "   FRAME: n={0} p50={1:N1}ms ({2:N0}fps) p90={3:N1} max={4:N1} hitch33={5:N1}%" -f `
        $ft.Count, $p50, $fps, $p90, $s[-1], (100.0*$h/$ft.Count)
    if ($cnt -gt 0) {
        $avg = @{}; foreach ($f in $fields) { $avg[$f] = $sum[$f]/$cnt }
        "   STEPS(avg over {0}): body={1:N1} gpu={2:N1} | reqPrep={3:N1} genPrep={4:N1} clipInterest={5:N1} clipPump={6:N1} surfExtract={7:N1} midMeshUpload={8:N1} trim={9:N1}" -f `
            $cnt,$avg.body,$avg.gpuMs,$avg.reqPrep,$avg.genPrep,$avg.clipInterest,$avg.clipPump,$avg.surfExtract,$avg.midMeshUpload,$avg.trim
    }
    ""
}

"###### VENPOD perf map (PerfMode 60fps, steady window frame>=$SteadyStart) ######"
""
foreach ($sc in $Scenarios) {
    switch ($sc) {
        "idle"     { Run-Scenario "idle"     0  0  }
        "look"     { Run-Scenario "look"     0  70 }
        "move"     { Run-Scenario "move"     30 0  }
        "movelook" { Run-Scenario "movelook" 30 70 }
    }
}
