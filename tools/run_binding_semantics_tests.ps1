# tools/run_binding_semantics_tests.ps1
#
# Regression for the DOM binding semantics layer (B1): one interface class per
# interface with a real prototype chain, constructor object, @@toStringTag and
# per-interface method ownership, plus the facade (nodeGetter/titleGetter) as
# the custom layer.
#
# tools/pages/cap_binding_semantics.html runs the assertion matrix in the page
# (Chrome's answers, see issues/wip/ant-dom-bindings.md §12) and leaves the
# result in window.__bindingResult; the probe eval only reports it. A page
# script that throws aborts the whole matrix, which shows up as a single
# "threw:" failure.
#
# The semantics matrix and the generated-table inventory (AGB_BINDING_CHECK=1)
# are two independent probe runs, started together. Wall time: ~2 s.
#
# Usage:
#   pwsh -File tools/run_binding_semantics_tests.ps1
#   pwsh -File tools/run_binding_semantics_tests.ps1 -BuildDir build-zig-native

param(
    [string]$BuildDir = 'build-zig-native'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path (Join-Path $root $BuildDir) 'agave_shell.exe'
$page = Join-Path $root 'tools/pages/cap_binding_semantics.html'

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }
if (-not (Test-Path $page)) { throw "page not found: $page" }

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking
$run = New-TestRun -Suite 'binding'

$failures = 0

$matrixJs = Join-Path $run.Dir 'binding_check.js'
@'
var r = window.__bindingResult;
if (!r) {
  probeExit('FAIL no result (page script did not run)');
} else if (r.fail.length) {
  probeExit('FAIL ' + r.pass + '/' + r.total + ' ' + r.fail.join('|'));
} else {
  probeExit('PASS ' + r.pass + '/' + r.total);
}
'@ | Set-Content -Path $matrixJs -Encoding ascii

$tableJs = Join-Path $run.Dir 'binding_table_check.js'
"probeExit('check');" | Set-Content -Path $tableJs -Encoding ascii

$matrixJob = Start-TestProbe -Run $run -Name 'matrix' -Url $page `
    -EvalFile $matrixJs -Wait 1200 -Settle 500 -Timeout 60000
$tableJob = Start-TestProbe -Run $run -Name 'tables' -Url $page `
    -EvalFile $tableJs -Wait 800 -Settle 300 -Timeout 60000 `
    -Env @{ AGB_BINDING_CHECK = '1' }

$matrix = Complete-TestProbe -Job $matrixJob
$table = Complete-TestProbe -Job $tableJob

# --- semantics matrix -------------------------------------------------------
if ($matrix.Code -ne 0) {
    $failures++
    Write-Host ("FAIL  binding semantics: probe exit {0}" -f $matrix.Code)
} elseif ($matrix.Stdout -notmatch 'PASS (\d+)/(\d+)') {
    $failures++
    Write-Host "FAIL  binding semantics: matrix did not pass"
} else {
    Write-Host ("PASS  binding semantics ({0}/{1} assertions)" -f $Matches[1], $Matches[2])
}

if ($matrix.Stdout -match 'FAIL (.*)') {
    Write-Host ("      failed: {0}" -f $Matches[1])
}
if ($matrix.Stderr -and $matrix.Stderr.Trim().Length -gt 0) {
    Write-Host "      stderr: $(($matrix.Stderr -split "`n" | Select-Object -First 3) -join ' ')"
}

# --- generated interface tables (A1 spike) ---------------------------------
# buildANT/Ant<Interface>.cpp is generated from the IDL by
# bindings/scripts/CodeGeneratorAnt.pm. The runtime does not consume the tables
# yet; AGB_BINDING_CHECK=1 makes it compare them against the hand-written facade
# and print one line per interface. This pins the count of IDL members and, more
# importantly, that the IDL and the facade agree on writability.
$tableText = $table.Text
$expected = @(
    @{ Name = 'Node';         Attrs = 13; Methods = 14; Constants = 12 },
    @{ Name = 'Element';      Attrs = 18; Methods = 21; Constants = 0 },
    @{ Name = 'HTMLElement';  Attrs = 15; Methods = 3;  Constants = 0 },
    @{ Name = 'Document';     Attrs = 29; Methods = 29; Constants = 0 },
    @{ Name = 'HTMLCollection'; Attrs = 1; Methods = 3; Constants = 0 },
    @{ Name = 'NodeList';     Attrs = 1;  Methods = 1;  Constants = 0 },
    # G1-1 coverage (18 -> 41 interfaces): the value-carrying interfaces added
    # there are pinned too, so a table that stops being compiled in (or an IDL
    # change nobody re-generated) fails here instead of at runtime.
    @{ Name = 'CharacterData'; Attrs = 2;  Methods = 5;  Constants = 0 },
    @{ Name = 'Text';          Attrs = 0;  Methods = 1;  Constants = 0 },
    @{ Name = 'Comment';       Attrs = 0;  Methods = 0;  Constants = 0 },
    @{ Name = 'DocumentFragment'; Attrs = 0; Methods = 0; Constants = 0 },
    @{ Name = 'HTMLTableElement'; Attrs = 14; Methods = 8; Constants = 0 },
    @{ Name = 'HTMLTableRowElement'; Attrs = 8; Methods = 2; Constants = 0 },
    @{ Name = 'HTMLTableCellElement'; Attrs = 15; Methods = 0; Constants = 0 },
    @{ Name = 'HTMLStyleElement'; Attrs = 4; Methods = 0; Constants = 0 },
    @{ Name = 'SVGElement';    Attrs = 3;  Methods = 0;  Constants = 0 },
    @{ Name = 'Window';        Attrs = 157; Methods = 12; Constants = 0 }
)

if ($table.Code -ne 0) {
    $failures++
    Write-Host ("FAIL  generated tables: probe exit {0}" -f $table.Code)
} else {
    $tableFailures = 0
    foreach ($want in $expected) {
        $pattern = '\[bind\] ' + $want.Name + ': idl (\d+) attrs / (\d+) methods / (\d+) consts'
        $m = [regex]::Match($tableText, $pattern)
        if (-not $m.Success) {
            Write-Host ("FAIL  generated tables: no summary for {0} (is buildANT/Ant{0}.cpp compiled in?)" -f $want.Name)
            $tableFailures++
            continue
        }
        if ([int]$m.Groups[1].Value -ne $want.Attrs -or
            [int]$m.Groups[2].Value -ne $want.Methods -or
            [int]$m.Groups[3].Value -ne $want.Constants) {
            Write-Host ("FAIL  generated tables: {0} idl {1}/{2}/{3}, want {4}/{5}/{6} (IDL or ownership table changed? re-generate)" -f
                $want.Name, $m.Groups[1].Value, $m.Groups[2].Value, $m.Groups[3].Value,
                $want.Attrs, $want.Methods, $want.Constants)
            $tableFailures++
        }
    }
    # The invariant: every table found its interface class.
    $summary = [regex]::Match($tableText, '\[bind\] generated tables: (\d+) interfaces, \d+ attrs, \d+ methods, \d+ consts, (\d+) missing owner\(s\)')
    if (-not $summary.Success) {
        Write-Host "FAIL  generated tables: no inventory line (AGB_BINDING_CHECK did not run?)"
        $tableFailures++
    } else {
        if ([int]$summary.Groups[2].Value -ne 0) {
            Write-Host ("FAIL  generated tables: {0} table(s) have no owning interface class (nothing installed)" -f $summary.Groups[2].Value)
            $tableFailures++
        }
        if ([int]$summary.Groups[1].Value -lt 41) {
            Write-Host ("FAIL  generated tables: only {0} interfaces registered, expected at least {1}" -f $summary.Groups[1].Value, $expected.Count)
            $tableFailures++
        }
    }
    $failures += $tableFailures
    if ($tableFailures -eq 0) {
        Write-Host ("PASS  generated tables ({0} interfaces, 0 missing owners)" -f $summary.Groups[1].Value)
    }
}

Remove-TestRun $run $failures

if ($failures -gt 0) {
    Write-Host "`nBINDING SEMANTICS CHECKS FAILED ($failures)"
    exit 1
}

Write-Host "`nALL BINDING SEMANTICS CHECKS PASSED"
exit 0
