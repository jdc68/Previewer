# Generates src\app.ico (multi-size, PNG-compressed entries).
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot '..\src\app.ico'
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$pngs = @()

foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)

    $r = [single]($s * 0.22)
    $rect = New-Object System.Drawing.RectangleF 0, 0, $s, $s
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc(0, 0, $d, $d, 180, 90)
    $path.AddArc($s - $d, 0, $d, $d, 270, 90)
    $path.AddArc($s - $d, $s - $d, $d, $d, 0, 90)
    $path.AddArc(0, $s - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::FromArgb(255, 41, 128, 255)), ([System.Drawing.Color]::FromArgb(255, 124, 77, 255)), 45
    $g.FillPath($grad, $path)

    # mountains
    $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 255, 255))
    $soft = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(170, 255, 255, 255))
    $p1 = @(
        (New-Object System.Drawing.PointF ($s * 0.14), ($s * 0.80)),
        (New-Object System.Drawing.PointF ($s * 0.40), ($s * 0.42)),
        (New-Object System.Drawing.PointF ($s * 0.66), ($s * 0.80))
    )
    $p2 = @(
        (New-Object System.Drawing.PointF ($s * 0.46), ($s * 0.80)),
        (New-Object System.Drawing.PointF ($s * 0.66), ($s * 0.54)),
        (New-Object System.Drawing.PointF ($s * 0.86), ($s * 0.80))
    )
    $g.FillPolygon($soft, $p2)
    $g.FillPolygon($white, $p1)
    # sun
    $sun = [single]($s * 0.17)
    $g.FillEllipse($white, [single]($s * 0.62), [single]($s * 0.18), $sun, $sun)
    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += , $ms.ToArray()
    $bmp.Dispose()
}

$fs = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $pngs[$i].Length
    $dim = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$len); $bw.Write([UInt32]$offset)
    $offset += $len
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Flush()
[System.IO.File]::WriteAllBytes((Resolve-Path (Split-Path $out)).Path + '\app.ico', $fs.ToArray())
Write-Host "Wrote $out"
