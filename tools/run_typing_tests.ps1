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
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root 'build-zig-native\agave_shell.exe'
$page = 'file:///' + ((Join-Path $PSScriptRoot 'pages\cap_typing.html') -replace '\\', '/')

if (-not (Test-Path $exe)) {
    "missing $exe - build first (pwsh -File tools/build_project.ps1)"
    exit 1
}

$failures = 0
$cases    = 0

function Run-Case {
    param(
        [string]$Label,
        [string]$Keys,
        [string]$Eval,
        [string]$Expect,
        [string]$Ime = ''
    )

    $script:cases++
    $extra = @()
    if ($Keys) { $extra += @('--keys', $Keys) }
    if ($Ime)  { $extra += @('--ime', $Ime) }

    $out  = & $exe --probe $page @extra --wait 300 --settle 300 --timeout 25000 --eval $Eval 2>&1
    $code = $LASTEXITCODE
    $text = ($out | Out-String)
    $line = (($text -split "`n") | Where-Object { $_ -match '\[exit\]' }) -join ''

    if (($code -eq 0) -and ($text -match $Expect)) {
        "PASS  $Label"
    } else {
        $script:failures++
        "FAIL  $Label   exit=$code expected /$Expect/"
        "      got: $line"
    }
}

Run-Case 'type ab1'      'ab1'              'probeExit(reportTyping())' 'value=ab1 len=3'
Run-Case 'backspace'     'abc{backspace}'  'probeExit(reportTyping())' 'value=ab len=2'
Run-Case 'tab + filter'  '{tab}a1'         'probeExit(reportTyping() + '' active='' + document.activeElement.id)' 'filter=1.*active=filter'
Run-Case 'arrow trap'    '{tab}{left}1'    'probeExit(reportTyping() + '' active='' + document.activeElement.id)' 'filter=1.*active=filter'
Run-Case 'textarea'      '{tab}{tab}ab{enter}cd' 'probeExit(''area='' + JSON.stringify(area.value))' 'area="ab\\ncd"'
# IME: WM_IME_CHAR per UTF-16 code unit - the path a Chinese IME uses.
Run-Case 'ime cjk'       ''                 'probeExit(''value='' + text.value + '' len='' + text.value.length)' 'value=中文输入 len=4' -Ime '中文输入'
Run-Case 'ime emoji'     ''                 'probeExit(''value='' + text.value + '' len='' + text.value.length)' 'value=😀 len=2' -Ime '😀'
# Astral character typed through the keyboard path, then more text: the caret
# must end up after the surrogate pair, not before it.
Run-Case 'emoji + text'  'a😀b'            'probeExit(reportTyping())' 'value=a😀b len=4'

# Caret geometry: a text field's caret is as tall as the *text*, not as the
# input box / line box (Chrome: 14px font with line-height:25px => ~17px caret,
# not 25). AGB_CARET_DEBUG=1 paints it red regardless of focus and blink phase,
# so the headless screenshot always has one; tools/caret_check.py measures it.
Write-Host '== caret height (text height, not the line box)'
$caretPage = 'file:///' + ((Join-Path $PSScriptRoot 'pages\cap_caret.html') -replace '\\', '/')
$caretShot = Join-Path $root 'build-zig-native\caret_typing.png'

function Test-Caret {
    param([string]$Label, [object[]]$Extra, [string]$Expect)

    $env:AGB_CARET_DEBUG = '1'
    try {
        $out = & $exe --probe $caretPage @Extra --wait 400 --settle 300 --timeout 25000 `
            --shot $caretShot --shot-rect 0,0,240,120 2>&1
    } finally {
        Remove-Item Env:\AGB_CARET_DEBUG -ErrorAction SilentlyContinue
    }

    $script:cases++
    $measured = ((& python (Join-Path $PSScriptRoot 'caret_check.py') $caretShot 2>&1) -join ' ').Trim()

    if ($measured -match $Expect) {
        "PASS  $Label   $measured"
    } else {
        $script:failures++
        "FAIL  $Label   expected /$Expect/ got: $measured"
        "      probe: $(($out | Out-String).Trim())"
    }
}

# Empty control (placeholder path, RenderTextControlInnerBlock::caretRect):
# line-height is 25px, so a 25px caret would mean the bug is back. 19px is the
# 14px font's ascent+descent (the family substitution of R123 makes it YaHei).
Test-Caret 'empty caret' @('--eval', "document.getElementById('a').focus()") `
    'x=12 y=14\.\.32 height=19'
# Typed text (RenderText::caretRect): same height, on the second input's line.
Test-Caret 'typed caret' @('--pre-eval', "document.getElementById('b').focus()", '--keys', 'Hxg', '--eval', '1') `
    'x=37 y=51\.\.69 height=19'

""
if ($failures -eq 0) {
    "ALL TYPING CHECKS PASSED ($cases cases)"
    exit 0
}
"$failures of $cases TYPING CHECKS FAILED"
exit 1
