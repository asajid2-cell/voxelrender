param(
    [string]$Log = "build\bin\venpod_runtime.log",
    [int]$Top = 120,
    [switch]$Bins
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Log)) {
    throw "Log not found: $Log"
}

$traceRows = New-Object System.Collections.Generic.List[object]
$binRows = New-Object System.Collections.Generic.List[object]

$tracePattern =
    'MIDMESH_WAVE_TRACE frame=(\d+) build=(\d+) cam=\(([-0-9.]+),([-0-9.]+),([-0-9.]+)\).*?resident=(\d+) emitted=(\d+) faces=(\d+) dirty=(\d+) removed=(\d+) culled=(\d+) preExtract=(\d+) reExtract=(\d+) deferred=(\d+) lodMerged=(\d+) childSuppressed=(\d+) hotBin=(\d+)\.\.(\d+)'
$binPattern =
    'MIDMESH_WAVE_BIN frame=(\d+) bin=(\d+) range=\[(\d+),(\d+)\].*?resident=(\d+) emitted=(\d+) faces=(\d+) dirty=(\d+) removed=(\d+) culled=(\d+) preExtract=(\d+) reExtract=(\d+) deferred=(\d+) lodMerged=(\d+) childSuppressed=(\d+) miss=new/recenter/lod/content/child/buildver:(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)'

Get-Content $Log | ForEach-Object {
    if ($_ -match $tracePattern) {
        $traceRows.Add([pscustomobject]@{
            Frame = [int]$matches[1]
            Build = [int]$matches[2]
            CamX = [double]$matches[3]
            CamY = [double]$matches[4]
            CamZ = [double]$matches[5]
            Resident = [int]$matches[6]
            Emitted = [int]$matches[7]
            Faces = [int64]$matches[8]
            Dirty = [int]$matches[9]
            Removed = [int]$matches[10]
            Culled = [int]$matches[11]
            PreExtract = [int]$matches[12]
            ReExtract = [int]$matches[13]
            Deferred = [int]$matches[14]
            LodMerged = [int]$matches[15]
            ChildSuppressed = [int]$matches[16]
            HotBinFirst = [int]$matches[17]
            HotBinLast = [int]$matches[18]
        })
    } elseif ($_ -match $binPattern) {
        $binRows.Add([pscustomobject]@{
            Frame = [int]$matches[1]
            Bin = [int]$matches[2]
            From = [int]$matches[3]
            To = [int]$matches[4]
            Resident = [int]$matches[5]
            Emitted = [int]$matches[6]
            Faces = [int64]$matches[7]
            Dirty = [int]$matches[8]
            Removed = [int]$matches[9]
            Culled = [int]$matches[10]
            PreExtract = [int]$matches[11]
            ReExtract = [int]$matches[12]
            Deferred = [int]$matches[13]
            LodMerged = [int]$matches[14]
            ChildSuppressed = [int]$matches[15]
            MissNew = [int]$matches[16]
            MissRecenter = [int]$matches[17]
            MissLod = [int]$matches[18]
            MissContent = [int]$matches[19]
            MissChild = [int]$matches[20]
            MissBuildVer = [int]$matches[21]
        })
    }
}

Write-Host "MIDMESH_WAVE_TRACE summary from $Log"
Write-Host "  trace rows: $($traceRows.Count)"
Write-Host "  bin rows:   $($binRows.Count)"

Write-Host ""
Write-Host "Hot frames:"
$hotFramesText = $traceRows |
    Where-Object { $_.Dirty -or $_.Removed -or $_.PreExtract -or $_.ReExtract -or $_.Deferred } |
    Select-Object -First $Top Frame,Build,Dirty,Removed,PreExtract,ReExtract,Deferred,LodMerged,ChildSuppressed,HotBinFirst,HotBinLast,CamX,CamZ |
    Format-Table -AutoSize |
    Out-String -Width 240
Write-Host $hotFramesText

if ($Bins) {
    Write-Host ""
    Write-Host "Hot bins:"
    $hotBinsText = $binRows |
        Where-Object { $_.Dirty -or $_.Removed -or $_.PreExtract -or $_.ReExtract -or $_.Deferred -or $_.MissLod -or $_.MissChild -or $_.MissRecenter -or $_.MissContent } |
        Select-Object -First $Top Frame,Bin,From,To,Dirty,Removed,PreExtract,ReExtract,Deferred,
            @{Name="Misses"; Expression={"new=$($_.MissNew) rec=$($_.MissRecenter) lod=$($_.MissLod) cont=$($_.MissContent) child=$($_.MissChild)"}},
            LodMerged,ChildSuppressed |
        Format-Table -AutoSize |
        Out-String -Width 240
    Write-Host $hotBinsText
}
