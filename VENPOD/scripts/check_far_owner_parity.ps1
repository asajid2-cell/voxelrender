param(
    [string]$Root = "Z:/328/CMPUT328-A2/codexworks/301/3d/VENPOD",
    [Nullable[double]]$ActualSurfaceY = $null,
    [uint32]$Seed = 12345,
    [double]$StartDistance = 256.0,
    [double]$EndDistance = 9000.0,
    [double]$Tolerance = 1.0,
    [double]$QuantumTolerance = 0.0,
    [double]$CameraX = 192.0,
    [double]$CameraY = 0.0,
    [double]$CameraZ = 224.0,
    [double]$FootprintStep = 4.0,
    [int]$MinFaces = 0,
    [int]$GridCellCount = 384,
    [double]$GridCellSize = 28.0,
    [double]$OwnerMaxDistance = 11000.0,
    [Nullable[double]]$GridOriginX = $null,
    [Nullable[double]]$GridOriginZ = $null,
    [switch]$AllowBelowSeaFaces,
    [switch]$AllowSideFaces,
    [switch]$RequireGridCoverage,
    [ValidateSet("all-bands", "handoff")]
    [string]$SampleMode = "all-bands",
    [string]$FaceCsv = ""
)

$ErrorActionPreference = "Stop"

$rootPath = (Resolve-Path -LiteralPath $Root).Path
$sourcePath = Join-Path $rootPath "tools/far_owner_parity.cpp"
$rendererPath = Join-Path $rootPath "src/Graphics/Renderer.cpp"
$buildDir = Join-Path $rootPath "build/tools"
$exePath = Join-Path $buildDir "far_owner_parity.exe"
$objPath = Join-Path $buildDir "far_owner_parity.obj"

if (!(Test-Path -LiteralPath $sourcePath)) {
    throw "Verifier source not found: $sourcePath"
}
if (!(Test-Path -LiteralPath $rendererPath)) {
    throw "Renderer source not found: $rendererPath"
}

if ([string]::IsNullOrWhiteSpace($FaceCsv) -and $null -eq $ActualSurfaceY) {
    $rendererText = Get-Content -LiteralPath $rendererPath -Raw
    if ($rendererText -notmatch "kFarHeightfieldCpuProbeSurfaceY\s*=\s*([0-9.+-]+)f?") {
        throw "Could not find kFarHeightfieldCpuProbeSurfaceY in $rendererPath. Pass -ActualSurfaceY explicitly."
    }
    $ActualSurfaceY = [double]$Matches[1]
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

$argsList = @()
if (![string]::IsNullOrWhiteSpace($FaceCsv)) {
    $argsList += @("--face-csv", $FaceCsv)
} else {
    $argsList += @("--actual-surface-y", ([string]$ActualSurfaceY))
}
$argsList += @(
    "--seed", ([string]$Seed),
    "--start-distance", ([string]$StartDistance),
    "--end-distance", ([string]$EndDistance),
    "--tolerance", ([string]$Tolerance),
    "--quantum-tolerance", ([string]$QuantumTolerance),
    "--camera-x", ([string]$CameraX),
    "--camera-y", ([string]$CameraY),
    "--camera-z", ([string]$CameraZ),
    "--footprint-step", ([string]$FootprintStep),
    "--min-faces", ([string]$MinFaces),
    "--grid-cell-count", ([string]$GridCellCount),
    "--grid-cell-size", ([string]$GridCellSize),
    "--owner-max-distance", ([string]$OwnerMaxDistance),
    "--sample-mode", $SampleMode
)
if ($null -ne $GridOriginX) {
    $argsList += @("--grid-origin-x", ([string]$GridOriginX))
}
if ($null -ne $GridOriginZ) {
    $argsList += @("--grid-origin-z", ([string]$GridOriginZ))
}
if ($AllowBelowSeaFaces) {
    $argsList += "--allow-below-sea-faces"
}
if ($AllowSideFaces) {
    $argsList += "--allow-side-faces"
}
if ($RequireGridCoverage) {
    $argsList += "--require-grid-coverage"
}

& $exePath @argsList
exit $LASTEXITCODE
