param(
    [string]$CaptureDir,
    [string]$OutputCsv = "",
    [int]$TopFrames = 5,
    [int]$AuditRows = 64
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($CaptureDir)) {
    throw "-CaptureDir is required"
}

$capturePath = (Resolve-Path -LiteralPath $CaptureDir).Path
if ([string]::IsNullOrWhiteSpace($OutputCsv)) {
    $OutputCsv = Join-Path $capturePath "worst_gap_frames.csv"
}

$statsPath = Join-Path $capturePath "image_stats.csv"
$logPath = Join-Path $capturePath "venpod_runtime.log"
$ownershipPath = Join-Path $capturePath "ownership_timeline.csv"
$layerPath = Join-Path $capturePath "layer_screen_timeline.csv"

foreach ($path in @($statsPath, $logPath, $ownershipPath, $layerPath)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required artifact missing: $path"
    }
}

function Get-FrameNumberFromFileName {
    param([string]$Name)
    if ($Name -match 'engine_frame_(\d+)\.bmp') {
        return [int]$Matches[1]
    }
    return -1
}

function To-Double {
    param($Value)
    if ($null -eq $Value -or $Value -eq "") { return 0.0 }
    return [double]::Parse([string]$Value, [Globalization.CultureInfo]::InvariantCulture)
}

function Parse-PerfState {
    param(
        [string]$Log,
        [int]$Frame
    )
    $state = [ordered]@{
        farSvoState = ""
        farStage = ""
        farCov = ""
        midCov = ""
    }
    $text = Get-Content -LiteralPath $Log -Raw
    $perfMatches = [regex]::Matches($text, 'PERF frame=(\d+).*?farSvo=([a-zA-Z]+).*?farStage=([a-zA-Z]+).*?farCov=([0-9.]+/[0-9.]+)', 'Singleline')
    $bestPerf = $null
    foreach ($match in $perfMatches) {
        $distance = [math]::Abs(([int]$match.Groups[1].Value) - $Frame)
        if ($null -eq $bestPerf -or $distance -lt $bestPerf.distance) {
            $bestPerf = [pscustomobject]@{ distance = $distance; match = $match }
        }
    }
    if ($bestPerf) {
        $state.farSvoState = $bestPerf.match.Groups[2].Value
        $state.farStage = $bestPerf.match.Groups[3].Value
        $state.farCov = $bestPerf.match.Groups[4].Value
    }
    $sparseMatches = [regex]::Matches($text, 'PERF_SPARSE frame=(\d+).*?midCov=([0-9.]+/[0-9.]+)', 'Singleline')
    $bestSparse = $null
    foreach ($match in $sparseMatches) {
        $distance = [math]::Abs(([int]$match.Groups[1].Value) - $Frame)
        if ($null -eq $bestSparse -or $distance -lt $bestSparse.distance) {
            $bestSparse = [pscustomobject]@{ distance = $distance; match = $match }
        }
    }
    if ($bestSparse) {
        $state.midCov = $bestSparse.match.Groups[2].Value
    }
    return [pscustomobject]$state
}

function Get-OwnershipForFrame {
    param(
        [array]$Rows,
        [int]$Frame
    )
    $exact = $Rows | Where-Object { [int]$_.shaderFrame -eq $Frame } | Select-Object -First 1
    if ($exact) { return $exact }
    return $Rows |
        Sort-Object { [math]::Abs(([int]$_.shaderFrame) - $Frame) } |
        Select-Object -First 1
}

function Get-LayerForFrame {
    param(
        [array]$Rows,
        [int]$Frame
    )
    $frameColumn = if (($Rows | Select-Object -First 1).PSObject.Properties.Name -contains "frame") { "frame" } else { "shaderFrame" }
    $exact = $Rows | Where-Object { [int]$_.$frameColumn -eq $Frame } | Select-Object -First 1
    if ($exact) { return $exact }
    return $Rows |
        Sort-Object { [math]::Abs(([int]$_.$frameColumn) - $Frame) } |
        Select-Object -First 1
}

$stats = Import-Csv -LiteralPath $statsPath
$ownership = Import-Csv -LiteralPath $ownershipPath
$layers = Import-Csv -LiteralPath $layerPath

$ranked = foreach ($row in $stats) {
    $frame = Get-FrameNumberFromFileName -Name $row.file
    if ($frame -lt 0) { continue }
    $skyRun = To-Double $row.skylineInteriorSkyRunPct
    $skyInterior = To-Double $row.skylineInteriorSkyPct
    $skyLike = To-Double $row.skyLikePct
    $terrainLike = To-Double $row.terrainLikePct
    $score = $skyRun * 3.0 + $skyInterior * 1.5 + $skyLike * 0.25 - $terrainLike * 0.02
    [pscustomobject]@{
        frame = $frame
        file = $row.file
        skylineRun = $skyRun
        skylineInteriorSky = $skyInterior
        skyLike = $skyLike
        terrainLike = $terrainLike
        score = $score
    }
}

$worst = $ranked |
    Sort-Object -Property @{Expression = "score"; Descending = $true}, @{Expression = "frame"; Descending = $false} |
    Select-Object -First $TopFrames

$inventory = New-Object System.Collections.Generic.List[object]
foreach ($row in $worst) {
    $framePath = Join-Path $capturePath $row.file
    $auditPath = Join-Path $capturePath ("hole_audit_frame_{0:D4}.csv" -f $row.frame)
    powershell -ExecutionPolicy Bypass -File .\terrain_hole_ray_audit.ps1 `
        -FramePath $framePath `
        -LogPath $logPath `
        -OutputCsv $auditPath `
        -Frame $row.frame `
        -MaxRows $AuditRows `
        -AutoSelect | Out-Host

    $frameAuditRows = Import-Csv -LiteralPath $auditPath
    $dominantAuditBucket = "no_auto_holes"
    if ($frameAuditRows.Count -gt 0) {
        $dominantAuditBucket = ($frameAuditRows |
            Group-Object dominantBucket |
            Sort-Object Count -Descending |
            Select-Object -First 1).Name
    }

    $owner = Get-OwnershipForFrame -Rows $ownership -Frame $row.frame
    $layer = Get-LayerForFrame -Rows $layers -Frame $row.frame
    $perf = Parse-PerfState -Log $logPath -Frame $row.frame
    $proxyPct = 0.0
    if ($layer) {
        if ($layer.PSObject.Properties.Name -contains "heightProxyScreenPct") {
            $proxyPct = To-Double $layer.heightProxyScreenPct
        } elseif ($layer.PSObject.Properties.Name -contains "heightProxyPct") {
            $proxyPct = To-Double $layer.heightProxyPct
        }
    }
    $farWaterPct = if ($layer -and ($layer.PSObject.Properties.Name -contains "farWaterScreenPct")) {
        To-Double $layer.farWaterScreenPct
    } elseif ($owner) {
        To-Double $owner.farWaterPct
    } else {
        0.0
    }
    $missPct = if ($owner) { To-Double $owner.missPct } else { 0.0 }

    $inventory.Add([pscustomobject]@{
        frame = $row.frame
        skylineRun = "{0:0.###}" -f $row.skylineRun
        skylineInteriorSky = "{0:0.###}" -f $row.skylineInteriorSky
        farSvoState = $perf.farSvoState
        farStage = $perf.farStage
        farCov = $perf.farCov
        midCov = $perf.midCov
        missPct = "{0:0.###}" -f $missPct
        proxyPct = "{0:0.###}" -f $proxyPct
        farWaterPct = "{0:0.###}" -f $farWaterPct
        dominantAuditBucket = $dominantAuditBucket
        artifactPath = $framePath
        auditPath = $auditPath
    })
}

$inventory | Export-Csv -LiteralPath $OutputCsv -NoTypeInformation
Write-Host "Wrote $OutputCsv"
