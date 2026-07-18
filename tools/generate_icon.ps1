# Generates resources/icon.ico as a multi-size PNG-in-ICO.
# Style: rounded dark square background with a light play triangle,
# color-matched to the app's overlay accent.

param(
    [string] $OutPath = "resources/icon.ico"
)

Add-Type -AssemblyName System.Drawing

$sizes = @(16, 32, 48, 64, 128, 256)
$pngBlobs = @{}

foreach ($sz in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($sz, $sz, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic

    # Rounded rect path.
    $inset = [Math]::Max(1, [int]($sz * 0.03))
    $r = [Math]::Max(2, [int]($sz * 0.18))
    $x = $inset; $y = $inset; $w = $sz - 2*$inset - 1; $h = $sz - 2*$inset - 1
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($x, $y, $r*2, $r*2, 180, 90)
    $path.AddArc($x + $w - $r*2, $y, $r*2, $r*2, 270, 90)
    $path.AddArc($x + $w - $r*2, $y + $h - $r*2, $r*2, $r*2, 0, 90)
    $path.AddArc($x, $y + $h - $r*2, $r*2, $r*2, 90, 90)
    $path.CloseFigure()

    # Diagonal gradient fill matching the app's blue accent.
    $rect = New-Object System.Drawing.RectangleF($x, $y, $w, $h)
    $c1 = [System.Drawing.Color]::FromArgb(255, 30, 34, 58)
    $c2 = [System.Drawing.Color]::FromArgb(255, 60, 82, 148)
    $bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $c1, $c2, 45)
    $g.FillPath($bgBrush, $path)

    # Soft outline.
    $penW = [Math]::Max(1.0, $sz / 32.0)
    $penColor = [System.Drawing.Color]::FromArgb(220, 130, 165, 240)
    $borderPen = New-Object System.Drawing.Pen($penColor, [single]$penW)
    $g.DrawPath($borderPen, $path)

    # Play triangle - centered, nudged slightly right for optical balance.
    $triFill = [System.Drawing.Color]::FromArgb(255, 240, 245, 255)
    $triBrush = New-Object System.Drawing.SolidBrush($triFill)
    $cx = $sz / 2.0
    $cy = $sz / 2.0
    $triW = $sz * 0.38
    $triH = $sz * 0.46
    $offX = $sz * 0.05
    $p1 = New-Object System.Drawing.PointF([single]($cx - $triW/2 + $offX), [single]($cy - $triH/2))
    $p2 = New-Object System.Drawing.PointF([single]($cx - $triW/2 + $offX), [single]($cy + $triH/2))
    $p3 = New-Object System.Drawing.PointF([single]($cx + $triW/2 + $offX), [single]$cy)
    $g.FillPolygon($triBrush, @($p1, $p2, $p3))

    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngBlobs[$sz] = $ms.ToArray()
    $bmp.Dispose()
    $ms.Dispose()
}

# Assemble PNG-in-ICO.
$fs = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($fs)

$bw.Write([UInt16]0)                # reserved
$bw.Write([UInt16]1)                # type = ICO
$bw.Write([UInt16]$sizes.Count)     # count

$dataOffset = 6 + (16 * $sizes.Count)
foreach ($sz in $sizes) {
    $bw.Write([Byte]($(if ($sz -ge 256) { 0 } else { $sz })))
    $bw.Write([Byte]($(if ($sz -ge 256) { 0 } else { $sz })))
    $bw.Write([Byte]0)              # palette color count
    $bw.Write([Byte]0)              # reserved
    $bw.Write([UInt16]1)            # planes
    $bw.Write([UInt16]32)           # bpp
    $bw.Write([UInt32]$pngBlobs[$sz].Length)
    $bw.Write([UInt32]$dataOffset)
    $dataOffset += $pngBlobs[$sz].Length
}

foreach ($sz in $sizes) {
    $bw.Write($pngBlobs[$sz])
}

$bw.Flush()
$outFull = Join-Path -Path (Get-Location) -ChildPath $OutPath
$outDir = Split-Path -Parent $outFull
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
[System.IO.File]::WriteAllBytes($outFull, $fs.ToArray())
$size = $fs.Length
$fs.Dispose()

Write-Output "Wrote $outFull ($size bytes, sizes: $($sizes -join ', '))"
