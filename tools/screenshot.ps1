<#
.SYNOPSIS
  Screenshot what agave paints for a URL, without showing a window.

.DESCRIPTION
  Runs the headless probe with --shot: the probe window is a hidden WS_POPUP
  (never shown, never focused, no taskbar entry) and the control blits its own
  mirror buffer into a DIB, so the capture needs neither a visible window nor
  PrintWindow. Nothing appears on the user's desktop (iron law 7).

  The probe writes the format -Out names: PNG is encoded by the shell itself
  (libpng), BMP is written raw, other types (jpg/gif/tif) round-trip through a
  temporary PNG. No manual conversion step is needed for the common case.

  -Rect crops to a CSS-pixel rect, origin at the viewport's top-left, so one
  element can be captured directly: pass the values of the element's
  getBoundingClientRect() (after scrolling it into view). The file stays device
  pixels, so a rect on a zoomed page - or on a high-DPI one, see -DeviceScale -
  comes out larger than the rect itself.

.EXAMPLE
  pwsh -File tools/screenshot.ps1 -Url https://chat.deepseek.com/ -Out debug/ds.png -Wait 9000

.EXAMPLE
  # just the 440x200 block on the local test page - no cropping afterwards
  pwsh -File tools/screenshot.ps1 -Url file:///E:/mybrowsers/agave/debug/shot_page.html `
      -Out build-zig-native/box.png -Rect 0,0,440,200
#>
param(
    [Parameter(Mandatory = $true)][string]$Url,
    [string]$Out = 'debug/shot.png',
    [int]$Wait = 8000,          # ms after the load event before capturing
    [string]$Rect,              # x,y,w,h in CSS px (getBoundingClientRect)
    [int]$DeviceScale = 0,      # display DPI as % of 96 (150 = 150% screen); 0 = 100
    [int]$TimeoutMs = 60000,
    [string]$UserData
)

$ErrorActionPreference = 'Stop'

function Get-ImageSize([string]$Path) {
    # Read the header directly: loading the image through System.Drawing would
    # lock the file and is overkill just to report two numbers.
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -gt 24 -and $bytes[0] -eq 0x89 -and $bytes[1] -eq 0x50) {
        # Shift operators keep the operand type, and a [byte] shifted by 24 is
        # 0 - cast to [int] before combining the four big-endian bytes.
        $w = ([int]$bytes[16] -shl 24) -bor ([int]$bytes[17] -shl 16) -bor ([int]$bytes[18] -shl 8) -bor [int]$bytes[19]
        $h = ([int]$bytes[20] -shl 24) -bor ([int]$bytes[21] -shl 16) -bor ([int]$bytes[22] -shl 8) -bor [int]$bytes[23]
        return "${w}x${h}"
    }
    if ($bytes.Length -gt 26 -and $bytes[0] -eq 0x42 -and $bytes[1] -eq 0x4D) {
        $w = [BitConverter]::ToInt32($bytes, 18)
        $h = [BitConverter]::ToInt32($bytes, 22)
        if ($h -lt 0) { $h = -$h }
        return "${w}x${h}"
    }
    Add-Type -AssemblyName System.Drawing
    $img = [System.Drawing.Image]::FromFile($Path)
    try { return "$($img.Width)x$($img.Height)" } finally { $img.Dispose() }
}

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build-zig-native\agave_shell.exe'
if (-not (Test-Path $exe)) { throw "missing $exe - run tools/build_project.ps1" }
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }

$outDir = Split-Path -Parent $Out
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }

$ext = [System.IO.Path]::GetExtension($Out).ToLowerInvariant()
if ($ext -notin @('.png', '.bmp', '.jpg', '.jpeg', '.gif', '.tif', '.tiff')) {
    throw "-Out wants .png/.bmp (written by the probe) or .jpg/.gif/.tif (converted here), got '$ext'"
}
# The probe encodes only PNG and BMP; anything else is converted from a PNG.
$direct = $ext -in @('.png', '.bmp')
$capture = if ($direct) { $Out } else { [System.IO.Path]::ChangeExtension($Out, '.png') }

if ($Rect -and $Rect -notmatch '^\s*-?\d+\s*,\s*-?\d+\s*,\s*\d+\s*,\s*\d+\s*$') {
    throw "-Rect wants x,y,w,h in CSS px (e.g. 10,10,320,200), got '$Rect'"
}

$probeArgs = @('--probe', $Url, '--shot', $capture, '--wait', $Wait, '--timeout', $TimeoutMs)
if ($Rect) { $probeArgs += @('--shot-rect', ($Rect -replace '\s', '')) }
if ($DeviceScale -gt 100) { $probeArgs += @('--device-scale', "$DeviceScale") }
if ($UserData) { $probeArgs += @('--userdata', $UserData) }

$output = & $exe @probeArgs 2>&1
$code = $LASTEXITCODE
$output | Where-Object { $_ -match '\[probe\]|\[nav\]|\[sanity\]' } | ForEach-Object { Write-Host $_ }

if (-not (Test-Path $capture)) { throw "no capture written (exit $code); see the probe output above" }

if ($capture -ne $Out) {
    Add-Type -AssemblyName System.Drawing
    $img = [System.Drawing.Image]::FromFile($capture)
    try {
        $format = switch ($ext) {
            '.jpg'  { [System.Drawing.Imaging.ImageFormat]::Jpeg }
            '.jpeg' { [System.Drawing.Imaging.ImageFormat]::Jpeg }
            '.gif'  { [System.Drawing.Imaging.ImageFormat]::Gif }
            '.tif'  { [System.Drawing.Imaging.ImageFormat]::Tiff }
            '.tiff' { [System.Drawing.Imaging.ImageFormat]::Tiff }
        }
        $img.Save($Out, $format)
    } finally { $img.Dispose() }
    Remove-Item -Force $capture
}

# The size is the cropped one when -Rect was used: that is the point of the flag.
$size = Get-ImageSize $Out
"shot=$Out $size bytes=$((Get-Item $Out).Length) exit=$code"
exit $code
