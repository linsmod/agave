# tools/run_hidpi_tests.ps1
#
# Display DPI (docs/design-hidpi.md): a page must be *rendered* at the monitor's
# device scale - 1 CSS px = 1.5 device px on a 150% display, with fonts laid out
# and rasterized larger - not stretched after the fact, and never confused with
# the user's page zoom.
#
# Five cases against tools/pages/hidpi/hidpi_probe.html (the process-backend
# case makes six probes):
#
#   1. baseline (no flags, 100%): dpr 1, 1100x800 viewport, 100x100 box, no
#      device-pixel-ratio media query matches.
#   2. --device-scale 150: dpr 1.5, innerWidth 734 (1100/1.5 + the DOM's
#      truncation hack), the 100 CSS px box still measures 100 - and the CSS
#      media rule matches rgb(1,2,3).
#   3. --zoom 200 --device-scale 150: the two multiply (dpr 3) and the dpr>=2
#      rule wins. A zoom must never overwrite the DPI half and vice versa.
#   4. --device-scale 150 --shot: the 100x100 box covers 150x150 *device* px,
#      the box at CSS left:200 starts at device 300 (i.e. the pixels are laid out
#      at the DPI, not scaled from a 1x frame), and the blue canvas/50% boxes
#      land where the CSS says.
#   5. --device-scale 150 --shot-rect 200,0,120,60: --shot-rect is CSS px, so the
#      crop is 180x90 device px and covers exactly the green box.
#
# plus 3b: the same scale through --backend process, so the command is proven to
# cross the control/renderer boundary.
#
# The probes run at once with their own profiles. Expected wall time: ~3 s.
#
# Usage:
#   pwsh -File tools/run_hidpi_tests.ps1
#   pwsh -File tools/run_hidpi_tests.ps1 -BuildDir build-zig-native

param(
    [string]$BuildDir = 'build-zig-native'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir
$exe = Join-Path $build 'agave_shell.exe'
$page = Join-Path $root 'tools/pages/hidpi/hidpi_probe.html'
$check = Join-Path $root 'tools/pages/hidpi/check_hidpi.js'
$pixelCheck = Join-Path $root 'tools/pixel_check.py'

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }
if (-not (Test-Path $page)) { throw "page not found: $page" }

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking
$run = New-TestRun -Suite 'hidpi'
$shotDir = Join-Path $run.Dir 'shots'
New-Item -ItemType Directory -Force -Path $shotDir | Out-Null

$failures = 0
$url = "file:///$($page -replace '\\','/')"
$timeout = 30000

# 1..3: what the page can see (dpr, CSS px, media queries).
$baseJob = Start-TestProbe -Run $run -Name 'dpr100' -Url $url -EvalFile $check -Wait 600 -Settle 100 -Timeout $timeout
$dpiJob = Start-TestProbe -Run $run -Name 'dpr150' -Url $url -EvalFile $check -Wait 600 -Settle 100 -Timeout $timeout `
    -Extra @('--device-scale', '150')
$mixJob = Start-TestProbe -Run $run -Name 'dpr300' -Url $url -EvalFile $check -Wait 600 -Settle 100 -Timeout $timeout `
    -Extra @('--zoom', '200', '--device-scale', '150')

# 3b: the same scale through the other render backend. The command crosses the
#     control/renderer boundary (AGW_VIEW_CMD_DEVICE_SCALE), so the DPI must
#     arrive in the render process and not only in the local engine.
$procJob = Start-TestProbe -Run $run -Name 'dpr150proc' -Url $url -EvalFile $check -Wait 1200 -Settle 300 -Timeout $timeout `
    -Extra @('--backend', 'process', '--device-scale', '150')

# 4: the pixels. --shot must run without --eval (probeExit() would close the
#    window before the capture).
$shot = Join-Path $shotDir 'hidpi150.png'
$shotJob = Start-TestProbe -Run $run -Name 'shot150' -Url $url -Wait 800 -Settle 200 -Timeout $timeout `
    -RawArgs @('--probe', $url, '--device-scale', '150', '--wait', '800', '--settle', '200',
               '--timeout', "$timeout", '--shot', $shot)

# 5: --shot-rect is CSS px, the crop is device px.
$crop = Join-Path $shotDir 'crop150.png'
$cropJob = Start-TestProbe -Run $run -Name 'crop150' -Url $url -Wait 800 -Settle 200 -Timeout $timeout `
    -RawArgs @('--probe', $url, '--device-scale', '150', '--wait', '800', '--settle', '200',
               '--timeout', "$timeout", '--shot', $crop, '--shot-rect', '200,0,120,60')

$base = Complete-TestProbe -Job $baseJob
$dpi = Complete-TestProbe -Job $dpiJob
$mix = Complete-TestProbe -Job $mixJob
$proc = Complete-TestProbe -Job $procJob
$shotRes = Complete-TestProbe -Job $shotJob
$cropRes = Complete-TestProbe -Job $cropJob

function Check {
    param([string]$Name, [bool]$Ok, [string]$Detail)

    if ($Ok) {
        Write-Host ("PASS  {0}" -f $Name)
    } else {
        $script:failures++
        Write-Host ("FAIL  {0}" -f $Name)
        if ($Detail) { Write-Host ("      " + $Detail.Trim()) }
    }
}

function Needles {
    param([string]$Name, $Result, [string[]]$Want)

    if ($Result.Code -ne 0) {
        Check $Name $false ("probe exit {0}: {1}" -f $Result.Code, $Result.Text)
        return
    }

    $missing = @()
    foreach ($needle in $Want) {
        if ($Result.Text -notmatch [regex]::Escape($needle)) { $missing += $needle }
    }

    Check $Name ($missing.Count -eq 0) $(if ($missing.Count) { 'missing: ' + ($missing -join ', ') + "`n" + $Result.Text })
}

# ---- what the page sees ---------------------------------------------------
Needles 'baseline 100%' $base @(
    'dpr=1 ', 'inner=1100x800', 'boxRect=0,0 100x100', 'boxOffset=100x100',
    'halfWidth=550', 'mqCSS=rgb(17, 17, 17)', 'mqMin15=false', 'mqMin2=false')

Needles 'device scale 150' $dpi @(
    'dpr=1.5 ', 'inner=734x534', 'boxRect=0,0 100x100', 'boxOffset=100x100',
    'targetRect=200,0 120x60', 'halfWidth=367', 'mqCSS=rgb(1, 2, 3)',
    'mqMin15=true', 'mqMin2=false', 'efpAt200=target')

# zoom and device scale are independent and multiply. #half is not asserted
# here: at 200% the page is taller than the viewport, so the 50% box measures
# against a scrollbar-narrowed content width.
Needles 'zoom 200 on a 150% display' $mix @(
    'dpr=3 ', 'inner=367x267', 'boxRect=0,0 100x100',
    'mqCSS=rgb(4, 5, 6)', 'mqMin2=true')

Needles 'device scale 150 (process backend)' $proc @(
    'dpr=1.5 ', 'inner=734x534', 'boxRect=0,0 100x100', 'mqCSS=rgb(1, 2, 3)',
    'mqMin15=true')

# ---- pixels: the page is laid out at the DPI, not stretched ---------------
if ($shotRes.Code -ne 0 -or -not (Test-Path $shot)) {
    Check 'shot at 150' $false ("probe exit {0}: {1}" -f $shotRes.Code, $shotRes.Text)
} else {
    $px = (& python $pixelCheck $shot '149,149,ff0000' '152,152,ffffff' '299,30,ffffff' `
            '305,30,00c000' '100,350,0000ff' '100,605,0000ff' '700,605,ffffff' 2>&1) -join "`n"
    Check 'shot at 150: boxes lay out 1.5x device px' ($LASTEXITCODE -eq 0) $px
}

if ($cropRes.Code -ne 0 -or -not (Test-Path $crop)) {
    Check 'shot-rect at 150' $false ("probe exit {0}: {1}" -f $cropRes.Code, $cropRes.Text)
} else {
    $b = [System.IO.File]::ReadAllBytes($crop)
    $w = ([int]$b[16] -shl 24) -bor ([int]$b[17] -shl 16) -bor ([int]$b[18] -shl 8) -bor [int]$b[19]
    $h = ([int]$b[20] -shl 24) -bor ([int]$b[21] -shl 16) -bor ([int]$b[22] -shl 8) -bor [int]$b[23]
    if ("${w}x${h}" -ne '180x90') {
        Check 'shot-rect at 150' $false "crop is ${w}x${h}, want 180x90 (120x60 CSS px at 150%)"
    } else {
        $px = (& python $pixelCheck $crop '5,5,00c000' '174,84,00c000' 2>&1) -join "`n"
        Check 'shot-rect at 150: CSS rect becomes device pixels' ($LASTEXITCODE -eq 0) $px
    }
}

Remove-TestRun $run $failures
if ($failures -gt 0) {
    Write-Host "`nHIDPI CHECKS FAILED ($failures)"
    exit 1
}

Write-Host "`nALL HIDPI CHECKS PASSED"
exit 0
