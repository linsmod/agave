# tools/crop_png.ps1
#
# Crop a rectangle out of a PNG and scale it up with nearest-neighbour, so a
# 5-10 px detail of a `--ui-shot` render can actually be looked at. Pixel-level
# reviews of the chrome kept needing this; a full-window screenshot is useless
# for judging a corner radius.
#
# Usage:
#   pwsh -File tools\crop_png.ps1 -In build-zig-native\ui-dark.png -Rect 150,0,250,80
#   pwsh -File tools\crop_png.ps1 -In shot.png -Rect 150,0,250,80 -Scale 6 -Out c.png
#
# -Rect is x,y,w,h in source pixels (the DIB's top-left origin). Default output
# is <input>-crop.png next to the input.

param(
    [Parameter(Mandatory = $true)][string]$In,
    [Parameter(Mandatory = $true)][string]$Rect,
    [int]$Scale = 4,
    [string]$Out = ''
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

$srcPath = (Resolve-Path -LiteralPath $In).Path
$parts = $Rect -split ','
if ($parts.Count -ne 4) { throw "-Rect wants x,y,w,h (got '$Rect')" }
$x, $y, $w, $h = $parts | ForEach-Object { [int]$_.Trim() }

if ($w -le 0 -or $h -le 0) { throw "-Rect width/height must be positive" }
if ($Scale -lt 1) { $Scale = 1 }

if (-not $Out) {
    $Out = [System.IO.Path]::Combine(
        [System.IO.Path]::GetDirectoryName($srcPath),
        ([System.IO.Path]::GetFileNameWithoutExtension($srcPath) + '-crop.png'))
}

$src = [System.Drawing.Image]::FromFile($srcPath)
$srcW = $src.Width
$srcH = $src.Height
try {
    $dw = $w * $Scale
    $dh = $h * $Scale
    $dst = New-Object System.Drawing.Bitmap $dw, $dh
    $g = [System.Drawing.Graphics]::FromImage($dst)
    try {
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
        $g.DrawImage($src,
                     (New-Object System.Drawing.Rectangle 0, 0, $dw, $dh),
                     (New-Object System.Drawing.Rectangle $x, $y, $w, $h),
                     [System.Drawing.GraphicsUnit]::Pixel)
        $dst.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $g.Dispose()
        $dst.Dispose()
    }
} finally {
    $src.Dispose()
}

Write-Host ("crop {0}x{1} at {2},{3} {4}x{5} scale={6} from {7}x{8} -> {9}" -f `
    $w, $h, $x, $y, $w, $h, $Scale, $srcW, $srcH, $Out)
