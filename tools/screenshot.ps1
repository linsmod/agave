<#
.SYNOPSIS
  Screenshot what agave paints for a URL, without showing a window.

.DESCRIPTION
  Runs the headless probe with --shot: the probe window is a hidden WS_POPUP
  (never shown, never focused, no taskbar entry) and the control blits its own
  mirror buffer into a DIB, so the capture needs neither a visible window nor
  PrintWindow. Nothing appears on the user's desktop (iron law 7).

  The probe writes a BMP; this script converts it to the requested image type
  and deletes the BMP.

.EXAMPLE
  pwsh -File tools/screenshot.ps1 -Url https://chat.deepseek.com/ -Out debug/ds.png -Wait 9000
#>
param(
    [Parameter(Mandatory = $true)][string]$Url,
    [string]$Out = 'debug/shot.png',
    [int]$Wait = 8000,          # ms after the load event before capturing
    [int]$TimeoutMs = 60000,
    [string]$UserData
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build-zig-native\agave_shell.exe'
if (-not (Test-Path $exe)) { throw "missing $exe - run tools/build_project.ps1" }
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }

$outDir = Split-Path -Parent $Out
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }

$bmp = [System.IO.Path]::ChangeExtension($Out, '.bmp')
$args = @('--probe', $Url, '--shot', $bmp, '--wait', $Wait, '--timeout', $TimeoutMs)
if ($UserData) { $args += @('--userdata', $UserData) }

$output = & $exe @args 2>&1
$code = $LASTEXITCODE
$output | Where-Object { $_ -match '\[probe\]|\[nav\]|\[sanity\]' } | ForEach-Object { Write-Host $_ }

if (-not (Test-Path $bmp)) { throw "no capture written (exit $code); see the probe output above" }

if ([System.IO.Path]::GetExtension($Out) -eq '.bmp') {
    if ($bmp -ne $Out) { Move-Item -Force $bmp $Out }
} else {
    Add-Type -AssemblyName System.Drawing
    $img = [System.Drawing.Image]::FromFile($bmp)
    try { $img.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png) }
    finally { $img.Dispose() }
    Remove-Item -Force $bmp
}

$size = (Get-Item $Out).Length
"shot=$Out bytes=$size exit=$code"
exit $code
