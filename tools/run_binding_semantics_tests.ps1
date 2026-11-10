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

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

if ($failures -gt 0) {
    Write-Host "`nBINDING SEMANTICS CHECKS FAILED ($failures)"
    exit 1
}

Write-Host "`nALL BINDING SEMANTICS CHECKS PASSED"
exit 0
