#!/usr/bin/env pwsh
# tools/run_hang_recovery_tests.ps1
#
# "Reload/navigate must always work, hung page or not" regression:
#
#   1. a stuck page is aborted by --interrupt-after (no need to wait for --timeout)
#   2. a navigation queued behind a stuck script aborts it (agwPost + watchdog)
#   3. an interrupt on an idle page changes nothing
#   4. repeated reloads of a *slow but legitimate* page are never interrupted
#      (the "several reloads and the page goes white" bug)
#   4b. a long script in a *timer* (outside the command queue) is never aborted
#   5. a hung page reloads into a working page (the interrupt request is consumed
#      once, so the reloaded document's scripts are not killed as well)
#   6. reload hammered over a progressive HTTP response keeps the document
#   7. more reloads in a row than the bridge's method-slot table used to hold:
#      the final document must keep the host API and its listeners (R77)
#
# Every case runs at the same time. The artifacts that used to force serial
# execution are per case now: cwd is the case directory (agb_crash.txt lands
# there), AGB_DIAG_DIR points at its own diag/ (agb_watchdog.txt /
# agb_busy_*.txt), AGB_USERDATA is a fresh profile, and the two HTTP servers get
# free ports. Only this suite's case directories are inspected, so a renderer
# crash in another suite can no longer trip it.
#
# Usage:  pwsh -File tools/run_hang_recovery_tests.ps1 [-TimeoutMs 60000]
# Exit:   0 all passed, 1 a case failed. Expected wall time: ~10 s.

param(
    [int]$TimeoutMs = 60000,
    [string]$Exe = ''
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $root 'build-zig-native\agave_shell.exe' }
if (-not (Test-Path $Exe)) { throw "missing $Exe (build it with tools\build_project.ps1)" }

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking

function PageUrl([string]$path) { 'file:///' + ($path -replace '\\', '/') }

$run = New-TestRun -Suite 'hang'
$failures = 0
$cases = 0

function Check {
    param([string]$Name, [bool]$Ok, [string]$Detail)

    $script:cases++
    if ($Ok) {
        Write-Host ("PASS  {0}  {1}" -f $Name, $Detail) -ForegroundColor Green
    } else {
        Write-Host ("FAIL  {0}  {1}" -f $Name, $Detail) -ForegroundColor Red
        $script:failures++
    }
}

function Find-Diag {
    param($Job, [string]$Filter)

    if (-not (Test-Path $Job.Diag)) { return @() }
    return @(Get-ChildItem $Job.Diag -Filter $Filter -ErrorAction SilentlyContinue)
}

$hang = Join-Path $root 'debug\hang_loop.html'
if (-not (Test-Path $hang)) { throw "missing $hang (20 s busy loop used as the 'stuck page')" }
$hangUrl = PageUrl $hang
$slow = Join-Path $root 'tools\pages\slow_parse.html'
$slowTimer = Join-Path $root 'tools\pages\slow_timer.html'
$hangOnce = Join-Path $root 'tools\pages\hang_once.html'

# ---- servers (free ports; one instance each: the handler is single-threaded) --
$srvScript = Join-Path $root 'tools\hammer_reload_server.py'
$haveServer = (Test-Path $srvScript) -and (Get-Command python -ErrorAction SilentlyContinue)
$srvPort1 = if ($haveServer) { Get-TestFreePort } else { 0 }
$srvPort2 = if ($haveServer) { Get-TestFreePort } else { 0 }
$srv1 = $null
$srv2 = $null
function Stop-Servers {
    foreach ($p in @($srv1, $srv2)) {
        if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
}

try {
    if ($haveServer) {
        $srv1 = Start-Process -FilePath 'python' -WindowStyle Hidden -PassThru `
            -ArgumentList @($srvScript, "$srvPort1")
        $srv2 = Start-Process -FilePath 'python' -WindowStyle Hidden -PassThru `
            -ArgumentList @($srvScript, "$srvPort2")
        foreach ($prt in @($srvPort1, $srvPort2)) {
            $up = $false
            for ($i = 0; $i -lt 40 -and -not $up; $i++) {
                Start-Sleep -Milliseconds 100
                try { Invoke-WebRequest -Uri "http://127.0.0.1:$prt/fast.html" -UseBasicParsing -TimeoutSec 1 | Out-Null; $up = $true } catch { }
            }
            if (-not $up) { throw "hammer reload server did not come up on $prt" }
        }
    }

    # ---- start every case -------------------------------------------------
    $jobs = [ordered]@{}

    # 1. --interrupt-after aborts the loop mid-parse; elapsed time is the
    #    criterion (after an aborted inline script the parser reports the
    #    document unfinished, so exit 4 is expected).
    $jobs.interrupt = Start-TestProbe -Run $run -Name 'interrupt' -Url $hangUrl `
        -Extra @('--interrupt-after', '2000') -Wait 3000 -Settle 500 -Timeout $TimeoutMs

    # 2. Navigation while the page is busy: the command waits at the queue
    #    head, the watchdog aborts the script, the navigation then runs.
    $jobs.nav = Start-TestProbe -Run $run -Name 'nav-interrupt' -Url $hangUrl `
        -RawArgs @('--probe', $hangUrl, '--nav-after', '2000', '--nav-url', 'about:blank',
                   '--wait', '1000', '--settle', '300', '--timeout', "$TimeoutMs")

    # 3. Idle page + a stray interrupt: document unaffected, no crash file.
    $jobs.idle = Start-TestProbe -Run $run -Name 'idle-interrupt' -Url 'about:blank' `
        -Extra @('--interrupt-after', '300') -Wait 500 -Timeout 15000

    # 4. Slow-but-legitimate page reloaded three times: every load must
    #    complete with its content, and nothing may be aborted.
    if (Test-Path $slow) {
        $jobs.slow = Start-TestProbe -Run $run -Name 'slow-parse-reloads' -Url (PageUrl $slow) `
            -RawArgs @('--probe', (PageUrl $slow), '--reload', '3',
                       '--wait', '2500', '--settle', '400', '--timeout', '40000',
                       '--eval', "probeExit(document.getElementById('out').textContent === 'done' ? 0 : 7)")
    }

    # 4b. A long script in a *timer* callback: it runs inside
    #     macross_event_dispatch(), not inside a command, so the host's
    #     commands queue up behind it. Nothing may be aborted, and the queued
    #     eval must still see the finished page.
    if (Test-Path $slowTimer) {
        $jobs.timer = Start-TestProbe -Run $run -Name 'timer-script' -Url (PageUrl $slowTimer) `
            -RawArgs @('--probe', (PageUrl $slowTimer), '--wait', '500', '--settle', '100',
                       '--timeout', '40000',
                       '--eval', "probeExit(document.getElementById('out').textContent)")
    }

    # 4c. The unresponsive verdict is advisory: a script that runs past the
    #     budget is reported to the host - real busy time + a per-event JS
    #     stack file - and the page keeps running.
    if (Test-Path $slowTimer) {
        $jobs.advisory = Start-TestProbe -Run $run -Name 'busy-advisory' -Url (PageUrl $slowTimer) `
            -RawArgs @('--probe', (PageUrl $slowTimer), '--wait', '500', '--settle', '100',
                       '--timeout', '40000',
                       '--eval', "probeExit(document.getElementById('out').textContent)") `
            -Env @{ AGB_UNRESPONSIVE_MS = '1000' }
    }

    # 5. Hung page reloaded: the stuck script is aborted, the reload runs, and
    #    the reloaded document must become usable.
    if (Test-Path $hangOnce) {
        $jobs.once = Start-TestProbe -Run $run -Name 'hang-once' -Url (PageUrl $hangOnce) `
            -RawArgs @('--probe', (PageUrl $hangOnce), '--nav-after', '1500',
                       '--nav-url', (PageUrl $hangOnce),
                       '--wait', '3000', '--settle', '500', '--timeout', '40000',
                       '--eval', "probeExit(document.title === 'ready' ? 0 : 7)")
    }

    # 6. Reload hammered over a progressive HTTP response.
    if ($haveServer) {
        $jobs.hammer = Start-TestProbe -Run $run -Name 'hammer-reload' -Url "http://127.0.0.1:$srvPort1/slow.html" `
            -RawArgs @('--probe', "http://127.0.0.1:$srvPort1/slow.html",
                       '--reload-every', '250', '--reload-count', '8',
                       '--wait', '3000', '--settle', '600', '--timeout', '40000',
                       '--eval', "probeExit(document.title === 'ready' && document.getElementById('a') && document.getElementById('a').textContent === 'done' ? 0 : 7)")
    }

    # 7. More reloads in a row than the bridge's native-method table used to
    #    have slots for.
    if ($haveServer) {
        $jobs.chain = Start-TestProbe -Run $run -Name 'reload-chain' -Url "http://127.0.0.1:$srvPort2/chain.html" `
            -RawArgs @('--probe', "http://127.0.0.1:$srvPort2/chain.html",
                       '--reload-every', '150', '--reload-count', '10',
                       '--wait', '5000', '--settle', '400', '--timeout', '40000',
                       '--eval', "probeExit((document.readyState === 'complete' && window.__loadCount === 1 && window.__scripts === 5 && typeof document.addEventListener === 'function') ? 0 : 7)")
    }

    $names = @($jobs.Keys)
    $jobList = @($jobs.Values)
    $results = @{}
    $done = Complete-TestProbes -Jobs $jobList
    for ($i = 0; $i -lt $names.Count; $i++) { $results[$names[$i]] = $done[$i] }

    # ---- assertions -------------------------------------------------------
    $r = $results.interrupt
    Check 'interrupt-after aborts a stuck page' ($r.Seconds -lt 10 -and ($r.Code -eq 0 -or $r.Code -eq 4)) `
          ("exit={0} {1}s" -f $r.Code, $r.Seconds)

    $r = $results.nav
    Check 'navigation interrupts a stuck page' ($r.Seconds -lt 10) `
          ("exit={0} {1}s" -f $r.Code, $r.Seconds)

    $r = $results.idle
    $crash = @(Get-ChildItem $r.Job.Dir -Filter 'agb_crash.txt' -ErrorAction SilentlyContinue)
    Check 'idle page survives a stray interrupt' ($r.Code -eq 6 -and $crash.Count -eq 0) `
          ("exit={0} crashFile={1}" -f $r.Code, $crash.Count)

    if ($results.ContainsKey('slow')) {
        $r = $results.slow
        $aborts = ([regex]::Matches($r.Text, 'aborting the page script')).Count
        Check 'slow page survives repeated reloads' ($r.Code -eq 0 -and $aborts -eq 0) `
              ("exit={0} aborts={1} {2}s" -f $r.Code, $aborts, $r.Seconds)
    }

    if ($results.ContainsKey('timer')) {
        $r = $results.timer
        $watchdog = @(Find-Diag $r.Job 'agb_watchdog.txt').Count
        $doneText = $r.Text -match '\[exit\] done'
        Check 'timer script longer than the queue timeout survives' ($doneText -and -not $watchdog) `
              ("done={0} watchdogFile={1} {2}s" -f $doneText, $watchdog, $r.Seconds)
    }

    if ($results.ContainsKey('advisory')) {
        $r = $results.advisory
        $stackFile = @(Find-Diag $r.Job 'agb_busy_*.txt') | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        $stackText = if ($stackFile) { Get-Content $stackFile.FullName -Raw } else { '' }
        $doneText = $r.Text -match '\[exit\] done'
        $reported = $r.Text -match 'reporting an unresponsive page script'
        $hasStack = $stackText -match 'at .*\('
        $hasBusy = $stackText -match 'busy=[1-9][0-9]*ms'
        $noAbort = $r.Text -notmatch 'aborting an unresponsive'
        Check 'unresponsive notice is advisory and leaves a busy-stack file' `
              ($doneText -and $reported -and $hasStack -and $hasBusy -and $noAbort) `
              ("done={0} reported={1} stack={2} busy={3} noAbort={4}" -f $doneText, $reported, $hasStack, $hasBusy, $noAbort)
    }

    if ($results.ContainsKey('once')) {
        $r = $results.once
        Check 'hung page reloads into a working page' ($r.Code -eq 0) `
              ("exit={0} {1}s (7 = reloaded page never became ready)" -f $r.Code, $r.Seconds)
    }

    if ($results.ContainsKey('hammer')) {
        $r = $results.hammer
        $aborts = ([regex]::Matches($r.Text, 'aborting the page script')).Count
        Check 'hammered reload over HTTP keeps the document' ($r.Code -eq 0 -and $aborts -eq 0) `
              ("exit={0} aborts={1} {2}s" -f $r.Code, $aborts, $r.Seconds)
    }

    if ($results.ContainsKey('chain')) {
        $r = $results.chain
        $emptyLoad = ([regex]::Matches($r.Text, 'window event load listeners=0')).Count
        Check 'reload chain keeps the host API' ($r.Code -eq 0 -and $emptyLoad -eq 0) `
              ("exit={0} emptyLoad={1} {2}s (7 = final document lost its methods/listeners)" -f $r.Code, $emptyLoad, $r.Seconds)
    }
} finally {
    Stop-Servers
    Remove-TestRun $run $failures
}

Write-Host ''
if ($failures -eq 0) {
    Write-Host 'ALL HANG RECOVERY CHECKS PASSED' -ForegroundColor Green
    exit 0
}
Write-Host ("{0} check(s) FAILED" -f $failures) -ForegroundColor Red
exit 1
