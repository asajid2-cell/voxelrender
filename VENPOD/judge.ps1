# judge.ps1 - MACHINE verifier for debug-69 mid-isolation captures.
# Pixel-counts the debug-69 cause-map colors in native full-res frames (no downscaling,
# ever) and emits a machine-readable verdict per frame + a PASS/FAIL exit code against
# thresholds. This replaces eyeballing as the primary gate; human/Codex visual judgment
# remains a second, independent gate for non-debug renders.
#
# Debug-69 palette (PS_Raymarch.hlsl mid-only pass):
#   GREEN  (13,255,26)   = real mid-voxel DDA hit            (GOOD)
#   ORANGE (255,128,0)   = DDA per-ray miss -> smooth column (BAD: approx terrain)
#   CYAN   (0,255,255)   = clipmap header invalid            (BAD: DDA dead)
#   BLUE   (0,0,255)     = mid residency gate                (BAD)
#   RED    (255,0,0)     = DDA path disabled (bit4)          (BAD)
#   MAGENTA(255,0,255)   = budget/quality gate               (BAD)
# The mid pass is upscale-composited bilinearly, so edges blend: match with windows.
param(
    [Parameter(Mandatory=$true)][string]$Path,   # PNG file or directory of engine_frame_*.png
    [ValidateSet("debug69","owner58")][string]$Palette = "debug69",
    [double]$MaxOrangePct = 0.5,                 # debug69: FAIL if orange exceeds this % of frame
    [double]$MaxGatePct   = 0.1,                 # debug69: FAIL if cyan+blue+red+magenta exceeds this %
    [double]$MinGreenPct  = -1.0,                # optional: FAIL if green below this % (-1 = skip)
    [double]$MaxMidHeightPct = 2.0,              # owner58: FAIL if MID_HEIGHT (smooth) exceeds this %
    [switch]$Quiet
)
# owner58 palette (debug 58, DebugOwnerLayerColor): the FULL pass paints which layer OWNS
# each background pixel. MID_HEIGHT = the smooth height/column layer = the "approx terrain"
# the goal bans. Near-raster pixels keep real colors (not painted) and aren't counted.
#   MID_VOXEL  (13,242,64) green | MID_HEIGHT (255,219,20) yellow | FAR_SVO (51,107,255) blue
#   FAR_HEIGHT (255,115,20) orange | FAR_WATER (5,199,255) cyan
# NOTE (2026-06-12): the debug69 orange metric UNDERCOUNTS smoothness — the mid-only pass's
# full misses fall through to the full pass's smooth mid, which mode 69 does not paint.
# owner58 MidHeightPct is the trustworthy anti-smooth machine metric.
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

function Judge-Frame([string]$file) {
    $img = [System.Drawing.Bitmap]::FromFile($file)
    try {
        $w = $img.Width; $h = $img.Height
        $rect = New-Object System.Drawing.Rectangle 0,0,$w,$h
        $data = $img.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
            [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
        $stride = $data.Stride
        $bytes = New-Object byte[] ($stride * $h)
        [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
        $img.UnlockBits($data)

        [long]$green=0; [long]$orange=0; [long]$cyan=0; [long]$blue=0; [long]$red=0; [long]$magenta=0
        [long]$midH=0; [long]$farW=0
        for ($y = 0; $y -lt $h; $y++) {
            $row = $y * $stride
            for ($x = 0; $x -lt $w; $x++) {
                $i = $row + $x*3
                $b = $bytes[$i]; $g = $bytes[$i+1]; $r = $bytes[$i+2]
                if ($script:Pal -eq "owner58") {
                    if     ($g -gt 200 -and $r -lt 90  -and $b -lt 110) { $green++ }              # MID_VOXEL
                    elseif ($r -gt 220 -and $g -gt 185 -and $g -lt 245 -and $b -lt 70) { $midH++ } # MID_HEIGHT
                    elseif ($r -lt 110 -and $g -lt 150 -and $b -gt 210) { $blue++ }               # FAR_SVO
                    elseif ($r -gt 220 -and $g -gt 80 -and $g -lt 160 -and $b -lt 70) { $orange++ } # FAR_HEIGHT
                    elseif ($r -lt 80  -and $g -gt 160 -and $b -gt 210) { $farW++ }               # FAR_WATER
                } else {
                    if     ($g -gt 200 -and $r -lt 90  -and $b -lt 90)  { $green++ }
                    elseif ($r -gt 200 -and $g -gt 90 -and $g -lt 175 -and $b -lt 60) { $orange++ }
                    elseif ($r -lt 80  -and $g -gt 200 -and $b -gt 200) { $cyan++ }
                    elseif ($r -lt 60  -and $g -lt 60  -and $b -gt 200) { $blue++ }
                    elseif ($r -gt 215 -and $g -lt 50  -and $b -lt 50)  { $red++ }
                    elseif ($r -gt 200 -and $g -lt 60  -and $b -gt 200) { $magenta++ }
                }
            }
        }
        $total = [double]($w * $h)
        [pscustomobject]@{
            file        = Split-Path -Leaf $file
            res         = "${w}x${h}"
            greenPct    = [math]::Round(100.0*$green/$total, 3)
            orangePct   = [math]::Round(100.0*$orange/$total, 3)
            cyanPct     = [math]::Round(100.0*$cyan/$total, 3)
            gatePct     = [math]::Round(100.0*($cyan+$blue+$red+$magenta)/$total, 3)
            midHeightPct= [math]::Round(100.0*$midH/$total, 3)
            farWaterPct = [math]::Round(100.0*$farW/$total, 3)
            farSvoPct   = [math]::Round(100.0*$blue/$total, 3)
        }
    } finally { $img.Dispose() }
}

$script:Pal = $Palette
$files = @()
if (Test-Path $Path -PathType Container) {
    $files = @(Get-ChildItem (Join-Path $Path "engine_frame_*.png") | Sort-Object Name | ForEach-Object FullName)
} else { $files = @((Resolve-Path $Path).Path) }
if ($files.Count -eq 0) { Write-Output "JUDGE: NO FRAMES at $Path"; exit 2 }

$fail = $false
foreach ($f in $files) {
    $v = Judge-Frame $f
    $verdicts = @()
    if ($Palette -eq "owner58") {
        if ($v.midHeightPct -gt $MaxMidHeightPct) { $verdicts += "MIDHEIGHT>$MaxMidHeightPct"; $fail = $true }
        if ($MinGreenPct -ge 0 -and $v.greenPct -lt $MinGreenPct) { $verdicts += "GREEN<$MinGreenPct"; $fail = $true }
        $tag = if ($verdicts.Count) { "FAIL[" + ($verdicts -join ",") + "]" } else { "PASS" }
        if (-not $Quiet -or $verdicts.Count) {
            Write-Output ("{0} {1} {2} midVoxel={3}% midHeight={4}% farSvo={5}% farHeight={6}% farWater={7}%" -f
                $tag, $v.file, $v.res, $v.greenPct, $v.midHeightPct, $v.farSvoPct, $v.orangePct, $v.farWaterPct)
        }
    } else {
        if ($v.orangePct -gt $MaxOrangePct) { $verdicts += "ORANGE>$MaxOrangePct"; $fail = $true }
        if ($v.gatePct   -gt $MaxGatePct)   { $verdicts += "GATE>$MaxGatePct";     $fail = $true }
        if ($MinGreenPct -ge 0 -and $v.greenPct -lt $MinGreenPct) { $verdicts += "GREEN<$MinGreenPct"; $fail = $true }
        $tag = if ($verdicts.Count) { "FAIL[" + ($verdicts -join ",") + "]" } else { "PASS" }
        if (-not $Quiet -or $verdicts.Count) {
            Write-Output ("{0} {1} {2} green={3}% orange={4}% cyan={5}% gates={6}%" -f
                $tag, $v.file, $v.res, $v.greenPct, $v.orangePct, $v.cyanPct, $v.gatePct)
        }
    }
}
if ($fail) { Write-Output "JUDGE: FAIL"; exit 1 } else { Write-Output "JUDGE: PASS"; exit 0 }
