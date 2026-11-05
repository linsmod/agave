<#
.SYNOPSIS
  One-command layout regression: agave vs headless Chrome on every page under
  tools/pages/layout, plus the transform-repaint pixel test.

.DESCRIPTION
  Two kinds of check, both mechanical:

    * geometry  - tools/layout_diff.py runs the same lib_measure.js in agave and
                  in headless Chrome (viewport pinned to 1084x800) and diffs
                  every data-case rect + computed property.
    * pixels    - transform_repaint.html changes a transform from a page timer,
                  the probe screenshots the partial repaint, and pixel_check.py
                  asserts where the transformed boxes actually landed.

  Exit code is the verdict: 0 all green, 1 any failure.

.EXAMPLE
  pwsh -File tools/run_layout_tests.ps1
#>
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
Push-Location $root

try {
    $pages = Get-ChildItem 'tools/pages/layout' -Filter '*.html' |
             Sort-Object Name |
             ForEach-Object { 'tools/pages/layout/' + $_.Name }
    if (-not $pages) { throw 'no layout pages found' }

    $failed = 0

    Write-Host ('== geometry (' + $pages.Count + ' pages)')
    & python tools/layout_diff.py @pages
    if ($LASTEXITCODE -ne 0) { $failed++ }

    Write-Host '== paint: transform repaint'
    $bmp = Join-Path $root 'debug\layout_transform.bmp'
    $url = 'file:///' + (Join-Path $root 'tools/pages/layout/transform_repaint.html').Replace('\', '/')
    & (Join-Path $root 'build-zig-native\agave_shell.exe') --probe $url `
        --wait 1500 --settle 100 --timeout 30000 --shot $bmp | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'transform_repaint: probe failed'
        $failed++
    } else {
        & python tools/pixel_check.py $bmp `
            250,150,ff0000 450,150,ffffff 160,330,0000ff 105,305,ffffff 300,350,ffffff
        if ($LASTEXITCODE -ne 0) { $failed++ }
    }

    if ($failed) {
        Write-Host ('LAYOUT=FAIL (' + $failed + ' check group(s))')
        exit 1
    }
    Write-Host 'LAYOUT=PASS'
    exit 0
}
finally {
    Pop-Location
}
