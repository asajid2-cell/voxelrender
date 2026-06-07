param(
    [string]$Config = "Release",
    [switch]$RefreshOnly,
    [switch]$WarmRaymarch,
    [switch]$CaptureFrame300,
    [int]$CaptureFrame = 300,
    [int]$SparseDebugMode = 0,
    [string]$CaptureName = ""
)

$ErrorActionPreference = "Stop"

function Write-Step { Write-Host "`n==> $args" -ForegroundColor Cyan }
function Write-Success { Write-Host "[OK] $args" -ForegroundColor Green }
function Write-Info { Write-Host "  $args" -ForegroundColor Gray }
function Write-Warn { Write-Host "[WARN] $args" -ForegroundColor Yellow }

$projectRoot = $PSScriptRoot
$buildBin = Join-Path $projectRoot "build\bin"
$sourceAssets = Join-Path $projectRoot "assets"
$runtimeAssets = Join-Path $buildBin "assets"
$sourceRaymarch = Join-Path $sourceAssets "shaders\Graphics\PS_Raymarch.hlsl"
$runtimeRaymarch = Join-Path $runtimeAssets "shaders\Graphics\PS_Raymarch.hlsl"
$rebrun = Join-Path $projectRoot "rebrun.ps1"

if (-not (Test-Path $rebrun)) { throw "rebrun.ps1 not found at $rebrun" }
if (-not (Test-Path $buildBin)) { throw "Build bin directory not found. Run build.ps1 first." }
if (-not (Test-Path $sourceRaymarch)) { throw "Source PS_Raymarch.hlsl not found at $sourceRaymarch" }

function Refresh-ShaderAssets {
    Write-Step "Refreshing runtime shader assets..."
    if (-not (Test-Path $runtimeAssets)) {
        New-Item -ItemType Directory -Path $runtimeAssets | Out-Null
    }
    Copy-Item -Path (Join-Path $sourceAssets "*") -Destination $runtimeAssets -Recurse -Force
    Assert-RaymarchParity
    Write-Success "Runtime assets match source"
}

function Assert-RaymarchParity {
    if (-not (Test-Path $runtimeRaymarch)) {
        throw "Runtime PS_Raymarch.hlsl missing at $runtimeRaymarch"
    }
    $sourceHash = (Get-FileHash -Algorithm SHA256 $sourceRaymarch).Hash
    $runtimeHash = (Get-FileHash -Algorithm SHA256 $runtimeRaymarch).Hash
    if ($sourceHash -ne $runtimeHash) {
        throw "Source/runtime PS_Raymarch.hlsl mismatch. Refresh assets before running."
    }
}

function Summarize-RaymarchShaderLog {
    param([string]$LogPath)
    if (-not (Test-Path $LogPath)) {
        Write-Warn "Runtime log not found at $LogPath"
        return
    }
    $text = Get-Content $LogPath -Raw
    if ($text -match "Shader cache miss: compiling PS_Raymarch\.hlsl") {
        $seconds = "unknown"
        if ($text -match "Shader compile complete: PS_Raymarch\.hlsl seconds=([0-9.]+)") {
            $seconds = $Matches[1]
        }
        Write-Info "PS_Raymarch cache: MISS; compile_time_sec=$seconds"
    } elseif ($text -match "Shader cache hit: PS_Raymarch\.hlsl") {
        Write-Info "PS_Raymarch cache: HIT"
    } else {
        Write-Warn "No PS_Raymarch cache hit/miss line found"
    }
}

function Invoke-RaymarchRun {
    param(
        [string]$Name,
        [int]$Frame,
        [int]$ExitAfterFrames
    )
    Assert-RaymarchParity
    $captureDir = Join-Path $projectRoot ("build\captures\{0}" -f $Name)
    New-Item -ItemType Directory -Path $captureDir -Force | Out-Null
    $env:VENPOD_CAPTURE_DIR = $captureDir
    $env:VENPOD_CAPTURE_START_FRAME = "$Frame"
    $env:VENPOD_CAPTURE_INTERVAL_FRAMES = "1"
    $env:VENPOD_CAPTURE_COUNT = "1"
    try {
        & $rebrun -Config $Config -NoBuild -SparseDebugMode $SparseDebugMode -ExitAfterFrames $ExitAfterFrames
        if ($LASTEXITCODE -ne 0) {
            throw "rebrun failed with exit code $LASTEXITCODE"
        }
        $runtimeLog = Join-Path $buildBin "venpod_runtime.log"
        $captureLog = Join-Path $captureDir "venpod_runtime.log"
        if (Test-Path $runtimeLog) {
            Copy-Item $runtimeLog $captureLog -Force
            Summarize-RaymarchShaderLog $captureLog
        }
        $framePath = Join-Path $captureDir ("engine_frame_{0:D4}.bmp" -f $Frame)
        if (Test-Path $framePath) {
            Write-Success "Captured $framePath"
        } else {
            throw "Expected capture was not written: $framePath"
        }
    } finally {
        Remove-Item Env:\VENPOD_CAPTURE_DIR,Env:\VENPOD_CAPTURE_START_FRAME,Env:\VENPOD_CAPTURE_INTERVAL_FRAMES,Env:\VENPOD_CAPTURE_COUNT -ErrorAction SilentlyContinue
    }
}

if (-not $RefreshOnly -and -not $WarmRaymarch -and -not $CaptureFrame300) {
    $WarmRaymarch = $true
    $CaptureFrame300 = $true
}

Refresh-ShaderAssets

if ($RefreshOnly) {
    return
}

if ($WarmRaymarch) {
    Invoke-RaymarchRun -Name "raymarch_shader_warm_$(Get-Date -Format yyyyMMdd_HHmmss)" -Frame 1 -ExitAfterFrames 2
}

if ($CaptureFrame300) {
    $name = $CaptureName
    if ([string]::IsNullOrWhiteSpace($name)) {
        $name = "raymarch_shader_frame$($CaptureFrame)_$(Get-Date -Format yyyyMMdd_HHmmss)"
    }
    Invoke-RaymarchRun -Name $name -Frame $CaptureFrame -ExitAfterFrames ($CaptureFrame + 15)
}
