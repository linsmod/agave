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
#   5. a hung page reloads into a working page (the interrupt request is consumed
#      once, so the reloaded document's scripts are not killed as well)
#   6. reload hammered over a progressive HTTP response keeps the document
#   7. more reloads in a row than the bridge's method-slot table used to hold:
#      the final document must keep the host API and its listeners (R77)
#
# Usage:  pwsh -File tools/run_hang_recovery_tests.ps1 [-TimeoutMs 60000]
# Exit:   0 all passed, 1 a case failed.

param(
    [int]$TimeoutMs = 60000,
    [string]$Exe = ''
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $root 'build-zig-native\agave_shell.exe' }
if (-not (Test-Path $Exe)) { throw "missing $Exe (build it with tools\build_project.ps1)" }

function PageUrl([string]$path) { 'file:///' + ($path -replace '\\', '/') }

function Invoke-Probe {
    param([string[]]$CliArgs)

    $log = [IO.Path]::GetTempFileName()
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $Exe @CliArgs *> $log
    $code = $LASTEXITCODE
    $sw.Stop()
    $text = Get-Content $log -Raw -ErrorAction SilentlyContinue
    Remove-Item $log -Force -ErrorAction SilentlyContinue
    return [pscustomobject]@{ Code = $code; Ms = [int]$sw.Elapsed.TotalMilliseconds; Text = "$text" }
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

$hang = Join-Path $root 'debug\hang_loop.html'
if (-not (Test-Path $hang)) { throw "missing $hang (20 s busy loop used as the 'stuck page')" }
$hangUrl = PageUrl $hang

# 1. --interrupt-after aborts the loop mid-parse; elapsed time is the criterion
#    (after an aborted inline script the parser reports the document unfinished,
#    so exit 4 is expected - before the fix this case sat until the loop ended).
$r = Invoke-Probe @('--probe', $hangUrl, '--interrupt-after', '2000',
                    '--wait', '3000', '--settle', '500', '--timeout', "$TimeoutMs")
Check 'interrupt-after aborts a stuck page' ($r.Ms -lt 10000 -and ($r.Code -eq 0 -or $r.Code -eq 4)) `
      ("exit={0} {1}ms" -f $r.Code, $r.Ms)

# 2. Navigation while the page is busy: the command waits at the queue head, the
#    watchdog aborts the script, the navigation then runs (about:blank = exit 6).
$r = Invoke-Probe @('--probe', $hangUrl, '--nav-after', '2000', '--nav-url', 'about:blank',
                    '--wait', '1000', '--settle', '300', '--timeout', "$TimeoutMs")
Check 'navigation interrupts a stuck page' ($r.Ms -lt 10000) `
      ("exit={0} {1}ms" -f $r.Code, $r.Ms)

# 3. Idle page + a stray interrupt: document unaffected, no crash file.
Remove-Item (Join-Path $root 'agb_crash.txt') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $root 'build-zig-native\agb_crash.txt') -Force -ErrorAction SilentlyContinue
$r = Invoke-Probe @('--probe', 'about:blank', '--interrupt-after', '300',
                    '--wait', '500', '--timeout', '15000')
$crash = @(Get-ChildItem $root -Filter 'agb_crash.txt' -ErrorAction SilentlyContinue) +
         @(Get-ChildItem (Join-Path $root 'build-zig-native') -Filter 'agb_crash.txt' -ErrorAction SilentlyContinue)
Check 'idle page survives a stray interrupt' ($r.Code -eq 6 -and $crash.Count -eq 0) `
      ("exit={0} crashFile={1}" -f $r.Code, $crash.Count)

# 4. Slow-but-legitimate page reloaded three times: every load must complete with
#    its content, and nothing may be aborted (the white-page regression).
#    slow_parse.html spins 2.5 s - past the 1.5 s queue threshold, so before R90 the
#    watchdog aborted it outright (exit 7 = "done" never written).
$slow = Join-Path $root 'tools\pages\slow_parse.html'
if (Test-Path $slow) {
    $r = Invoke-Probe @('--probe', (PageUrl $slow), '--reload', '3',
                        '--wait', '2500', '--settle', '400', '--timeout', '40000',
                        '--eval', "probeExit(document.getElementById('out').textContent === 'done' ? 0 : 7)")
    $aborts = ([regex]::Matches($r.Text, 'aborting the page script')).Count
    Check 'slow page survives repeated reloads' ($r.Code -eq 0 -and $aborts -eq 0) `
          ("exit={0} aborts={1} {2}ms" -f $r.Code, $aborts, $r.Ms)
}

# 5. Hung page reloaded: the stuck script is aborted, the reload runs, and the
#    reloaded document must become usable (a stale interrupt request would kill
#    its scripts too and leave it blank).
$hangOnce = Join-Path $root 'tools\pages\hang_once.html'
if (Test-Path $hangOnce) {
    $url = PageUrl $hangOnce
    $profile = Join-Path $env:TEMP ('agb-hangonce-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force -Path $profile | Out-Null
    $env:AGB_USERDATA = $profile
    try {
        $r = Invoke-Probe @('--probe', $url, '--nav-after', '1500', '--nav-url', $url,
                            '--wait', '3000', '--settle', '500', '--timeout', '40000',
                            '--eval', "probeExit(document.title === 'ready' ? 0 : 7)")
    } finally {
        $env:AGB_USERDATA = $null
        Remove-Item $profile -Recurse -Force -ErrorAction SilentlyContinue
    }
    Check 'hung page reloads into a working page' ($r.Code -eq 0) `
          ("exit={0} {1}ms (7 = reloaded page never became ready)" -f $r.Code, $r.Ms)
}


# 6. Reload hammered over a progressive HTTP response: the old response delivers
#    its tail after the new load started. FrameLoader must drop those bytes (they
#    belong to a stopped loader) or the new document never finishes - the
#    "reload until it goes white" report.
$srvScript = Join-Path $root 'tools\hammer_reload_server.py'
if ((Test-Path $srvScript) -and (Get-Command python -ErrorAction SilentlyContinue)) {
    $srv = Start-Process -FilePath 'python' -ArgumentList @($srvScript, '8982') -PassThru -WindowStyle Hidden
    try {
        Start-Sleep -Milliseconds 800
        $r = Invoke-Probe @('--probe', 'http://127.0.0.1:8982/slow.html',
                            '--reload-every', '250', '--reload-count', '8',
                            '--wait', '3000', '--settle', '600', '--timeout', '40000',
                            '--eval', "probeExit(document.title === 'ready' && document.getElementById('a') && document.getElementById('a').textContent === 'done' ? 0 : 7)")
    } finally {
        Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue
    }
    $aborts = ([regex]::Matches($r.Text, 'aborting the page script')).Count
    Check 'hammered reload over HTTP keeps the document' ($r.Code -eq 0 -and $aborts -eq 0) `
          ("exit={0} aborts={1} {2}ms" -f $r.Code, $aborts, $r.Ms)
}

# 7. More reloads in a row than the bridge's native-method table used to have
#    slots for. Each document realm installs ~170 bridged methods, and the table
#    is process wide: after ~6 reloads antb_define_method() started failing
#    silently, so the next document got a `document` object without methods, its
#    load listener was never registered and the page stayed blank. The final
#    document must still have the host API and must have run all its scripts.
if ((Test-Path $srvScript) -and (Get-Command python -ErrorAction SilentlyContinue)) {
    $srv = Start-Process -FilePath 'python' -ArgumentList @($srvScript, '8984') -PassThru -WindowStyle Hidden
    try {
        Start-Sleep -Milliseconds 800
        $r = Invoke-Probe @('--probe', 'http://127.0.0.1:8984/chain.html',
                            '--reload-every', '150', '--reload-count', '10',
                            '--wait', '5000', '--settle', '400', '--timeout', '40000',
                            '--eval', "probeExit((document.readyState === 'complete' && window.__loadCount === 1 && window.__scripts === 5 && typeof document.addEventListener === 'function') ? 0 : 7)")
    } finally {
        Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue
    }
    $emptyLoad = ([regex]::Matches($r.Text, 'window event load listeners=0')).Count
    Check 'reload chain keeps the host API' ($r.Code -eq 0 -and $emptyLoad -eq 0) `
          ("exit={0} emptyLoad={1} {2}ms (7 = final document lost its methods/listeners)" -f $r.Code, $emptyLoad, $r.Ms)
}

Write-Host ''
if ($failures -eq 0) {
    Write-Host 'ALL HANG RECOVERY CHECKS PASSED' -ForegroundColor Green
    exit 0
}
Write-Host ("{0} check(s) FAILED" -f $failures) -ForegroundColor Red
exit 1
