# tools/run_shadow_tests.ps1
#
# Regression for Shadow DOM + DOM2 Traversal + Fullscreen (issues #27):
#   * shadow scope: component styles do not leak out, page styles do not leak
#     in, :host / :host(*) / ::slotted() / ::part() resolve like Chrome;
#   * slot distribution: assignedNodes/assignedSlot, host.innerHTML keeps the
#     shadow tree, shadowRoot.innerHTML keeps the light DOM, closed roots;
#   * slotchange / fullscreenchange are asynchronous (microtask/task), so the
#     page records the outcome and the probe only reports window.__*Result;
#   * createTreeWalker: SHOW_*, FILTER_ACCEPT/REJECT/SKIP, root/currentNode.
#
# Two probe runs in parallel; wall time ~2 s.
#
# Usage:
#   pwsh -File tools/run_shadow_tests.ps1
#   pwsh -File tools/run_shadow_tests.ps1 -BuildDir build-zig-native

param(
    [string]$BuildDir = 'build-zig-native'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path (Join-Path $root $BuildDir) 'agave_shell.exe'
$page = Join-Path $root 'tools/pages/cap_shadow_dom.html'
$asyncPage = Join-Path $root 'tools/pages/cap_shadow_async.html'

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }
if (-not (Test-Path $page)) { throw "page not found: $page" }
if (-not (Test-Path $asyncPage)) { throw "page not found: $asyncPage" }

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking
$run = New-TestRun -Suite 'shadow'

$failures = 0

$syncJs = Join-Path $run.Dir 'shadow_check.js'
@'
var r = window.__shadowResult;
if (!r) {
  probeExit('FAIL no result (page script did not run)');
} else if (r.fail.length) {
  probeExit('FAIL ' + r.pass + '/' + r.total + ' ' + r.fail.join('|'));
} else {
  probeExit('PASS ' + r.pass + '/' + r.total);
}
'@ | Set-Content -Path $syncJs -Encoding ascii

$asyncJs = Join-Path $run.Dir 'shadow_async_check.js'
@'
var r = window.__asyncResult;
if (!r) {
  probeExit('FAIL no async result (timers did not run)');
} else if (r.fail.length) {
  probeExit('FAIL ' + r.pass + '/' + r.total + ' ' + r.fail.join('|'));
} else {
  probeExit('PASS ' + r.pass + '/' + r.total);
}
'@ | Set-Content -Path $asyncJs -Encoding ascii

$syncJob = Start-TestProbe -Run $run -Name 'shadow-dom' -Url $page `
    -EvalFile $syncJs -Wait 1200 -Settle 500 -Timeout 60000
$asyncJob = Start-TestProbe -Run $run -Name 'shadow-async' -Url $asyncPage `
    -EvalFile $asyncJs -Wait 1500 -Settle 500 -Timeout 60000

$sync = Complete-TestProbe -Job $syncJob
$async = Complete-TestProbe -Job $asyncJob

foreach ($case in @(@{ Name = 'shadow dom'; Result = $sync }, @{ Name = 'shadow async'; Result = $async })) {
    $result = $case.Result

    if ($result.Code -ne 0) {
        $failures++
        Write-Host ("FAIL  {0}: probe exit {1}" -f $case.Name, $result.Code)
    } elseif ($result.Stdout -notmatch 'PASS (\d+)/(\d+)') {
        $failures++
        Write-Host ("FAIL  {0}: did not pass" -f $case.Name)
    } else {
        Write-Host ("PASS  {0} ({1}/{2} assertions)" -f $case.Name, $Matches[1], $Matches[2])
    }

    if ($result.Stdout -match 'FAIL (.*)') {
        Write-Host ("      failed: {0}" -f $Matches[1])
    }
    if ($result.Stderr -and $result.Stderr.Trim().Length -gt 0) {
        Write-Host "      stderr: $(($result.Stderr -split "`n" | Select-Object -First 3) -join ' ')"
    }
}

Remove-TestRun $run $failures

if ($failures -ne 0) {
    Write-Host ("SHADOW CHECKS FAILED ({0})" -f $failures)
    exit 1
}

Write-Host "ALL SHADOW CHECKS PASSED"
exit 0
