<#
.SYNOPSIS
    Deterministic walking benchmark for VENPOD (Phase 0 measurement foundation).

.DESCRIPTION
    The existing perf_noncapture_smoke.ps1 walk scenario runs with VARIABLE dt by
    default, so the camera path depends on frame timing -> non-reproducible runs and
    a perf<->input feedback loop. Worse, the harness's -WalkFixedDtMs sets
    VENPOD_SPARSE_WALK_TEST_FIXED_DT, but the launcher reads
    VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS, so the knob was a no-op.

    This wrapper:
      1. Forces a fixed simulation dt by setting the CORRECT env var directly, so the
         walk camera path is byte-identical every run.
      2. Disables vsync so we measure true frame cost, not refresh quantization.
      3. Runs the walk scenario N times and parses every PERF_FRAME_END line.
      4. Reports median / mean / p99 / max for rawMs and body, plus a per-stage
         bottleneck table, and the NOISE BAND (spread of per-run medians) so future
         changes can be judged "real" only if they exceed it.
      5. Lists the worst hitch frames with the dominant stage, so episodic hitches
         are attributed instead of averaged away.

    Accept/reject rule of thumb: a change is real only if it moves the median AND p99
    beyond the noise band reported here.

.EXAMPLE
    .\walk_bench.ps1 -Runs 5
    .\walk_bench.ps1 -Runs 5 -SmokeArgs @('-StackPreset','none')
    .\walk_bench.ps1 -ParseOnly -OutRoot build\captures\walk_bench_xxxx
#>
param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo")]
    [string]$Config = "Release",
    [int]$Runs = 5,
    [int]$WalkFrame = 600,
    [int]$WalkFixedDtMs = 16,
    [int]$AnalyzeStartFrame = 300,
    [int]$TopHitches = 8,
    [string]$OutRoot = "",
    [switch]$NoBuild,
    [switch]$ParseOnly,
    # Extra args passed straight through to perf_noncapture_smoke.ps1 (e.g. stack flags).
    [string[]]$SmokeArgs = @()
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

if ([string]::IsNullOrWhiteSpace($OutRoot)) {
    # No Date.now in this env's other tooling, but PowerShell can stamp freely.
    $stamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
    $OutRoot = "build\captures\walk_bench_$stamp"
}
$OutRootFull = Join-Path $root $OutRoot

# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------
$frameEndRegex = [regex](
    'PERF_FRAME_END frame=(?<frame>\d+).*?' +
    'gaps=postWait/prePhys/preRender/postRender:(?<postWait>[-+0-9.]+)/(?<prePhys>[-+0-9.]+)/(?<preRender>[-+0-9.]+)/(?<postRender>[-+0-9.]+).*?' +
    'sparsePost=feedback/cmd/begin/midSnap/plan/upload/publish/midUpload/stats/surfExtract/surfPlan/surfSnap/surfStage/surfEmit:' +
    '(?<feedback>[-+0-9.]+)/(?<cmd>[-+0-9.]+)/(?<begin>[-+0-9.]+)/(?<midSnap>[-+0-9.]+)/(?<plan>[-+0-9.]+)/(?<spUpload>[-+0-9.]+)/(?<publish>[-+0-9.]+)/(?<midUpload>[-+0-9.]+)/(?<stats>[-+0-9.]+)/(?<surfExtract>[-+0-9.]+)/(?<surfPlan>[-+0-9.]+)/(?<surfSnap>[-+0-9.]+)/(?<surfStage>[-+0-9.]+)/(?<surfEmit>[-+0-9.]+).*?' +
    'body=(?<body>[-+0-9.]+).*?gapPrev=(?<gapPrev>[-+0-9.]+).*?rawMs=(?<raw>[-+0-9.]+)')

$cpuDetailRegex = [regex](
    'PERF_SPARSE_CPU_DETAIL frame=(?<frame>\d+) reqMs=(?<req>[-+0-9.]+) genMs=(?<gen>[-+0-9.]+) clipMs=(?<clip>[-+0-9.]+)')

function Get-Percentile {
    param([double[]]$Values, [double]$P)
    if ($Values.Count -eq 0) { return 0.0 }
    $sorted = $Values | Sort-Object
    $idx = [int][Math]::Ceiling(($P / 100.0) * $sorted.Count) - 1
    if ($idx -lt 0) { $idx = 0 }
    if ($idx -ge $sorted.Count) { $idx = $sorted.Count - 1 }
    return [double]$sorted[$idx]
}

function Get-Median { param([double[]]$Values) return (Get-Percentile -Values $Values -P 50) }

function Get-StdDev {
    param([double[]]$Values)
    if ($Values.Count -lt 2) { return 0.0 }
    $mean = ($Values | Measure-Object -Average).Average
    $sum = 0.0
    foreach ($v in $Values) { $sum += [Math]::Pow($v - $mean, 2) }
    return [Math]::Sqrt($sum / ($Values.Count - 1))
}

function Parse-WalkLog {
    param([string]$LogPath, [int]$StartFrame)

    $text = Get-Content -LiteralPath $LogPath -Raw
    $frames = @{}

    foreach ($m in $frameEndRegex.Matches($text)) {
        $f = [int]$m.Groups["frame"].Value
        if ($f -lt $StartFrame) { continue }
        $frames[$f] = [pscustomobject]@{
            frame      = $f
            raw        = [double]$m.Groups["raw"].Value
            body       = [double]$m.Groups["body"].Value
            postWait   = [double]$m.Groups["postWait"].Value
            postRender = [double]$m.Groups["postRender"].Value
            gapPrev    = [double]$m.Groups["gapPrev"].Value
            surfExtract= [double]$m.Groups["surfExtract"].Value
            upload     = [double]$m.Groups["spUpload"].Value
            publish    = [double]$m.Groups["publish"].Value
            stats      = [double]$m.Groups["stats"].Value
            plan       = [double]$m.Groups["plan"].Value
            midUpload  = [double]$m.Groups["midUpload"].Value
            clip       = $null
            req        = $null
            gen        = $null
        }
    }

    foreach ($m in $cpuDetailRegex.Matches($text)) {
        $f = [int]$m.Groups["frame"].Value
        if ($frames.ContainsKey($f)) {
            $frames[$f].clip = [double]$m.Groups["clip"].Value
            $frames[$f].req  = [double]$m.Groups["req"].Value
            $frames[$f].gen  = [double]$m.Groups["gen"].Value
        }
    }

    return ($frames.Values | Sort-Object frame)
}

function Mean-Of {
    param([object[]]$Rows, [string]$Field)
    $vals = @($Rows | Where-Object { $null -ne $_.$Field } | ForEach-Object { [double]$_.$Field })
    if ($vals.Count -eq 0) { return $null }
    return ($vals | Measure-Object -Average).Average
}

function Summarize-Run {
    param([object[]]$Rows)
    $raw  = @($Rows | ForEach-Object { [double]$_.raw })
    $body = @($Rows | ForEach-Object { [double]$_.body })
    [pscustomobject]@{
        frames        = $Rows.Count
        rawMedian     = [Math]::Round((Get-Median  $raw), 2)
        rawMean       = [Math]::Round((($raw | Measure-Object -Average).Average), 2)
        rawP99        = [Math]::Round((Get-Percentile -Values $raw -P 99), 2)
        rawMax        = [Math]::Round((($raw | Measure-Object -Maximum).Maximum), 2)
        bodyMedian    = [Math]::Round((Get-Median $body), 2)
        postWaitMean  = [Math]::Round((Mean-Of $Rows 'postWait'), 2)
        surfExtMean   = [Math]::Round((Mean-Of $Rows 'surfExtract'), 2)
        uploadMean    = [Math]::Round((Mean-Of $Rows 'upload'), 2)
        publishMean   = [Math]::Round((Mean-Of $Rows 'publish'), 2)
        postRenderMean= [Math]::Round((Mean-Of $Rows 'postRender'), 2)
        clipMean      = $(if ((Mean-Of $Rows 'clip') -ne $null) { [Math]::Round((Mean-Of $Rows 'clip'), 2) } else { 'n/a' })
        reqMean       = $(if ((Mean-Of $Rows 'req')  -ne $null) { [Math]::Round((Mean-Of $Rows 'req'),  2) } else { 'n/a' })
        genMean       = $(if ((Mean-Of $Rows 'gen')  -ne $null) { [Math]::Round((Mean-Of $Rows 'gen'),  2) } else { 'n/a' })
    }
}

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $OutRootFull | Out-Null

if (-not $ParseOnly) {
    if (-not $NoBuild) {
        Write-Host "[walk_bench] Building $Config once..." -ForegroundColor Cyan
        & (Join-Path $root "build.ps1") -Config $Config
        if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed with exit code $LASTEXITCODE" }
    }

    # The decisive determinism fix: set the env var the LAUNCHER actually reads.
    # (The harness's -WalkFixedDtMs sets the wrong name; this _MS var is NOT in the
    # harness's managed-clear list, so it survives.)
    $env:VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS = [string]$WalkFixedDtMs
    # Per-frame PERF_FRAME_END is requested via the harness's own -FrameEndLogInterval
    # (below); setting the env var directly does NOT work because the harness clears it.
    Write-Host "[walk_bench] Forcing VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS=$WalkFixedDtMs + per-frame PERF_FRAME_END (deterministic, dense)" -ForegroundColor Cyan

    for ($i = 1; $i -le $Runs; $i++) {
        $runDir = Join-Path $OutRoot ("run_{0}" -f $i)
        Write-Host "[walk_bench] Run $i/$Runs -> $runDir" -ForegroundColor Yellow
        $smoke = @{
            Config             = $Config
            Scenario           = "walk"
            WalkFrame          = $WalkFrame
            DisableVSync       = $true
            KillExisting       = $true
            NoBuild            = $true
            FrameEndLogInterval = 1
            OutputDir          = $runDir
        }
        # Hashtable splat binds named params; $SmokeArgs array splat appends extras.
        & (Join-Path $root "perf_noncapture_smoke.ps1") @smoke @SmokeArgs
    }
}

# ---------------------------------------------------------------------------
# Aggregate
# ---------------------------------------------------------------------------
$runSummaries = New-Object System.Collections.Generic.List[object]
$allHitchRows = New-Object System.Collections.Generic.List[object]

for ($i = 1; $i -le $Runs; $i++) {
    $log = Join-Path $OutRootFull ("run_{0}\walk_realtime\venpod_runtime.log" -f $i)
    if (-not (Test-Path $log)) {
        Write-Host "[walk_bench] WARNING: missing log $log" -ForegroundColor Red
        continue
    }
    $rows = Parse-WalkLog -LogPath $log -StartFrame $AnalyzeStartFrame
    if ($rows.Count -eq 0) {
        Write-Host "[walk_bench] WARNING: no frames >= $AnalyzeStartFrame in run $i" -ForegroundColor Red
        continue
    }
    $s = Summarize-Run -Rows $rows
    $s | Add-Member -NotePropertyName run -NotePropertyValue $i -PassThru | Out-Null
    $runSummaries.Add($s)
    foreach ($r in $rows) { $allHitchRows.Add(($r | Add-Member -NotePropertyName runIdx -NotePropertyValue $i -PassThru)) }
}

if ($runSummaries.Count -eq 0) { throw "No runs parsed. Nothing to report." }

Write-Host ""
Write-Host "================ PER-RUN SUMMARY (frames >= $AnalyzeStartFrame, fixed dt=$WalkFixedDtMs ms, vsync off) ================" -ForegroundColor Green
$runSummaries | Select-Object run, frames, rawMedian, rawMean, rawP99, rawMax, bodyMedian, clipMean, postWaitMean, surfExtMean, uploadMean, publishMean, postRenderMean, reqMean, genMean | Format-Table -AutoSize

# Noise band across per-run medians + p99
$medians = @($runSummaries | ForEach-Object { [double]$_.rawMedian })
$p99s    = @($runSummaries | ForEach-Object { [double]$_.rawP99 })
$medMin = ($medians | Measure-Object -Minimum).Minimum
$medMax = ($medians | Measure-Object -Maximum).Maximum
$medStd = Get-StdDev $medians
$medAvg = ($medians | Measure-Object -Average).Average
$bandPct = if ($medAvg -gt 0) { [Math]::Round((($medMax - $medMin) / $medAvg) * 100.0, 1) } else { 0 }

Write-Host "================ NOISE BAND ================" -ForegroundColor Green
Write-Host ("  raw median across runs : min {0:N2}  max {1:N2}  spread {2:N2} ms  (stddev {3:N2})" -f $medMin, $medMax, ($medMax - $medMin), $medStd)
Write-Host ("  raw p99    across runs : min {0:N2}  max {1:N2}" -f (($p99s | Measure-Object -Minimum).Minimum), (($p99s | Measure-Object -Maximum).Maximum))
Write-Host ("  => A change is REAL only if it moves median AND p99 by more than ~{0:N2} ms ({1}%)." -f ($medMax - $medMin), $bandPct) -ForegroundColor Cyan

# Bottleneck table: average of per-run means, sorted desc
Write-Host ""
Write-Host "================ BOTTLENECK TABLE (avg of per-run means) ================" -ForegroundColor Green
$buckets = @(
    @{ name = "clip (mid interest)"; field = "clipMean" },
    @{ name = "postWait gap (total)"; field = "postWaitMean" },
    @{ name = "  - surfExtract";      field = "surfExtMean" },
    @{ name = "request";              field = "reqMean" },
    @{ name = "generation";           field = "genMean" },
    @{ name = "upload";               field = "uploadMean" },
    @{ name = "publish";              field = "publishMean" },
    @{ name = "postRender gap";       field = "postRenderMean" }
)
$btab = foreach ($b in $buckets) {
    $vals = @($runSummaries | Where-Object { $_.($b.field) -ne 'n/a' -and $null -ne $_.($b.field) } | ForEach-Object { [double]$_.($b.field) })
    $avg = if ($vals.Count -gt 0) { [Math]::Round((($vals | Measure-Object -Average).Average), 2) } else { 'n/a' }
    [pscustomobject]@{ stage = $b.name; meanMs = $avg }
}
$btab | Format-Table -AutoSize

# Worst hitches across all runs
Write-Host "================ TOP $TopHitches HITCH FRAMES (all runs, dominant stage) ================" -ForegroundColor Green
$hitches = $allHitchRows | Sort-Object { [double]$_.raw } -Descending | Select-Object -First $TopHitches
$hitchTab = foreach ($h in $hitches) {
    # pick dominant explicit bucket
    $cand = @(
        @{ n = "postWait"; v = [double]$h.postWait },
        @{ n = "surfExtract"; v = [double]$h.surfExtract },
        @{ n = "gapPrev"; v = [double]$h.gapPrev },
        @{ n = "postRender"; v = [double]$h.postRender },
        @{ n = "upload"; v = [double]$h.upload }
    )
    if ($null -ne $h.clip) { $cand += @{ n = "clip"; v = [double]$h.clip } }
    $dom = $cand | Sort-Object { $_.v } -Descending | Select-Object -First 1
    [pscustomobject]@{
        run = $h.runIdx; frame = $h.frame
        rawMs = [Math]::Round([double]$h.raw, 2)
        dominantStage = $dom.n
        dominantMs = [Math]::Round([double]$dom.v, 2)
    }
}
$hitchTab | Format-Table -AutoSize

# Persist
$csvPath = Join-Path $OutRootFull "walk_bench_summary.csv"
$runSummaries | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding UTF8
Write-Host "[walk_bench] Per-run summary written to $csvPath" -ForegroundColor Cyan
Write-Host "[walk_bench] Done." -ForegroundColor Green
