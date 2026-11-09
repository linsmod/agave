# tools/run_sticky_tests.ps1
#
# Regression for `position: sticky` (hist-lessons R82).
#
# Two layers:
#   1. tools/pages/layout/sticky.html + sticky_measure.js through the probe:
#      geometry at several scroll offsets — a sticky `top:0` header stays at the
#      viewport top, a sticky `bottom:0` footer stays at the bottom until its
#      containing block ends, an inner overflow box pins its own sticky bar, and
#      hit-testing follows the shifted box (`elementFromPoint`).
#   2. the same page with `--shot`: what got *painted* after a scroll, which is
#      the half geometry cannot see (the sticky box must not blit away with the
#      rest of the content).
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

$tmp = Join-Path ([IO.Path]::GetTempPath()) ('agb-sticky-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

$failures = 0

function Invoke-Probe {
    param([string[]]$Extra)

    $stdout = Join-Path $tmp 'out.txt'
    $stderr = Join-Path $tmp 'err.txt'
    $args = @('--probe', "file:///$($page -replace '\\', '/')",
              '--eval-file', $measure, '--wait', '900', '--settle', '500',
              '--timeout', '60000') + $Extra

    $p = Start-Process -FilePath $exe -NoNewWindow -PassThru -Wait `
        -ArgumentList $args -RedirectStandardOutput $stdout -RedirectStandardError $stderr

    [pscustomobject]@{
        Code   = $p.ExitCode
        Stdout = (Get-Content $stdout -Raw) -as [string]
    }
}

# 1. geometry: every expectation below is the value Chrome reports for this page
#    at the probe's 1100x800 viewport.
$probe = Invoke-Probe -Extra @()
$out = $probe.Stdout

$expected = @(
    'pos=sticky',      # the declaration survives the cascade
    'h0=0',            # header at the top before scrolling
    'f0=770',          # sticky bottom pulls the footer into view (800-30)
    'h500=0',          # header pinned while scrolled
    'f500=770',        # footer still pinned
    'hit=head',        # hit-test follows the painted position
    'hmax=0',          # still pinned at the bottom of the document
    'fmaxNatural=326',  # footer released: it sits at its own offset
    's0=2', 's100=2', 's400=2'  # inner scroller keeps its bar at the box edge
)

$missing = @()
foreach ($want in $expected) {
    if ($out -notmatch [regex]::Escape($want)) { $missing += $want }
}

if ($probe.Code -ne 0 -or $missing.Count -gt 0) {
    $failures++
    Write-Host ("FAIL  sticky geometry (exit {0}): missing {1}" -f $probe.Code, ($missing -join ', '))
    Write-Host (($out -split "`n" | Select-Object -Last 2) -join "`n")
} else {
    Write-Host ("PASS  sticky geometry ({0} assertions)" -f $expected.Count)
}

# 2. paint: scroll to 500 in --eval, then capture. Red header must be at the
#    top, the spacer under it, the blue footer at the bottom edge.
#    The script must NOT call probeExit(): that posts __agb_exit__ and closes
#    the probe window before --shot can run, so the capture never happens and
#    this group fails with "no such file". Letting --settle finish is what
#    reaches the shot path.
# The probe writes the capture itself with fopen(); a path under %TEMP% fails
# with EACCES in this environment (see dev-debugging.md), so the shot goes next
# to the build output and the pixel check reads it from there.
$shot = Join-Path $build 'sticky_shot.bmp'
Remove-Item $shot -ErrorAction SilentlyContinue
$scrollJs = Join-Path $tmp 'scroll.js'
"window.scrollTo(0, 500);" |
    Set-Content -Path $scrollJs -Encoding ascii

$stdout = Join-Path $tmp 'shotout.txt'
$stderr = Join-Path $tmp 'shoterr.txt'
$p = Start-Process -FilePath $exe -NoNewWindow -PassThru -Wait -ArgumentList @(
    '--probe', "file:///$($page -replace '\\', '/')",
    '--eval-file', $scrollJs, '--wait', '900', '--settle', '500',
    '--shot', $shot, '--timeout', '60000'
) -RedirectStandardOutput $stdout -RedirectStandardError $stderr

# Footer ink sits at the left edge ("FOOT", no padding); sample the solid blue
# away from the glyphs so anti-aliased text edges (R167) cannot decide the test.
$specs = @('10,10,cc0000', '300,20,cc0000', '10,300,eeeeee', '500,780,0000cc')
& python $pixel $shot @specs 2>&1 | ForEach-Object { $_.ToString() }
if ($LASTEXITCODE -ne 0) {
    $failures++
    Write-Host "FAIL  sticky paint after scroll"
} else {
    Write-Host "PASS  sticky paint after scroll (header pinned, footer pinned)"
}

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

if ($failures -gt 0) {
    Write-Host ("STICKY=FAIL ({0})" -f $failures)
    exit 1
}
Write-Host "STICKY=PASS"
exit 0
