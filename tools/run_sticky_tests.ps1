# tools/run_sticky_tests.ps1
#
# Regression for `position: sticky` (hist-lessons R82).
#
# Two layers:
#   1. tools/pages/layout/sticky.html + sticky_measure.js through the probe:
#      geometry at several scroll offsets - a sticky `top:0` header stays at the
#      viewport top, a sticky `bottom:0` footer stays at the bottom until its
#      containing block ends, an inner overflow box pins its own sticky bar, and
#      hit-testing follows the shifted box (`elementFromPoint`).
#   2. the same page with `--shot`: what got *painted* after a scroll, which is
#      the half geometry cannot see (the sticky box must not blit away with the
#      rest of the content).
#
# Both probes run at once; expected wall time ~2.5 s.
#
# Usage:
#   pwsh -File tools/run_sticky_tests.ps1
#   pwsh -File tools/run_sticky_tests.ps1 -BuildDir build-zig-native

param(
    [string]$BuildDir = 'build-zig-native'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir
$exe = Join-Path $build 'agave_shell.exe'
$page = Join-Path $root 'tools/pages/layout/sticky.html'
$measure = Join-Path $root 'tools/pages/layout/sticky_measure.js'
$pixel = Join-Path $root 'tools/pixel_check.py'

foreach ($p in @($exe, $page, $measure, $pixel)) {
    if (-not (Test-Path $p)) { throw "not found: $p" }
}

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking
$run = New-TestRun -Suite 'sticky'

$failures = 0
$url = "file:///$($page -replace '\\', '/')"

$geomJob = Start-TestProbe -Run $run -Name 'geometry' -Url $url `
    -EvalFile $measure -Wait 900 -Settle 500 -Timeout 60000

$scrollDir = Join-Path $run.Dir 'paint'
New-Item -ItemType Directory -Force -Path $scrollDir | Out-Null
$scrollJs = Join-Path $scrollDir 'scroll.js'
'window.scrollTo(0, 500);' | Set-Content -Path $scrollJs -Encoding ascii
$shot = Join-Path $scrollDir 'sticky.bmp'
$paintJob = Start-TestProbe -Run $run -Name 'paint' -Url $url `
    -RawArgs @('--probe', $url, '--eval-file', $scrollJs, '--wait', '900', '--settle', '500',
               '--shot', $shot, '--timeout', '60000')

$probe = Complete-TestProbe -Job $geomJob
$paint = Complete-TestProbe -Job $paintJob

# 1. geometry: every expectation below is the value Chrome reports for this page
#    at the probe's 1100x800 viewport.
$out = $probe.Text
$expected = @(
    'pos=sticky',      # the declaration survives the cascade
    'h0=0',            # header at the top before scrolling
    'f0=770',          # sticky bottom pulls the footer into view (800-30)
    'h500=0',          # header pinned while scrolled
    'f500=770',        # footer still pinned
    'hit=head',        # hit-test follows the painted position
    'hmax=0',          # still pinned at the bottom of the document
    'fmaxNatural=326', # footer released: it sits at its own offset
    's0=2', 's100=2', 's400=2'  # inner scroller keeps its bar at the box edge
)

$missing = @()
foreach ($want in $expected) {
    if ($out -notmatch [regex]::Escape($want)) { $missing += $want }
}

if ($probe.Code -ne 0 -or $missing.Count -gt 0) {
    $failures++
    Write-Host ("FAIL  sticky geometry (exit {0}): missing {1}" -f $probe.Code, ($missing -join ', '))
} else {
    Write-Host ("PASS  sticky geometry ({0} assertions)" -f $expected.Count)
}

# 2. paint: red header at the top, spacer under it, blue footer at the bottom.
#    Footer ink sits at the left edge ("FOOT", no padding); sample the solid blue
#    away from the glyphs so anti-aliased text edges (R167) cannot decide it.
if (-not (Test-Path $shot)) {
    $failures++
    Write-Host ("FAIL  sticky paint after scroll (probe exit {0}, no capture)" -f $paint.Code)
} else {
    $specs = @('10,10,cc0000', '300,20,cc0000', '10,300,eeeeee', '500,780,0000cc')
    & python $pixel $shot @specs 2>&1 | ForEach-Object { $_.ToString() }
    if ($LASTEXITCODE -ne 0) {
        $failures++
        Write-Host "FAIL  sticky paint after scroll"
    } else {
        Write-Host "PASS  sticky paint after scroll (header pinned, footer pinned)"
    }
}

Remove-TestRun $run $failures

if ($failures -gt 0) {
    Write-Host ("STICKY=FAIL ({0})" -f $failures)
    exit 1
}
Write-Host "STICKY=PASS"
exit 0
