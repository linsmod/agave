# run_typing_tests.ps1 - regression for keyboard text input (WM_KEYDOWN/WM_CHAR).
#
# Every case types into tools/pages/cap_typing.html with the probe's --keys,
# which replays real Win32 key messages through the control's window procedure
# (headless: the window is never shown).
#
#   pwsh -File tools/run_typing_tests.ps1
#
# What each case pins down:
#   ab1                one character per key, inserted once, at the caret
#   abc{backspace}     Backspace deletes one character, not two
#   {tab}a1            Tab moves focus; a keydown preventDefault refuses a char
#   {tab}{left}1       a cancelled arrow key must not swallow the next char
#   {tab}{tab}...      Enter inserts exactly one newline in a textarea
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
        [string]$Expect
    )

    $script:cases++
    $out  = & $exe --probe $page --keys $Keys --wait 300 --settle 300 --timeout 25000 --eval $Eval 2>&1
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

""
if ($failures -eq 0) {
    "ALL TYPING CHECKS PASSED ($cases cases)"
    exit 0
}
"$failures of $cases TYPING CHECKS FAILED"
exit 1
