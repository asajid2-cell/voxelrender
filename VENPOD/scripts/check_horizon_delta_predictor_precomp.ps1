param(
    [string]$Reference = "build/bin/captures/vis_full_aggr015_current/engine_frame_0220.bmp",
    [string]$Candidate = "build/bin/captures/precomp_bg025_aggr015/engine_frame_0220.bmp",
    [string]$LowresSource = "build/bin/captures/precomp_bg025_aggr015/background_pass_frame_0220.bmp",
    [int]$TileSize = 8,
    [int]$CoverBudget = 645,
    [string]$Feature = "varianceTimesEdge",
    [double]$LowresBaseMs = 15.95,
    [double]$PerTileMs = 0.0057,
    [double]$TargetMs = 19.63,
    [double]$ScoreThreshold = 0.00388,
    [bool]$RequireScoreThreshold = $true,
    [int]$Y0 = 320,
    [int]$Y1 = 480,
    [int]$OracleY0 = 320,
    [int]$OracleY1 = 480,
    [int]$VisualThreshold = 96
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tool = Join-Path $root "tools/horizon_delta_predictor_sweep.js"
$ref = Join-Path $root $Reference
$candidatePath = Join-Path $root $Candidate
$lowresPath = Join-Path $root $LowresSource

foreach ($path in @($tool, $ref, $candidatePath, $lowresPath)) {
    if (!(Test-Path $path)) {
        throw "missing required file: $path"
    }
}

Write-Host "precomp tile=$TileSize coverBudget=$CoverBudget feature=$Feature targetMs=$TargetMs scoreThreshold=$ScoreThreshold requireScoreThreshold=$RequireScoreThreshold"
Write-Host "reference=$Reference"
Write-Host "candidate=$Candidate"
Write-Host "lowresSource=$LowresSource"

$nodeArgs = @(
    $tool,
    "--reference", $ref,
    "--candidate", $candidatePath,
    "--lowres-source", $lowresPath,
    "--tile-size", "$TileSize",
    "--bg-width", "480",
    "--bg-height", "270",
    "--lowres", "center",
    "--visual-threshold", "$VisualThreshold",
    "--y0", "$Y0",
    "--y1", "$Y1",
    "--oracle-y0", "$OracleY0",
    "--oracle-y1", "$OracleY1",
    "--budgets", "320,613,640,$CoverBudget,960,1280,1920,3200,4800",
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
    throw "pre-composite horizon delta predictor failed with feature=$Feature budget=$CoverBudget"
}
