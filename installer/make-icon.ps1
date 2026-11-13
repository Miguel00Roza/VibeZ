# Renders the VibeZ application icons (multi-size .ico with PNG frames).
#
# Two variants share the same dark rounded panel and green accent:
#   app    - monitor with an equalizer (VibeZ client)
#   server - broadcast waves (VibeZ Server)
#
# The files are generated artifacts committed to the repository; rerun this
# script after changing the drawing code below.
#
#   powershell -ExecutionPolicy Bypass -File installer\make-icon.ps1
param(
    [ValidateSet('app', 'server', 'all')]
    [string]$Variant = 'all',
    [string]$OutputPath
)

Add-Type -AssemblyName System.Drawing

$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$accent = [System.Drawing.Color]::FromArgb(255, 56, 224, 139)

function New-RoundedRectanglePath {
    param([single]$X, [single]$Y, [single]$Width, [single]$Height, [single]$Radius)
    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $diameter = $Radius * 2
    $path.AddArc($X, $Y, $diameter, $diameter, 180, 90)
    $path.AddArc($X + $Width - $diameter, $Y, $diameter, $diameter, 270, 90)
    $path.AddArc($X + $Width - $diameter, $Y + $Height - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($X, $Y + $Height - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-Panel {
    param([System.Drawing.Graphics]$Graphics, [single]$Scale)
    $inset = [single]($Scale * 0.04)
    $body = New-RoundedRectanglePath -X $inset -Y $inset `
        -Width ($Scale - 2 * $inset) -Height ($Scale - 2 * $inset) -Radius ([single]($Scale * 0.22))
    $rect = [System.Drawing.RectangleF]::new(0, 0, $Scale, $Scale)
    $brush = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
        $rect,
        [System.Drawing.Color]::FromArgb(255, 31, 42, 56),
        [System.Drawing.Color]::FromArgb(255, 9, 13, 19),
        62.0)
    $Graphics.FillPath($brush, $body)
    $brush.Dispose()
    return $body
}

function Add-MonitorGlyph {
    param([System.Drawing.Graphics]$Graphics, [single]$Scale)

    $pen = [System.Drawing.Pen]::new($accent, [single]($Scale * 0.055))
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $screen = New-RoundedRectanglePath `
        -X ([single]($Scale * 0.17)) -Y ([single]($Scale * 0.20)) `
        -Width ([single]($Scale * 0.66)) -Height ([single]($Scale * 0.44)) `
        -Radius ([single]($Scale * 0.09))
    $Graphics.DrawPath($pen, $screen)

    $brush = [System.Drawing.SolidBrush]::new($accent)
    $Graphics.FillRectangle($brush, [single]($Scale * 0.46), [single]($Scale * 0.66), [single]($Scale * 0.08), [single]($Scale * 0.08))
    $Graphics.FillRectangle($brush, [single]($Scale * 0.34), [single]($Scale * 0.73), [single]($Scale * 0.32), [single]($Scale * 0.05))

    # Equalizer bars: "voice over the shared screen".
    $barHeights = @(0.16, 0.26, 0.20)
    $barWidth = [single]($Scale * 0.075)
    $gap = [single]($Scale * 0.055)
    $totalWidth = 3 * $barWidth + 2 * $gap
    $startX = [single](($Scale - $totalWidth) / 2)
    for ($i = 0; $i -lt 3; $i++) {
        $height = [single]($Scale * $barHeights[$i])
        $x = [single]($startX + $i * ($barWidth + $gap))
        $y = [single](0.42 * $Scale - $height / 2)
        # Bars narrower than a pixel would disappear at 16x16.
        $width = [single]([Math]::Max($barWidth, 1.0))
        $bar = New-RoundedRectanglePath -X $x -Y $y -Width $width -Height $height -Radius ([single]($width / 2))
        $Graphics.FillPath($brush, $bar)
        $bar.Dispose()
    }

    $screen.Dispose()
    $brush.Dispose()
    $pen.Dispose()
}

function Add-BroadcastGlyph {
    param([System.Drawing.Graphics]$Graphics, [single]$Scale)

    $centerX = [single]($Scale * 0.5)
    $centerY = [single]($Scale * 0.74)
    $penWidth = [single]([Math]::Max($Scale * 0.06, 1.0))
    $pen = [System.Drawing.Pen]::new($accent, $penWidth)
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round

    # Three arcs opening upwards, centred on the dot: a broadcast/antenna mark.
    foreach ($radiusFactor in @(0.15, 0.27, 0.39)) {
        $radius = [single]($Scale * $radiusFactor)
        $rect = [System.Drawing.RectangleF]::new(
            [single]($centerX - $radius), [single]($centerY - $radius),
            [single]($radius * 2), [single]($radius * 2))
        $Graphics.DrawArc($pen, $rect, 225, 90)
    }

    $brush = [System.Drawing.SolidBrush]::new($accent)
    $dotRadius = [single]([Math]::Max($Scale * 0.075, 1.0))
    $Graphics.FillEllipse($brush,
        [single]($centerX - $dotRadius), [single]($centerY - $dotRadius),
        [single]($dotRadius * 2), [single]($dotRadius * 2))

    $brush.Dispose()
    $pen.Dispose()
}

function New-IconFrame {
    param([int]$Size, [string]$Kind)

    $bitmap = [System.Drawing.Bitmap]::new($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.Clear([System.Drawing.Color]::Transparent)

    $scale = [single]$Size
    $body = New-Panel -Graphics $graphics -Scale $scale
    if ($Kind -eq 'server') {
        Add-BroadcastGlyph -Graphics $graphics -Scale $scale
    } else {
        Add-MonitorGlyph -Graphics $graphics -Scale $scale
    }

    $body.Dispose()
    $graphics.Dispose()
    return $bitmap
}

function Write-IconFile {
    param([string]$Path, [string]$Kind)

    $frames = @()
    foreach ($size in $sizes) {
        $bitmap = New-IconFrame -Size $size -Kind $Kind
        $stream = [System.IO.MemoryStream]::new()
        $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
        $frames += @{ Size = $size; Data = $stream.ToArray() }
        $stream.Dispose()
        $bitmap.Dispose()
    }

    # ICONDIR + ICONDIRENTRY[] + PNG payloads (Vista and newer read PNG frames).
    $output = [System.IO.MemoryStream]::new()
    $writer = [System.IO.BinaryWriter]::new($output)
    $writer.Write([uint16]0)                # reserved
    $writer.Write([uint16]1)                # type: icon
    $writer.Write([uint16]$frames.Count)    # image count

    $offset = 6 + 16 * $frames.Count
    foreach ($frame in $frames) {
        $dimension = if ($frame.Size -ge 256) { 0 } else { $frame.Size }
        $writer.Write([byte]$dimension)
        $writer.Write([byte]$dimension)
        $writer.Write([byte]0)              # palette size
        $writer.Write([byte]0)              # reserved
        $writer.Write([uint16]1)            # color planes
        $writer.Write([uint16]32)           # bits per pixel
        $writer.Write([uint32]$frame.Data.Length)
        $writer.Write([uint32]$offset)
        $offset += $frame.Data.Length
    }
    foreach ($frame in $frames) {
        $writer.Write($frame.Data)
    }
    $writer.Flush()

    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path $directory)) { New-Item -ItemType Directory -Path $directory | Out-Null }
    [System.IO.File]::WriteAllBytes($Path, $output.ToArray())
    $writer.Dispose()
    $output.Dispose()

    Write-Host "Generated $Path ($((Get-Item $Path).Length) bytes, $($frames.Count) frames)"
}

$targets = @()
if ($Variant -in @('app', 'all')) {
    $targets += @{ Kind = 'app'; Path = if ($OutputPath -and $Variant -eq 'app') { $OutputPath } else { Join-Path $PSScriptRoot 'vibez.ico' } }
}
if ($Variant -in @('server', 'all')) {
    $targets += @{ Kind = 'server'; Path = if ($OutputPath -and $Variant -eq 'server') { $OutputPath } else { Join-Path $PSScriptRoot 'vibez-server.ico' } }
}

foreach ($target in $targets) {
    Write-IconFile -Path $target.Path -Kind $target.Kind
}
