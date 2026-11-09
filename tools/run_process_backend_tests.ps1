# tools/run_process_backend_tests.ps1
#
# The `process` render backend (S3 B2): one agave_render_host.exe per view, the
# protocol over a named shared section (src/webview_control/AgwViewIpc.h).
#
# Every case runs the probe with AGB_VIEW_BACKEND=process and asserts the same
# things a local view answers, so the boundary cannot silently degrade:
#
#   1. load + title + timers   (page JS runs in the renderer; its title and
#                               progress cross as events)
#   2. paint                   (--shot: the canvas in shared memory lands in the
#                               control's mirror)
#   3. input                   (--keys goes control -> ring -> renderer -> DOM)
#   4. renderer crash          (renderer dies -> control reports the load error,
#                               probe exits 5 - not a hang)
#   5. default backend         (no env: the local backend still runs the page)
#
# After every case no agave_render_host.exe may be left behind.
#
# Usage:
#   pwsh -File tools/run_process_backend_tests.ps1

param(
    [string]$BuildDir = 'build-zig-native'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir
$exe = Join-Path $build 'agave_shell.exe'
$renderer = Join-Path $build 'agave_render_host.exe'
$pixelCheck = Join-Path $root 'tools/pixel_check.py'
$pages = Join-Path $root 'tools/pages'

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }
if (-not (Test-Path $renderer)) { throw "agave_render_host.exe not found: $renderer" }

$failures = 0

function Invoke-Probe {
    param(
        [string]$Page,
        [string]$Js,
        [int]$Wait = 1500,
        [int]$Settle = 300,
        [string[]]$Extra = @(),
        [string]$Backend = 'process'
    )

    $out = Join-Path $build 'process_backend_out.txt'
    $err = Join-Path $build 'process_backend_err.txt'
    $script = Join-Path $build 'process_backend_eval.js'

    if ($Js) { Set-Content -Path $script -Value $Js -Encoding ascii }

    $saved = $env:AGB_VIEW_BACKEND
    if ($Backend) { $env:AGB_VIEW_BACKEND = $Backend }
    else { Remove-Item Env:AGB_VIEW_BACKEND -ErrorAction SilentlyContinue }

    try {
        $args = @('--probe', "file:///$($Page -replace '\\','/')",
                  '--wait', "$Wait", '--settle', "$Settle", '--timeout', '25000')
        if ($Js) { $args += @('--eval-file', $script) }
        if ($Extra.Count -gt 0) { $args += $Extra }

        $p = Start-Process -FilePath $exe -NoNewWindow -PassThru -Wait `
            -ArgumentList $args -RedirectStandardOutput $out -RedirectStandardError $err

        [pscustomobject]@{
            Code   = $p.ExitCode
            Stdout = (Get-Content $out -Raw) -as [string]
            Stderr = (Get-Content $err -Raw) -as [string]
        }
    } finally {
        if ($null -eq $saved) { Remove-Item Env:AGB_VIEW_BACKEND -ErrorAction SilentlyContinue }
        else { $env:AGB_VIEW_BACKEND = $saved }
    }
}

# Renderers this run owns. Every renderer carries `--tag <AGB_RENDER_TAG>` from
# the control that spawned it, and this script gives each run a fresh tag, so:
# a renderer without our tag belongs to a developer's own shell - never counted,
# never killed. (The owner pid is in the command line too, for diagnostics.)
$script:runTag = 'agvtest' + ([guid]::NewGuid().ToString('N').Substring(0, 12))
$env:AGB_RENDER_TAG = $script:runTag

function Get-StrayRenderers {
    $stray = @()
    foreach ($p in (Get-CimInstance Win32_Process -Filter "Name='agave_render_host.exe'" -ErrorAction SilentlyContinue)) {
        if ($p.CommandLine -and $p.CommandLine.Contains($script:runTag))
            { $stray += $p }
    }
    return $stray
}

function Assert-NoRenderer {
    param([string]$What)

    # Teardown is asynchronous: the control interrupts, sends EXIT and only then
    # waits/kills, so a renderer may still be unwinding when the probe returns.
    # Give it a moment before calling it a leak.
    for ($i = 0; $i -lt 30; $i++) {
        $left = @(Get-StrayRenderers)
        if ($left.Count -eq 0) { return $true }
        Start-Sleep -Milliseconds 100
    }

    $left = @(Get-StrayRenderers)
    if ($left.Count -gt 0) {
        $script:failures++
        Write-Host ("FAIL  {0}: {1} orphaned renderer process(es): {2}" -f $What, $left.Count,
                    (($left | ForEach-Object { $_.ProcessId }) -join ', '))
        $left | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
        return $false
    }
    return $true
}

# 1. load + title + timers
$load = Invoke-Probe -Page (Join-Path $pages 'cap_hidden_view.html') `
    -Js 'probeExit("title=" + document.title + " ticks=" + window.__ticks);'
$ticks = if ($load.Stdout -match 'ticks=(\d+)') { [int]$Matches[1] } else { -1 }

if ($load.Code -eq 0 -and $load.Stdout -match 'title=hidden view' -and $ticks -ge 5) {
    Write-Host ("PASS  process backend: page runs in the renderer (title crossed, ticks={0})" -f $ticks)
} else {
    $failures++
    Write-Host ("FAIL  process backend: load/title/timers (exit {0})" -f $load.Code)
    Write-Host (($load.Stdout -split "`n" | Select-Object -Last 3) -join "`n")
}
Assert-NoRenderer 'load/title/timers' | Out-Null

# 2. paint: the blue box in the shared canvas reaches the mirror
$shot = Join-Path $build 'process_backend_shot.bmp'
Remove-Item $shot -ErrorAction SilentlyContinue
$paint = Invoke-Probe -Page (Join-Path $pages 'cap_hidden_view.html') -Wait 1500 -Settle 400 `
    -Extra @('--shot', $shot)

if ($paint.Code -eq 0 -and (Test-Path $shot)) {
    $pixels = & python $pixelCheck $shot '150,120,1478c8' --tol 12 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "PASS  process backend: frames cross the boundary (canvas -> mirror)"
    } else {
        $failures++
        Write-Host "FAIL  process backend: frame pixels wrong"
        Write-Host (($pixels -split "`n" | Select-Object -Last 4) -join "`n")
    }
} else {
    $failures++
    Write-Host ("FAIL  process backend: shot (exit {0}, file={1})" -f $paint.Code, (Test-Path $shot))
}
Remove-Item $shot -ErrorAction SilentlyContinue
Assert-NoRenderer 'paint' | Out-Null

# 3. input: keys go control -> ring -> renderer -> DOM
$input = Invoke-Probe -Page (Join-Path $pages 'cap_typing.html') -Wait 800 -Settle 300 `
    -Js 'probeExit("value=" + document.getElementById("text").value);' `
    -Extra @('--keys', 'ab1')

if ($input.Code -eq 0 -and $input.Stdout -match 'value=ab1') {
    Write-Host "PASS  process backend: keyboard input reaches the renderer DOM"
} else {
    $failures++
    Write-Host ("FAIL  process backend: input (exit {0})" -f $input.Code)
    Write-Host (($input.Stdout -split "`n" | Select-Object -Last 2) -join "`n")
}
Assert-NoRenderer 'input' | Out-Null

# 4. crash: the renderer dies, the control reports an error instead of hanging
$env:AGB_RENDER_CRASH_MS = '400'
try {
    $crash = Invoke-Probe -Page (Join-Path $pages 'cap_hidden_view.html') -Wait 1200 -Settle 200 `
        -Js 'probeExit("alive");'
} finally {
    Remove-Item Env:AGB_RENDER_CRASH_MS -ErrorAction SilentlyContinue
}

# The host now hears about the crash through on_crashed (it used to fall
# through to on_error), so the probe reports the crash and exits OK. `AGB_RENDER_CRASH_MS`
# is a clean ExitProcess(42) - there is no crash report, only the exit code.
$crashText = $crash.Stdout + $crash.Stderr

if ($crash.Code -eq 0 -and $crashText -match '\[probe\] crash code=42' -and
    $crashText -match 'report_bytes=0') {
    Write-Host "PASS  process backend: renderer crash reaches the host (code 42, no report)"
} else {
    $failures++
    Write-Host ("FAIL  process backend: crash handling (exit {0})" -f $crash.Code)
    Write-Host (($crashText -split "`n" | Where-Object { $_ -match 'crash|respawn|title=' } | Select-Object -Last 3) -join "`n")
}
Assert-NoRenderer 'crash' | Out-Null

# 5. two views, one stuck: the process backend keeps the other responsive, and
#    an explicit interrupt reaches exactly the stuck renderer.
$mv = Join-Path $build 'agave_multiview_probe.exe'
if (-not (Test-Path $mv)) { throw "agave_multiview_probe.exe not found: $mv" }

$mvOut = Join-Path $build 'multiview_out.txt'
$p = Start-Process -FilePath $mv -NoNewWindow -PassThru -Wait `
     -ArgumentList @('--backend', 'process', '--interrupt-test') `
     -RedirectStandardOutput $mvOut -RedirectStandardError (Join-Path $build 'multiview_err.txt')
$text = (Get-Content $mvOut -Raw) -as [string]
$pong = if ($text -match 'pong_ms=(\d+)') { [int]$Matches[1] } else { -1 }
$alive = if ($text -match 'alive_ms=(\d+)') { [int]$Matches[1] } else { -1 }

if ($p.ExitCode -eq 0 -and $pong -ge 0 -and $pong -lt 200 -and $alive -ge 0 -and
    $alive -lt 300 -and $text -notmatch 'spin=0') {
    Write-Host ("PASS  process backend: stuck tab does not block the other (pong={0} ms), interrupt reaches it ({1} ms)" -f $pong, $alive)
} else {
    $failures++
    Write-Host ("FAIL  process backend: two-view responsiveness (exit {0}, pong={1}, alive={2})" -f $p.ExitCode, $pong, $alive)
    Write-Host (($text -split "`n" | Select-Object -Last 3) -join "`n")
}
Assert-NoRenderer 'multiview' | Out-Null

# 6. the local baseline really serialises: the same two views, shared engine
#    thread, B never answers while A spins (an eval is not a watchdog "user
#    action", so nothing aborts A). That is the problem S3 exists for.
$p = Start-Process -FilePath $mv -NoNewWindow -PassThru -Wait `
     -ArgumentList @('--backend', 'local', '--allow-blocked', '--pong-wait', '2500') `
     -RedirectStandardOutput $mvOut -RedirectStandardError (Join-Path $build 'multiview_err.txt')
$text = (Get-Content $mvOut -Raw) -as [string]

if ($p.ExitCode -eq 0 -and $text -match 'blocked=1' -and $text -notmatch 'spin=0') {
    Write-Host "PASS  local baseline: the stuck tab blocks the other (shared engine thread)"
} else {
    $failures++
    Write-Host ("FAIL  local baseline: expected blocked=1 (exit {0})" -f $p.ExitCode)
    Write-Host (($text -split "`n" | Select-Object -Last 3) -join "`n")
}
Assert-NoRenderer 'local baseline' | Out-Null
Remove-Item $mvOut -ErrorAction SilentlyContinue

# 7. after a crash the view is respawn-ready: the same view loads a page into a
#    fresh renderer (that is what the shell's sad tab does with load_html).
$env:AGB_RENDER_CRASH_MS = '400'
$env:AGB_PROBE_RESPAWN = '1'
try {
    $respawn = Invoke-Probe -Page (Join-Path $pages 'cap_hidden_view.html') -Wait 1200 -Settle 300 `
        -Js 'probeExit("title=" + document.title);'
} finally {
    Remove-Item Env:AGB_RENDER_CRASH_MS -ErrorAction SilentlyContinue
    Remove-Item Env:AGB_PROBE_RESPAWN -ErrorAction SilentlyContinue
}
$respawnText = $respawn.Stdout + $respawn.Stderr

if ($respawnText -match 'respawn: loading' -and $respawnText -match 'title=respawned') {
    Write-Host "PASS  process backend: crash leaves the view respawn-ready (fresh renderer loads a page)"
} else {
    $failures++
    Write-Host ("FAIL  process backend: respawn after crash (exit {0})" -f $respawn.Code)
    Write-Host (($respawnText -split "`n" | Select-Object -Last 3) -join "`n")
}
Assert-NoRenderer 'respawn' | Out-Null

# 8. closing a hung tab is immediate: the interrupt breaks the script, the
#    renderer exits, and the process is killed if it does not (no 2s wait).
$p = Start-Process -FilePath $mv -NoNewWindow -PassThru -Wait `
     -ArgumentList @('--backend', 'process', '--allow-blocked', '--pong-wait', '1200', '--destroy-test') `
     -RedirectStandardOutput $mvOut -RedirectStandardError (Join-Path $build 'multiview_err.txt')
$text = (Get-Content $mvOut -Raw) -as [string]
$destroy = if ($text -match 'destroy_ms=(\d+)') { [int]$Matches[1] } else { -1 }

if ($p.ExitCode -eq 0 -and $destroy -ge 0 -and $destroy -lt 500) {
    Write-Host ("PASS  process backend: closing a hung tab is immediate ({0} ms)" -f $destroy)
} else {
    $failures++
    Write-Host ("FAIL  process backend: hung-tab close (exit {0}, destroy_ms={1})" -f $p.ExitCode, $destroy)
}
Assert-NoRenderer 'hung-tab close' | Out-Null
Remove-Item $mvOut -ErrorAction SilentlyContinue

# 9. protocol cost: AGB_IPC_STATS reports frame publish -> control poll latency and
#    the mirror copy. The copy is the number the RVVM handoff cares about (one
#    full copy per frame on that path); the latency includes this loop's poll
#    cadence, so the bound is loose.
$env:AGB_IPC_STATS = '1'
try {
    $stats = Invoke-Probe -Page (Join-Path $pages 'cap_hidden_view.html') -Wait 1600 -Settle 200 `
        -Js 'probeExit("stats");'
} finally {
    Remove-Item Env:AGB_IPC_STATS -ErrorAction SilentlyContinue
}
$statsText = $stats.Stdout + $stats.Stderr
$copyMax = 0
$copyMin = 0
foreach ($m in [regex]::Matches($statsText, 'copy_us=(\d+)')) {
    $v = [int]$m.Groups[1].Value
    if ($v -gt $copyMax) { $copyMax = $v }
    if ($copyMin -eq 0 -or $v -lt $copyMin) { $copyMin = $v }
}
$frameLines = [regex]::Matches($statsText, 'frames=\d+ latency_avg_us=\d+ max_us=\d+')

# The first frame of a process copies cold (page cache miss + first touch of the
# mirror DIB), so it is 2.5-3.2 ms; every later copy is ~0.5-0.7 ms. Bound the
# steady state (min) and keep a loose sanity ceiling on the cold one, otherwise
# this case flakes on whichever run happens to stop after one frame.
if ($stats.Code -eq 0 -and $frameLines.Count -gt 0 -and $copyMin -gt 0 -and
    $copyMin -lt 2000 -and $copyMax -lt 8000) {
    Write-Host ("PASS  protocol stats: copy min {0} us / max {1} us" -f $copyMin, $copyMax)
} else {
    $failures++
    Write-Host ("FAIL  protocol stats (exit {0}, frames={1}, copyMin={2}, copyMax={3})" -f $stats.Code, $frameLines.Count, $copyMin, $copyMax)
}
Assert-NoRenderer 'stats' | Out-Null

# 10. default backend: the local path still runs the same page
$local = Invoke-Probe -Page (Join-Path $pages 'cap_hidden_view.html') `
    -Js 'probeExit("title=" + document.title);' -Backend $null

if ($local.Code -eq 0 -and $local.Stdout -match 'title=hidden view') {
    Write-Host "PASS  default backend: local still runs the page"
} else {
    $failures++
    Write-Host ("FAIL  default backend: local run (exit {0})" -f $local.Code)
}


# 10. the shell flag (-–backend) does the same thing as the env: a probe
#     started with --backend process uses a renderer, no environment involved.
$flagRun = Invoke-Probe -Page (Join-Path $pages 'cap_hidden_view.html') `
    -Js 'probeExit("title=" + document.title);' -Backend $null `
    -Extra @('--backend', 'process')

if ($flagRun.Code -eq 0 -and $flagRun.Stdout -match 'title=hidden view' -and
    ($flagRun.Stdout + $flagRun.Stderr) -match 'process backend: view') {
    Write-Host "PASS  --backend process flag: same renderer path as the env"
} else {
    $failures++
    Write-Host ("FAIL  --backend process flag (exit {0})" -f $flagRun.Code)
    Write-Host (($flagRun.Stdout -split "`n" | Select-Object -Last 2) -join "`n")
}
Assert-NoRenderer '--backend flag' | Out-Null

# 11. a backend name the shell does not know is refused, not ignored.
$bogus = Invoke-Probe -Page (Join-Path $pages 'blank.html') -Backend $null `
    -Extra @('--backend', 'bogus')
if ($bogus.Code -ne 0 -and ($bogus.Stdout + $bogus.Stderr) -match "backend wants local or process") {
    Write-Host "PASS  unknown backend is refused with a message"
} else {
    $failures++
    Write-Host ("FAIL  unknown backend: exit {0}" -f $bogus.Code)
}

# 12. the launcher starts the GUI shell in process mode by default (and
#     falls back to local on request); -DryRun keeps the test headless.
$launcher = Join-Path $root 'tools/agave.ps1'
$dry = & pwsh -File $launcher -DryRun 2>&1 | Out-String
$dryLocal = & pwsh -File $launcher -DryRun -Local 2>&1 | Out-String
if ($dry -match '--backend process' -and $dryLocal -match '--backend local') {
    Write-Host "PASS  launcher: process by default, -Local opts out"
} else {
    $failures++
    Write-Host "FAIL  launcher dry run did not name the expected backend"
    Write-Host $dry
    Write-Host $dryLocal
}

# 13. a command the host issues before there is a page is reported as dropped.
#     It used to vanish: eval on a view that has not navigated yet queued, found
#     no backend view, and nothing happened.
$dead = Invoke-Probe -Page (Join-Path $pages 'blank.html') -Wait 300 -Settle 200 `
    -Extra @('--eval-before-nav', "document.title='before-nav'") `
    -Js 'probeExit("title=" + document.title);'
$deadText = $dead.Stdout + $dead.Stderr
if ($dead.Code -eq 0 -and $deadText -match 'dropped=\d+ kind=eval reason=(no page yet|no live document)' -and
    $deadText -notmatch 'title=before-nav') {
    Write-Host "PASS  dropped: eval with no page is reported, not swallowed"
} else {
    $failures++
    Write-Host ("FAIL  dropped: eval with no page (exit {0})" -f $dead.Code)
    Write-Host (($deadText -split "`n" | Where-Object { $_ -match 'dropped|title=|exit' } | Select-Object -Last 4) -join "`n")
}

# 14. the contract for "eval during a navigation": either it ran or the host was
#     told it was dropped - never both missing. --eval-early posts the marker
#     right after navigate() returns, i.e. while the document swap is in flight,
#     so this lands either before a document exists (dropped) or in the old one
#     (the marker arrives through on_message).
$early = Invoke-Probe -Page (Join-Path $pages 'blank.html') -Wait 600 -Settle 300 `
    -Extra @('--eval-early', "window.agavePostMessage('early-ran')") `
    -Js 'probeExit("done");'
$earlyText = $early.Stdout + $early.Stderr
$earlyRan = $earlyText -match 'early-ran'
$earlyDropped = $earlyText -match 'dropped=\d+ kind=eval'
if ($early.Code -eq 0 -and (($earlyRan -and -not $earlyDropped) -or ($earlyDropped -and -not $earlyRan))) {
    Write-Host ("PASS  eval during navigation: {0}" -f $(if ($earlyRan) { 'ran' } else { 'reported as dropped' }))
} else {
    $failures++
    Write-Host ("FAIL  eval during navigation (exit {0}, ran={1}, dropped={2})" -f $early.Code, $earlyRan, $earlyDropped)
    Write-Host (($earlyText -split "`n" | Where-Object { $_ -match 'early-ran|dropped|exit' } | Select-Object -Last 4) -join "`n")
}

# 15. the renderer refuses a command stamped for a document it has left, and the
#     control turns that into on_command_dropped. AGB_IPC_STALE_EPOCH makes the
#     renderer treat every stamp as one document old (the real window - a
#     command that waits out a navigation commit - is too narrow to hit on
#     purpose), so this is the deterministic check of the reporting path.
$env:AGB_IPC_STALE_EPOCH = '1'
try {
    $stale = Invoke-Probe -Page (Join-Path $pages 'blank.html') -Wait 300 -Settle 200 `
        -Js 'probeExit("should-not-run");'
} finally {
    Remove-Item Env:AGB_IPC_STALE_EPOCH -ErrorAction SilentlyContinue
}
$staleText = $stale.Stdout + $stale.Stderr
if ($staleText -match 'dropped=\d+ kind=eval reason=document replaced by a navigation' -and
    $staleText -notmatch 'should-not-run') {
    Write-Host "PASS  stale document: renderer refuses the command, host is told"
} else {
    $failures++
    Write-Host "FAIL  stale document: no drop reported by the renderer path"
    Write-Host (($staleText -split "`n" | Where-Object { $_ -match 'dropped|should-not-run|exit' } | Select-Object -Last 4) -join "`n")
}
Assert-NoRenderer 'stale' | Out-Null

# 17. a renderer that really crashes leaves a report: environment, the faulting
#     address and a symbol-resolved stack (PDBs sit next to the modules). The
#     host shows that text on its "Page crashed" page - the page is written out
#     here and loaded back, so the assertion is about what a user would read.
$sadPage = Join-Path $build 'process_backend_sadpage.html'
Remove-Item $sadPage -ErrorAction SilentlyContinue
$env:AGB_PROBE_CRASH_PAGE = $sadPage
$env:AGB_RENDER_CRASH_AV = '600'
try {
    $crash = Invoke-Probe -Page (Join-Path $pages 'blank.html') -Wait 900 -Settle 300
} finally {
    Remove-Item Env:AGB_PROBE_CRASH_PAGE -ErrorAction SilentlyContinue
    Remove-Item Env:AGB_RENDER_CRASH_AV -ErrorAction SilentlyContinue
}
$crashText = $crash.Stdout + $crash.Stderr
$crashOk = $crash.Code -eq 0 -and
           $crashText -match '\[probe\] crash code=-1073741819' -and
           $crashText -match 'exception: 0xc0000005 \(ACCESS_VIOLATION\)' -and
           $crashText -match 'fault: write 0x0' -and
           $crashText -match 'symbols: dbghelp' -and
           $crashText -match '#00 agave_render_host\.exe!\S+\+0x[0-9a-f]+ \(' -and
           $crashText -match 'AGB_VIEW_BACKEND=process'

$sadOk = $false
if (Test-Path $sadPage) {
    $shown = Invoke-Probe -Page $sadPage -Wait 300 -Settle 200 `
        -Js 'probeExit("crashed=" + (document.title === "Page crashed") + " exception=" + (document.body.innerText.indexOf("exception: 0xc0000005") >= 0) + " frames=" + (document.body.innerText.indexOf(".c:") >= 0));'
    $shownText = $shown.Stdout + $shown.Stderr
    $sadOk = $shown.Code -eq 0 -and $shownText -match 'crashed=true exception=true frames=true'
}

if ($crashOk -and $sadOk) {
    Write-Host "PASS  crash report: environment + symbols, shown on the crashed page"
} else {
    $failures++
    Write-Host ("FAIL  crash report (report={0} page={1})" -f $crashOk, $sadOk)
    Write-Host (($crashText -split "`n" | Where-Object { $_ -match 'crash code=|exception:|symbols:|#00 ' } | Select-Object -First 4) -join "`n")
}
Assert-NoRenderer 'crash report' | Out-Null
Remove-Item $sadPage -ErrorAction SilentlyContinue

Remove-Item (Join-Path $build 'process_backend_out.txt'),
            (Join-Path $build 'process_backend_err.txt'),
            (Join-Path $build 'process_backend_eval.js') -ErrorAction SilentlyContinue

if ($failures -gt 0) {
    Write-Host "`nPROCESS BACKEND CHECKS FAILED ($failures)"
    exit 1
}

Write-Host "`nALL PROCESS BACKEND CHECKS PASSED"
exit 0