param(
    [Parameter(Mandatory = $true)]
    [string]$Reference,
    [Parameter(Mandatory = $true)]
    [string]$Candidate,
    [string]$Root = "Z:/328/CMPUT328-A2/codexworks/301/3d/VENPOD",
    [double]$FullMaeMax = 4.0,
    [double]$BandMaeMax = 8.0,
    [double]$BandChannelMax = 12.0,
    [int]$PixelMax = 96,
    [switch]$Json
)

$ErrorActionPreference = "Stop"

$rootPath = (Resolve-Path -LiteralPath $Root).Path
$scriptPath = Join-Path $rootPath "tools/far_horizon_visual_check.js"
if (!(Test-Path -LiteralPath $scriptPath)) {
    throw "Visual verifier source not found: $scriptPath"
}

$args = @(
    $scriptPath,
    "--reference", (Resolve-Path -LiteralPath $Reference).Path,
    "--candidate", (Resolve-Path -LiteralPath $Candidate).Path,
    "--full-mae-max", $FullMaeMax,
    "--band-mae-max", $BandMaeMax,
    "--band-channel-max", $BandChannelMax,
    "--pixel-max", $PixelMax
)
if ($Json) {
    $args += "--json"
}

& node @args
exit $LASTEXITCODE
