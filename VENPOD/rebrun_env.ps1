# =============================================================================
# VENPOD - Clean one-run environment wrapper for rebrun.ps1
# Usage:
#   .\rebrun_env.ps1 -NoBuild -Env NAME=value -Env OTHER=value -SaveLog path
# =============================================================================

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rebrun = Join-Path $scriptDir "rebrun.ps1"

if (-not (Test-Path $rebrun)) {
    throw "rebrun.ps1 not found at $rebrun"
}

$envAssignments = @()
$forwardArgs = @()
$saveLog = ""

for ($i = 0; $i -lt $args.Count; $i++) {
    $arg = [string]$args[$i]

    if ($arg -eq "-Env" -or $arg -eq "-E") {
        if ($i + 1 -ge $args.Count) {
            throw "$arg requires NAME=value"
        }
        $i++
        $envAssignments += [string]$args[$i]
        continue
    }

    if ($arg -eq "-SaveLog") {
        if ($i + 1 -ge $args.Count) {
            throw "$arg requires a destination path"
        }
        $i++
        $saveLog = [string]$args[$i]
        continue
    }

    $forwardArgs += $arg
}

$savedVenpodEnv = @{}
Get-ChildItem Env:VENPOD_* | ForEach-Object {
    $savedVenpodEnv[$_.Name] = $_.Value
}

try {
    Get-ChildItem Env:VENPOD_* | ForEach-Object {
        Remove-Item "Env:$($_.Name)" -ErrorAction SilentlyContinue
    }

    foreach ($assignment in $envAssignments) {
        $eq = $assignment.IndexOf("=")
        if ($eq -le 0) {
            throw "Invalid -Env assignment '$assignment'. Expected NAME=value."
        }

        $name = $assignment.Substring(0, $eq)
        $value = $assignment.Substring($eq + 1)

        if ($name -notmatch "^VENPOD_[A-Za-z0-9_]+$") {
            throw "Refusing to set non-VENPOD env var '$name'."
        }

        Set-Item "Env:$name" $value
    }

    Write-Host "Clean VENPOD env for this run:"
    if ($envAssignments.Count -eq 0) {
        Write-Host "  (no VENPOD_* overrides)"
    } else {
        foreach ($assignment in $envAssignments) {
            Write-Host "  $assignment"
        }
    }

    $powerShellExe = (Get-Process -Id $PID).Path
    if ([string]::IsNullOrWhiteSpace($powerShellExe)) {
        $powerShellExe = "powershell.exe"
    }

    & $powerShellExe -NoProfile -ExecutionPolicy Bypass -File $rebrun @forwardArgs

    if (-not [string]::IsNullOrWhiteSpace($saveLog)) {
        $runtimeLog = Join-Path $scriptDir "build\bin\venpod_runtime.log"
        if (Test-Path $runtimeLog) {
            $saveLogPath = $saveLog
            if (-not [System.IO.Path]::IsPathRooted($saveLogPath)) {
                $saveLogPath = Join-Path $scriptDir $saveLogPath
            }
            $saveLogDir = Split-Path -Parent $saveLogPath
            if (-not [string]::IsNullOrWhiteSpace($saveLogDir)) {
                New-Item -ItemType Directory -Force -Path $saveLogDir | Out-Null
            }
            Copy-Item -LiteralPath $runtimeLog -Destination $saveLogPath -Force
            Write-Host "Saved runtime log: $saveLogPath"
        } else {
            Write-Warning "Runtime log not found: $runtimeLog"
        }
    }
}
finally {
    Get-ChildItem Env:VENPOD_* | ForEach-Object {
        Remove-Item "Env:$($_.Name)" -ErrorAction SilentlyContinue
    }

    foreach ($name in $savedVenpodEnv.Keys) {
        Set-Item "Env:$name" $savedVenpodEnv[$name]
    }
}
