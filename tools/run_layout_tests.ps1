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
                  @media rule, @font-face, @page, CSS.supports('selector(...)')
                  and <style> elements added after load (in body and in <html>).
    * flexom    - getComputedStyle(el).flex / .flexGrow / .flexShrink /
                  .flexBasis, compared with the strings headless Chrome reports
                  for the same page (cap_flex_cssom.html, R115).
    * shot      - the probe's capture path: --shot writes PNG itself and
                  --shot-rect crops in client pixels (shot_rect.html).
    * lifecycle - document.readyState is loading/interactive/complete as the
                  parser advances, and document.body is null (not undefined)
                  until <body> exists - the branch bilibili's log-reporter
                  takes when it decides whether to wait for DOMContentLoaded.

  Exit code is the verdict: 0 all green, 1 any failure.

.EXAMPLE
  pwsh -File tools/run_layout_tests.ps1
#>
$ErrorActionPreference = 'Stop'

# Scratch captures (BMP/PNG) go next to the executable, not into debug/: the
# headless probe writes them from inside its sandbox, whose only writable tree is
# the build directory (the same reason run_media_tests.ps1 keeps its shots there).
# With them under debug/ every shot silently fails and the pixel groups report
# "probe failed".
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
    $bmp = Join-Path $root 'build-zig-native\layout_transform.bmp'
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
    $bmp = Join-Path $root 'build-zig-native\layout_transform_text.bmp'
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
    $bmp = Join-Path $root 'build-zig-native\layout_background.bmp'
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

    Write-Host '== paint: border radius scaling (oversized radii)'
    $bmp = Join-Path $root 'build-zig-native\layout_radius.bmp'
    $url = 'file:///' + (Join-Path $root 'tools/pages/layout/border_radius.html').Replace('\', '/')
    & (Join-Path $root 'build-zig-native\agave_shell.exe') --probe $url `
        --wait 1000 --settle 100 --timeout 30000 --shot $bmp | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'border_radius: probe failed'
        $failed++
    } else {
        # #overflow is a 200x48 pill at (20,20): the corner pixel inside the
        # border box must stay white (a square border painted it grey), the
        # centre keeps the fill. (20,90) is the control box's corner and must
        # be the black border.
        & python tools/pixel_check.py $bmp `
            21,21,ffffff 120,44,f9fafb 20,90,000000 120,112,3964fe
        if ($LASTEXITCODE -ne 0) { $failed++ }
    }

    Write-Host '== shot: PNG output + --shot-rect crop'
    # Pins the capture path itself: the probe encodes PNG by itself (no BMP
    # round trip) and --shot-rect crops in client pixels. shot_rect.html is
    # red on rows 0..99 and blue on 100..199, so a cropped capture proves the
    # offset, the height AND the encoder at once.
    function Get-PngSize([string]$Path) {
        $b = [System.IO.File]::ReadAllBytes($Path)
        $w = ([int]$b[16] -shl 24) -bor ([int]$b[17] -shl 16) -bor ([int]$b[18] -shl 8) -bor [int]$b[19]
        $h = ([int]$b[20] -shl 24) -bor ([int]$b[21] -shl 16) -bor ([int]$b[22] -shl 8) -bor [int]$b[23]
        return "${w}x${h}"
    }
    $url = 'file:///' + (Join-Path $root 'tools/pages/shot_rect.html').Replace('\', '/')
    $full = Join-Path $root 'build-zig-native\shot_full.png'
    & (Join-Path $root 'build-zig-native\agave_shell.exe') --probe $url `
        --wait 800 --settle 100 --timeout 30000 --shot $full | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'shot png: probe failed'
        $failed++
    } else {
        $size = Get-PngSize $full
        if ($size -ne '1100x800') { Write-Host "shot png: size $size, want 1100x800"; $failed++ }
        & python tools/pixel_check.py $full 10,10,c02020 10,150,2050c0 500,500,ffffff
        if ($LASTEXITCODE -ne 0) { $failed++ }
    }

    $crop = Join-Path $root 'build-zig-native\shot_rect.png'
    & (Join-Path $root 'build-zig-native\agave_shell.exe') --probe $url `
        --wait 800 --settle 100 --timeout 30000 --shot $crop --shot-rect 0,50,200,100 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'shot rect: probe failed'
        $failed++
    } else {
        $size = Get-PngSize $crop
        if ($size -ne '200x100') { Write-Host "shot rect: size $size, want 200x100"; $failed++ }
        # Rows 0..49 of the crop are still red, 50..99 are already blue.
        & python tools/pixel_check.py $crop 10,10,c02020 10,49,c02020 10,50,2050c0 10,90,2050c0 199,0,c02020
        if ($LASTEXITCODE -ne 0) { $failed++ }
    }

    Write-Host '== custom properties (computed values + inline API)'
    $url = 'file:///' + (Join-Path $root 'tools/pages/layout/custom_props.html').Replace('\', '/')
    $out = & (Join-Path $root 'build-zig-native\agave_shell.exe') --probe $url `
        --eval-file (Join-Path $root 'tools/pages/layout/check_custom_props.js') `
        --wait 700 --settle 200 --timeout 30000 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'custom_props: probe failed'
        $failed++
    } else {
        $text = $out -join "`n"
        $missing = 0
        foreach ($needle in @('sheetTint=rgb(1, 2, 3)', 'sheetPad=4px 8px', 'inlineOwn=rgb(1, 2, 3)',
                              'inlineCssText=[--mine: rgb(1, 2, 3); ]', 'inlineColor=rgb(1, 2, 3)',
                              'chainedColor=rgb(1, 2, 3)', 'removed=[var(--tint)]', 'afterRemove=[]')) {
            if ($text -notmatch [regex]::Escape($needle)) {
                Write-Host ('custom_props: missing ' + $needle)
                $missing++
            }
        }
        if ($missing) { $failed++ } else { Write-Host 'custom_props: ok' }
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
        foreach ($needle in @('sheets=3', 'list.item=function', 'sheet0.rules=3',
                              'rule0.sel=.a', 'rule0.color=rgb(1, 2, 3)',
                              'rule1.ml=7px', 'rule1.prio=important',
                              'rule2.media=all and (min-width: 1px)', 'rule2.inner=1',
                              'sheet1.fontface=5', 'stylesheet.sheet=3',
                              'stylesheet.owner=true', 'identity.list=true',
                              'identity.sheet=true', 'insert.index=3', 'insert.length=4',
                              'insert.color=rgb(9, 8, 7)', 'delete.length=3',
                              'doc.url=string', 'doc.title=cssom',
                              'dynamic.sheets=8', 'dynamic.rules=1',
                              # <style> is a sheet wherever it is: written in the
                              # body, appended into body, appended to <html>
                              # after <body> (css3test's harness does that).
                              'bodystyle.sheet=1', 'bodystyle.color=rgb(11, 12, 13)',
                              'append-body.sheet=1', 'append-body.color=rgb(21, 22, 23)',
                              'append-root.sheet=1', 'append-root.color=rgb(31, 32, 33)',
                              'append-root-late=1', 'append-root-late.color=rgb(41, 42, 43)',
                              # CSS.supports('selector(...)') goes through the
                              # selector parser and leaves no probe sheet behind.
                              'supports.sel-type=true', 'supports.sel-pseudo=true',
                              'supports.sel-attr=true', 'supports.sel-bad=false',
                              # @page is a PAGE_RULE with its declaration block.
                              'page.count=2', 'page.type=6', 'page.selector=:left',
                              'page.styleLen=4', 'page.margin=2cm',
                              # width:33.333% and width:calc(33.333%) must
                              # round the same way (267 = round(33.333% * 800)).
                              'pct.plain=267', 'pct.calc=267')) {
            if ($text -notmatch [regex]::Escape($needle)) {
                Write-Host ('cssom: missing ' + $needle)
                $missing++
            }
        }
        if ($missing) { $failed++ } else { Write-Host 'cssom: ok' }
    }

    Write-Host '== flex computed values (flex / flex-grow / flex-shrink / flex-basis)'
    $url = 'file:///' + (Join-Path $root 'tools/pages/cap_flex_cssom.html').Replace('\', '/')
    $out = & (Join-Path $root 'build-zig-native\agave_shell.exe') --probe $url `
        --eval-file (Join-Path $root 'tools/pages/check_flex_cssom.js') `
        --wait 500 --settle 200 --timeout 30000 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'flexom: probe failed'
        $failed++
    } else {
        $text = $out -join "`n"
        $missing = 0
        foreach ($needle in @(
                              # Expected strings come from headless Chrome on
                              # the same page (see check_flex_cssom.js).
                              'a.flex=2 3 40px', 'a.grow=2', 'a.shrink=3',
                              'a.basis=40px',
                              'b.flex=1 1 0%', 'c.flex=1 1 auto',
                              'd.flex=0 0 auto', 'e.flex=4 0.5 25%',
                              'f.basis=0px', 'g.basis=calc(50% - 4px)',
                              'h.basis=33.333%',
                              # calc() inside the `flex` shorthand (the encoded
                              # dimension token the sheet rewriter emits).
                              'i.flex=1 1 calc(50% - 4px)',
                              'j.flex=0 0 calc(25% + 10px)',
                              'gv.shrink=2', 'gv.basis=30px',
                              'set.flex=1 7 5px')) {
            if ($text -notmatch [regex]::Escape($needle)) {
                Write-Host ('flexom: missing ' + $needle)
                $missing++
            }
        }
        if ($missing) { $failed++ } else { Write-Host 'flexom: ok' }
    }

    Write-Host '== DOM lifecycle (document.readyState / document.body before <body>)'
    $url = 'file:///' + (Join-Path $root 'tools/pages/cap_lifecycle.html').Replace('\', '/')
    $out = & (Join-Path $root 'build-zig-native\agave_shell.exe') --probe $url `
        --eval-file (Join-Path $root 'tools/pages/check_lifecycle.js') `
        --wait 500 --settle 200 --timeout 30000 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'lifecycle: probe failed'
        $failed++
    } else {
        $text = $out -join "`n"
        $missing = 0
        foreach ($needle in @('head:ready=loading', 'head:body=null', 'head:head=HEAD',
                              'body:ready=loading', 'body:el=BODY',
                              'dcl:ready=interactive', 'dcl:body=BODY',
                              'load:ready=complete', 'load:body=BODY',
                              'appended=yes')) {
            if ($text -notmatch [regex]::Escape($needle)) {
                Write-Host ('lifecycle: missing ' + $needle)
                $missing++
            }
        }
        # The head script must have taken the DOMContentLoaded branch.
        if ($text -match 'branch=WRONG') {
            Write-Host 'lifecycle: head script saw a non-loading readyState'
            $missing++
        }
        if ($missing) { $failed++ } else { Write-Host 'lifecycle: ok' }
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
