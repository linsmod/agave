# tools/run_webp_tests.ps1
#
# WebP decoding (issues #19): the image pipeline sniffs the container format in
# createDecoder(); WebP was missing, so every .webp resource decoded to nothing
# (sizeAvail=0, broken image). The three fixtures cover the three container
# variants a page actually ships:
#
#   fixtures/webp_lossy.webp     VP8   (lossy, alpha-less)
#   fixtures/webp_lossless.webp  VP8L  (lossless)
#   fixtures/webp_alpha.webp     VP8   with alpha (transparent right half)
#
# Each is 40x30, generated with Pillow (method=0 so the fixtures stay tiny and
# deterministic). The probe loads them through `new Image()` and asserts
# naturalWidth/naturalHeight through the normal resource loading path, not the
# decoder directly.
#
# Expected wall time: ~2 s (one probe).
#
# Usage:
#   pwsh -File tools/run_webp_tests.ps1
#   pwsh -File tools/run_webp_tests.ps1 -BuildDir build-zig-native

param(
    [string]$BuildDir = 'build-zig-native'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir
$exe = Join-Path $build 'agave_shell.exe'
$fixtures = Join-Path $root 'tools/pages/fixtures'

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking
$run = New-TestRun -Suite 'webp'

$failures = 0

try {
    $fixtureUrl = 'file:///' + ($fixtures -replace '\\', '/') + '/'
    $js = @"
var names = ['webp_lossy', 'webp_lossless', 'webp_alpha'];
var out = [], done = 0;
for (var i = 0; i < names.length; ++i) {
  (function (name) {
    var img = new Image();
    img.onload = function () {
      out.push(name + '=' + img.naturalWidth + 'x' + img.naturalHeight);
      if (++done === names.length) probeExit(out.sort().join(' '));
    };
    img.onerror = function () {
      out.push(name + '=ERR');
      if (++done === names.length) probeExit(out.sort().join(' '));
    };
    img.src = '$fixtureUrl' + name + '.webp';
  })(names[i]);
}
"@
    $jsFile = Join-Path $run.Dir 'webp.js'
    Set-Content -Path $jsFile -Value $js -Encoding ascii

    $emptyPage = Join-Path $root 'tools/pages/cap_dom.html'
    $probe = Start-TestProbe -Run $run -Name 'decode' -Url ('file:///' + ($emptyPage -replace '\\', '/')) `
        -Wait 1500 -Settle 400 `
        -RawArgs @('--probe', ('file:///' + ($emptyPage -replace '\\', '/')), '--wait', '1500',
                   '--settle', '400', '--timeout', '30000', '--eval-file', $jsFile)

    $results = Complete-TestProbes -Jobs @($probe)
    $result = $results[0]
    $text = $result.Text

    $expected = @{
        'webp_lossy=40x30' = $false
        'webp_lossless=40x30' = $false
        'webp_alpha=40x30' = $false
    }
    $exitLine = ([regex]::Match($text, '\[exit\]\s*(.*)')).Groups[1].Value.Trim()

    if ($result.Code -ne 0) {
        Write-Host ("FAIL  webp: probe exit {0}" -f $result.Code)
        $failures++
    } else {
        foreach ($key in @($expected.Keys)) {
            if ($exitLine -match [regex]::Escape($key)) { $expected[$key] = $true }
        }
        $missing = @($expected.Keys | Where-Object { -not $expected[$_] })
        if ($missing.Count -gt 0 -or $exitLine -match 'ERR') {
            Write-Host ("FAIL  webp decode: got '{0}' (missing: {1})" -f $exitLine, ($missing -join ', '))
            $failures++
        } else {
            Write-Host ("PASS  webp decode ({0})" -f $exitLine)
        }
    }
} finally {
    Remove-TestRun $run
}

if ($failures -ne 0) {
    Write-Host ("WEBP CHECKS FAILED ({0})" -f $failures)
    exit 1
}

Write-Host 'ALL WEBP CHECKS PASSED'
exit 0
