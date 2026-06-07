param(
    [string]$BaseAuditCsv,
    [string]$OwnerFrame,
    [string]$MaterialFrame,
    [string]$LodFrame,
    [string]$NormalFrame,
    [string]$FaceFrame,
    [string]$DistanceFrame,
    [string]$OccupancyFrame = "",
    [string]$OutputCsv,
    [int]$MaxRows = 128,
    [switch]$MidOnly
)

$ErrorActionPreference = "Stop"

foreach ($path in @($BaseAuditCsv, $OwnerFrame, $MaterialFrame, $LodFrame, $NormalFrame, $FaceFrame, $DistanceFrame)) {
    if ([string]::IsNullOrWhiteSpace($path) -or -not (Test-Path $path)) {
        throw "Required input not found: $path"
    }
}
if ([string]::IsNullOrWhiteSpace($OutputCsv)) { throw "-OutputCsv is required" }

Add-Type -AssemblyName System.Drawing

function Get-Pixel([System.Drawing.Bitmap]$Bitmap, [int]$X, [int]$Y) {
    $Bitmap.GetPixel($X, $Y)
}

function NearColor([System.Drawing.Color]$Color, [int]$R, [int]$G, [int]$B, [int]$Tolerance) {
    return ([Math]::Abs([int]$Color.R - $R) -le $Tolerance) -and
        ([Math]::Abs([int]$Color.G - $G) -le $Tolerance) -and
        ([Math]::Abs([int]$Color.B - $B) -le $Tolerance)
}

function Classify-Owner([System.Drawing.Color]$Color) {
    if (NearColor $Color 255 242 13 70) { return "exact_sparse_surface" }
    if (NearColor $Color 13 242 64 80) { return "mid_voxel" }
    if (NearColor $Color 51 107 255 80) { return "far_svo" }
    if (NearColor $Color 255 20 5 80) { return "mid_interior_fallback" }
    if (NearColor $Color 219 46 255 80) { return "mid_parent_fallback" }
    if (NearColor $Color 5 224 255 80) { return "water" }
    return "other"
}

function Classify-Material([System.Drawing.Color]$Color) {
    if (NearColor $Color 13 97 255 45) { return "water" }
    if (NearColor $Color 255 214 31 55) { return "sand" }
    if (NearColor $Color 46 199 51 55) { return "dirt" }
    if (NearColor $Color 140 140 140 65) { return "stone" }
    return "other"
}

function Classify-Face([System.Drawing.Color]$Color, [string]$OwnerState) {
    if ($OwnerState -eq "mid_interior_fallback" -or (NearColor $Color 255 20 5 70)) { return "interior_fallback" }
    if ($OwnerState -eq "mid_parent_fallback" -or (NearColor $Color 219 46 255 70)) { return "parent_or_coarse_fallback" }
    if (NearColor $Color 13 242 46 80) { return "top_surface" }
    if (NearColor $Color 242 13 242 80) { return "underside" }
    if (NearColor $Color 255 122 13 90) { return "side_face" }
    return "unclassified"
}

function Decode-Normal([System.Drawing.Color]$Color) {
    $nx = ([double]$Color.R / 255.0) * 2.0 - 1.0
    $ny = ([double]$Color.G / 255.0) * 2.0 - 1.0
    $nz = ([double]$Color.B / 255.0) * 2.0 - 1.0
    return @($nx, $ny, $nz)
}

function Decode-Lod([System.Drawing.Color]$Color, [string]$Owner) {
    if ($Owner -eq "mid_voxel") {
        $ringFloat = ([double]$Color.R / 255.0) * 4.0 - 1.0
        $ring = [Math]::Max(0, [Math]::Min(3, [int][Math]::Round($ringFloat)))
        $cell = @(12, 24, 48, 96)[$ring]
        return @("ring$ring", $cell)
    }
    if ($Owner -eq "far_svo") {
        $cell = [Math]::Round(([double]$Color.G / 255.0) * 160.0, 1)
        return @("far_svo_cell", $cell)
    }
    return @("", "")
}

function Decode-Distance([System.Drawing.Color]$Color) {
    [Math]::Round(([double]$Color.R / 255.0) * 6400.0, 1)
}

function Likely-Cause([string]$Owner, [double]$Distance, [double]$CellSize, [string]$FaceType, [bool]$Fallback, [string]$Material) {
    if ($Fallback) { return "parent_or_interior_fallback" }
    if ($Owner -eq "far_svo" -and $Distance -lt 2200.0) { return "far_svo_visible_too_near" }
    if ($Owner -eq "far_svo" -and $CellSize -ge 24.0) { return "coarse_far_svo_surface" }
    if ($Owner -eq "mid_voxel" -and $CellSize -le 12.0 -and $FaceType -eq "side_face") { return "fine_mid_voxel_exposed_side_faces" }
    if ($Owner -eq "mid_voxel" -and $CellSize -gt 12.0) { return "mid_voxel_cell_size_or_lod" }
    if ($FaceType -eq "underside" -or $FaceType -eq "interior_fallback") { return "exposed_under_or_interior_faces" }
    if ($Material -eq "stone" -and ($FaceType -eq "side_face" -or $FaceType -eq "top_surface")) { return "stone_geometry_readability" }
    return "needs_visual_review"
}

$rows = Import-Csv $BaseAuditCsv | Where-Object { $_.ownerLayer -in @("mid_voxel", "far_svo") } | Select-Object -First $MaxRows

$ownerBmp = [System.Drawing.Bitmap]::new((Resolve-Path $OwnerFrame).Path)
$materialBmp = [System.Drawing.Bitmap]::new((Resolve-Path $MaterialFrame).Path)
$lodBmp = [System.Drawing.Bitmap]::new((Resolve-Path $LodFrame).Path)
$normalBmp = [System.Drawing.Bitmap]::new((Resolve-Path $NormalFrame).Path)
$faceBmp = [System.Drawing.Bitmap]::new((Resolve-Path $FaceFrame).Path)
$distanceBmp = [System.Drawing.Bitmap]::new((Resolve-Path $DistanceFrame).Path)
$occupancyBmp = if ([string]::IsNullOrWhiteSpace($OccupancyFrame)) {
    $null
} else {
    [System.Drawing.Bitmap]::new((Resolve-Path $OccupancyFrame).Path)
}

try {
    $outRows = foreach ($row in $rows) {
        $x = [int]$row.pixelX
        $y = [int]$row.pixelY
        $ownerState = Classify-Owner (Get-Pixel $ownerBmp $x $y)
        $owner = if ($ownerState -like "mid_*") { "mid_voxel" } elseif ($ownerState -eq "far_svo") { "far_svo" } else { $row.ownerLayer }
        $material = Classify-Material (Get-Pixel $materialBmp $x $y)
        $lod = Decode-Lod (Get-Pixel $lodBmp $x $y) $owner
        $normal = Decode-Normal (Get-Pixel $normalBmp $x $y)
        $faceType = Classify-Face (Get-Pixel $faceBmp $x $y) $ownerState
        $distance = Decode-Distance (Get-Pixel $distanceBmp $x $y)
        $fallback = $ownerState -eq "mid_parent_fallback" -or $ownerState -eq "mid_interior_fallback" -or
            $faceType -eq "parent_or_coarse_fallback" -or $faceType -eq "interior_fallback"
        $cellSize = if ($lod[1] -eq "") { 0.0 } else { [double]$lod[1] }
        $airNeighborCount = ""
        $solidNeighborCount = ""
        $generatedOccupancyClass = ""
        if ($occupancyBmp -ne $null -and $owner -eq "mid_voxel") {
            $occ = Get-Pixel $occupancyBmp $x $y
            $airNeighborCount = [Math]::Max(0, [Math]::Min(6, [int][Math]::Round(([double]$occ.R / 255.0) * 6.0)))
            $solidNeighborCount = 6 - $airNeighborCount
            $generatedOccupancyClass = if ($airNeighborCount -le 1) {
                "mostly_embedded"
            } elseif ($airNeighborCount -le 3) {
                "terrain_boundary"
            } else {
                "thin_or_isolated"
            }
        }
        [pscustomobject]@{
            frame = $row.frame
            region = $row.region
            pixelX = $x
            pixelY = $y
            owner = $owner
            ownerDebugState = $ownerState
            material = $material
            shaderHitDistance = $distance
            lodOrDepth = $lod[0]
            cellSize = $lod[1]
            normalX = [Math]::Round($normal[0], 3)
            normalY = [Math]::Round($normal[1], 3)
            normalZ = [Math]::Round($normal[2], 3)
            faceType = $faceType
            airNeighborCount = $airNeighborCount
            solidNeighborCount = $solidNeighborCount
            generatedOccupancyClass = $generatedOccupancyClass
            parentOrCoarseFallback = $fallback
            exactShouldOwnInstead = if ($owner -in @("mid_voxel", "far_svo")) { "not_indicated_by_owner_debug" } else { "" }
            likelyCause = Likely-Cause $owner $distance $cellSize $faceType $fallback $material
        }
    }
    $parent = Split-Path -Parent $OutputCsv
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    if ($MidOnly) {
        $outRows = @($outRows | Where-Object { $_.owner -eq "mid_voxel" })
    }
    $outRows | Export-Csv -NoTypeInformation -Path $OutputCsv
    Write-Host "Wrote $OutputCsv"
    $outRows
} finally {
    $ownerBmp.Dispose()
    $materialBmp.Dispose()
    $lodBmp.Dispose()
    $normalBmp.Dispose()
    $faceBmp.Dispose()
    $distanceBmp.Dispose()
    if ($occupancyBmp -ne $null) {
        $occupancyBmp.Dispose()
    }
}
