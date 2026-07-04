param(
    [ValidateSet("idle", "walk", "yaw", "edit")]
    [string]$Scenario = "idle",
    [string]$OutputDir = "build\captures\stabilize_quality",
    [int]$Frames = 900,
    [string]$Label = "",
    [int]$SummaryLogInterval = 30,
    [double]$RenderScale = 0,
    [double]$ExperimentalBackgroundPassScale = 0,
    [int]$FrameLatencyWaitable = -1,
    [int]$OwnershipInterval = -1,
    [int]$CaptureStartFrame = -1,
    [int]$CaptureIntervalFrames = 30,
    [int]$CaptureCount = 0,
    [switch]$AbsoluteWalkFrame,
    [switch]$ExperimentalBackgroundPassNoSurfaceFill,
    [switch]$ExperimentalBackgroundPassForegroundMask,
    [int]$ExperimentalPrePublishGeneralSurfaceBudget = -1,
    [int]$UploadBudget = -1,
    [int]$MidPumpBudget = -1,
    [int]$CleanCatchupBudget = -1,
    [switch]$DisableMidFeedback,
    [switch]$RestTrimOld,
    [switch]$ViewFollowTrimOff,
    [switch]$ViewFollowTrimWhileStationary,
    [int]$ExperimentalPrePublishEditGeneralSurfaceBudget = -1,
    [int]$ExperimentalPrePublishPostEditGeneralSurfaceBudget = -1,
    [int]$ExperimentalPrePublishPostEditGeneralSpillFrames = -1,
    [double]$ExperimentalPrePublishPostEditGeneralSpillPressureMs = -1,
    [int]$ExperimentalHiddenExactPostOpenProbeMaxMsTenths = -1,
    [int]$ExperimentalHiddenExactPostOpenGenerationBudget = -1,
    [int]$ExperimentalHiddenExactPostOpenSurfaceBudget = -1,
    [int]$ExperimentalPrePublishHiddenTrackedSurfaceBudget = -1,
    [switch]$ExperimentalSurfaceAsyncPerCoordEditGate,
    [switch]$ExperimentalSparseRequestExplicitSourceLanes,
    [switch]$ExperimentalHiddenExactPostOpenRepairLane,
    [switch]$ExperimentalHiddenExactDeferProactive,
    [int]$ExperimentalHiddenExactPostOpenRepairLaneMaxRequests = -1,
    [switch]$ClipInterestProfile,
    [switch]$ClipInterestDetail,
    [int]$ExperimentalVoxelInterestRebuildRingsPerFrame = -1,
    [double]$ExperimentalMidClipmapPumpHardBudgetMs = -1,
    [int]$ExperimentalSurfaceRouteGeneralAsync = -1,
    [int]$ExperimentalSurfaceRouteAsyncBacklogLimit = -1,
    [int]$ExperimentalSurfaceRoutePublishPendingLimit = -1,
    [int]$ExperimentalSurfaceRoutePublishAgeLimit = -1,
    [int]$ExperimentalSurfaceRoutePagePublishLimit = -1,
    [switch]$BrushSmokeErase,
    [switch]$BrushSmokeUserPath,
    [int]$BrushSmokeCase = -1,
    [switch]$BrushSmokeRealAim,
    [int]$BrushSmokeRadiusTenths = -1,
    [int]$BrushSmokeEndFrame = -1,
    [int]$HiddenExactProbeInterval = -1,
    [int]$BrushApplyCadence = -1,
    [int]$BrushStampSpacingLargePct = -1,
    [int]$WalkSpeed = -1,
    [int]$WalkYawDegPerSec = -1,
    [int]$EditTelemetryInterval = -1,
    [switch]$HeightSerialTrace,
    [switch]$BgTemporal,
    [switch]$WalkFly,
    [int]$WalkEyeOffsetY = 0,
    [int]$WalkPitchDeg = -1000,
    [int]$SparseDebugMode = -1,
    [int]$TimeoutSeconds = 900
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path $PSScriptRoot -Parent
$captureScript = Join-Path $PSScriptRoot "stabilize_quality_capture.ps1"
if (-not $Label) {
    $Label = $Scenario
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$statusPath = Join-Path $OutputDir "$Label.status.txt"
Remove-Item -LiteralPath $statusPath -ErrorAction SilentlyContinue

$taskName = "VENPOD_Codex_${Scenario}_$([guid]::NewGuid().ToString('N').Substring(0, 8))"
$powerShellExe = Join-Path $PSHOME "powershell.exe"
$taskArgs = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$captureScript`"",
    "-Scenario", $Scenario,
    "-OutputDir", "`"$OutputDir`"",
    "-Frames", [string]$Frames,
    "-Label", $Label,
    "-SummaryLogInterval", [string]$SummaryLogInterval
) -join " "
if ($RenderScale -gt 0) {
    $taskArgs = "$taskArgs -RenderScale $RenderScale"
}
if ($ExperimentalBackgroundPassScale -gt 0) {
    $taskArgs = "$taskArgs -ExperimentalBackgroundPassScale $ExperimentalBackgroundPassScale"
    if ($ExperimentalBackgroundPassNoSurfaceFill) {
        $taskArgs = "$taskArgs -ExperimentalBackgroundPassNoSurfaceFill"
    }
    if ($ExperimentalBackgroundPassForegroundMask) {
        $taskArgs = "$taskArgs -ExperimentalBackgroundPassForegroundMask"
    }
}
if ($ExperimentalPrePublishGeneralSurfaceBudget -ge 0) {
    $taskArgs = "$taskArgs -ExperimentalPrePublishGeneralSurfaceBudget $ExperimentalPrePublishGeneralSurfaceBudget"
}
if ($UploadBudget -ge 0) {
    $taskArgs = "$taskArgs -UploadBudget $UploadBudget"
}
if ($MidPumpBudget -ge 0) {
    $taskArgs = "$taskArgs -MidPumpBudget $MidPumpBudget"
}
if ($CleanCatchupBudget -ge 0) {
    $taskArgs = "$taskArgs -CleanCatchupBudget $CleanCatchupBudget"
}
if ($DisableMidFeedback) {
    $taskArgs = "$taskArgs -DisableMidFeedback"
}
if ($RestTrimOld) {
    $taskArgs = "$taskArgs -RestTrimOld"
}
if ($ViewFollowTrimOff) {
    $taskArgs = "$taskArgs -ViewFollowTrimOff"
}
if ($ViewFollowTrimWhileStationary) {
    $taskArgs = "$taskArgs -ViewFollowTrimWhileStationary"
}
if ($ExperimentalPrePublishEditGeneralSurfaceBudget -ge 0) {
    $taskArgs = "$taskArgs -ExperimentalPrePublishEditGeneralSurfaceBudget $ExperimentalPrePublishEditGeneralSurfaceBudget"
}
if ($ExperimentalPrePublishPostEditGeneralSurfaceBudget -ge 0) {
    $taskArgs = "$taskArgs -ExperimentalPrePublishPostEditGeneralSurfaceBudget $ExperimentalPrePublishPostEditGeneralSurfaceBudget"
}
if ($ExperimentalPrePublishPostEditGeneralSpillFrames -ge 0) {
    $taskArgs = "$taskArgs -ExperimentalPrePublishPostEditGeneralSpillFrames $ExperimentalPrePublishPostEditGeneralSpillFrames"
}
if ($ExperimentalPrePublishPostEditGeneralSpillPressureMs -ge 0) {
    $taskArgs = "$taskArgs -ExperimentalPrePublishPostEditGeneralSpillPressureMs $ExperimentalPrePublishPostEditGeneralSpillPressureMs"
}
if ($ExperimentalHiddenExactPostOpenProbeMaxMsTenths -ge 0) {
    $taskArgs = "$taskArgs -ExperimentalHiddenExactPostOpenProbeMaxMsTenths $ExperimentalHiddenExactPostOpenProbeMaxMsTenths"
}
if ($ExperimentalHiddenExactPostOpenGenerationBudget -ge 0) {
    $taskArgs = "$taskArgs -ExperimentalHiddenExactPostOpenGenerationBudget $ExperimentalHiddenExactPostOpenGenerationBudget"
}
if ($ExperimentalHiddenExactPostOpenSurfaceBudget -ge 0) {
    $taskArgs = "$taskArgs -ExperimentalHiddenExactPostOpenSurfaceBudget $ExperimentalHiddenExactPostOpenSurfaceBudget"
}
if ($ExperimentalPrePublishHiddenTrackedSurfaceBudget -ge 0) {
    $taskArgs = "$taskArgs -ExperimentalPrePublishHiddenTrackedSurfaceBudget $ExperimentalPrePublishHiddenTrackedSurfaceBudget"
}
if ($ExperimentalSurfaceAsyncPerCoordEditGate) {
    $taskArgs = "$taskArgs -ExperimentalSurfaceAsyncPerCoordEditGate"
}
if ($ExperimentalSparseRequestExplicitSourceLanes) {
    $taskArgs = "$taskArgs -ExperimentalSparseRequestExplicitSourceLanes"
}
if ($ExperimentalHiddenExactPostOpenRepairLane) {
    $taskArgs = "$taskArgs -ExperimentalHiddenExactPostOpenRepairLane"
}
if ($ExperimentalHiddenExactDeferProactive) {
    $taskArgs = "$taskArgs -ExperimentalHiddenExactDeferProactive"
}
if ($ExperimentalHiddenExactPostOpenRepairLaneMaxRequests -ge 0) {
    $taskArgs = "$taskArgs -ExperimentalHiddenExactPostOpenRepairLaneMaxRequests $ExperimentalHiddenExactPostOpenRepairLaneMaxRequests"
}
if ($ClipInterestProfile) {
    $taskArgs = "$taskArgs -ClipInterestProfile"
}
if ($ClipInterestDetail) {
    $taskArgs = "$taskArgs -ClipInterestDetail"
}
if ($ExperimentalVoxelInterestRebuildRingsPerFrame -ge 0) {
    $taskArgs = "$taskArgs -ExperimentalVoxelInterestRebuildRingsPerFrame $ExperimentalVoxelInterestRebuildRingsPerFrame"
}
if ($ExperimentalMidClipmapPumpHardBudgetMs -ge 0) {
    $taskArgs = "$taskArgs -ExperimentalMidClipmapPumpHardBudgetMs $ExperimentalMidClipmapPumpHardBudgetMs"
}
if ($ExperimentalSurfaceRouteGeneralAsync -ge 0) {
    $taskArgs = "$taskArgs -ExperimentalSurfaceRouteGeneralAsync $ExperimentalSurfaceRouteGeneralAsync"
}
if ($ExperimentalSurfaceRouteAsyncBacklogLimit -ge 0) {
    $taskArgs = "$taskArgs -ExperimentalSurfaceRouteAsyncBacklogLimit $ExperimentalSurfaceRouteAsyncBacklogLimit"
}
if ($ExperimentalSurfaceRoutePublishPendingLimit -ge 0) {
    $taskArgs = "$taskArgs -ExperimentalSurfaceRoutePublishPendingLimit $ExperimentalSurfaceRoutePublishPendingLimit"
}
if ($ExperimentalSurfaceRoutePublishAgeLimit -ge 0) {
    $taskArgs = "$taskArgs -ExperimentalSurfaceRoutePublishAgeLimit $ExperimentalSurfaceRoutePublishAgeLimit"
}
if ($ExperimentalSurfaceRoutePagePublishLimit -ge 0) {
    $taskArgs = "$taskArgs -ExperimentalSurfaceRoutePagePublishLimit $ExperimentalSurfaceRoutePagePublishLimit"
}
if ($BrushSmokeErase) {
    $taskArgs = "$taskArgs -BrushSmokeErase"
}
if ($BrushSmokeUserPath) {
    $taskArgs = "$taskArgs -BrushSmokeUserPath"
}
if ($BrushSmokeCase -ge 0) {
    $taskArgs = "$taskArgs -BrushSmokeCase $BrushSmokeCase"
}
if ($BrushSmokeRealAim) {
    $taskArgs = "$taskArgs -BrushSmokeRealAim"
}
if ($BrushSmokeRadiusTenths -ge 0) {
    $taskArgs = "$taskArgs -BrushSmokeRadiusTenths $BrushSmokeRadiusTenths"
}
if ($BrushSmokeEndFrame -ge 0) {
    $taskArgs = "$taskArgs -BrushSmokeEndFrame $BrushSmokeEndFrame"
}
if ($HiddenExactProbeInterval -ge 1) {
    $taskArgs = "$taskArgs -HiddenExactProbeInterval $HiddenExactProbeInterval"
}
if ($BrushApplyCadence -ge 1) {
    $taskArgs = "$taskArgs -BrushApplyCadence $BrushApplyCadence"
}
if ($BrushStampSpacingLargePct -ge 1) {
    $taskArgs = "$taskArgs -BrushStampSpacingLargePct $BrushStampSpacingLargePct"
}
if ($WalkSpeed -ge 0) {
    $taskArgs = "$taskArgs -WalkSpeed $WalkSpeed"
}
if ($WalkYawDegPerSec -ge 0) {
    $taskArgs = "$taskArgs -WalkYawDegPerSec $WalkYawDegPerSec"
}
if ($EditTelemetryInterval -ge 1) {
    $taskArgs = "$taskArgs -EditTelemetryInterval $EditTelemetryInterval"
}
if ($HeightSerialTrace) {
    $taskArgs = "$taskArgs -HeightSerialTrace"
}
if ($BgTemporal) {
    $taskArgs = "$taskArgs -BgTemporal"
}
if ($WalkFly) {
    $taskArgs = "$taskArgs -WalkFly"
}
if ($WalkEyeOffsetY -ne 0) {
    $taskArgs = "$taskArgs -WalkEyeOffsetY $WalkEyeOffsetY"
}
if ($WalkPitchDeg -gt -1000) {
    $taskArgs = "$taskArgs -WalkPitchDeg $WalkPitchDeg"
}
if ($SparseDebugMode -ge 0) {
    $taskArgs = "$taskArgs -SparseDebugMode $SparseDebugMode"
}
if ($FrameLatencyWaitable -ge 0) {
    $taskArgs = "$taskArgs -FrameLatencyWaitable $FrameLatencyWaitable"
}
if ($OwnershipInterval -gt 0) {
    $taskArgs = "$taskArgs -OwnershipInterval $OwnershipInterval"
}
if ($CaptureCount -gt 0) {
    $taskArgs = "$taskArgs -CaptureStartFrame $CaptureStartFrame -CaptureIntervalFrames $CaptureIntervalFrames -CaptureCount $CaptureCount"
}
if ($AbsoluteWalkFrame) {
    $taskArgs = "$taskArgs -AbsoluteWalkFrame"
}

$action = New-ScheduledTaskAction -Execute $powerShellExe -Argument $taskArgs -WorkingDirectory $projectRoot
$trigger = New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(5)
$currentUser = (whoami).Trim()
$principal = New-ScheduledTaskPrincipal -UserId $currentUser -LogonType Interactive -RunLevel Highest

try {
    try {
        Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principal -Force -ErrorAction Stop | Out-Null
    } catch {
        # Registering a Highest-runlevel task requires an elevated shell. The engine
        # capture does not need an admin token, so fall back to Limited when the
        # caller is unelevated (identical interactive-session measurement conditions).
        $principal = New-ScheduledTaskPrincipal -UserId $currentUser -LogonType Interactive -RunLevel Limited
        Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principal -Force -ErrorAction Stop | Out-Null
    }
    Start-ScheduledTask -TaskName $taskName -ErrorAction Stop

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $completed = $false
    do {
        Start-Sleep -Seconds 2
        if ((Test-Path $statusPath) -and (Select-String -LiteralPath $statusPath -Pattern "END|ERROR" -Quiet)) {
            $completed = $true
            break
        }
    } while ((Get-Date) -lt $deadline)

    $finalTask = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    $finalInfo = Get-ScheduledTaskInfo -TaskName $taskName -ErrorAction SilentlyContinue
    $statusText = @()
    if (Test-Path $statusPath) {
        $statusText = @(Get-Content -LiteralPath $statusPath)
        $statusText
    } else {
        $finalState = if ($finalTask) { $finalTask.State } else { "Unknown" }
        $lastResult = if ($finalInfo) { $finalInfo.LastTaskResult } else { "unknown" }
        "NO_STATUS_FILE status=$finalState lastResult=$lastResult"
    }

    if (-not $completed) {
        $lastResult = if ($finalInfo) { $finalInfo.LastTaskResult } else { "unknown" }
        Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
        Get-Process VENPOD -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        throw "Interactive capture task $taskName did not complete before timeout or status END/ERROR; lastResult=$lastResult"
    }

    if ((Get-Date) -ge $deadline) {
        throw "Timed out waiting for interactive capture task $taskName"
    }

    $statusJoined = $statusText -join "`n"
    if ($statusJoined -match "ERROR") {
        throw "Interactive capture task $taskName failed: $statusJoined"
    }
    if ($statusJoined -match "END .* exit=(?<exit>-?\d+)") {
        $captureExit = [int]$Matches.exit
        if ($captureExit -ne 0) {
            throw "Interactive capture task $taskName exited with code $captureExit"
        }
    }
} finally {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
}
