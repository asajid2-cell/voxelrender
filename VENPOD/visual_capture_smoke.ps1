# =============================================================================
# VENPOD - Visual Capture Smoke
# Launches the sparse renderer, captures the real Windows application window,
# writes PNG frames/contact sheet, and records simple image statistics.
# =============================================================================

param(
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$NoBuild,
    [int]$ExitAfterFrames = 240,
    [int]$CaptureCount = 8,
    [int]$CaptureIntervalMs = 350,
    [int]$InitialDelayMs = 1200,
    [string]$OutputDir = "",
    [string]$WindowTitle = "VENPOD - Voxel Physics Engine"
)

$ErrorActionPreference = "Stop"

function Write-Step { Write-Host "`n==> $args" -ForegroundColor Cyan }
function Write-Success { Write-Host "[OK] $args" -ForegroundColor Green }
function Write-Info { Write-Host "  $args" -ForegroundColor Gray }
function Write-Warn { Write-Host "[WARN] $args" -ForegroundColor Yellow }
function Stop-OnFailure {
    param(
        [int]$Code,
        [string]$Stage
    )
    if ($Code -ne 0) {
        Write-Host "[ERROR] $Stage failed with exit code $Code" -ForegroundColor Red
        exit $Code
    }
}

$projectRoot = $PSScriptRoot
$buildScript = Join-Path $projectRoot "build.ps1"
$buildDir = Join-Path $projectRoot "build"

if (-not (Test-Path $buildScript)) {
    throw "build.ps1 not found at $buildScript"
}

if (-not $NoBuild) {
    Write-Step "Building latest code..."
    if ($Clean) {
        & $buildScript -Config $Config -Clean
    } else {
        & $buildScript -Config $Config
    }
    Stop-OnFailure -Code $LASTEXITCODE -Stage "Build"
} else {
    Write-Info "Build step: skipped (-NoBuild)"
}

$possibleExePaths = @(
    (Join-Path $buildDir "bin\$Config\VENPOD.exe"),
    (Join-Path $buildDir "bin\VENPOD.exe"),
    (Join-Path $buildDir "$Config\VENPOD.exe"),
    (Join-Path $buildDir "VENPOD.exe")
)
$exePath = $possibleExePaths | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exePath) {
    throw "VENPOD.exe not found. Searched: $($possibleExePaths -join ', ')"
}
$exeDir = Split-Path $exePath -Parent

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputDir = Join-Path $buildDir "captures\sparse_visual_$stamp"
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

$savedEnv = @{
    VENPOD_LOG_FILE = $env:VENPOD_LOG_FILE
    VENPOD_DIAGNOSTICS = $env:VENPOD_DIAGNOSTICS
    VENPOD_MODE = $env:VENPOD_MODE
    VENPOD_DISABLE_PHYSICS = $env:VENPOD_DISABLE_PHYSICS
    VENPOD_ENABLE_EXPERIMENTAL_SPARSE = $env:VENPOD_ENABLE_EXPERIMENTAL_SPARSE
    VENPOD_RENDER_BACKEND = $env:VENPOD_RENDER_BACKEND
    VENPOD_SPARSE_RAYMARCH = $env:VENPOD_SPARSE_RAYMARCH
    VENPOD_SPARSE_ONLY = $env:VENPOD_SPARSE_ONLY
    VENPOD_SPARSE_SURFACE_AUTHORITATIVE = $env:VENPOD_SPARSE_SURFACE_AUTHORITATIVE
    VENPOD_SPARSE_DEBUG_MODE = $env:VENPOD_SPARSE_DEBUG_MODE
    VENPOD_SPARSE_RENDER_OWNERSHIP = $env:VENPOD_SPARSE_RENDER_OWNERSHIP
    VENPOD_SPARSE_RENDER_OWNERSHIP_INTERVAL = $env:VENPOD_SPARSE_RENDER_OWNERSHIP_INTERVAL
    VENPOD_SPARSE_REQUIRE_PIPE_READY = $env:VENPOD_SPARSE_REQUIRE_PIPE_READY
    VENPOD_SPARSE_PIPE_READY_FRAME = $env:VENPOD_SPARSE_PIPE_READY_FRAME
    VENPOD_SPARSE_REQUIRE_OWNERSHIP_QUALITY = $env:VENPOD_SPARSE_REQUIRE_OWNERSHIP_QUALITY
    VENPOD_SPARSE_OWNERSHIP_QUALITY_READY_FRAME = $env:VENPOD_SPARSE_OWNERSHIP_QUALITY_READY_FRAME
    VENPOD_SPARSE_MIN_TERRAIN_PIXELS_PCT = $env:VENPOD_SPARSE_MIN_TERRAIN_PIXELS_PCT
    VENPOD_SPARSE_MAX_MISS_PIXELS_PCT = $env:VENPOD_SPARSE_MAX_MISS_PIXELS_PCT
    VENPOD_SPARSE_REQUIRE_OWNERSHIP_STABILITY = $env:VENPOD_SPARSE_REQUIRE_OWNERSHIP_STABILITY
    VENPOD_SPARSE_OWNERSHIP_STABILITY_READY_FRAME = $env:VENPOD_SPARSE_OWNERSHIP_STABILITY_READY_FRAME
    VENPOD_SPARSE_MAX_TERRAIN_DELTA_PCT = $env:VENPOD_SPARSE_MAX_TERRAIN_DELTA_PCT
    VENPOD_SPARSE_MAX_MISS_DELTA_PCT = $env:VENPOD_SPARSE_MAX_MISS_DELTA_PCT
    VENPOD_EXIT_AFTER_FRAMES = $env:VENPOD_EXIT_AFTER_FRAMES
}

function Restore-Env {
    foreach ($name in $savedEnv.Keys) {
        if ($null -eq $savedEnv[$name]) {
            Remove-Item "env:$name" -ErrorAction SilentlyContinue
        } else {
            Set-Item "env:$name" $savedEnv[$name]
        }
    }
}

$nativeSource = @"
using System;
using System.Runtime.InteropServices;

public static class VenpodCaptureNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern IntPtr GetWindowDC(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern int ReleaseDC(IntPtr hWnd, IntPtr hDC);

    [DllImport("gdi32.dll")]
    public static extern bool BitBlt(
        IntPtr hdcDest,
        int xDest,
        int yDest,
        int width,
        int height,
        IntPtr hdcSrc,
        int xSrc,
        int ySrc,
        int rasterOp);

    public const int SRCCOPY = 0x00CC0020;
}
"@

try {
    Add-Type -AssemblyName System.Drawing
    if (-not ("VenpodCaptureNative" -as [type])) {
        Add-Type -TypeDefinition $nativeSource
    }

    $env:VENPOD_LOG_FILE = "1"
    $env:VENPOD_DIAGNOSTICS = "1"
    $env:VENPOD_MODE = "sandbox"
    $env:VENPOD_DISABLE_PHYSICS = "1"
    $env:VENPOD_ENABLE_EXPERIMENTAL_SPARSE = "1"
    $env:VENPOD_RENDER_BACKEND = "sparse"
    $env:VENPOD_SPARSE_RAYMARCH = "1"
    $env:VENPOD_SPARSE_ONLY = "1"
    $env:VENPOD_SPARSE_SURFACE_AUTHORITATIVE = "1"
    $env:VENPOD_SPARSE_DEBUG_MODE = "50"
    $env:VENPOD_SPARSE_RENDER_OWNERSHIP = "1"
    $env:VENPOD_SPARSE_RENDER_OWNERSHIP_INTERVAL = "1"
    $env:VENPOD_SPARSE_REQUIRE_PIPE_READY = "1"
    $env:VENPOD_SPARSE_PIPE_READY_FRAME = "120"
    $env:VENPOD_SPARSE_REQUIRE_OWNERSHIP_QUALITY = "1"
    $env:VENPOD_SPARSE_OWNERSHIP_QUALITY_READY_FRAME = "120"
    $env:VENPOD_SPARSE_MIN_TERRAIN_PIXELS_PCT = "35"
    $env:VENPOD_SPARSE_MAX_MISS_PIXELS_PCT = "15"
    $env:VENPOD_SPARSE_REQUIRE_OWNERSHIP_STABILITY = "1"
    $env:VENPOD_SPARSE_OWNERSHIP_STABILITY_READY_FRAME = "120"
    $env:VENPOD_SPARSE_MAX_TERRAIN_DELTA_PCT = "8"
    $env:VENPOD_SPARSE_MAX_MISS_DELTA_PCT = "4"
    $env:VENPOD_EXIT_AFTER_FRAMES = "$ExitAfterFrames"

    Write-Host "VENPOD - Visual Capture Smoke" -ForegroundColor Magenta
    Write-Info "Executable: $exePath"
    Write-Info "Output: $OutputDir"
    Write-Info "Captures: $CaptureCount every ${CaptureIntervalMs}ms after ${InitialDelayMs}ms"

    $process = Start-Process -FilePath $exePath -WorkingDirectory $exeDir -PassThru
    $hwnd = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline -and $hwnd -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 100
        $process.Refresh()
        if ($process.HasExited) {
            throw "VENPOD exited before a window was available. ExitCode=$($process.ExitCode)"
        }
        if ($process.MainWindowHandle -ne 0) {
            $hwnd = [IntPtr]$process.MainWindowHandle
            break
        }
        $candidate = Get-Process | Where-Object {
            $_.MainWindowTitle -like "*$WindowTitle*"
        } | Select-Object -First 1
        if ($candidate) {
            $hwnd = [IntPtr]$candidate.MainWindowHandle
            break
        }
    }
    if ($hwnd -eq [IntPtr]::Zero) {
        throw "Could not find VENPOD window titled '$WindowTitle'."
    }

    [VenpodCaptureNative]::SetForegroundWindow($hwnd) | Out-Null
    Start-Sleep -Milliseconds $InitialDelayMs

    $frames = New-Object System.Collections.Generic.List[string]
    $stats = New-Object System.Collections.Generic.List[string]
    $stats.Add("file,width,height,sampled,skyLikePct,darkPct,terrainLikePct,uniqueSampleColors")
    $blackCaptureFrames = 0

    for ($i = 0; $i -lt $CaptureCount; ++$i) {
        if ($process.HasExited) {
            Write-Warn "VENPOD exited before capture $i."
            break
        }

        $rect = New-Object VenpodCaptureNative+RECT
        if (-not [VenpodCaptureNative]::GetWindowRect($hwnd, [ref]$rect)) {
            throw "GetWindowRect failed for VENPOD window."
        }
        $width = [Math]::Max(1, $rect.Right - $rect.Left)
        $height = [Math]::Max(1, $rect.Bottom - $rect.Top)
        $bitmap = New-Object System.Drawing.Bitmap($width, $height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $destDc = $graphics.GetHdc()
        $srcDc = [VenpodCaptureNative]::GetWindowDC($hwnd)
        try {
            [VenpodCaptureNative]::BitBlt($destDc, 0, 0, $width, $height, $srcDc, 0, 0, [VenpodCaptureNative]::SRCCOPY) | Out-Null
        } finally {
            if ($srcDc -ne [IntPtr]::Zero) { [VenpodCaptureNative]::ReleaseDC($hwnd, $srcDc) | Out-Null }
            $graphics.ReleaseHdc($destDc)
            $graphics.Dispose()
        }

        $path = Join-Path $OutputDir ("frame_{0:D3}.png" -f $i)
        $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
        $frames.Add($path)

        $sampled = 0
        $skyLike = 0
        $dark = 0
        $terrainLike = 0
        $colors = New-Object 'System.Collections.Generic.HashSet[string]'
        $stepX = [Math]::Max(1, [Math]::Floor($width / 96))
        $stepY = [Math]::Max(1, [Math]::Floor($height / 54))
        for ($y = 0; $y -lt $height; $y += $stepY) {
            for ($x = 0; $x -lt $width; $x += $stepX) {
                $c = $bitmap.GetPixel($x, $y)
                ++$sampled
                $colors.Add("$($c.R),$($c.G),$($c.B)") | Out-Null
                if ($c.R -ge 120 -and $c.R -le 190 -and $c.G -ge 155 -and $c.G -le 220 -and $c.B -ge 190) {
                    ++$skyLike
                }
                if (($c.R + $c.G + $c.B) -lt 90) {
                    ++$dark
                }
                if (($c.R + $c.G + $c.B) -ge 90 -and -not ($c.R -ge 120 -and $c.R -le 190 -and $c.G -ge 155 -and $c.G -le 220 -and $c.B -ge 190)) {
                    ++$terrainLike
                }
            }
        }
        $skyPct = [Math]::Round(($skyLike * 100.0) / [Math]::Max(1, $sampled), 2)
        $darkPct = [Math]::Round(($dark * 100.0) / [Math]::Max(1, $sampled), 2)
        $terrainPct = [Math]::Round(($terrainLike * 100.0) / [Math]::Max(1, $sampled), 2)
        $stats.Add(("{0},{1},{2},{3},{4},{5},{6},{7}" -f (Split-Path $path -Leaf), $width, $height, $sampled, $skyPct, $darkPct, $terrainPct, $colors.Count))
        if ($darkPct -ge 90.0 -and $colors.Count -le 4) {
            ++$blackCaptureFrames
        }
        $bitmap.Dispose()

        Start-Sleep -Milliseconds $CaptureIntervalMs
    }

    if ($frames.Count -eq 0) {
        throw "No frames were captured."
    }

    $thumbW = 320
    $thumbH = 180
    $columns = [Math]::Min(4, [Math]::Max(1, $frames.Count))
    $rows = [Math]::Ceiling($frames.Count / [double]$columns)
    $sheet = New-Object System.Drawing.Bitmap($($thumbW * $columns), $($thumbH * $rows), [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($sheet)
    $g.Clear([System.Drawing.Color]::Black)
    for ($i = 0; $i -lt $frames.Count; ++$i) {
        $img = [System.Drawing.Image]::FromFile($frames[$i])
        $x = ($i % $columns) * $thumbW
        $y = [Math]::Floor($i / $columns) * $thumbH
        $g.DrawImage($img, $x, $y, $thumbW, $thumbH)
        $img.Dispose()
    }
    $g.Dispose()
    $contactSheet = Join-Path $OutputDir "contact_sheet.png"
    $sheet.Save($contactSheet, [System.Drawing.Imaging.ImageFormat]::Png)
    $sheet.Dispose()

    $statsPath = Join-Path $OutputDir "image_stats.csv"
    $stats | Set-Content -Path $statsPath -Encoding ASCII

    $runtimeLog = Join-Path $exeDir "venpod_runtime.log"
    if (Test-Path $runtimeLog) {
        Copy-Item -Path $runtimeLog -Destination (Join-Path $OutputDir "venpod_runtime.log") -Force
    }

    if (-not $process.HasExited) {
        $process.WaitForExit(10000) | Out-Null
    }
    if (-not $process.HasExited) {
        Write-Warn "VENPOD did not exit after captures; stopping process."
        $process.Kill()
        $process.WaitForExit()
        exit 12
    }
    Stop-OnFailure -Code $process.ExitCode -Stage "VENPOD visual capture run"
    if ($blackCaptureFrames -eq $frames.Count) {
        Write-Host "[ERROR] Window capture produced only black/near-black frames." -ForegroundColor Red
        Write-Host "        This usually means GDI BitBlt cannot read the DX12 flip-model window surface." -ForegroundColor Red
        Write-Host "        Use engine_capture_smoke.ps1 for verified renderer output until a Windows.Graphics.Capture path is added." -ForegroundColor Red
        exit 13
    }

    Write-Success "Visual capture smoke passed."
    Write-Info "Contact sheet: $contactSheet"
    Write-Info "Stats: $statsPath"
    exit 0
} finally {
    Restore-Env
}
