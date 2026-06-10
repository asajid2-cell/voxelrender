# makesheets.ps1 - (re)build contact sheets for capture dirs given as args
param([string[]]$Tags)
Set-Location $PSScriptRoot
[Environment]::CurrentDirectory = $PSScriptRoot
Add-Type -AssemblyName System.Drawing
foreach ($tag in $Tags) {
    $dir = "build\captures\$tag"
    $bmps = @(Get-ChildItem "$dir\*.bmp" -EA SilentlyContinue | Sort-Object Name)
    if ($bmps.Count -eq 0) { Write-Output "${tag}: no frames"; continue }
    $tw = 320; $th = 180; $cols = 4
    $rows = [math]::Ceiling($bmps.Count / $cols)
    $sheet = New-Object System.Drawing.Bitmap ($tw * $cols), (($th + 16) * $rows)
    $g = [System.Drawing.Graphics]::FromImage($sheet)
    $g.Clear([System.Drawing.Color]::Black)
    $font = New-Object System.Drawing.Font "Consolas", 9
    for ($i = 0; $i -lt $bmps.Count; $i++) {
        $img = [System.Drawing.Image]::FromFile($bmps[$i].FullName)
        $x = ($i % $cols) * $tw; $y = [math]::Floor($i / $cols) * ($th + 16)
        $g.DrawImage($img, $x, $y, $tw, $th)
        $g.DrawString($bmps[$i].BaseName, $font, [System.Drawing.Brushes]::Yellow, $x + 2, $y + $th)
        $img.Dispose()
    }
    $g.Dispose()
    $abs = Join-Path $PSScriptRoot "$dir\contact_sheet.png"
    $sheet.Save($abs, [System.Drawing.Imaging.ImageFormat]::Png)
    $sheet.Dispose()
    Write-Output "${tag}: $($bmps.Count) frames -> $abs"
}
