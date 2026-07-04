param(
    [Parameter(Mandatory = $true)]
    [string]$BaselineDir,
    [Parameter(Mandatory = $true)]
    [string]$ExperimentDir,
    [string]$OutputCsv = "",
    [int]$SampleStep = 2,
    [int]$LargeThreshold = 30,
    [int]$VeryLargeThreshold = 90,
    [double]$MaxMeanRgbSum = -1,
    [double]$MaxPctOverLarge = -1
)

$ErrorActionPreference = "Stop"

if ($SampleStep -lt 1) {
    throw "SampleStep must be >= 1"
}
if (-not (Test-Path -LiteralPath $BaselineDir)) {
    throw "BaselineDir not found: $BaselineDir"
}
if (-not (Test-Path -LiteralPath $ExperimentDir)) {
    throw "ExperimentDir not found: $ExperimentDir"
}

Add-Type -AssemblyName System.Drawing

$baselineFiles = @{}
Get-ChildItem -LiteralPath $BaselineDir -Filter *.bmp -File | ForEach-Object {
    $baselineFiles[$_.Name] = $_.FullName
}

$rows = New-Object System.Collections.Generic.List[object]
Get-ChildItem -LiteralPath $ExperimentDir -Filter *.bmp -File | Sort-Object Name | ForEach-Object {
    if (-not $baselineFiles.ContainsKey($_.Name)) {
        return
    }

    $baselinePath = $baselineFiles[$_.Name]
    $experimentPath = $_.FullName
    $baseline = [System.Drawing.Bitmap]::FromFile($baselinePath)
    $experiment = [System.Drawing.Bitmap]::FromFile($experimentPath)
    try {
        if ($baseline.Width -ne $experiment.Width -or $baseline.Height -ne $experiment.Height) {
            throw "Image size mismatch for $($_.Name): baseline=$($baseline.Width)x$($baseline.Height) experiment=$($experiment.Width)x$($experiment.Height)"
        }

        [double]$sum = 0
        [double]$sumSq = 0
        [int64]$count = 0
        [int64]$large = 0
        [int64]$veryLarge = 0
        [int]$max = 0
        for ($y = 0; $y -lt $baseline.Height; $y += $SampleStep) {
            for ($x = 0; $x -lt $baseline.Width; $x += $SampleStep) {
                $a = $baseline.GetPixel($x, $y)
                $b = $experiment.GetPixel($x, $y)
                $delta =
                    [Math]::Abs($a.R - $b.R) +
                    [Math]::Abs($a.G - $b.G) +
                    [Math]::Abs($a.B - $b.B)
                $sum += $delta
                $sumSq += $delta * $delta
                if ($delta -gt $max) {
                    $max = $delta
                }
                if ($delta -gt $LargeThreshold) {
                    ++$large
                }
                if ($delta -gt $VeryLargeThreshold) {
                    ++$veryLarge
                }
                ++$count
            }
        }

        $mean = $sum / [double]$count
        $pctLarge = 100.0 * [double]$large / [double]$count
        $rows.Add([pscustomobject]@{
            file = $_.Name
            width = $baseline.Width
            height = $baseline.Height
            sampleStep = $SampleStep
            samples = $count
            meanAbsRgbSum = [Math]::Round($mean, 6)
            rmsRgbSum = [Math]::Round([Math]::Sqrt($sumSq / [double]$count), 6)
            maxRgbSum = $max
            pctSamplesRgbSumOverLarge = [Math]::Round($pctLarge, 6)
            pctSamplesRgbSumOverVeryLarge = [Math]::Round(100.0 * [double]$veryLarge / [double]$count, 6)
        }) | Out-Null
    } finally {
        $baseline.Dispose()
        $experiment.Dispose()
    }
}

if ($rows.Count -eq 0) {
    throw "No matching BMP files found between $BaselineDir and $ExperimentDir"
}

if ($OutputCsv) {
    $parent = Split-Path -Parent $OutputCsv
    if ($parent) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    $rows | Export-Csv -Path $OutputCsv -NoTypeInformation -Encoding ASCII
}

$rows | Format-Table -AutoSize

$failed = $false
if ($MaxMeanRgbSum -ge 0) {
    $maxObservedMean = ($rows | Measure-Object meanAbsRgbSum -Maximum).Maximum
    if ($maxObservedMean -gt $MaxMeanRgbSum) {
        Write-Error "Max meanAbsRgbSum $maxObservedMean exceeded threshold $MaxMeanRgbSum"
        $failed = $true
    }
}
if ($MaxPctOverLarge -ge 0) {
    $maxObservedPct = ($rows | Measure-Object pctSamplesRgbSumOverLarge -Maximum).Maximum
    if ($maxObservedPct -gt $MaxPctOverLarge) {
        Write-Error "Max pctSamplesRgbSumOverLarge $maxObservedPct exceeded threshold $MaxPctOverLarge"
        $failed = $true
    }
}

if ($failed) {
    exit 2
}
