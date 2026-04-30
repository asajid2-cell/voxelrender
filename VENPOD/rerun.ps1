# Short alias for the rebuild-and-run loop.
param(
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$Diagnostics,
    [switch]$BoundaryTest,
    [switch]$InfinitePhysics,
    [switch]$DisablePhysics,
    [switch]$D3DDebug,
    [switch]$ForceSync
)

$argsForRebrun = @("-Config", $Config)
if ($Clean) { $argsForRebrun += "-Clean" }
if ($Diagnostics) { $argsForRebrun += "-Diagnostics" }
if ($BoundaryTest) { $argsForRebrun += "-BoundaryTest" }
if ($InfinitePhysics) { $argsForRebrun += "-InfinitePhysics" }
if ($DisablePhysics) { $argsForRebrun += "-DisablePhysics" }
if ($D3DDebug) { $argsForRebrun += "-D3DDebug" }
if ($ForceSync) { $argsForRebrun += "-ForceSync" }

& (Join-Path $PSScriptRoot "rebrun.ps1") @argsForRebrun
exit $LASTEXITCODE
