param(
    [int]$TileSize = 8,
    [int]$CoverBudget = 645,
    [string]$Feature = "varianceTimesEdge",
    [double]$LowresBaseMs = 15.95,
    [double]$PerTileMs = 0.0057,
    [double]$TargetMs = 19.63,
    [double]$ScoreThreshold = 0.00225,
    [bool]$RequireScoreThreshold = $true,
    [int]$Y0 = 320,
    [int]$Y1 = 480
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tool = Join-Path $root "tools/horizon_delta_predictor_sweep.js"
$ref = Join-Path $root "build/bin/captures/vis_full_aggr015/engine_frame_0220.bmp"
$cases = @(
    @{ Label = "bg025"; Scale = "0.25"; Candidate = "build/bin/captures/vis_bg025_aggr015/engine_frame_0220.bmp" },
    @{ Label = "bg050"; Scale = "0.50"; Candidate = "build/bin/captures/vis_bg050_aggr015/engine_frame_0220.bmp" },
    @{ Label = "bg075"; Scale = "0.75"; Candidate = "build/bin/captures/vis_bg075_aggr015/engine_frame_0220.bmp" }
)

if (!(Test-Path $tool)) { throw "missing tool: $tool" }
if (!(Test-Path $ref)) { throw "missing reference capture: $ref" }

foreach ($case in $cases) {
    $candidate = Join-Path $root $case.Candidate
    if (!(Test-Path $candidate)) {
        throw "missing candidate capture for $($case.Label): $candidate"
    }
    Write-Host "[$($case.Label)] scale=$($case.Scale) tile=$TileSize coverBudget=$CoverBudget feature=$Feature targetMs=$TargetMs scoreThreshold=$ScoreThreshold requireScoreThreshold=$RequireScoreThreshold"
    $nodeArgs = @(
        $tool,
        "--reference", $ref,
        "--candidate", $candidate,
        "--tile-size", "$TileSize",
        "--bg-scale", $case.Scale,
        "--y0", "$Y0",
        "--y1", "$Y1",
        "--budgets", "320,640,$CoverBudget,960,1280",
        "--require-feature", $Feature,
        "--lowres-base-ms", "$LowresBaseMs",
        "--per-tile-ms", "$PerTileMs",
        "--target-ms", "$TargetMs",
        "--require-cover-budget", "$CoverBudget"
    )
    if ($ScoreThreshold -ge 0.0) {
        $nodeArgs += @("--score-threshold", "$ScoreThreshold")
    }
    if ($RequireScoreThreshold) {
        $nodeArgs += "--require-score-threshold"
    }
    node @nodeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "horizon delta predictor failed for $($case.Label) with feature=$Feature budget=$CoverBudget"
    }
}
