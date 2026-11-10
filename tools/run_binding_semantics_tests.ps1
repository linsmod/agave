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
# Run time: one probe, ~2 s (plus process start). No network, no fixed ports.
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

$tmp = Join-Path ([IO.Path]::GetTempPath()) ('agb-bindings-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

$checkJs = Join-Path $tmp 'binding_check.js'
@'
var r = window.__bindingResult;
if (!r) {
  probeExit('FAIL no result (page script did not run)');
} else if (r.fail.length) {
  probeExit('FAIL ' + r.pass + '/' + r.total + ' ' + r.fail.join('|'));
} else {
  probeExit('PASS ' + r.pass + '/' + r.total);
}
'@ | Set-Content -Path $checkJs -Encoding ascii

$stdout = Join-Path $tmp 'out.txt'
$stderr = Join-Path $tmp 'err.txt'

$p = Start-Process -FilePath $exe -NoNewWindow -PassThru -Wait -ArgumentList @(
    '--probe', $page, '--eval-file', $checkJs,
    '--wait', '1200', '--settle', '500', '--timeout', '60000'
) -RedirectStandardOutput $stdout -RedirectStandardError $stderr

$text = (Get-Content $stdout -Raw) -as [string]
$err = (Get-Content $stderr -Raw) -as [string]
$failures = 0

if ($p.ExitCode -ne 0) {
    $failures++
    Write-Host ("FAIL  binding semantics: probe exit {0}" -f $p.ExitCode)
} elseif ($text -notmatch 'PASS (\d+)/(\d+)') {
    $failures++
    Write-Host "FAIL  binding semantics: matrix did not pass"
} else {
    Write-Host ("PASS  binding semantics ({0}/{1} assertions)" -f $Matches[1], $Matches[2])
}

if ($text -match 'FAIL (.*)') {
    $failed = $Matches[1]
    Write-Host ("      failed: {0}" -f $failed)
}

if ($err -and $err.Trim().Length -gt 0) {
    Write-Host "      stderr: $(($err -split "`n" | Select-Object -First 3) -join ' ')"
}

# --- generated interface tables (A1 spike) ---------------------------------
# buildANT/Ant<Interface>.cpp is generated from the IDL by
# bindings/scripts/CodeGeneratorAnt.pm. The runtime does not consume the tables
# yet; AGB_BINDING_CHECK=1 makes it compare them against the hand-written facade
# and print one line per interface. This pins the count of IDL members and, more
# importantly, that the IDL and the facade agree on writability (a mismatch means
# the accessor lets a read-only property be assigned, or hides a setter).
$checkJs = Join-Path $tmp 'binding_table_check.js'
"probeExit('check');" | Set-Content -Path $checkJs -Encoding ascii

$env:AGB_BINDING_CHECK = '1'
# agbLog writes the [bind] lines to stderr: this run needs both streams.
$stdout2 = Join-Path $tmp 'tables_out.txt'
$stderr2 = Join-Path $tmp 'tables_err.txt'
$p2 = Start-Process -FilePath $exe -NoNewWindow -PassThru -Wait -ArgumentList @(
    '--probe', $page, '--eval-file', $checkJs,
    '--wait', '800', '--settle', '300', '--timeout', '60000'
) -RedirectStandardOutput $stdout2 -RedirectStandardError $stderr2
Remove-Item Env:AGB_BINDING_CHECK

$tableText = ((Get-Content $stdout2 -Raw) -as [string]) + ((Get-Content $stderr2 -Raw) -as [string])

# Interface name -> expected [bind] summary line, e.g.
#   [bind] Node: idl 13 attrs / 14 methods / 12 consts (parent -)
# Counts are the generated tables' own (the IDL's members, ownership included),
# so an IDL change or a generator regression shows up as a count change here.
$expected = @(
    @{ Name = 'Node';         Attrs = 13; Methods = 14; Constants = 12 },
    @{ Name = 'Element';      Attrs = 18; Methods = 23; Constants = 0 },
    @{ Name = 'HTMLElement';  Attrs = 15; Methods = 0;  Constants = 0 },
    @{ Name = 'Document';     Attrs = 29; Methods = 29; Constants = 0 },
    @{ Name = 'HTMLCollection'; Attrs = 1; Methods = 3; Constants = 0 },
    @{ Name = 'NodeList';     Attrs = 1;  Methods = 1;  Constants = 0 }
)

if ($p2.ExitCode -ne 0) {
    $failures++
    Write-Host ("FAIL  generated tables: probe exit {0}" -f $p2.ExitCode)
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
    # The invariant: every table found its interface class (a table without one
    # would silently install nothing). The counts themselves are allowed to grow
    # as more IDLs are migrated; the six pinned above are the ones whose shape
    # the semantics matrix depends on.
    $summary = [regex]::Match($tableText, '\[bind\] generated tables: (\d+) interfaces, \d+ attrs, \d+ methods, \d+ consts, (\d+) missing owner\(s\)')
    if (-not $summary.Success) {
        Write-Host "FAIL  generated tables: no inventory line (AGB_BINDING_CHECK did not run?)"
        $tableFailures++
    } else {
        if ([int]$summary.Groups[2].Value -ne 0) {
            Write-Host ("FAIL  generated tables: {0} table(s) have no owning interface class (nothing installed)" -f $summary.Groups[2].Value)
            $tableFailures++
        }
        if ([int]$summary.Groups[1].Value -lt $expected.Count) {
            Write-Host ("FAIL  generated tables: only {0} interfaces registered, expected at least {1}" -f $summary.Groups[1].Value, $expected.Count)
            $tableFailures++
        }
    }
    $failures += $tableFailures
    if ($tableFailures -eq 0) {
        Write-Host ("PASS  generated tables ({0} interfaces, 0 missing owners)" -f $summary.Groups[1].Value)
    }
}

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

if ($failures -gt 0) {
    Write-Host "`nBINDING SEMANTICS CHECKS FAILED ($failures)"
    exit 1
}

Write-Host "`nALL BINDING SEMANTICS CHECKS PASSED"
exit 0


