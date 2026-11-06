#!/usr/bin/env pwsh
# tools/run_hang_recovery_tests.ps1
#
# Regression for "a page whose script never returns can be recovered without
# killing the browser": the probe gets an --interrupt-after hook, and any queued
# navigation/exec command interrupts first (agwPost choke point).
#
# Page: debug/hang_loop.html runs a 20 s busy loop during parse, so without the
# interrupt every case below would sit until --timeout.
#
# Usage:  pwsh -File tools/run_hang_recovery_tests.ps1 [-TimeoutMs 60000]
#
# Exit: 0 all passed, 1 a case failed.

param(
    [int]$TimeoutMs = 60000,
    [string]$Exe = ''
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $root 'build-zig-native\agave_shell.exe' }
if (-not (Test-Path $Exe)) { throw "missing $Exe (build it with tools\build_project.ps1)" }

$page = Join-Path $root 'debug\hang_loop.html'
if (-not (Test-Path $page)) { throw "missing $page" }
$url = 'file:///' + ($page -replace '\\', '/')

function Invoke-Probe {
    param([string[]]$CliArgs)

    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $Exe @CliArgs *> $null
    $code = $LASTEXITCODE
    $sw.Stop()
    return [pscustomobject]@{ Code = $code; Ms = [int]$sw.Elapsed.TotalMilliseconds }
}

$failures = 0
function Check {
    param([string]$Name, [bool]$Ok, [string]$Detail)

    if ($Ok) {
        Write-Host ("PASS  {0}  {1}" -f $Name, $Detail) -ForegroundColor Green
    } else {
        Write-Host ("FAIL  {0}  {1}" -f $Name, $Detail) -ForegroundColor Red
        $script:failures++
    }
}

# 1. --interrupt-after aborts the loop; the probe stops ~2s + wait + settle.
#    The parser reports the document as unfinished after an aborted inline
#    script (exit 4), so the判据 is *elapsed time*, not exit 0: before the
#    interrupt landed this case sat until the 20s loop returned on its own.
$r = Invoke-Probe @('--probe', $url, '--interrupt-after', '2000',
                    '--wait', '3000', '--settle', '500', '--timeout', "$TimeoutMs")
Check 'interrupt-after aborts a stuck page' ($r.Ms -lt 10000 -and ($r.Code -eq 0 -or $r.Code -eq 4)) `
      ("exit={0} {1}ms" -f $r.Code, $r.Ms)

# 2. Navigation while the page is busy goes through the agwPost() choke point:
#    it must abort the script first, otherwise the navigate command waits behind
#    the loop. Target is about:blank, so the probe reports an empty document.
$r = Invoke-Probe @('--probe', $url, '--nav-after', '2000', '--nav-url', 'about:blank',
                    '--wait', '1000', '--settle', '300', '--timeout', "$TimeoutMs")
Check 'navigation interrupts a stuck page' ($r.Ms -lt 10000) `
      ("exit={0} {1}ms" -f $r.Code, $r.Ms)

# 3. Nothing to interrupt: an idle page with --interrupt-after must not break the
#    document or crash (about:blank is empty, so the probe reports exit 6).
$r = Invoke-Probe @('--probe', 'about:blank', '--interrupt-after', '300',
                    '--wait', '500', '--timeout', '15000')
$crash = @(Get-ChildItem $root -Filter 'agb_crash.txt' -ErrorAction SilentlyContinue) +
         @(Get-ChildItem (Join-Path $root 'build-zig-native') -Filter 'agb_crash.txt' -ErrorAction SilentlyContinue)
Check 'idle page survives a stray interrupt' ($r.Code -eq 6 -and $crash.Count -eq 0) `
      ("exit={0} crashFile={1}" -f $r.Code, $crash.Count)

Write-Host ''
if ($failures -eq 0) {
    Write-Host 'ALL HANG RECOVERY CHECKS PASSED' -ForegroundColor Green
    exit 0
}
Write-Host ("{0} check(s) FAILED" -f $failures) -ForegroundColor Red
exit 1
