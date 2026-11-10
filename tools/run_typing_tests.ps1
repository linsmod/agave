# run_typing_tests.ps1 - regression for keyboard / IME text input.
#
# Every case types into tools/pages/cap_typing.html with the probe's --keys
# (real WM_KEYDOWN/WM_CHAR/WM_KEYUP) or --ime (real WM_IME_CHAR, which is what
# a Chinese IME delivers). Headless: the window is never shown.
#
#   pwsh -File tools/run_typing_tests.ps1
#
# What each case pins down:
#   ab1                one character per key, inserted once, at the caret
#   abc{backspace}     Backspace deletes one character, not two
#   {tab}a1            Tab moves focus; a keydown preventDefault refuses a char
#   {tab}{left}1       a cancelled arrow key must not swallow the next char
#   {tab}{tab}...      Enter inserts exactly one newline in a textarea
#   ime cjk            WM_IME_CHAR text arrives as UTF-8, not as code-page bytes
#   ime emoji          a surrogate pair survives the two-message split
#   emoji + text       the caret ends up after an astral character (R38)
#
# All cases (and the two caret captures) run at once; expected wall time ~3 s.
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root 'build-zig-native\agave_shell.exe'
$page = 'file:///' + ((Join-Path $PSScriptRoot 'pages\cap_typing.html') -replace '\\', '/')

if (-not (Test-Path $exe)) {
    "missing $exe - build first (pwsh -File tools/build_project.ps1)"
    exit 1
}

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking
$run = New-TestRun -Suite 'typing'

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

$list = @(
    @{ Name = 'type ab1';     Extra = @('--keys', 'ab1');               Eval = 'probeExit(reportTyping())'; Expect = 'value=ab1 len=3' },
    @{ Name = 'backspace';    Extra = @('--keys', 'abc{backspace}');   Eval = 'probeExit(reportTyping())'; Expect = 'value=ab len=2' },
    @{ Name = 'tab + filter'; Extra = @('--keys', '{tab}a1');          Eval = "probeExit(reportTyping() + ' active=' + document.activeElement.id)"; Expect = 'filter=1.*active=filter' },
    @{ Name = 'arrow trap';   Extra = @('--keys', '{tab}{left}1');     Eval = "probeExit(reportTyping() + ' active=' + document.activeElement.id)"; Expect = 'filter=1.*active=filter' },
    @{ Name = 'textarea';     Extra = @('--keys', '{tab}{tab}ab{enter}cd'); Eval = "probeExit('area=' + JSON.stringify(area.value))"; Expect = 'area="ab\\ncd"' },
    # IME: WM_IME_CHAR per UTF-16 code unit - the path a Chinese IME uses.
    @{ Name = 'ime cjk';      Extra = @('--ime', '中文输入');          Eval = "probeExit('value=' + text.value + ' len=' + text.value.length)"; Expect = 'value=中文输入 len=4' },
    @{ Name = 'ime emoji';    Extra = @('--ime', '😀');                Eval = "probeExit('value=' + text.value + ' len=' + text.value.length)"; Expect = 'value=😀 len=2' },
    # Astral character typed through the keyboard path, then more text.
    @{ Name = 'emoji + text'; Extra = @('--keys', 'a😀b');             Eval = 'probeExit(reportTyping())'; Expect = 'value=a😀b len=4' }
)

$jobs = @()
foreach ($c in $list) {
    $jobs += Start-TestProbe -Run $run -Name $c.Name -Url $page -Eval $c.Eval `
        -Extra $c.Extra -Wait 300 -Settle 300 -Timeout 25000
}

# Caret geometry: a text field's caret is as tall as the *text*, not as the
# input box / line box. AGB_CARET_DEBUG=1 paints it red regardless of focus and
# blink phase, so the headless screenshot always has one; caret_check.py
# measures it.
$caretPage = 'file:///' + ((Join-Path $PSScriptRoot 'pages\cap_caret.html') -replace '\\', '/')
$caretShotDir = Join-Path $run.Dir 'carets'
New-Item -ItemType Directory -Force -Path $caretShotDir | Out-Null
$caretCases = @(
    @{ Name = 'empty caret'; Shot = (Join-Path $caretShotDir 'empty.png'); Extra = @('--eval', "document.getElementById('a').focus()"); Expect = 'x=12 y=15\.\.31 height=17' },
    @{ Name = 'typed caret'; Shot = (Join-Path $caretShotDir 'typed.png'); Extra = @('--pre-eval', "document.getElementById('b').focus()", '--keys', 'Hxg', '--eval', '1'); Expect = 'x=36 y=52\.\.68 height=17' }
)
$caretJobs = @()
foreach ($c in $caretCases) {
    $caretJobs += Start-TestProbe -Run $run -Name $c.Name -Url $caretPage `
        -RawArgs (@('--probe', $caretPage, '--wait', '400', '--settle', '300', '--timeout', '25000',
                    '--shot', $c.Shot, '--shot-rect', '0,0,240,120') + $c.Extra) `
        -Env @{ AGB_CARET_DEBUG = '1' }
}

$results = Complete-TestProbes -Jobs $jobs
for ($i = 0; $i -lt $list.Count; $i++) {
    $c = $list[$i]
    $r = $results[$i]
    Check $c.Name ($r.Code -eq 0 -and $r.Payload -match $c.Expect) ("exit=$($r.Code) payload=[$($r.Payload)]")
}

$caretRes = Complete-TestProbes -Jobs $caretJobs
for ($i = 0; $i -lt $caretCases.Count; $i++) {
    $c = $caretCases[$i]
    $r = $caretRes[$i]
    $measured = ''
    if (Test-Path $c.Shot) {
        $measured = ((& python (Join-Path $PSScriptRoot 'caret_check.py') $c.Shot 2>&1) -join ' ').Trim()
    }
    Check $c.Name ($measured -match $c.Expect) ("measured=$measured probeExit=$($r.Code)")
}

Remove-TestRun $run $failures

""
if ($failures -eq 0) {
    "ALL TYPING CHECKS PASSED ($cases cases)"
    exit 0
}
"$failures of $cases TYPING CHECKS FAILED"
exit 1
