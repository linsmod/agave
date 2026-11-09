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

function Assert-NoRenderer {
    param([string]$What)

    # Teardown is asynchronous: the control interrupts, sends EXIT and only then
    # waits/kills, so a renderer may still be unwinding when the probe returns.
    # Give it a moment before calling it a leak.
    for ($i = 0; $i -lt 20; $i++) {
        $left = @(Get-Process agave_render_host -ErrorAction SilentlyContinue)
        if ($left.Count -eq 0) { return $true }
        Start-Sleep -Milliseconds 100
    }

    $left = @(Get-Process agave_render_host -ErrorAction SilentlyContinue)
    if ($left.Count -gt 0) {
        $script:failures++
        Write-Host ("FAIL  {0}: {1} renderer process(es) left behind" -f $What, $left.Count)
        $left | Stop-Process -Force
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

if ($crash.Code -eq 5 -and ($crash.Stdout + $crash.Stderr) -match 'load error code=42') {
    Write-Host "PASS  process backend: renderer crash is reported as a load error (exit 5)"
} else {
    $failures++
    Write-Host ("FAIL  process backend: crash handling (exit {0})" -f $crash.Code)
    Write-Host (($crash.Stdout -split "`n" | Select-Object -Last 3) -join "`n")
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

Remove-Item (Join-Path $build 'process_backend_out.txt'),
            (Join-Path $build 'process_backend_err.txt'),
            (Join-Path $build 'process_backend_eval.js') -ErrorAction SilentlyContinue

if ($failures -gt 0) {
    Write-Host "`nPROCESS BACKEND CHECKS FAILED ($failures)"
    exit 1
}

Write-Host "`nALL PROCESS BACKEND CHECKS PASSED"
exit 0