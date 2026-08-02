# Generates resources/icon.ico as a multi-size PNG-in-ICO.
# Style: rounded warm-dark square background with a bright banana crescent
# whose inner curve opens rightward, doubling as a play-arrow silhouette.
#
# Design rationale:
#   - 16 px is the smallest Explorer/taskbar bucket; a full "banana with
#     stem + shadow" wouldn't survive. We render a fat crescent from two
#     circle-arc paths so the shape stays recognizable at every size.
#   - The crescent is tilted ~-25 deg so the inner concave edge points
#     roughly right, giving the icon a subtle "play" read without a
#     literal triangle glued on top (which reads as clutter at 16 px).
#   - Larger sizes (64/128/256) get a small stem cap + specular highlight
#     to sell the "banana" read; smaller sizes drop them automatically
#     (thresholds inside the loop).

param(
    [string] $OutPath = "resources/icon.ico"
)

Add-Type -AssemblyName System.Drawing

$sizes = @(16, 32, 48, 64, 128, 256)
$pngBlobs = @{}

# Palette shared with Style::SetupDarkTheme so app icon + in-app theme
# feel like the same product.
$bgDark    = [System.Drawing.Color]::FromArgb(255,  22,  20,  16)   # warm near-black
$bgLight   = [System.Drawing.Color]::FromArgb(255,  50,  42,  28)   # warm mid
$borderCol = [System.Drawing.Color]::FromArgb(210, 120,  92,  40)   # peel-tone hairline
$bananaHi  = [System.Drawing.Color]::FromArgb(255, 255, 222, 122)   # highlight (accent-light)
$bananaMid = [System.Drawing.Color]::FromArgb(255, 255, 201,  58)   # banana yellow
$bananaLo  = [System.Drawing.Color]::FromArgb(255, 227, 154,  28)   # peel-toe shadow
$stemCol   = [System.Drawing.Color]::FromArgb(255,  90,  62,  32)   # dark stem
$sheenCol  = [System.Drawing.Color]::FromArgb(190, 255, 245, 200)   # specular

foreach ($sz in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($sz, $sz, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic

    # ---------- Rounded rect background ----------
    $inset = [Math]::Max(1, [int]($sz * 0.03))
    $r     = [Math]::Max(2, [int]($sz * 0.20))
    $x = $inset; $y = $inset; $w = $sz - 2*$inset - 1; $h = $sz - 2*$inset - 1
    $bgPath = New-Object System.Drawing.Drawing2D.GraphicsPath
    $bgPath.AddArc($x,             $y,             $r*2, $r*2, 180, 90)
    $bgPath.AddArc($x + $w - $r*2, $y,             $r*2, $r*2, 270, 90)
    $bgPath.AddArc($x + $w - $r*2, $y + $h - $r*2, $r*2, $r*2,   0, 90)
    $bgPath.AddArc($x,             $y + $h - $r*2, $r*2, $r*2,  90, 90)
    $bgPath.CloseFigure()

    $rectF   = New-Object System.Drawing.RectangleF($x, $y, $w, $h)
    $bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rectF, $bgDark, $bgLight, 120)
    $g.FillPath($bgBrush, $bgPath)

    # Peel-toned hairline border only on 32+; at 16 px it eats the shape.
    if ($sz -ge 32) {
        $penW = [Math]::Max(1.0, $sz / 42.0)
        $borderPen = New-Object System.Drawing.Pen($borderCol, [single]$penW)
        $g.DrawPath($borderPen, $bgPath)
        $borderPen.Dispose()
    }

    # ---------- Banana crescent ----------
    # Build the crescent from two overlapping ellipses in a subtractive
    # composite: outer ellipse (banana body) minus a slightly-offset
    # inner ellipse (bite that carves the crescent's concave side).
    # We rotate the whole composite so the concave side opens rightward.
    #
    # Coordinates below are pre-rotation, centered on the tile.
    $state = $g.Save()
    $cx = $sz / 2.0
    $cy = $sz / 2.0
    $g.TranslateTransform([single]$cx, [single]$cy)
    $g.RotateTransform([single](-55.0))   # tilt: banana curve horizontal, concave side down-right (play direction)

    # Outer ellipse (banana body). Nudge upward so the tilted crescent
    # ends up roughly centered in the tile after rotation.
    $bw = $sz * 0.90      # wide, spans most of the tile
    $bh = $sz * 0.90
    $bx = -$bw / 2.0
    $by = -$bh / 2.0 - $sz * 0.02

    $bananaPath = New-Object System.Drawing.Drawing2D.GraphicsPath
    $bananaPath.AddEllipse([single]$bx, [single]$by, [single]$bw, [single]$bh)

    # Inner "bite" ellipse - slightly LARGER than the body and offset
    # less than half the body's width. Result: the leftover sliver is
    # a slimmer crescent whose two tips pinch to near-points, reading
    # as a banana rather than a moon. Offset direction (toward the
    # lower-right of pre-rotation space) means after the -28 deg tilt
    # the concave side opens roughly rightward - the "play direction".
    $bitePath = New-Object System.Drawing.Drawing2D.GraphicsPath
    $biteW = $bw * 1.02
    $biteH = $bh * 1.02
    $offX = $sz * 0.28
    $offY = $sz * 0.28
    $bitePath.AddEllipse([single]($bx + $offX - ($biteW - $bw) * 0.5),
                         [single]($by + $offY - ($biteH - $bh) * 0.5),
                         [single]$biteW, [single]$biteH)

    # Combine: full body region minus the bite region = crescent region.
    $crescentRegion = New-Object System.Drawing.Region($bananaPath)
    $biteRegion     = New-Object System.Drawing.Region($bitePath)
    $crescentRegion.Exclude($biteRegion)

    # Fill the crescent with a yellow gradient (bright top-left,
    # peel-toe bottom-right). Use a rect that comfortably encloses
    # the pre-rotation crescent bounding area.
    $fillRect = New-Object System.Drawing.RectangleF(
        [single]($bx - 2), [single]($by - 2),
        [single]($bw + 4), [single]($bh + 4))
    $bananaBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $fillRect, $bananaHi, $bananaLo, 135)

    # Add a mid-color colour blend for a richer shading than a two-stop gradient.
    $blend = New-Object System.Drawing.Drawing2D.ColorBlend(3)
    $blend.Colors    = @($bananaHi, $bananaMid, $bananaLo)
    $blend.Positions = @([single]0.0, [single]0.45, [single]1.0)
    $bananaBrush.InterpolationColors = $blend

    $g.FillRegion($bananaBrush, $crescentRegion)

    # Subtle outline for definition at every size.
    $outlinePen = New-Object System.Drawing.Pen($bananaLo,
                                                [single]([Math]::Max(0.8, $sz / 96.0)))
    # Outline the crescent boundary by stroking body & bite ellipses; the
    # resulting overlapping strokes visually approximate the crescent
    # perimeter without a costly region-to-path conversion.
    $g.SetClip($crescentRegion, [System.Drawing.Drawing2D.CombineMode]::Replace)
    $g.DrawPath($outlinePen, $bananaPath)
    $g.DrawPath($outlinePen, $bitePath)
    $g.ResetClip()

    # Sheen highlight - a soft short arc riding on the OUTER (upper-left)
    # curve of the crescent so it reads as light on the top of the peel.
    # Only rendered at 48+ where the arc has enough pixels to look
    # intentional; below that it turns into a "scratched icon" artifact.
    if ($sz -ge 48) {
        $sheenPen = New-Object System.Drawing.Pen($sheenCol,
                                                  [single]([Math]::Max(1.2, $sz / 44.0)))
        # Clip to the crescent so the arc can't spill past the peel edge.
        $g.SetClip($crescentRegion, [System.Drawing.Drawing2D.CombineMode]::Replace)
        # Slightly inset from the outer edge, arcing along ~55 deg of the
        # top-left of the body ellipse (pre-rotation). Tilt puts this
        # arc on the upper edge of the crescent post-rotation.
        $sheenRect = New-Object System.Drawing.RectangleF(
            [single]($bx + $sz * 0.05), [single]($by + $sz * 0.05),
            [single]($bw - $sz * 0.10), [single]($bh - $sz * 0.10))
        $g.DrawArc($sheenPen, $sheenRect, [single]205, [single]55)
        $g.ResetClip()
        $sheenPen.Dispose()
    }

    # (No stem cap - the crescent alone carries the banana read at every
    # size, and adding a small dark dot at the tip landed inconsistently
    # once the rotation angle was tuned. Keeping the shape clean beats a
    # decorative element that only works at one size.)

    $g.Restore($state)

    # Cleanup per-tile GDI objects.
    $bananaBrush.Dispose()
    $outlinePen.Dispose()
    $crescentRegion.Dispose()
    $biteRegion.Dispose()
    $bananaPath.Dispose()
    $bitePath.Dispose()
    $bgBrush.Dispose()
    $bgPath.Dispose()
    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngBlobs[$sz] = $ms.ToArray()
    $bmp.Dispose()
    $ms.Dispose()
}

# ---------- Assemble PNG-in-ICO ----------
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
