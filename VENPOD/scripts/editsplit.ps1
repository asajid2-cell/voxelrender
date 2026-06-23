# editsplit.ps1 - drive the REAL edit replay (mtns_edit.rec) with the GPU production path on
# and the producer sub-phase timers on, then dump the midUpload split at the edit-burst frames.
# This is the harness the perf_noncapture_smoke.ps1 walk/highalt scenarios never were: it replays
# the actual recorded edits, so the 459ms producer stall reproduces and gets decomposed.
param(
    [int]$ExitAfterFrames = 760,
    [string]$OutDir = "build/bin/editsplit",
    [switch]$TimersOff,                # baseline (no timer flag) for the bit-equal/no-overhead A/B
    [int[]]$BurstFrames = @(248,249,250,251,252,253,254,255,298,299,300,301,302,303,304,670)
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$exe = Join-Path $root "build/bin/VENPOD.exe"
$rec = Join-Path $root "build/bin/mtns_edit.rec"
if (-not (Test-Path $exe)) { throw "no VENPOD.exe - build first" }
if (-not (Test-Path $rec)) { throw "no mtns_edit.rec" }

# kill stale
Get-Process VENPOD -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
Start-Sleep -Milliseconds 300

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$log = Join-Path $OutDir ("run_" + ($(if($TimersOff){"timersOFF"}else{"timersON"})) + ".log")

# Common candidate env (mirrors perf_noncapture_smoke.ps1 Set-CommonCandidateEnv), sparse + sandbox.
$env:VENPOD_LOG_FILE = $log
$env:VENPOD_EXIT_AFTER_FRAMES = "$ExitAfterFrames"
$env:VENPOD_MODE = "sandbox"
$env:VENPOD_DISABLE_PHYSICS = "1"
$env:VENPOD_VSYNC = "0"
$env:VENPOD_ENABLE_EXPERIMENTAL_SPARSE = "1"
$env:VENPOD_RENDER_BACKEND = "sparse"
$env:VENPOD_SPARSE_RAYMARCH = "1"
$env:VENPOD_SPARSE_ONLY = "1"
$env:VENPOD_SPARSE_SURFACE_AUTHORITATIVE = "1"
$env:VENPOD_SPARSE_MISS_FEEDBACK = "1"
$env:VENPOD_SPARSE_REQUIRE_STARTUP_READY = "1"
$env:VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_BLOCKS = "1"
$env:VENPOD_SPARSE_STARTUP_MIN_FRAMES = "90"
$env:VENPOD_SPARSE_SURFACE_PROMOTION_POLICY = "strict"
$env:VENPOD_SPARSE_MAX_PAGES = "65536"
$env:VENPOD_SPARSE_PAGE_TABLE = "131072"
$env:VENPOD_SPARSE_STATS_SINGLE_FLUSH = "1"
$env:VENPOD_PERF_SUMMARY_LOG_INTERVAL = "20"
$env:VENPOD_PERF_FRAME_END_LOG_INTERVAL = "1"
# the actual edit replay (re-applies recorded brush strokes => producer path active on edit frames)
$env:VENPOD_REPLAY = $rec
# GPU mid-mesh production path ON (the path that owns the 459ms producer prep + the line-2117 wait)
$env:VENPOD_MIDMESH_GPU_DRAW = "1"
# producer sub-phase timers (the loop's lever) - default OFF flag, on unless -TimersOff
if ($TimersOff) { $env:VENPOD_MIDMESH_GPU_PROD_PHASE_TIMERS = "0" }
else            { $env:VENPOD_MIDMESH_GPU_PROD_PHASE_TIMERS = "1" }

Write-Host "Running edit replay (timers $(if($TimersOff){'OFF'}else{'ON'})) -> $log"
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& $exe 2>&1 | Out-Null
$sw.Stop()
Write-Host "exit=$LASTEXITCODE in $([Math]::Round($sw.Elapsed.TotalSeconds,1))s"

if (-not (Test-Path $log)) { throw "no log produced at $log" }

# Pull the midUploadSplit + the coarse midUpload at the burst frames.
Write-Host "`n=== PERF_FRAME_END split at edit-burst frames (hostPrep/recordSubmit/producerWait) ==="
$lines = Get-Content $log
foreach ($f in $BurstFrames) {
    $m = $lines | Where-Object { $_ -match "PERF_FRAME_END" -and $_ -match "(^|[^0-9])frame=$f([^0-9]|$)" } | Select-Object -First 1
    if ($m) {
        $split = if ($m -match "midUploadSplit=[^:]*:([0-9./]+)") { $Matches[1] } else { "?" }
        # coarse midUpload is the 8th value in sparsePost=feedback/cmd/begin/midSnap/plan/upload/publish/midUpload/stats/...:v1/.../v8/...
        $coarse = "?"
        if ($m -match "sparsePost=[^:]*:([0-9./]+)") { $sp=$Matches[1].Split('/'); if ($sp.Count -ge 8) { $coarse=$sp[7] } }
        $body = if ($m -match "\bbody=([0-9.]+)") { $Matches[1] } else { "?" }
        Write-Host ("frame=$f  midUpload=$coarse  split=$split  body=$body")
    }
}
# Also surface the single worst midUpload frame in the run (the real spike), with its split.
Write-Host "`n=== worst midUpload frame in run ==="
$worst = $lines | Where-Object { $_ -match "PERF_FRAME_END" -and $_ -match "midUploadSplit" } |
    ForEach-Object {
        $fr = if ($_ -match "frame=([0-9]+)") { [int]$Matches[1] } else { -1 }
        $mu = 0.0
        if ($_ -match "sparsePost=[^:]*:([0-9./]+)") { $v=$Matches[1].Split('/'); if ($v.Count -ge 8) { $mu=[double]$v[7] } }
        $sp = if ($_ -match "midUploadSplit=[^:]*:([0-9./]+)") { $Matches[1] } else { "?" }
        [pscustomobject]@{ frame=$fr; midUpload=$mu; split=$sp }
    } | Sort-Object midUpload -Descending | Select-Object -First 5
$worst | ForEach-Object { Write-Host ("frame=$($_.frame)  midUpload=$($_.midUpload)  split=$($_.split)") }
Write-Host "`nlog: $log"
