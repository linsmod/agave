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
    * resize    - resize_probe.html is resized from 1100x800 to 500x400 through
                  the probe (real WM_SIZE path); the check asserts that vw/vh,
                  calc(100vw ...), @media breakpoints and the resize event all
                  follow the new viewport.
    * zoom      - the same page at --zoom 125: 100vw/50vh/calc are CSS pixels,
                  so they shrink with innerWidth instead of the device pixels.
    * cssom     - document.styleSheets / cssRules / <style>.sheet, including a
                  @media rule, @font-face and a <style> added after load.

  Exit code is the verdict: 0 all green, 1 any failure.

.EXAMPLE
  pwsh -File tools/run_layout_tests.ps1
#>
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
Push-Location $root

try {
    # scrollbar_hidden.html has no data-case elements on purpose: hiding the
    # scrollbar changes the viewport width, which cannot be compared against
    # Chrome's fixed emulation. It is asserted by the dedicated check below.
    $pages = Get-ChildItem 'tools/pages/layout' -Filter '*.html' |
             Where-Object { $_.Name -ne 'scrollbar_hidden.html' } |
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

    Write-Host '== paint: transform text (glyphs follow the matrix)'
    $bmp = Join-Path $root 'debug\layout_transform_text.bmp'
    $url = 'file:///' + (Join-Path $root 'tools/pages/layout/transform_text.html').Replace('\', '/')
    & (Join-Path $root 'build-zig-native\agave_shell.exe') --probe $url `
        --wait 1200 --settle 100 --timeout 30000 --shot $bmp | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'transform_text: probe failed'
        $failed++
    } else {
        # (8,8) inside the rotated tab; (20,120) only blue when the box really
        # rotated (an unrotated 140x56 box stops at y=56); (120,20) only blue
        # when it did not. The rect specs count ink: white glyphs inside the
        # tab, ink near the far end of the run (collapsed advances leave the
        # top of the column empty) and the doubled glyph area of #scale.
        & python tools/pixel_check.py $bmp `
            8,8,204080 20,120,204080 120,20,ffffff `
            0,0,55,139,ffffff,700 0,40,55,59,ffffff,250 280,20,360,100,000000,600
        if ($LASTEXITCODE -ne 0) { $failed++ }
    }

    Write-Host '== paint: background shorthand (image + colour / position / size)'
    $bmp = Join-Path $root 'debug\layout_background.bmp'
    $url = 'file:///' + (Join-Path $root 'tools/pages/layout/background_paint.html').Replace('\', '/')
    & (Join-Path $root 'build-zig-native\agave_shell.exe') --probe $url `
        --wait 1200 --settle 200 --timeout 30000 --shot $bmp | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'background_paint: probe failed'
        $failed++
    } else {
        # (5,5)/(95,95) prove the 10x10 texture repeats over the tint, (130,5)
        # and (175,55) that `center / cover` scaled it, (205,5)/(235,35) that
        # `0 0 / 40px 40px` did, (5,125)/(95,215) that a var() colour keeps the
        # relative url() alive after expansion, (125,125)/(215,215) that a data:
        # URI is decoded locally (background *and* <img>), and the page below is white.
        & python tools/pixel_check.py $bmp `
            5,5,ff0000 95,95,ff0000 130,5,ff0000 175,55,ff0000 `
            205,5,ff0000 235,35,ff0000 5,125,ff0000 95,215,ff0000 `
            125,125,ff0000 215,215,ff0000 250,130,ff0000 50,300,ffffff
        if ($LASTEXITCODE -ne 0) { $failed++ }
    }

    Write-Host '== scrollbar hiding (::-webkit-scrollbar)'
    $url = 'file:///' + (Join-Path $root 'tools/pages/layout/scrollbar_hidden.html').Replace('\', '/')
    $out = & (Join-Path $root 'build-zig-native\agave_shell.exe') --probe $url `
        --eval-file (Join-Path $root 'tools/pages/layout/check_scrollbar_hidden.js') `
        --wait 700 --settle 200 --timeout 30000 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'scrollbar_hidden: probe failed'
        $failed++
    } else {
        $text = $out -join "`n"
        $missing = 0
        foreach ($needle in @('client=1100x800 inner=1100x800', 'doc scroll=1100', 'full width=1100', 'box clientWidth=300 offsetWidth=300')) {
            if ($text -notmatch [regex]::Escape($needle)) {
                Write-Host ('scrollbar_hidden: missing ' + $needle)
                $missing++
            }
        }
        if ($missing) { $failed++ } else { Write-Host 'scrollbar_hidden: ok' }
    }

    Write-Host '== window resize (viewport units, media queries, resize event)'
    $url = 'file:///' + (Join-Path $root 'tools/pages/resize/resize_probe.html').Replace('\', '/')
    $out = & (Join-Path $root 'build-zig-native\agave_shell.exe') --probe $url --resize 500x400 `
        --eval-file (Join-Path $root 'tools/pages/resize/check_resize.js') `
        --wait 1200 --settle 200 --timeout 30000 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'resize: probe failed'
        $failed++
    } else {
        $text = $out -join "`n"
        $missing = 0
        foreach ($needle in @('viewport inner=500x400', 'vwBox=500x200',
                              'calcBox=400x150', 'mediaCSS=rgb(200, 30, 40)',
                              'mediaCSSwide=rgb(40, 30, 20)',
                              'matchMedia(max-width:600px)=true',
                              'resizeEvents addEventListener=500x400')) {
            if ($text -notmatch [regex]::Escape($needle)) {
                Write-Host ('resize: missing ' + $needle)
                $missing++
            }
        }
        if ($missing) { $failed++ } else { Write-Host 'resize: ok' }
    }

    Write-Host '== page zoom (vw/vh are CSS px like innerWidth)'
    $out = & (Join-Path $root 'build-zig-native\agave_shell.exe') --probe $url --zoom 125 `
        --eval-file (Join-Path $root 'tools/pages/resize/check_resize.js') `
        --wait 1000 --settle 200 --timeout 30000 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'zoom: probe failed'
        $failed++
    } else {
        $text = $out -join "`n"
        $missing = 0
        foreach ($needle in @('viewport inner=880x640', 'vwBox=880x320', 'calcBox=780x270',
                              'pctBox=440')) {
            if ($text -notmatch [regex]::Escape($needle)) {
                Write-Host ('zoom: missing ' + $needle)
                $missing++
            }
        }
        if ($missing) { $failed++ } else { Write-Host 'zoom: ok' }
    }

    Write-Host '== CSSOM (document.styleSheets / cssRules / style.sheet)'
    $url = 'file:///' + (Join-Path $root 'tools/pages/cap_cssom.html').Replace('\', '/')
    $out = & (Join-Path $root 'build-zig-native\agave_shell.exe') --probe $url `
        --eval-file (Join-Path $root 'tools/pages/check_cssom.js') `
        --wait 500 --settle 200 --timeout 30000 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'cssom: probe failed'
        $failed++
    } else {
        $text = $out -join "`n"
        $missing = 0
        foreach ($needle in @('sheets=2', 'list.item=function', 'sheet0.rules=3',
                              'rule0.sel=.a', 'rule0.color=rgb(1, 2, 3)',
                              'rule1.ml=7px', 'rule1.prio=important',
                              'rule2.media=all and (min-width: 1px)', 'rule2.inner=1',
                              'sheet1.fontface=5', 'stylesheet.sheet=3',
                              'stylesheet.owner=true', 'identity.list=true',
                              'identity.sheet=true', 'insert.index=3', 'insert.length=4',
                              'insert.color=rgb(9, 8, 7)', 'delete.length=3',
                              'doc.url=string', 'doc.title=cssom',
                              'dynamic.sheets=3', 'dynamic.rules=1')) {
            if ($text -notmatch [regex]::Escape($needle)) {
                Write-Host ('cssom: missing ' + $needle)
                $missing++
            }
        }
        if ($missing) { $failed++ } else { Write-Host 'cssom: ok' }
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
