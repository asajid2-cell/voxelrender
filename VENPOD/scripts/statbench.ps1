# statbench.ps1 - stationary-camera A/B for temporal Stage 2a. Runs the engine at spawn (NO replay =>
# camera static) with the quality env, per-frame PERF_GPU, and measures gpuFrameMs once the camera is
# settled. -Temporal 1 enables VENPOD_RAYMARCH_TEMPORAL (the tile-Bayer amortization).
param(
  [int]$Temporal = 0,
  [int]$Frames = 600,
  [string]$Label = "stat",
  [int]$ShaderUnsafeBlocks = 1
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build/bin/VENPOD.exe"
Get-Process VENPOD -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
Start-Sleep -Milliseconds 300
$outDir = Join-Path $root ("build/bin/statbench/" + $Label)
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$log = Join-Path $outDir "run.log"
# sparse sandbox + quality env (mirrors buildbench), NO VENPOD_REPLAY (camera stays at spawn).
$env:VENPOD_LOG_FILE = $log; $env:VENPOD_EXIT_AFTER_FRAMES = "$Frames"
$env:VENPOD_MODE = "sandbox"; $env:VENPOD_DISABLE_PHYSICS = "1"; $env:VENPOD_VSYNC = "0"
$env:VENPOD_ENABLE_EXPERIMENTAL_SPARSE = "1"; $env:VENPOD_RENDER_BACKEND = "sparse"
$env:VENPOD_SPARSE_RAYMARCH = "1"; $env:VENPOD_SPARSE_ONLY = "1"
$env:VENPOD_SPARSE_SURFACE_AUTHORITATIVE = "1"; $env:VENPOD_SPARSE_MISS_FEEDBACK = "1"
$env:VENPOD_SPARSE_REQUIRE_STARTUP_READY = "1"; $env:VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_BLOCKS = "$ShaderUnsafeBlocks"
$env:VENPOD_SPARSE_STARTUP_MIN_FRAMES = "90"; $env:VENPOD_SPARSE_SURFACE_PROMOTION_POLICY = "strict"
$env:VENPOD_SPARSE_MAX_PAGES = "65536"; $env:VENPOD_SPARSE_PAGE_TABLE = "131072"
$env:VENPOD_SPARSE_STATS_SINGLE_FLUSH = "1"; $env:VENPOD_PERF_SUMMARY_LOG_INTERVAL = "20"
$env:VENPOD_PERF_FRAME_END_LOG_INTERVAL = "1"
Remove-Item Env:VENPOD_REPLAY -EA SilentlyContinue
$env:VENPOD_RAYMARCH_RENDER_SCALE = "1.0"
if ($Temporal -ne 0) { $env:VENPOD_RAYMARCH_TEMPORAL = "1" } else { Remove-Item Env:VENPOD_RAYMARCH_TEMPORAL -EA SilentlyContinue }
& $exe 2>(Join-Path $outDir "stderr.txt") | Out-Null
Write-Host "[$Label temporal=$Temporal] exit=$LASTEXITCODE"
$lines = Get-Content $log
$miss = ($lines | Select-String "visibleMissing=([1-9][0-9]*)" | Measure-Object).Count
# gpuFrameMs over the SETTLED second half (camera static + history converged)
$gpu = $lines | Select-String "PERF_GPU.*valid=1" | ForEach-Object { if ($_ -match "frame=([0-9]+).*gpuFrameMs=([0-9.]+)") { [pscustomobject]@{ f=[int]$Matches[1]; ms=[double]$Matches[2] } } }
$settled = $gpu | Where-Object { $_.f -gt ($Frames * 0.5) } | ForEach-Object { $_.ms } | Sort-Object
if ($settled.Count -gt 0) {
  $p50 = $settled[[int]($settled.Count*0.5)]; $p90 = $settled[[int]($settled.Count*0.9)]
  Write-Host ("[$Label temporal=$Temporal] settled gpuFrameMs p50=$p50 p90=$p90 max=$($settled[-1]) n=$($settled.Count)  visibleMissingNonzero=$miss")
} else { Write-Host "[$Label temporal=$Temporal] no settled PERF_GPU samples  visibleMissingNonzero=$miss" }
