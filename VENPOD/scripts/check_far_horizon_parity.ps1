param(
    [string]$Root = "Z:/328/CMPUT328-A2/codexworks/301/3d/VENPOD",
    [uint32]$Seed = 12345,
    [int]$Bins = 4096,
    [int]$SlopeBins = 192,
    [int]$PixelStep = 8,
    [int]$YMin = 190,
    [int]$YMax = 340,
    [double]$CameraX = -197.88,
    [double]$CameraY = 346.0,
    [double]$CameraZ = 187.96,
    [double]$ForwardX = -0.763,
    [double]$ForwardY = -0.276,
    [double]$ForwardZ = 0.584,
    [double]$StartDistance = 256.0,
    [double]$HitTRelTolerance = 0.35,
    [ValidateSet("slope", "grid", "mipdda", "screenmask")]
    [string]$Mode = "slope",
    [switch]$ConservativeOnly,
    [double]$GridCellSize = 32.0,
    [double]$GridExtent = 10912.0,
    [double]$GridQueryStep = 12.0,
    [double]$GridHeightPad = 12.0,
    [int]$GridSubsamples = 3,
    [switch]$GridExactRefine,
    [switch]$GridRefineReject,
    [double]$GridRefineWindow = 144.0,
    [double]$GridRefineStep = 12.0,
    [switch]$StructuralCheck,
    [int]$StructuralSamples = 3,
    [switch]$FallbackDiagnostic,
    [switch]$HierarchicalDda,
    [int]$HierarchicalDdaMaxLevel = -1,
    [double]$HierarchicalDdaMinRayY = -2.0,
    [switch]$RequireNetWorkGain,
    [double]$DiagnosticHeightEvalWork = 1.0,
    [double]$CacheCellWork = 0.25,
    [double]$RefineHeightEvalWork = 1.0,
    [switch]$CohortDiagnostics,
    [int]$ScreenMaskTileWidth = 8,
    [double]$ScreenMaskDilationPixels = 2.0,
    [int]$TraceX = -1,
    [int]$TraceY = -1
)

$ErrorActionPreference = "Stop"

$rootPath = (Resolve-Path -LiteralPath $Root).Path
$sourcePath = Join-Path $rootPath "tools/far_horizon_parity.cpp"
$buildDir = Join-Path $rootPath "build/tools"
$exePath = Join-Path $buildDir "far_horizon_parity.exe"
$objPath = Join-Path $buildDir "far_horizon_parity.obj"

if (!(Test-Path -LiteralPath $sourcePath)) {
    throw "Verifier source not found: $sourcePath"
}

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$needsBuild = !(Test-Path -LiteralPath $exePath)
if (!$needsBuild) {
    $sourceTime = (Get-Item -LiteralPath $sourcePath).LastWriteTimeUtc
    $exeTime = (Get-Item -LiteralPath $exePath).LastWriteTimeUtc
    $needsBuild = $sourceTime -gt $exeTime
}

if ($needsBuild) {
    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
    if ($null -eq $cl) {
        throw "cl.exe not found. Run this script from a VS developer shell or after vcvars64.bat."
    }
    & cl.exe /nologo /std:c++20 /EHsc /O2 /W4 /permissive- "/Fe:$exePath" "/Fo$objPath" $sourcePath
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

& $exePath `
    --seed $Seed `
    --bins $Bins `
    --slope-bins $SlopeBins `
    --pixel-step $PixelStep `
    --y-min $YMin `
    --y-max $YMax `
    --camera-x $CameraX `
    --camera-y $CameraY `
    --camera-z $CameraZ `
    --forward-x $ForwardX `
    --forward-y $ForwardY `
    --forward-z $ForwardZ `
    --start-distance $StartDistance `
    --hit-t-rel-tolerance $HitTRelTolerance `
    --mode $Mode `
    --grid-cell-size $GridCellSize `
    --grid-extent $GridExtent `
    --grid-query-step $GridQueryStep `
    --grid-height-pad $GridHeightPad `
    --grid-subsamples $GridSubsamples `
    --grid-refine-window $GridRefineWindow `
    --grid-refine-step $GridRefineStep `
    --structural-samples $StructuralSamples `
    --diagnostic-height-eval-work $DiagnosticHeightEvalWork `
    --cache-cell-work $CacheCellWork `
    --refine-height-eval-work $RefineHeightEvalWork `
    --screen-mask-tile-width $ScreenMaskTileWidth `
    --screen-mask-dilation-pixels $ScreenMaskDilationPixels `
    --trace-x $TraceX `
    --trace-y $TraceY `
    --hierarchical-dda-max-level $HierarchicalDdaMaxLevel `
    --hierarchical-dda-min-ray-y $HierarchicalDdaMinRayY `
    $(if ($ConservativeOnly) { "--conservative-only" }) `
    $(if ($GridExactRefine) { "--grid-exact-refine" }) `
    $(if ($GridRefineReject) { "--grid-refine-reject" }) `
    $(if ($StructuralCheck) { "--structural-check" }) `
    $(if ($FallbackDiagnostic) { "--fallback-diagnostic" }) `
    $(if ($HierarchicalDda) { "--hierarchical-dda" }) `
    $(if ($CohortDiagnostics) { "--cohort-diagnostics" }) `
    $(if ($RequireNetWorkGain) { "--require-net-work-gain" })
exit $LASTEXITCODE
