# =============================================================================
# VENPOD - Public Demo Capture
# Produces a validated sparse-engine contact sheet and MP4 from in-engine DX12
# backbuffer readbacks. Artifacts are written under build/captures by default.
# =============================================================================

param(
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$NoBuild,
    [string]$OutputDir = "",
    [int]$CaptureStartFrame = 120,
    [int]$CaptureFrames = 90,
    [int]$PlaybackFps = 30,
    [switch]$StressCamera,
    [switch]$BoundaryTest,
    [switch]$ShowDiagnostics,
    [switch]$DisablePhysics,
    [switch]$SkipVideo,
    [switch]$ReviewReel
)

$ErrorActionPreference = "Stop"

function Write-Step { Write-Host "`n==> $args" -ForegroundColor Cyan }
function Write-Success { Write-Host "[OK] $args" -ForegroundColor Green }
function Write-Info { Write-Host "  $args" -ForegroundColor Gray }
function Stop-OnFailure {
    param([int]$Code, [string]$Stage)
    if ($Code -ne 0) {
        Write-Host "[ERROR] $Stage failed with exit code $Code" -ForegroundColor Red
        exit $Code
    }
}

function Assert-CaptureParameters {
    param(
        [int]$CaptureStartFrame,
        [int]$CaptureFrames,
        [int]$PlaybackFps
    )

    if ($CaptureStartFrame -lt 0) {
        throw "CaptureStartFrame must be >= 0, got $CaptureStartFrame"
    }
    if ($CaptureFrames -lt 1) {
        throw "CaptureFrames must be >= 1, got $CaptureFrames"
    }
    if ($PlaybackFps -lt 1) {
        throw "PlaybackFps must be >= 1, got $PlaybackFps"
    }
    # Ownership and capture readbacks retire a few frames after the shader
    # writes them. Keep a tail after the final requested frame so short public
    # captures still validate the ready-frame ownership sample instead of
    # exiting with a false "no post-ready sample" failure.
    $exitAfterFrames64 = [int64]$CaptureStartFrame + [int64]$CaptureFrames + 16L
    if ($exitAfterFrames64 -gt [int64][int]::MaxValue) {
        throw "CaptureStartFrame + CaptureFrames is too large to compute ExitAfterFrames safely"
    }
    return [int]$exitAfterFrames64
}

function Assert-SafeCaptureOutputDir {
    param(
        [string]$ResolvedOutputDir,
        [string]$ProjectRoot,
        [string]$BuildDir
    )

    function Normalize-GuardPath {
        param([string]$Path)
        return [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    }
    function Test-IsPathUnder {
        param([string]$Path, [string]$Parent)
        $normalizedPath = Normalize-GuardPath $Path
        $normalizedParent = Normalize-GuardPath $Parent
        return $normalizedPath.StartsWith($normalizedParent + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)
    }

    $forbidden = @(
        (Normalize-GuardPath $ProjectRoot),
        (Normalize-GuardPath $BuildDir),
        (Normalize-GuardPath (Get-Location).ProviderPath)
    )
    $projectParent = [System.IO.Directory]::GetParent((Normalize-GuardPath $ProjectRoot))
    if ($projectParent) {
        $forbidden += Normalize-GuardPath $projectParent.FullName
    }
    foreach ($relative in @("captures", "logs", "bin")) {
        $forbidden += Normalize-GuardPath (Join-Path $BuildDir $relative)
    }
    $callerBuildDir = Join-Path (Get-Location).ProviderPath "build"
    $forbidden += Normalize-GuardPath $callerBuildDir
    foreach ($relative in @("captures", "logs", "bin")) {
        $forbidden += Normalize-GuardPath (Join-Path $callerBuildDir $relative)
    }

    $normalizedOutputDir = Normalize-GuardPath $ResolvedOutputDir
    $root = [System.IO.Path]::GetPathRoot($normalizedOutputDir)
    if ($normalizedOutputDir -ieq $root.TrimEnd('\')) {
        throw "Refusing to use filesystem root as public demo output directory: $ResolvedOutputDir"
    }
    $allowedParents = @(
        (Join-Path $BuildDir "captures"),
        (Join-Path $BuildDir "logs")
    )
    $insideAllowedTree = $false
    foreach ($path in $allowedParents) {
        if (Test-IsPathUnder $normalizedOutputDir $path) {
            $insideAllowedTree = $true
            break
        }
    }
    if (-not $insideAllowedTree) {
        throw "Refusing to use public demo output outside build/captures or build/logs: $ResolvedOutputDir"
    }
    foreach ($path in @((Join-Path $BuildDir "bin"), (Join-Path $callerBuildDir "bin"))) {
        if (Test-IsPathUnder $normalizedOutputDir $path) {
            throw "Refusing to use runtime binary tree as public demo output directory: $ResolvedOutputDir"
        }
    }
    foreach ($path in $forbidden) {
        if ($normalizedOutputDir -ieq $path) {
            throw "Refusing to clean broad public demo output directory: $ResolvedOutputDir"
        }
    }
}

$projectRoot = $PSScriptRoot
$engineCaptureScript = Join-Path $projectRoot "engine_capture_smoke.ps1"
$buildDir = Join-Path $projectRoot "build"

if (-not (Test-Path $engineCaptureScript)) {
    throw "engine_capture_smoke.ps1 not found at $engineCaptureScript"
}

$exitAfterFrames = Assert-CaptureParameters `
    -CaptureStartFrame $CaptureStartFrame `
    -CaptureFrames $CaptureFrames `
    -PlaybackFps $PlaybackFps

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $buildDir "captures\public_demo"
} elseif (-not [System.IO.Path]::IsPathRooted($OutputDir)) {
    $OutputDir = Join-Path $projectRoot $OutputDir
}
Assert-SafeCaptureOutputDir -ResolvedOutputDir $OutputDir -ProjectRoot $projectRoot -BuildDir $buildDir
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).ProviderPath

Get-ChildItem -LiteralPath $OutputDir -Filter "engine_frame_*.bmp" -File |
    Remove-Item -Force
foreach ($artifact in @("contact_sheet.png", "image_stats.csv", "venpod_runtime.log", "sparse-public-demo.mp4")) {
    $path = Join-Path $OutputDir $artifact
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Force
    }
}

Write-Host "VENPOD - Public Demo Capture" -ForegroundColor Magenta
Write-Info "Output: $OutputDir"
Write-Info "Capture: start=$CaptureStartFrame frames=$CaptureFrames fps=$PlaybackFps exit=$exitAfterFrames"

if ($ReviewReel) {
    if ($BoundaryTest -or $StressCamera -or $ShowDiagnostics) {
        throw "-ReviewReel owns the scenario camera setup; do not combine it with -BoundaryTest, -StressCamera, or -ShowDiagnostics"
    }

    $segments = @(
        [pscustomobject]@{
            Name = "normal"
            StartFrame = 120
            Args = @("-MaxHeightProxyPct", "65")
        },
        [pscustomobject]@{
            Name = "high-flight"
            StartFrame = 240
            Args = @(
                "-StressCamera",
                "-StressCameraRadius", "1400",
                "-StressCameraHeight", "260",
                "-StressCameraBaseHeight", "620",
                "-StressCameraSpeed", "160",
                "-MinUniqueSampleColors", "50",
                "-MinFarSvoPct", "35",
                "-MaxHeightProxyPct", "60"
            )
        },
        [pscustomobject]@{
            Name = "waterline"
            StartFrame = 200
            Args = @(
                "-StressCamera",
                "-WaterlineCamera",
                "-StressCameraRadius", "28",
                "-StressCameraHeight", "6",
                "-StressCameraBaseHeight", "-22",
                "-StressCameraSpeed", "36",
                "-MinUniqueSampleColors", "50",
                "-MaxHeightProxyPct", "25"
            )
        }
    )

    $combinedDir = Join-Path $OutputDir "review_reel_frames"
    if (Test-Path -LiteralPath $combinedDir) {
        Remove-Item -LiteralPath $combinedDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $combinedDir -Force | Out-Null

    $manifestRows = New-Object System.Collections.Generic.List[string]
    $manifestRows.Add("segment,startFrame,captureFrames,directory,contactSheet,imageStats,runtimeLog") | Out-Null
    $globalFrame = 0
    $segmentIndex = 0

    foreach ($segment in $segments) {
        $segmentDir = Join-Path $OutputDir $segment.Name
        if (Test-Path -LiteralPath $segmentDir) {
            Remove-Item -LiteralPath $segmentDir -Recurse -Force
        }
        New-Item -ItemType Directory -Path $segmentDir -Force | Out-Null

        $segmentExitAfter = Assert-CaptureParameters `
            -CaptureStartFrame $segment.StartFrame `
            -CaptureFrames $CaptureFrames `
            -PlaybackFps $PlaybackFps

        $segmentArgs = @(
            "-ExecutionPolicy", "Bypass",
            "-File", $engineCaptureScript,
            "-Config", $Config,
            "-OutputDir", $segmentDir,
            "-CaptureStartFrame", "$($segment.StartFrame)",
            "-CaptureIntervalFrames", "1",
            "-CaptureCount", "$CaptureFrames",
            "-ExitAfterFrames", "$segmentExitAfter"
        ) + $segment.Args
        if ($Clean -and $segmentIndex -eq 0) { $segmentArgs += "-Clean" }
        if ($NoBuild -or $segmentIndex -gt 0) { $segmentArgs += "-NoBuild" }
        if (-not $DisablePhysics) { $segmentArgs += "-SparsePhysics" }

        Write-Step "Capturing review reel segment '$($segment.Name)'..."
        & powershell @segmentArgs
        Stop-OnFailure -Code $LASTEXITCODE -Stage "Review reel segment $($segment.Name)"

        $frames = Get-ChildItem -LiteralPath $segmentDir -Filter "engine_frame_*.bmp" -File | Sort-Object Name
        if ($frames.Count -lt $CaptureFrames) {
            Write-Host "[ERROR] Review reel segment '$($segment.Name)' expected $CaptureFrames frames but found $($frames.Count)" -ForegroundColor Red
            exit 24
        }

        foreach ($frame in ($frames | Select-Object -First $CaptureFrames)) {
            $target = Join-Path $combinedDir ("engine_frame_{0:D4}.bmp" -f $globalFrame)
            Copy-Item -LiteralPath $frame.FullName -Destination $target -Force
            ++$globalFrame
        }

        $manifestRows.Add(('{0},{1},{2},"{3}","{4}","{5}","{6}"' -f `
            $segment.Name,
            $segment.StartFrame,
            $CaptureFrames,
            $segmentDir,
            (Join-Path $segmentDir "contact_sheet.png"),
            (Join-Path $segmentDir "image_stats.csv"),
            (Join-Path $segmentDir "venpod_runtime.log"))) | Out-Null
        ++$segmentIndex
    }

    $manifestPath = Join-Path $OutputDir "review_reel_manifest.csv"
    $manifestRows | Set-Content -LiteralPath $manifestPath -Encoding UTF8

    $reviewNotesPath = Join-Path $OutputDir "PUBLIC_DEMO_REVIEW_REEL.md"
    @(
        "# Public Demo Review Reel",
        "",
        "This review reel is generated by `public_demo_capture.ps1 -ReviewReel`.",
        "",
        "Segments:",
        "",
        "- normal: default public camera with sparse physics enabled unless `-DisablePhysics` is passed.",
        "- high-flight: fast stress-camera route with `MinFarSvoPct>=35` and current proxy ceiling.",
        "- waterline: submerged/waterline route with current water proxy ceiling.",
        "",
        "Passing generation means each segment passed the capture smoke gates. It does not by itself mean final visual acceptance.",
        "",
        "Review the per-segment contact sheets and logs listed in `review_reel_manifest.csv`."
    ) | Set-Content -LiteralPath $reviewNotesPath -Encoding UTF8

    if (-not $SkipVideo) {
        $ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
        if (-not $ffmpeg) {
            Write-Host "[ERROR] ffmpeg was not found on PATH. Re-run with -SkipVideo for contact-sheet only." -ForegroundColor Red
            exit 21
        }

        $videoPath = Join-Path $OutputDir "sparse-public-review-reel.mp4"
        $inputPattern = Join-Path $combinedDir "engine_frame_%04d.bmp"
        Write-Step "Encoding public review reel MP4..."
        & ffmpeg `
            -y `
            -hide_banner `
            -loglevel error `
            -framerate $PlaybackFps `
            -start_number 0 `
            -i $inputPattern `
            -vf "scale=1280:-2,format=yuv420p" `
            -c:v libx264 `
            -preset veryfast `
            -crf 20 `
            -movflags +faststart `
            $videoPath
        Stop-OnFailure -Code $LASTEXITCODE -Stage "Review reel MP4 encode"
        if (-not (Test-Path -LiteralPath $videoPath) -or (Get-Item -LiteralPath $videoPath).Length -le 0) {
            Write-Host "[ERROR] Review reel MP4 was not created at $videoPath" -ForegroundColor Red
            exit 22
        }
        $ffprobe = Get-Command ffprobe -ErrorAction SilentlyContinue
        if ($ffprobe) {
            $probeLines = & ffprobe `
                -v error `
                -select_streams v:0 `
                -show_entries stream=width,height,nb_frames `
                -of default=noprint_wrappers=1:nokey=0 `
                $videoPath
            Stop-OnFailure -Code $LASTEXITCODE -Stage "Review reel MP4 probe"
            $probe = @{}
            foreach ($line in $probeLines) {
                $parts = $line -split "=", 2
                if ($parts.Count -eq 2) {
                    $probe[$parts[0]] = $parts[1]
                }
            }
            $width = [int]($probe["width"])
            $height = [int]($probe["height"])
            $nbFramesRaw = $probe["nb_frames"]
            $encodedFrames = 0
            if ($nbFramesRaw -and $nbFramesRaw -ne "N/A") {
                $encodedFrames = [int]$nbFramesRaw
            }
            if ($width -le 0 -or $height -le 0) {
                Write-Host "[ERROR] Review reel MP4 probe reported invalid dimensions ${width}x${height}" -ForegroundColor Red
                exit 23
            }
            $expectedFrames = $CaptureFrames * $segments.Count
            if ($encodedFrames -gt 0 -and $encodedFrames -lt $expectedFrames) {
                Write-Host "[ERROR] Review reel MP4 probe reported too few frames: $encodedFrames expected at least $expectedFrames" -ForegroundColor Red
                exit 23
            }
            $encodedFrameSummary = "unknown"
            if ($encodedFrames -gt 0) {
                $encodedFrameSummary = "$encodedFrames"
            }
            Write-Info "Review reel video stream verified: ${width}x${height} frames=$encodedFrameSummary"
        }
        Write-Info "Review reel video: $videoPath"
    }

    Write-Info "Review reel frames: $combinedDir"
    Write-Info "Review reel manifest: $manifestPath"
    Write-Info "Review reel notes: $reviewNotesPath"
    Write-Success "Public demo review reel artifacts are ready."
    return
}

$captureArgs = @(
    "-ExecutionPolicy", "Bypass",
    "-File", $engineCaptureScript,
    "-Config", $Config,
    "-OutputDir", $OutputDir,
    "-CaptureStartFrame", "$CaptureStartFrame",
    "-CaptureIntervalFrames", "1",
    "-CaptureCount", "$CaptureFrames",
    "-ExitAfterFrames", "$exitAfterFrames"
)
if ($Clean) { $captureArgs += "-Clean" }
if ($NoBuild) { $captureArgs += "-NoBuild" }
if ($StressCamera) { $captureArgs += "-StressCamera" }
if ($BoundaryTest) { $captureArgs += "-BoundaryTest" }
if ($ShowDiagnostics) { $captureArgs += "-ShowDiagnostics" }
if (-not $DisablePhysics) { $captureArgs += "-SparsePhysics" }

Write-Step "Capturing validated sparse backbuffer frames..."
& powershell @captureArgs
Stop-OnFailure -Code $LASTEXITCODE -Stage "Engine backbuffer capture"

$frames = Get-ChildItem -LiteralPath $OutputDir -Filter "engine_frame_*.bmp" -File | Sort-Object Name
if ($frames.Count -lt $CaptureFrames) {
    Write-Host "[ERROR] Expected $CaptureFrames frames but found $($frames.Count)" -ForegroundColor Red
    exit 20
}

if (-not $SkipVideo) {
    $ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if (-not $ffmpeg) {
        Write-Host "[ERROR] ffmpeg was not found on PATH. Re-run with -SkipVideo for contact-sheet only." -ForegroundColor Red
        exit 21
    }

    $videoPath = Join-Path $OutputDir "sparse-public-demo.mp4"
    $inputPattern = Join-Path $OutputDir "engine_frame_%04d.bmp"
    Write-Step "Encoding public demo MP4..."
    & ffmpeg `
        -y `
        -hide_banner `
        -loglevel error `
        -framerate $PlaybackFps `
        -start_number $CaptureStartFrame `
        -i $inputPattern `
        -vf "scale=1280:-2,format=yuv420p" `
        -c:v libx264 `
        -preset veryfast `
        -crf 20 `
        -movflags +faststart `
        $videoPath
    Stop-OnFailure -Code $LASTEXITCODE -Stage "MP4 encode"
    if (-not (Test-Path -LiteralPath $videoPath) -or (Get-Item -LiteralPath $videoPath).Length -le 0) {
        Write-Host "[ERROR] MP4 was not created at $videoPath" -ForegroundColor Red
        exit 22
    }
    $ffprobe = Get-Command ffprobe -ErrorAction SilentlyContinue
    if ($ffprobe) {
        $probeLines = & ffprobe `
            -v error `
            -select_streams v:0 `
            -show_entries stream=width,height,nb_frames `
            -of default=noprint_wrappers=1:nokey=0 `
            $videoPath
        Stop-OnFailure -Code $LASTEXITCODE -Stage "MP4 probe"
        $probe = @{}
        foreach ($line in $probeLines) {
            $parts = $line -split "=", 2
            if ($parts.Count -eq 2) {
                $probe[$parts[0]] = $parts[1]
            }
        }
        $width = [int]($probe["width"])
        $height = [int]($probe["height"])
        $nbFramesRaw = $probe["nb_frames"]
        $encodedFrames = 0
        if ($nbFramesRaw -and $nbFramesRaw -ne "N/A") {
            $encodedFrames = [int]$nbFramesRaw
        }
        if ($width -le 0 -or $height -le 0) {
            Write-Host "[ERROR] MP4 probe reported invalid dimensions ${width}x${height}" -ForegroundColor Red
            exit 23
        }
        if ($encodedFrames -gt 0 -and $encodedFrames -lt $CaptureFrames) {
            Write-Host "[ERROR] MP4 probe reported too few frames: $encodedFrames expected at least $CaptureFrames" -ForegroundColor Red
            exit 23
        }
        $encodedFrameSummary = "unknown"
        if ($encodedFrames -gt 0) {
            $encodedFrameSummary = "$encodedFrames"
        }
        Write-Info "Video stream verified: ${width}x${height} frames=$encodedFrameSummary"
    } else {
        & ffmpeg `
            -v error `
            -i $videoPath `
            -f null `
            NUL
        Stop-OnFailure -Code $LASTEXITCODE -Stage "MP4 decode verification"
        Write-Info "Video decode verification passed"
    }
    Write-Info "Video: $videoPath"
}

Write-Info "Contact sheet: $(Join-Path $OutputDir 'contact_sheet.png')"
Write-Info "Image stats: $(Join-Path $OutputDir 'image_stats.csv')"
Write-Info "Runtime log: $(Join-Path $OutputDir 'venpod_runtime.log')"
Write-Success "Public demo capture artifacts are ready."
