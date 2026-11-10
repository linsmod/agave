# run_selection_tests.ps1 - regression for text selection and its shortcuts.
#
# Everything runs through the headless probe against tools/pages/cap_selection.html:
# real WM_MOUSEMOVE/WM_LBUTTONDOWN/WM_LBUTTONUP drags (--drag / --click /
# --shift-click), simulated modifiers (--mod, the control's probe key state),
# real WM_KEYDOWN/WM_KEYUP typed keys (--keys) and the Editor command entry the
# context menu uses (--editor). Selection state is read back through the DOM
# Selection API (window.getSelection / document.createRange / execCommand).
#
#   pwsh -File tools/run_selection_tests.ps1            # local backend
#   pwsh -File tools/run_selection_tests.ps1 -Process   # + process backend cases
#
# The shell's native right-click menu itself is not automated (TrackPopupMenu
# blocks, and the probe replaces the host context-menu callback with its own
# logger); its Copy / Select all items call agave_webview_editor_command(), the
# exact path the 'editor Copy' / 'editor SelectAll' cases cover.
#
# Expected runtime: about 35-45 s (local), +15 s with -Process; serial by design
# (the clipboard cases touch the shared OS clipboard, so the suite overwrites
# it - it does not save/restore it).
param([switch]$Process)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root 'build-zig-native\agave_shell.exe'
$page = 'file:///' + ((Join-Path $PSScriptRoot 'pages\cap_selection.html') -replace '\\', '/')
$shot = Join-Path $root 'build-zig-native\selection_shot.png'

if (-not (Test-Path $exe)) {
    "missing $exe - build first (pwsh -File tools/build_project.ps1)"
    exit 1
}

$failures = 0
$cases    = 0

# One probe run. $Extra is the argument list before --eval; $Eval is the script;
# $Expect is matched against the [exit] line (and $ExitCode against the probe's
# exit code when given).
function Run-Probe {
    param(
        [string]$Label,
        [object[]]$Extra,
        [string]$Eval,
        [string]$Expect,
        [int]$ExitCode = 0,
        [string]$Backend = 'local'
    )

    $script:cases++
    $backendArgs = @()
    if ($Backend -eq 'process') { $backendArgs = @('--backend', 'process') }

    $out  = & $exe --probe $page @backendArgs @Extra --wait 250 --settle 300 --timeout 25000 --eval $Eval 2>&1
    $code = $LASTEXITCODE
    $text = ($out | Out-String)
    $line = (($text -split "`n") | Where-Object { $_ -match '\[exit\]' }) -join ''
    # Match only what the script printed (the line also carries a timestamp and
    # the probe prints "[exit] " before the payload).
    $payload = ($line -replace '^.*\[exit\] ?', '').TrimEnd("`r")

    if (($code -eq $ExitCode) -and ($payload -match $Expect)) {
        "PASS  $Label   $($line.Trim())"
    } else {
        $script:failures++
        "FAIL  $Label   exit=$code (want $ExitCode) expected /$Expect/"
        "      got: $line"
    }
}

# Clipboard helper: one probe run that leaves text on the clipboard, then the
# assertion on Get-Clipboard.
function Run-Clipboard {
    param(
        [string]$Label,
        [object[]]$Extra,
        [string]$Eval,
        [string]$Expect
    )

    $script:cases++
    Set-Clipboard -Value 'agave-precondition'
    $out  = & $exe --probe $page @Extra --wait 250 --settle 450 --timeout 25000 --eval $Eval 2>&1
    $code = $LASTEXITCODE
    $clip = (Get-Clipboard) -join "`n"

    if (($code -eq 0) -and ($clip -ceq $Expect)) {
        "PASS  $Label   clipboard='$clip'"
    } else {
        $script:failures++
        "FAIL  $Label   exit=$code clipboard='$clip' expected '$Expect'"
        "      got: $((($out | Out-String) -split "`n" | Where-Object { $_ -match '\[exit\]' }) -join '')"
    }
}

$selectText = "probeExit(getSelection().toString())"
$selLength  = "probeExit('len=' + getSelection().toString().length)"

Write-Host '== drag selection (local) =='
# (15,33) is inside "The quick brown fox..." (p1 starts at y=26/16px font).
Run-Probe 'drag forward' @('--drag', '15,33,200,33') $selectText ' quick brown fox jum'
Run-Probe 'drag backward' @('--drag', '200,33,15,33') $selectText '^The quick brown fox j$'
Run-Probe 'drag multi-paragraph' @('--drag', '15,33,200,150') $selectText 'quick brown fox jumps over the lazy dog\.\s*Second paragraph with some more'
Run-Probe 'drag is range' @('--drag', '15,33,200,33') "probeExit(getSelection().type + ':' + getSelection().rangeCount)" 'Range:1'

Write-Host '== click gestures =='
Run-Probe 'double click word' @('--click', '62,33', '--click', '62,33') $selectText '^quick$'
Run-Probe 'double click dblclick event' @('--click', '62,33', '--click', '62,33') "probeExit('dbl=' + window.__dblEvents)" 'dbl=1'
Run-Probe 'triple click paragraph' @('--click', '62,33', '--click', '62,33', '--click', '62,33') $selectText '^The quick brown fox jumps over the lazy dog\.\s*$'
Run-Probe 'shift click extends' @('--click', '62,33', '--shift-click', '120,33') $selectText 'ick bro'
Run-Probe 'selectionchange fires' @('--drag', '15,33,200,33') "probeExit('events=' + (window.__selEvents > 0 ? 1 : 0))" 'events=1'

Write-Host '== user-select =='
Run-Probe 'user-select none line' @('--drag', '15,205,300,205') $selectText '^$'
Run-Probe 'user-select text inside none' @('--drag', '15,230,105,230') $selectText 't this span'

Write-Host '== keyboard shortcuts (simulated modifiers) =='
Run-Probe 'ctrl+a page' @('--mod', 'ctrl', '--keys', 'a') $selLength 'len=199'
Run-Probe 'shift+right extends' @('--click', '62,33', '--mod', 'shift', '--keys', '{right}{right}') $selectText 'ic'
Run-Probe 'ctrl+shift+right words' @('--click', '62,33', '--mod', 'ctrl+shift', '--keys', '{right}{right}') $selectText 'ick brown'
Run-Clipboard 'ctrl+c copies selection' @('--drag', '15,33,200,33', '--mod', 'ctrl', '--keys', 'c') 'probeExit(1)' ' quick brown fox jum'

Write-Host '== editor commands (context menu path) =='
Run-Clipboard 'editor Copy' @('--drag', '15,33,200,33', '--editor', 'Copy') 'probeExit(1)' ' quick brown fox jum'
Run-Probe 'editor SelectAll' @('--editor', 'SelectAll') $selLength 'len=199'

Write-Host '== field editing =='
Run-Probe 'ctrl+a in field selects field text' @('--pre-eval', "document.getElementById('field').focus()", '--mod', 'ctrl', '--keys', 'a') "probeExit(getSelection().toString())" '^editable text field$'
Run-Clipboard 'ctrl+x cuts field' @('--pre-eval', "document.getElementById('field').focus()", '--mod', 'ctrl', '--keys', 'x') "probeExit('val=' + JSON.stringify(document.getElementById('field').value))" 'editable text field'
Set-Clipboard -Value 'PASTED'
Run-Probe 'ctrl+v pastes field' @('--click', '100,281', '--mod', 'ctrl', '--keys', 'v') "probeExit('val=' + document.getElementById('field').value)" 'PASTED'

Write-Host '== DOM Selection / Range API =='
$apiJs = "var g=window.getSelection(), r=document.createRange(), p=document.getElementById('p1');" +
         "r.setStart(p.firstChild,0); r.setEnd(p.firstChild,19); g.removeAllRanges(); g.addRange(r);" +
         "var b=r.getBoundingClientRect();" +
         "probeExit(JSON.stringify({t:g.toString(),type:g.type,rc:g.rangeCount,col:g.isCollapsed,a:g.anchorOffset,f:g.focusOffset," +
         "same:window.getSelection()===document.getSelection(),sc:r.startContainer===p.firstChild,so:r.startOffset," +
         "ec:r.endContainer===p.firstChild,eo:r.endOffset,left:b.left,top:b.top,w:b.width,h:b.height," +
         "rects:r.getClientRects().length,sup:document.queryCommandSupported('copy'),en:document.queryCommandEnabled('copy')}))"
Run-Probe 'selection + range API' @() $apiJs '"t":"The quick brown fox","type":"Range","rc":1,"col":false,"a":0,"f":19,"same":true,"sc":true,"so":0,"ec":true,"eo":19,"left":10,"top":26.*"rects":1,"sup":true,"en":true'
Run-Clipboard 'execCommand copy' @() "var g=getSelection(),r=document.createRange(),p=document.getElementById('p2');r.setStart(p.firstChild,7);r.setEnd(p.firstChild,17);g.removeAllRanges();g.addRange(r);probeExit(document.execCommand('copy'))" 'paragraph ' 
Run-Probe 'execCommand selectAll' @() "probeExit('n=' + (document.execCommand('selectAll') ? getSelection().toString().length : -1))" 'n=199'
Run-Probe 'execCommand unknown false' @() "probeExit('bogus=' + document.execCommand('bogus'))" 'bogus=false'

Write-Host '== selection colour (pixels) =='
# Active selection: Chrome's rgb(51,103,209); our painting pipeline lands one
# unit low (#3367D0), so the check allows a small tolerance.
& $exe --probe $page --drag 15,33,200,33 --wait 250 --settle 400 --timeout 25000 `
    --shot $shot --shot-rect 0,0,300,60 --eval 1 2>&1 | Out-Null
$script:cases++
$px = (& python (Join-Path $PSScriptRoot 'pixel_check.py') $shot 20,20,280,45,3367D0,300 --tol 4 2>&1) -join ' '
if ($LASTEXITCODE -eq 0) {
    "PASS  selection highlight colour   $($px.Trim())"
} else {
    $script:failures++
    "FAIL  selection highlight colour   $($px.Trim())"
}

if ($Process) {
    Write-Host '== process backend =='
    Run-Probe 'drag selection' @('--drag', '15,33,200,33') $selectText ' quick brown fox jum' -Backend 'process'
    Run-Probe 'ctrl+a page' @('--mod', 'ctrl', '--keys', 'a') $selLength 'len=199' -Backend 'process'
    Run-Probe 'user-select none' @('--drag', '15,205,300,205') $selectText '^$' -Backend 'process'
    Run-Probe 'double click word' @('--click', '62,33', '--click', '62,33') $selectText '^quick$' -Backend 'process'

    $script:cases++
    Set-Clipboard -Value 'agave-precondition'
    $out = & $exe --probe $page --backend process --drag 15,33,200,33 --mod ctrl --keys 'c' `
        --wait 250 --settle 500 --timeout 25000 --eval 'probeExit(1)' 2>&1
    $clip = (Get-Clipboard) -join "`n"
    if (($LASTEXITCODE -eq 0) -and ($clip -ceq ' quick brown fox jum')) {
        "PASS  ctrl+c copies selection   clipboard='$clip'"
    } else {
        $script:failures++
        "FAIL  ctrl+c copies selection   exit=$LASTEXITCODE clipboard='$clip'"
    }

    $script:cases++
    Set-Clipboard -Value 'agave-precondition'
    $out = & $exe --probe $page --backend process --drag 15,33,200,33 --editor Copy `
        --wait 250 --settle 500 --timeout 25000 --eval 'probeExit(1)' 2>&1
    $clip = (Get-Clipboard) -join "`n"
    if (($LASTEXITCODE -eq 0) -and ($clip -ceq ' quick brown fox jum')) {
        "PASS  editor Copy (process)   clipboard='$clip'"
    } else {
        $script:failures++
        "FAIL  editor Copy (process)   exit=$LASTEXITCODE clipboard='$clip'"
    }
}

""
if ($failures -eq 0) {
    "ALL SELECTION CHECKS PASSED ($cases cases)"
    exit 0
}
"$failures of $cases SELECTION CHECKS FAILED"
exit 1
