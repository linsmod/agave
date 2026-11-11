<#
.SYNOPSIS
  One-command layout regression: agave vs headless Chrome on every page under
  tools/pages/layout, plus the transform-repaint pixel test.

.DESCRIPTION
  Two kinds of check, both mechanical:

    * geometry  - tools/layout_diff.py runs the same lib_measure.js in agave and
                  in headless Chrome (viewport pinned to 1084x800) and diffs
                  every data-case rect + computed property. Its agave probes run
                  in a small pool while Chrome measures the same pages.
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

  Everything runs in parallel: geometry (its own bounded probe pool + one
  Chrome) starts first, then all pixel/JS probes are launched together, each
  with its own profile/cwd. Expected wall time: ~8 s.

  Exit code is the verdict: 0 all green, 1 any failure.

.EXAMPLE
  pwsh -File tools/run_layout_tests.ps1
#>
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
Push-Location $root

try {
    Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking
    $run = New-TestRun -Suite 'layout'
    $shotDir = Join-Path $run.Dir 'shots'
    New-Item -ItemType Directory -Force -Path $shotDir | Out-Null
    $pixel = Join-Path $root 'tools/pixel_check.py'
    $fontAa = Join-Path $root 'tools/font_aa_check.py'

    # scrollbar_hidden.html has no data-case elements on purpose: hiding the
    # scrollbar changes the viewport width, which cannot be compared against
    # Chrome's fixed emulation. It is asserted by the dedicated check below.
    $pages = Get-ChildItem 'tools/pages/layout' -Filter '*.html' |
             Where-Object { $_.Name -ne 'scrollbar_hidden.html' } |
             Sort-Object Name |
             ForEach-Object { 'tools/pages/layout/' + $_.Name }
    if (-not $pages) { throw 'no layout pages found' }

    $failed = 0

    # ---- geometry starts now, the probe pool runs underneath -------------
    Write-Host ('== geometry (' + $pages.Count + ' pages)')
    # Geometry's own stdout/stderr are large; %TEMP% avoids the workspace
    # write filter that makes redirect files slow (the probes themselves keep
    # their profiles/shots in the build tree).
    $geomLog = Join-Path ([IO.Path]::GetTempPath()) ('agb-layout-geom-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force -Path $geomLog | Out-Null
    $geomOut = Join-Path $geomLog 'geometry.txt'
    $geomErr = Join-Path $geomLog 'geometry.err.txt'
    $geomArgs = @('tools/layout_diff.py') + @($pages)
    $geom = Start-Process -FilePath 'python' -NoNewWindow -PassThru `
        -ArgumentList ($geomArgs -join ' ') `
        -RedirectStandardOutput $geomOut -RedirectStandardError $geomErr

    # ---- probe cases ------------------------------------------------------
    $jobs = @{}
    $shots = @{}
    $probeTimeout = 30000

    function Start-ShotCase {
        param([string]$Name, [string]$Url, [int]$Wait, [int]$Settle, [string[]]$Extra, [string]$File)

        $shot = Join-Path $shotDir $File
        Remove-Item $shot -ErrorAction SilentlyContinue
        $raw = @('--probe', $Url, '--wait', "$Wait", '--settle', "$Settle",
                 '--timeout', "$probeTimeout", '--shot', $shot) + $Extra
        $script:shots[$Name] = $shot
        $script:jobs[$Name] = Start-TestProbe -Run $run -Name $Name -RawArgs $raw
    }

    function Start-EvalCase {
        param([string]$Name, [string]$Url, [string]$EvalFile, [int]$Wait, [int]$Settle, [string[]]$Extra = @())

        $script:jobs[$Name] = Start-TestProbe -Run $run -Name $Name -Url $Url -EvalFile $EvalFile `
            -Wait $Wait -Settle $Settle -Timeout $probeTimeout -Extra $Extra
    }

    $pageUrl = { param($rel) 'file:///' + ((Join-Path $root $rel) -replace '\\', '/') }

    $url = & $pageUrl 'tools/pages/layout/transform_repaint.html'
    Start-ShotCase 'transform_repaint' $url 1500 100 @() 'transform_repaint.bmp'

    $url = & $pageUrl 'tools/pages/layout/transform_text.html'
    Start-ShotCase 'transform_text' $url 1200 100 @() 'transform_text.bmp'

    $url = & $pageUrl 'tools/pages/font_antialias.html'
    Start-ShotCase 'font_antialias' $url 800 300 @() 'font_antialias.png'

    $url = & $pageUrl 'tools/pages/layout/background_paint.html'
    Start-ShotCase 'background_paint' $url 1200 200 @() 'background.bmp'

    $url = & $pageUrl 'tools/pages/layout/background_layers.html'
    Start-ShotCase 'background_layers' $url 800 200 @() 'background_layers.png'
    Start-EvalCase 'background_layers_computed' $url (Join-Path $root 'tools/pages/layout/check_background_layers.js') 600 200

    $url = & $pageUrl 'tools/pages/layout/border_radius.html'
    Start-ShotCase 'border_radius' $url 1000 100 @() 'radius.bmp'

    $url = & $pageUrl 'tools/pages/layout/fieldset_legend.html'
    Start-ShotCase 'fieldset_legend' $url 1000 100 @() 'fieldset.bmp'

    $selectUrl = & $pageUrl 'tools/pages/form_select.html'
    Start-ShotCase 'select_menu' $selectUrl 500 300 @('--click', '30,28') 'select.bmp'

    $js = Join-Path $root 'tools/pages/check_form_select.js'
    $wantKeys = 'v=two;i=1;n=3;t=Three;type=select-one;log=change=two idx=1'
    $jobs['select_keys'] = Start-TestProbe -Run $run -Name 'select_keys' `
        -RawArgs @('--probe', $selectUrl, '--click', '30,28', '--keys', '{down}{enter}',
                   '--eval-file', $js, '--wait', '400', '--settle', '300', '--timeout', "$probeTimeout")

    $wantMouse = 'v=three;i=2;n=3;t=Three;type=select-one;log=change=three idx=2'
    $jobs['select_mouse'] = Start-TestProbe -Run $run -Name 'select_mouse' `
        -RawArgs @('--probe', $selectUrl, '--click', '30,28', '--click', '30,79',
                   '--eval-file', $js, '--wait', '400', '--settle', '300', '--timeout', "$probeTimeout")

    $url = & $pageUrl 'tools/pages/shot_rect.html'
    Start-ShotCase 'shot_full' $url 800 100 @() 'shot_full.png'
    $shotFull = $shots['shot_full']
    Start-ShotCase 'shot_crop' $url 800 100 @('--shot-rect', '0,50,200,100') 'shot_rect.png'

    $url = & $pageUrl 'tools/pages/layout/custom_props.html'
    Start-EvalCase 'custom_props' $url (Join-Path $root 'tools/pages/layout/check_custom_props.js') 700 200

    $url = & $pageUrl 'tools/pages/layout/scrollbar_hidden.html'
    Start-EvalCase 'scrollbar_hidden' $url (Join-Path $root 'tools/pages/layout/check_scrollbar_hidden.js') 700 200

    $resizeUrl = & $pageUrl 'tools/pages/resize/resize_probe.html'
    Start-EvalCase 'resize' $resizeUrl (Join-Path $root 'tools/pages/resize/check_resize.js') 1200 200 @('--resize', '500x400')
    Start-EvalCase 'zoom' $resizeUrl (Join-Path $root 'tools/pages/resize/check_resize.js') 1000 200 @('--zoom', '125')

    $url = & $pageUrl 'tools/pages/cap_cssom.html'
    Start-EvalCase 'cssom' $url (Join-Path $root 'tools/pages/check_cssom.js') 500 200

    $url = & $pageUrl 'tools/pages/cap_computed.html'
    Start-EvalCase 'computed' $url (Join-Path $root 'tools/pages/check_computed.js') 500 200

    $url = & $pageUrl 'tools/pages/cap_flex_cssom.html'
    Start-EvalCase 'flexom' $url (Join-Path $root 'tools/pages/check_flex_cssom.js') 500 200

    $url = & $pageUrl 'tools/pages/cap_lifecycle.html'
    Start-EvalCase 'lifecycle' $url (Join-Path $root 'tools/pages/check_lifecycle.js') 500 200

    # ---- collect (each wait blocks on a probe that is already running) -----
    $res = @{}
    foreach ($name in @($jobs.Keys)) {
        $res[$name] = Complete-TestProbe -Job $jobs[$name]
    }

    function Check {
        param([string]$Name, [bool]$Ok, [string]$Detail)

        if ($Ok) {
            Write-Host ("PASS  {0}" -f $Name)
        } else {
            $script:failed++
            Write-Host ("FAIL  {0}" -f $Name)
            if ($Detail) { $Detail -split "`n" | Select-Object -First 6 | ForEach-Object { Write-Host ("      " + $_.TrimEnd()) } }
        }
    }

    function Test-Pixels {
        param([string]$Shot, [string[]]$Specs)

        if (-not (Test-Path $Shot)) { return @{ Ok = $false; Out = "no capture: $Shot" } }
        $out = (& python $pixel $Shot @Specs 2>&1) -join "`n"
        return @{ Ok = ($LASTEXITCODE -eq 0); Out = $out }
    }

    function Pixel-Check {
        param([string]$Name, [string]$Shot, [string[]]$Specs)

        $r = Test-Pixels $Shot $Specs
        Check $Name $r.Ok $r.Out
    }

    function Needles {
        param([string]$Name, $Result, [string[]]$Want, [string]$Forbid = '')

        if ($Result.Code -ne 0) { Check $Name $false ("probe exit {0}: {1}" -f $Result.Code, $Result.Text); return }
        $missing = @()
        foreach ($needle in $Want) {
            if ($Result.Text -notmatch [regex]::Escape($needle)) { $missing += $needle }
        }
        $bad = $missing.Count -gt 0
        if ($Forbid -and $Result.Text -match $Forbid) { $bad = $true; $missing += "forbidden /$Forbid/" }
        $detail = if ($bad) { 'missing: ' + ($missing -join ', ') } else { '' }
        Check $Name (-not $bad) $detail
    }

    function Get-PngSize([string]$Path) {
        $b = [System.IO.File]::ReadAllBytes($Path)
        $w = ([int]$b[16] -shl 24) -bor ([int]$b[17] -shl 16) -bor ([int]$b[18] -shl 8) -bor [int]$b[19]
        $h = ([int]$b[20] -shl 24) -bor ([int]$b[21] -shl 16) -bor ([int]$b[22] -shl 8) -bor [int]$b[23]
        return "${w}x${h}"
    }

    # ---- pixel groups -----------------------------------------------------
    Pixel-Check 'transform repaint' $shots['transform_repaint'] @(
        '250,150,ff0000', '450,150,ffffff', '160,330,0000ff', '105,305,ffffff', '300,350,ffffff')

    Pixel-Check 'transform text' $shots['transform_text'] @(
        '8,8,204080', '20,120,204080', '120,20,ffffff',
        '0,0,55,139,ffffff,400', '0,40,55,59,ffffff,170', '280,20,360,100,000000,600')

    $png = $shots['font_antialias']
    if (Test-Path $png) {
        $out = (& python $fontAa $png 0,0,420,100 --min-mid 500 2>&1) -join "`n"
        Check 'font antialiasing' ($LASTEXITCODE -eq 0) $out
    } else {
        Check 'font antialiasing' $false 'no capture'
    }

    Pixel-Check 'background paint' $shots['background_paint'] @(
        '5,5,ff0000', '95,95,ff0000', '130,5,ff0000', '175,55,ff0000',
        '205,5,ff0000', '235,35,ff0000', '5,125,ff0000', '95,215,ff0000',
        '125,125,ff0000', '215,215,ff0000', '250,130,ff0000', '50,300,ffffff')

    Pixel-Check 'background layers' $shots['background_layers'] @(
        '22,60,ff0000', '324,60,0000ff', '170,22,7f0080', '170,60,ffffff',
        '80,137,00ff00', '80,130,ffffff', '80,170,ffffff',
        '26,246,200,290,a9a9a9,15')

    Pixel-Check 'border radius' $shots['border_radius'] @(
        '21,21,ffffff', '120,44,f9fafb', '21,89,ffffff', '120,111,f9fafb',
        '20,111,d0d5dd', '20,180,000000', '120,180,3964fe')

    Pixel-Check 'fieldset legend' $shots['fieldset_legend'] @(
        '500,28,000000', '500,29,000000', '500,40,ffffff', '500,10,ffffff')

    # The menu's paint and the two property routes depend on the engine having
    # laid the popup out before the next input; a loaded machine can miss that
    # window. Each one gets one clean rerun after the rest of the suite is done.
    $menuShot = $shots['select_menu']
    $menuPixels = @('30,40,9db7cf', '60,45,9db7cf', '20,45,a0a0a0', '130,60,ffffff', '60,82,a0a0a0', '5,5,ffffff')
    $menu = Test-Pixels $menuShot $menuPixels
    if (-not $menu.Ok) {
        Remove-Item $menuShot -ErrorAction SilentlyContinue
        Start-ShotCase 'select_menu_retry' $selectUrl 500 300 @('--click', '30,28') 'select_retry.bmp'
        $null = Complete-TestProbe -Job $jobs['select_menu_retry']
        $menu = Test-Pixels $shots['select_menu_retry'] $menuPixels
    }
    Check 'select menu paint' $menu.Ok $menu.Out

    # <select> property routes: keyboard and mouse. The mouse route posts two
    # clicks; under a loaded machine their spacing can fall outside the
    # double-click window.
    $keys = $res['select_keys']
    if ($keys.Text -notmatch [regex]::Escape($wantKeys)) {
        $keys = Complete-TestProbe -Job (Start-TestProbe -Run $run -Name 'select_keys' `
            -RawArgs @('--probe', $selectUrl, '--click', '30,28', '--keys', '{down}{enter}',
                       '--eval-file', $js, '--wait', '400', '--settle', '300', '--timeout', "$probeTimeout"))
    }
    Check 'select keys' ($keys.Text -match [regex]::Escape($wantKeys)) $keys.Text

    $mouse = $res['select_mouse']
    if ($mouse.Text -notmatch [regex]::Escape($wantMouse)) {
        $mouse = Complete-TestProbe -Job (Start-TestProbe -Run $run -Name 'select_mouse' `
            -RawArgs @('--probe', $selectUrl, '--click', '30,28', '--click', '30,79',
                       '--eval-file', $js, '--wait', '400', '--settle', '300', '--timeout', "$probeTimeout"))
    }
    Check 'select mouse' ($mouse.Text -match [regex]::Escape($wantMouse)) $mouse.Text

    # ---- shot path --------------------------------------------------------
    $fullShot = $shots['shot_full']
    if (-not (Test-Path $fullShot)) {
        Check 'shot png' $false 'no capture'
    } else {
        $size = Get-PngSize $fullShot
        if ($size -ne '1100x800') { Check 'shot png' $false "size $size, want 1100x800" }
        else { Pixel-Check 'shot png' $fullShot @('10,10,c02020', '10,150,2050c0', '500,500,ffffff') }
    }

    $cropShot = $shots['shot_crop']
    if (-not (Test-Path $cropShot)) {
        Check 'shot rect' $false 'no capture'
    } else {
        $size = Get-PngSize $cropShot
        if ($size -ne '200x100') { Check 'shot rect' $false "size $size, want 200x100" }
        else { Pixel-Check 'shot rect' $cropShot @('10,10,c02020', '10,49,c02020', '10,50,2050c0', '10,90,2050c0', '199,0,c02020') }
    }

    # ---- JS groups --------------------------------------------------------
    Needles 'custom properties' $res['custom_props'] @(
        'sheetTint=rgb(1, 2, 3)', 'sheetPad=4px 8px', 'inlineOwn=rgb(1, 2, 3)',
        'inlineCssText=[--mine: rgb(1, 2, 3); ]', 'inlineColor=rgb(1, 2, 3)',
        'chainedColor=rgb(1, 2, 3)', 'removed=[var(--tint)]', 'afterRemove=[]')

    Needles 'scrollbar hidden' $res['scrollbar_hidden'] @(
        'client=1100x800 inner=1100x800', 'doc scroll=1100', 'full width=1100',
        'box clientWidth=300 offsetWidth=300')

    Needles 'resize' $res['resize'] @(
        'viewport inner=500x400', 'vwBox=500x200', 'calcBox=400x150',
        'mediaCSS=rgb(200, 30, 40)', 'mediaCSSwide=rgb(40, 30, 20)',
        'mediaCSSrem=rgb(2, 60, 120)', 'mediaCSSremWide=rgb(3, 4, 5)',
        'matchMedia(min-width:20rem)=true', 'matchMedia(min-width:40rem)=false',
        'matchMedia(max-width:600px)=true', 'resizeEvents addEventListener=500x400')

    Needles 'zoom' $res['zoom'] @(
        'viewport inner=880x640', 'vwBox=880x320', 'calcBox=780x270', 'pctBox=440')

    Needles 'cssom' $res['cssom'] @(
        'sheets=3', 'list.item=function', 'sheet0.rules=3',
        'rule0.sel=.a', 'rule0.color=rgb(1, 2, 3)',
        'rule1.ml=7px', 'rule1.prio=important',
        'rule2.media=all and (min-width: 1px)', 'rule2.inner=1',
        'sheet1.fontface=5', 'stylesheet.sheet=3',
        'stylesheet.owner=true', 'identity.list=true',
        'identity.sheet=true', 'insert.index=3', 'insert.length=4',
        'insert.color=rgb(9, 8, 7)', 'delete.length=3',
        'doc.url=string', 'doc.title=cssom',
        'dynamic.sheets=8', 'dynamic.rules=1',
        'bodystyle.sheet=1', 'bodystyle.color=rgb(11, 12, 13)',
        'append-body.sheet=1', 'append-body.color=rgb(21, 22, 23)',
        'append-root.sheet=1', 'append-root.color=rgb(31, 32, 33)',
        'append-root-late=1', 'append-root-late.color=rgb(41, 42, 43)',
        'supports.sel-type=true', 'supports.sel-pseudo=true',
        'supports.sel-attr=true', 'supports.sel-bad=false',
        'page.count=2', 'page.type=6', 'page.selector=:left',
        'page.styleLen=4', 'page.margin=2cm',
        'pct.plain=267', 'pct.calc=267')

    Needles 'computed style' $res['computed'] @(
        'a.margin=ok', 'a.padding=ok', 'a.border=ok',
        'a.borderWidth=ok', 'a.borderTop=ok', 'a.camel=ok',
        'a.styleCamel=ok',
        'b.borderEmpty=ok', 'b.borderWidth=ok',
        'b.borderStyle=ok', 'b.borderColor=ok',
        'c.radius=ok', 'c.background=ok',
        'd.font=ok', 'd.lineHeight=ok',
        'e.listStyle=ok', 'e.overflow=ok', 'e.flex=ok',
        'f.radius=ok', 'g.margin=ok', 'h.overflowXY=ok',
        'i.radius=ok', 'i.tl=ok', 'i.tr=ok',
        'j.outline=ok', 'j.outlineWidth=ok', 'a.outline=ok',
        'l.background=ok', 'l.origin=ok', 'l.clip=ok',
        'named.missing=ok', 'named.minWidth=ok',
        'named.webkitAppearance=ok', 'named.length=ok',
        'named.item=ok', 'spelling.camelEmpty=ok')

    Needles 'background layers computed' $res['background_layers_computed'] @(
        'bg.image=2', 'bg.clip=padding-box, border-box',
        'bg.origin=padding-box, border-box', 'bg.shadowSpread=ok',
        'bg.placeholderAttr=ok', 'bg.dpr=ok')

    Needles 'flex computed' $res['flexom'] @(
        'a.flex=2 3 40px', 'a.grow=2', 'a.shrink=3',
        'a.basis=40px',
        'b.flex=1 1 0%', 'c.flex=1 1 auto',
        'd.flex=0 0 auto', 'e.flex=4 0.5 25%',
        'f.basis=0px', 'g.basis=calc(50% - 4px)',
        'h.basis=33.333%',
        'i.flex=1 1 calc(50% - 4px)',
        'j.flex=0 0 calc(25% + 10px)',
        'gv.shrink=2', 'gv.basis=30px',
        'set.flex=1 7 5px')

    $lc = $res['lifecycle']
    Needles 'DOM lifecycle' $lc @(
        'head:ready=loading', 'head:body=null', 'head:head=HEAD',
        'body:ready=loading', 'body:el=BODY',
        'dcl:ready=interactive', 'dcl:body=BODY',
        'load:ready=complete', 'load:body=BODY',
        'appended=yes') 'branch=WRONG'

    # ---- geometry result --------------------------------------------------
    $geom.WaitForExit()
    $geomText = if (Test-Path $geomOut) { Get-Content $geomOut -Raw } else { '' }
    $geomText += if (Test-Path $geomErr) { Get-Content $geomErr -Raw } else { '' }
    Write-Host $geomText
    if ($geom.ExitCode -ne 0) { $failed++ }

    if ($failed -eq 0) { Remove-Item -Recurse -Force $geomLog -ErrorAction SilentlyContinue }
    Remove-TestRun $run $failed

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
