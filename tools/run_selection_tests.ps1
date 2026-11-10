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
# Parallel by design: every case is one probe with its own AGB_USERDATA and its
# own mock clipboard file (AGB_CLIPBOARD_FILE), so the copies no longer race on
# the shared Win32 clipboard - and a test run cannot clobber the developer's
# clipboard. Expected wall time: ~4 s (local), ~5 s with -Process.
param([switch]$Process)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root 'build-zig-native\agave_shell.exe'
$page = 'file:///' + ((Join-Path $PSScriptRoot 'pages\cap_selection.html') -replace '\\', '/')

if (-not (Test-Path $exe)) {
    "missing $exe - build first (pwsh -File tools/build_project.ps1)"
    exit 1
}

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force
$run = New-TestRun -Suite 'selection'
$pixel = Join-Path $PSScriptRoot 'pixel_check.py'

$failures = 0
$cases    = 0

function Check {
    param([string]$Label, [bool]$Ok, [string]$Detail)

    $script:cases++
    if ($Ok) {
        "PASS  $Label   $Detail"
    } else {
        $script:failures++
        "FAIL  $Label"
        if ($Detail) { "      $Detail" }
    }
}

$selectText = "probeExit(getSelection().toString())"
$selLength  = "probeExit('len=' + getSelection().toString().length)"

# ---------------------------------------------------------------------------
# Case table: name, extra probe args, eval, expected payload, optional backend
# and clipboard precondition.
# ---------------------------------------------------------------------------
$list = @(
    @{ Name = 'drag forward';          Args = @('--drag', '15,33,200,33'); Eval = $selectText; Expect = ' quick brown fox jum' },
    @{ Name = 'drag backward';         Args = @('--drag', '200,33,15,33'); Eval = $selectText; Expect = '^The quick brown fox j$' },
    @{ Name = 'drag multi-paragraph';  Args = @('--drag', '15,33,200,150'); Eval = $selectText; Expect = 'quick brown fox jumps over the lazy dog\.\s*Second paragraph with some more' },
    @{ Name = 'drag is range';         Args = @('--drag', '15,33,200,33'); Eval = "probeExit(getSelection().type + ':' + getSelection().rangeCount)"; Expect = 'Range:1' },

    @{ Name = 'double click word';     Args = @('--click','62,33','--click','62,33'); Eval = $selectText; Expect = '^quick$' },
    @{ Name = 'double click dblclick'; Args = @('--click','62,33','--click','62,33'); Eval = "probeExit('dbl=' + window.__dblEvents)"; Expect = 'dbl=1' },
    @{ Name = 'triple click paragraph'; Args = @('--click','62,33','--click','62,33','--click','62,33'); Eval = $selectText; Expect = '^The quick brown fox jumps over the lazy dog\.\s*$' },
    @{ Name = 'shift click extends';   Args = @('--click','62,33','--shift-click','120,33'); Eval = $selectText; Expect = 'ick bro' },
    @{ Name = 'selectionchange fires'; Args = @('--drag','15,33,200,33'); Eval = "probeExit('events=' + (window.__selEvents > 0 ? 1 : 0))"; Expect = 'events=1' },

    @{ Name = 'user-select none line'; Args = @('--drag','15,205,300,205'); Eval = $selectText; Expect = '^$' },
    @{ Name = 'user-select text inside none'; Args = @('--drag','15,230,105,230'); Eval = $selectText; Expect = 't this span' },

    @{ Name = 'ctrl+a page';           Args = @('--mod','ctrl','--keys','a'); Eval = $selLength; Expect = 'len=199' },
    @{ Name = 'shift+right extends';   Args = @('--click','62,33','--mod','shift','--keys','{right}{right}'); Eval = $selectText; Expect = 'ic' },
    @{ Name = 'ctrl+shift+right words'; Args = @('--click','62,33','--mod','ctrl+shift','--keys','{right}{right}'); Eval = $selectText; Expect = 'ick brown' },
    @{ Name = 'ctrl+c copies selection'; Args = @('--drag','15,33,200,33','--mod','ctrl','--keys','c'); Eval = 'probeExit(1)'; Expect = '^1$'; Clipboard = ' quick brown fox jum' },

    @{ Name = 'editor Copy';           Args = @('--drag','15,33,200,33','--editor','Copy'); Eval = 'probeExit(1)'; Expect = '^1$'; Clipboard = ' quick brown fox jum' },
    @{ Name = 'editor SelectAll';      Args = @('--editor','SelectAll'); Eval = $selLength; Expect = 'len=199' },

    @{ Name = 'ctrl+a in field';       Args = @('--pre-eval', "document.getElementById('field').focus()", '--mod', 'ctrl', '--keys', 'a'); Eval = "probeExit(getSelection().toString())"; Expect = '^editable text field$' },
    @{ Name = 'ctrl+x cuts field';     Args = @('--pre-eval', "document.getElementById('field').focus()", '--mod', 'ctrl', '--keys', 'x'); Eval = "probeExit('val=' + JSON.stringify(document.getElementById('field').value))"; Expect = ''; Clipboard = 'editable text field' },
    @{ Name = 'ctrl+v pastes field';   Args = @('--click','100,281','--mod','ctrl','--keys','v'); Eval = "probeExit('val=' + document.getElementById('field').value)"; Expect = 'PASTED'; Preload = 'PASTED' },

    @{ Name = 'selection + range API'; Args = @(); Eval = ("var g=window.getSelection(), r=document.createRange(), p=document.getElementById('p1');" +
         "r.setStart(p.firstChild,0); r.setEnd(p.firstChild,19); g.removeAllRanges(); g.addRange(r);" +
         "var b=r.getBoundingClientRect();" +
         "probeExit(JSON.stringify({t:g.toString(),type:g.type,rc:g.rangeCount,col:g.isCollapsed,a:g.anchorOffset,f:g.focusOffset," +
         "same:window.getSelection()===document.getSelection(),sc:r.startContainer===p.firstChild,so:r.startOffset," +
         "ec:r.endContainer===p.firstChild,eo:r.endOffset,left:b.left,top:b.top,w:b.width,h:b.height," +
         "rects:r.getClientRects().length,sup:document.queryCommandSupported('copy'),en:document.queryCommandEnabled('copy')}))");
       Expect = '"t":"The quick brown fox","type":"Range","rc":1,"col":false,"a":0,"f":19,"same":true,"sc":true,"so":0,"ec":true,"eo":19,"left":10,"top":26.*"rects":1,"sup":true,"en":true' },
    @{ Name = 'execCommand copy';      Args = @(); Eval = "var g=getSelection(),r=document.createRange(),p=document.getElementById('p2');r.setStart(p.firstChild,7);r.setEnd(p.firstChild,17);g.removeAllRanges();g.addRange(r);probeExit(document.execCommand('copy'))"; Expect = '^true$'; Clipboard = 'paragraph ' },
    @{ Name = 'execCommand selectAll'; Args = @(); Eval = "probeExit('n=' + (document.execCommand('selectAll') ? getSelection().toString().length : -1))"; Expect = 'n=199' },
    @{ Name = 'execCommand unknown false'; Args = @(); Eval = "probeExit('bogus=' + document.execCommand('bogus'))"; Expect = 'bogus=false' }
)

if ($Process) {
    $list += @(
        @{ Name = 'process: drag selection'; Args = @('--drag','15,33,200,33'); Eval = $selectText; Expect = ' quick brown fox jum'; Backend = 'process' },
        @{ Name = 'process: ctrl+a page';    Args = @('--mod','ctrl','--keys','a'); Eval = $selLength; Expect = 'len=199'; Backend = 'process' },
        @{ Name = 'process: user-select none'; Args = @('--drag','15,205,300,205'); Eval = $selectText; Expect = '^$'; Backend = 'process' },
        @{ Name = 'process: double click word'; Args = @('--click','62,33','--click','62,33'); Eval = $selectText; Expect = '^quick$'; Backend = 'process' },
        @{ Name = 'process: ctrl+c copies';  Args = @('--drag','15,33,200,33','--mod','ctrl','--keys','c'); Eval = 'probeExit(1)'; Expect = '^1$'; Clipboard = ' quick brown fox jum'; Backend = 'process' },
        @{ Name = 'process: editor Copy';    Args = @('--drag','15,33,200,33','--editor','Copy'); Eval = 'probeExit(1)'; Expect = '^1$'; Clipboard = ' quick brown fox jum'; Backend = 'process' }
    )
}

# ---------------------------------------------------------------------------
# Start every probe at once. The mock clipboard is preloaded per case.
# ---------------------------------------------------------------------------
function Start-CaseProbe {
    param($Case)

    $p = @{
        Run    = $run
        Name   = $Case.Name
        Url    = $page
        Eval   = $Case.Eval
        Extra  = $Case.Args
        Wait   = 250
        Settle = 450
        Timeout = 25000
    }
    if ($Case.ContainsKey('Backend')) { $p.Backend = $Case.Backend }
    if ($Case.ContainsKey('Preload')) {
        $p.ClipboardValue = $Case.Preload
    } elseif ($Case.ContainsKey('Clipboard')) {
        # Preload a sentinel: if the page does not copy, the assertion sees it.
        $p.ClipboardValue = 'agave-precondition'
    }
    return Start-TestProbe @p
}

$jobs = @()
foreach ($c in $list) { $jobs += Start-CaseProbe $c }

# Selection highlight pixel case (no probeExit: the capture happens on settle).
$shotDir = Join-Path $run.Dir 'highlight'
New-Item -ItemType Directory -Force -Path $shotDir | Out-Null
$shot = Join-Path $shotDir 'shot.png'
$shotJob = Start-TestProbe -Run $run -Name 'selection highlight' -Url $page `
    -RawArgs @('--probe', $page, '--drag', '15,33,200,33', '--wait', '250',
               '--settle', '400', '--timeout', '25000', '--shot', $shot,
               '--shot-rect', '0,0,300,60', '--eval', '1')

function Test-CaseResult {
    param($Case, $Result)

    if ($Result.Code -ne 0) { return $false }
    if ($Case.ContainsKey('Expect') -and $Result.Payload -notmatch $Case.Expect) { return $false }
    if ($Case.ContainsKey('Clipboard') -and (Get-TestClipboard $Result.Job) -cne $Case.Clipboard) { return $false }
    return $true
}

function Format-CaseFailure {
    param($Case, $Result)

    $line = (($Result.Text -split "`n") | Where-Object { $_ -match '\[exit\]' }) -join ''
    $detail = "exit=$($Result.Code) expected /$($Case.Expect)/ got: $line"
    if ($Case.ContainsKey('Clipboard')) { $detail += " clipboard='$(Get-TestClipboard $Result.Job)'" }
    return $detail
}

$results = @{}
$i = 0
foreach ($r in (Complete-TestProbes -Jobs $jobs)) {
    $results[$list[$i].Name] = $r
    $i++
}
$shotResult = Complete-TestProbe -Job $shotJob

$retry = @()
foreach ($c in $list) {
    $r = $results[$c.Name]
    if (Test-CaseResult $c $r) {
        $clip = if ($c.ContainsKey('Clipboard')) { "   clipboard='$(Get-TestClipboard $r.Job)'" } else { '' }
        Check $c.Name $true $clip
    } else {
        $retry += $c
    }
}

# Click timing (a double click is two posted clicks; under full-batch load the
# pair can straddle the double-click interval) is load-sensitive, so a failing
# case gets one clean rerun after the rest of the suite has finished.
foreach ($c in $retry) {
    $ok = $false
    $r = $null
    for ($attempt = 0; $attempt -lt 2 -and -not $ok; $attempt++) {
        $r = Complete-TestProbe -Job (Start-CaseProbe $c)
        $ok = Test-CaseResult $c $r
    }
    if ($ok) {
        $clip = if ($c.ContainsKey('Clipboard')) { "   clipboard='$(Get-TestClipboard $r.Job)'" } else { '' }
        Check $c.Name $true ("$clip (retried)")
    } else {
        Check $c.Name $false (Format-CaseFailure $c $r)
    }
}

# Pixel case: Chrome's rgb(51,103,209); our pipeline lands #3367D0.
$pxOk = $false
$px = ''
for ($attempt = 0; $attempt -lt 2 -and -not $pxOk; $attempt++) {
    if ($attempt -gt 0) {
        Remove-Item $shot -ErrorAction SilentlyContinue
        $shotResult = Complete-TestProbe -Job (Start-TestProbe -Run $run -Name 'selection highlight' -Url $page `
            -RawArgs @('--probe', $page, '--drag', '15,33,200,33', '--wait', '250',
                       '--settle', '400', '--timeout', '25000', '--shot', $shot,
                       '--shot-rect', '0,0,300,60', '--eval', '1'))
    }
    if (Test-Path $shot) {
        $px = ((& python $pixel $shot 20,20,280,45,3367D0,300 --tol 4 2>&1) -join ' ').Trim()
        $pxOk = $LASTEXITCODE -eq 0
    } else {
        $px = "no shot (probe exit $($shotResult.Code))"
    }
}
Check 'selection highlight colour' $pxOk $px

Remove-TestRun $run $failures

""
if ($failures -eq 0) {
    "ALL SELECTION CHECKS PASSED ($cases cases)"
    exit 0
}
"$failures of $cases SELECTION CHECKS FAILED"
exit 1
