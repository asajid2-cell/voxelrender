# buildbench.ps1 — ship-config CPU mid-mesh build A/B: drive mtns_edit.rec with NO GPU production
# (the editing dip in the shipped config IS the CPU BuildMidHeightSurfaceSnapshot pre-pass), apply
# arbitrary VENPOD_MIDMESH_* build-config flags, and report MIDMESH_BUILDSPLIT
# (lodScan/preExtract/extract/assembly) + body + visibleMissing at the edit-spike frames.
param(
    [string]$Label = "baseline",
    [hashtable]$Flags = @{},          # extra VENPOD_* env, e.g. @{ VENPOD_MIDMESH_WORKSTEAL = "1" }
    [int]$ExitAfterFrames = 760,
    [int[]]$Spikes = @(400,622,642,666,686),
    [int]$CaptureStart = 0,
    [int]$CaptureCount = 0,
    [string]$Replay = "mtns_edit.rec",
    [string]$PerfMode = "none"            # "quality" applies the rebrun.ps1 quality-mode env (the ship config)
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build/bin/VENPOD.exe"
$rec = Join-Path $root ("build/bin/" + $Replay)
$outDir = Join-Path $root ("build/bin/buildbench/" + $Label)
Get-Process VENPOD -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
Start-Sleep -Milliseconds 300
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$log = Join-Path $outDir "run.log"

# SHIP config: sparse sandbox, NO VENPOD_MIDMESH_GPU_DRAW / _EXTRACT_PRODUCTION (CPU build is primary).
$env:VENPOD_LOG_FILE = $log
$env:VENPOD_EXIT_AFTER_FRAMES = "$ExitAfterFrames"
$env:VENPOD_MODE = "sandbox"; $env:VENPOD_DISABLE_PHYSICS = "1"; $env:VENPOD_VSYNC = "0"
$env:VENPOD_ENABLE_EXPERIMENTAL_SPARSE = "1"; $env:VENPOD_RENDER_BACKEND = "sparse"
$env:VENPOD_SPARSE_RAYMARCH = "1"; $env:VENPOD_SPARSE_ONLY = "1"
$env:VENPOD_SPARSE_SURFACE_AUTHORITATIVE = "1"; $env:VENPOD_SPARSE_MISS_FEEDBACK = "1"
$env:VENPOD_SPARSE_REQUIRE_STARTUP_READY = "1"; $env:VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_BLOCKS = "1"
$env:VENPOD_SPARSE_STARTUP_MIN_FRAMES = "90"; $env:VENPOD_SPARSE_SURFACE_PROMOTION_POLICY = "strict"
$env:VENPOD_SPARSE_MAX_PAGES = "65536"; $env:VENPOD_SPARSE_PAGE_TABLE = "131072"
$env:VENPOD_SPARSE_STATS_SINGLE_FLUSH = "1"; $env:VENPOD_PERF_SUMMARY_LOG_INTERVAL = "20"
$env:VENPOD_PERF_FRAME_END_LOG_INTERVAL = "1"; $env:VENPOD_REPLAY = $rec
# explicitly clear GPU production so the CPU build is the primary path (ship config)
Remove-Item Env:VENPOD_MIDMESH_GPU_DRAW -EA SilentlyContinue
Remove-Item Env:VENPOD_MIDMESH_GPU_EXTRACT_PRODUCTION -EA SilentlyContinue
Remove-Item Env:VENPOD_MIDMESH_GPU_PROD_PHASE_TIMERS -EA SilentlyContinue
# clear any build-config flags from a prior variant, then apply this variant's
foreach ($k in @("VENPOD_MIDMESH_WORKSTEAL","VENPOD_MIDMESH_DIRTY_REGION_EXTRACT","VENPOD_MIDMESH_DIRTY_REGION_VALIDATE","VENPOD_MIDMESH_ASYNC_REMESH","VENPOD_MIDMESH_EXTRACT_SCRATCH","VENPOD_MIDMESH_LOD_CACHE","VENPOD_MIDMESH_EXTRACT_MAX_WORKERS")) {
    Remove-Item "Env:$k" -EA SilentlyContinue
}
# rebrun.ps1 quality-mode env (mirrors rebrun.ps1:654-806 with $quality=true) -- the ship config.
if ($PerfMode -eq "quality") {
    $env:VENPOD_STREAMING_V2 = "1"
    $env:VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH = "0"
    $env:VENPOD_SPARSE_MID_CLIPMAP_PUMP_HARD_BUDGET_MS = "24"
    $env:VENPOD_SPARSE_SURFACE_STRICT_TIME_BUDGET = "1"
    $env:VENPOD_SPARSE_SURFACE_EXTRACTION_MAX_MS = "24"
    $env:VENPOD_SPARSE_PRE_PUBLISH_SURFACE_EXTRACTION_MAX_MS = "24"
    $env:VENPOD_SPARSE_POST_OPEN_PRE_PUBLISH_SURFACE_EXTRACTION_MAX_MS = "24"
    $env:VENPOD_SPARSE_SURFACE_ASYNC_EXTRACTION = "1"
    $env:VENPOD_SPARSE_SURFACE_ASYNC_MAX_WORKERS = "8"
    $env:VENPOD_SPARSE_SURFACE_EXTRACTION_BUDGET = "128"
    $env:VENPOD_SPARSE_SURFACE_ASYNC_MAX_APPLY_PER_FRAME = "256"
    $env:VENPOD_SPARSE_SURFACE_UPLOAD_MIN_INTERVAL_FRAMES = "1"
    $env:VENPOD_SPARSE_EXACT_ASYNC_GENERATION = "1"
    $env:VENPOD_SPARSE_EXACT_ASYNC_VISIBLE = "1"
    $env:VENPOD_SPARSE_EXACT_ASYNC_PREFETCH_LANE = "1"
    $env:VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_GEN = "1"
    $env:VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_GEN = "1"
    $env:VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_MAX_ENQUEUE = "256"
    $env:VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_MAX_APPLY = "256"
    $env:VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_VISIBLE_PUMP = "1"
    $env:VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PERSISTENT_WORKERS = "1"
    $env:VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PUMP_MAX_WORKERS = "10"
    $env:VENPOD_SPARSE_MID_VOXEL_COVERAGE_CATCHUP_BUDGET = "192"
    $env:VENPOD_SPARSE_MID_CLIPMAP_GPU_GENERATION = "1"
    $env:VENPOD_SPARSE_MID_INTEREST_INTERVAL = "1"
    $env:VENPOD_RAYMARCH_RENDER_SCALE = "1.0"
    # quality: NO background pass (full-res far), NO stats-single-flush, NO interest-signature reuse
    Remove-Item Env:VENPOD_SPARSE_STATS_SINGLE_FLUSH -EA SilentlyContinue
}
foreach ($k in $Flags.Keys) { Set-Item "Env:$k" $Flags[$k] }
# optional frame capture (PNG) for the visual quality A/B
foreach ($k in @("VENPOD_CAPTURE_DIR","VENPOD_CAPTURE_START_FRAME","VENPOD_CAPTURE_INTERVAL_FRAMES","VENPOD_CAPTURE_COUNT")) { Remove-Item "Env:$k" -EA SilentlyContinue }
if ($CaptureCount -gt 0) {
    $capDir = Join-Path $outDir "cap"
    New-Item -ItemType Directory -Path $capDir -Force | Out-Null
    $env:VENPOD_CAPTURE_DIR = $capDir
    $env:VENPOD_CAPTURE_START_FRAME = "$CaptureStart"
    $env:VENPOD_CAPTURE_INTERVAL_FRAMES = "1"   # consecutive frames
    $env:VENPOD_CAPTURE_COUNT = "$CaptureCount"
    Write-Host "[$Label] capturing frames $CaptureStart..$($CaptureStart+$CaptureCount-1) -> $capDir"
}

Write-Host "[$Label] flags: $(($Flags.GetEnumerator() | ForEach-Object { $_.Key + '=' + $_.Value }) -join ' ')"
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& $exe 2>(Join-Path $outDir "stderr.txt") | Out-Null
$sw.Stop()
Write-Host "[$Label] exit=$LASTEXITCODE in $([Math]::Round($sw.Elapsed.TotalSeconds,1))s"

$lines = Get-Content $log
# visibleMissing must be all-zero (no hole)
$missNonZero = ($lines | Select-String "visibleMissing=([0-9]+)" -AllMatches | ForEach-Object { $_.Matches } | Where-Object { [int]$_.Groups[1].Value -ne 0 }).Count
Write-Host "[$Label] visibleMissing nonzero-samples = $missNonZero"

Write-Host "[$Label] spike frames (surfacePrefetch is the real dip term; buildMs is the mid-mesh build):"
$totPf = 0.0; $maxBody = 0.0; $maxReq = 0.0
foreach ($f in $Spikes) {
    $sp = $lines | Where-Object { $_ -match "PERF_SPIKE_SPARSE_REQUEST" -and $_ -match "frame=$f " } | Select-Object -First 1
    $req  = if ($sp -match "reqMs=([0-9.]+)") { [double]$Matches[1] } else { 0.0 }
    $pref = if ($sp -match "surfacePrefetch=([0-9.]+)") { [double]$Matches[1] } else { 0.0 }
    $hier = if ($sp -match "hierarchy=([0-9.]+)") { [double]$Matches[1] } else { 0.0 }
    $st = $lines | Where-Object { $_ -match "MIDMESH_SELFTIME" -and $_ -match "frame=$f " } | Select-Object -First 1
    $bm = if ($st -match "buildMs=([0-9.]+)") { [double]$Matches[1] } else { 0.0 }
    $fe = $lines | Where-Object { $_ -match "PERF_FRAME_END" -and $_ -match "frame=$f " } | Select-Object -First 1
    $body = if ($fe -match "\bbody=([0-9.]+)") { [double]$Matches[1] } else { 0.0 }
    if ($body -gt $maxBody) { $maxBody = $body }
    if ($req -gt $maxReq) { $maxReq = $req }
    $totPf += $pref
    Write-Host ("  frame=$f  surfacePrefetch=$pref  hierarchy=$hier  reqMs=$req  buildMs=$bm  body=$body")
}
Write-Host ("[$Label] SUM surfacePrefetch(spikes)=$([Math]::Round($totPf,2))  maxReqMs=$([Math]::Round($maxReq,2))  maxBody=$([Math]::Round($maxBody,2))  missNonZero=$missNonZero")
