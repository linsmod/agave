# tools/run_process_backend_tests.ps1
#
# The `process` render backend (S3 B2): one agave_render_host.exe per view, the
# protocol over a named shared section (src/webview_control/AgwViewIpc.h).
#
# Every case runs the probe with AGB_VIEW_BACKEND=process and asserts the same
# things a local view answers, so the boundary cannot silently degrade:
#
#   - load + title + timers, paint (--shot), keyboard input, injected assets
#     (inspector.js, vConsole), clipboard copy, history counts, context menu
#   - large command text (32 KB injection, 300 KiB --eval-file), which used to
#     be cut at 4 KiB by the renderer (R169)
#   - renderer crash / respawn / crash report, hung-tab close, multiview
#     responsiveness, protocol stats, launcher/backend selection
#
# Execution model (2026-11-10): independent cases run **in parallel** (default
# 4 at a time), the same pool pattern tools/run_all_tests.ps1 uses for suites.
# Each case is its own probe process with its own AGB_USERDATA profile and its
# own AGB_RENDER_TAG, so cookie/cache state and the "no orphaned renderer"
# assertion stay disjoint. Cases with a hard conflict stay serial:
#
#   - crash / respawn / crash-report  write agb_crash.txt in the cwd and set
#                                     AGB_PROBE_CRASH_PAGE / AGB_RENDER_CRASH_*
#   - protocol stats                  measures copy latency; parallel load would
#                                     make the numbers meaningless
#   - multiview probes                assert two-view latency (pong/alive ms)
#   - launcher / hung-tab close       spawn their own processes and timing
#
# Environment is inherited by Start-Process at spawn time; the pool sets the
# per-case values immediately before each spawn (single-threaded spawn loop), so
# cases with different env/backend never race.
#
# Usage:
#   pwsh -File tools/run_process_backend_tests.ps1            # ~30 s
#   pwsh -File tools/run_process_backend_tests.ps1 -Parallel 8
#   pwsh -File tools/run_process_backend_tests.ps1 -Sequential  # debug order

param(
    [string]$BuildDir = 'build-zig-native',
    [int]$Parallel = 4,
    [switch]$Sequential
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

if ($Sequential) { $Parallel = 1 }
$Parallel = [math]::Max(1, [math]::Min(8, $Parallel))

$failures = 0
$total = [Diagnostics.Stopwatch]::StartNew()
$results = New-Object System.Collections.ArrayList

# Probe artifacts (profiles, shots, crash pages, per-case logs) live under the
# build tree, not %TEMP%: the probe process has to write them itself, and some
# sandboxes only grant writes inside the workspace. The directory is removed
# after a green run.
$runRoot = Join-Path $build ('process-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null

# Renderers this run owns. Every renderer carries `--tag <AGB_RENDER_TAG>` from
# the control that spawned it; each case gets its own tag under this prefix, so
# a renderer without it belongs to a developer's own shell - never counted,
# never killed. (The owner pid is in the command line too, for diagnostics.)
$runTag = 'agvtest' + ([guid]::NewGuid().ToString('N').Substring(0, 12))

$optionalEnv = @(
    'AGB_HISTORY_DEBUG', 'AGB_IPC_STALE_EPOCH', 'AGB_IPC_STATS',
    'AGB_RENDER_CRASH_MS', 'AGB_RENDER_CRASH_AV', 'AGB_PROBE_CRASH_EVAL',
    'AGB_PROBE_CRASH_PAGE', 'AGB_PROBE_RESPAWN', 'AGB_RENDER_LOG'
)
$savedEnv = @{}
foreach ($name in @('AGB_VIEW_BACKEND', 'AGB_USERDATA', 'AGB_RENDER_TAG') + $optionalEnv) {
    $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name)
}

function Reset-OptionalEnv {
    foreach ($name in $optionalEnv) {
        Remove-Item "Env:$name" -ErrorAction SilentlyContinue
    }
}

# A page path becomes file:///...; a scheme URL (agave://, http://) passes
# through unchanged.
function File-Url {
    param([string]$Path)
    if ($Path -match '^[a-zA-Z][a-zA-Z0-9+.-]*:') { return $Path }
    return 'file:///' + ($Path -replace '\\', '/')
}

function Get-StrayRenderers {
    param([string]$Tag)
    $stray = @()
    foreach ($p in (Get-CimInstance Win32_Process -Filter "Name='agave_render_host.exe'" -ErrorAction SilentlyContinue)) {
        if ($p.CommandLine -and $p.CommandLine.Contains($Tag)) { $stray += $p }
    }
    return $stray
}

function Assert-NoRenderer {
    param([string]$Tag, [string]$What)

    # Teardown is asynchronous: the control interrupts, sends EXIT and only then
    # waits/kills, so a renderer may still be unwinding when the probe returns.
    for ($i = 0; $i -lt 30; $i++) {
        $left = @(Get-StrayRenderers -Tag $Tag)
        if ($left.Count -eq 0) { return $true }
        Start-Sleep -Milliseconds 100
    }

    $left = @(Get-StrayRenderers -Tag $Tag)
    if ($left.Count -gt 0) {
        $script:failures++
        Write-Host ("FAIL  {0}: {1} orphaned renderer process(es): {2}" -f $What, $left.Count,
                    (($left | ForEach-Object { $_.ProcessId }) -join ', '))
        $left | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
        return $false
    }
    return $true
}

# ---- case definition -----------------------------------------------------

$cases = New-Object System.Collections.ArrayList

function Add-Case {
    param(
        [string]$Name,
        [string]$Url,
        [string]$Js = '',
        [int]$Wait = 1500,
        [int]$Settle = 300,
        [string[]]$Extra = @(),
        [string]$Backend = 'process',
        [hashtable]$Env = @{},
        [int]$Timeout = 25000,
        [scriptblock]$Prepare,
        [scriptblock]$Check,
        [switch]$Serial
    )

    [void]$cases.Add([pscustomobject]@{
        Name = $Name; Url = $Url; Js = $Js; Wait = $Wait; Settle = $Settle
        Extra = $Extra; Backend = $Backend; Env = $Env; Timeout = $Timeout
        Prepare = $Prepare; Check = $Check; Serial = [bool]$Serial
    })
}

# ---- probe job (start now, complete later) -------------------------------

function Start-Probe {
    param($Case)

    $dir = Join-Path $runRoot $Case.Name
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $out = Join-Path $dir 'out.txt'
    $err = Join-Path $dir 'err.txt'
    $evalFile = Join-Path $dir 'eval.js'

    if ($Case.Js) { Set-Content -Path $evalFile -Value $Case.Js -Encoding ascii }

    # Per-case environment snapshot. Start-Process copies the environment now,
    # so these values cannot race with the pool's other spawns.
    $profile = Join-Path $dir 'profile'
    New-Item -ItemType Directory -Force -Path $profile | Out-Null
    $env:AGB_USERDATA = $profile
    $tag = "$runTag-$($Case.Name)"
    $env:AGB_RENDER_TAG = $tag
    Reset-OptionalEnv
    if ($Case.Backend) { $env:AGB_VIEW_BACKEND = $Case.Backend }
    else { Remove-Item Env:AGB_VIEW_BACKEND -ErrorAction SilentlyContinue }
    foreach ($key in $Case.Env.Keys) { Set-Item "Env:$key" $Case.Env[$key] }

    if ($Case.Prepare) { & $Case.Prepare }

    $args = @('--probe', $Case.Url, '--wait', "$($Case.Wait)",
              '--settle', "$($Case.Settle)", '--timeout', "$($Case.Timeout)")
    if ($Case.Js) { $args += @('--eval-file', $evalFile) }
    if ($Case.Extra.Count -gt 0) { $args += $Case.Extra }

    $proc = Start-Process -FilePath $exe -NoNewWindow -PassThru `
        -ArgumentList $args -RedirectStandardOutput $out -RedirectStandardError $err

    return [pscustomobject]@{
        Name = $Case.Name; Case = $Case; Proc = $proc; Out = $out; Err = $err
        Tag = $tag; Started = [Diagnostics.Stopwatch]::StartNew(); Serial = $false
    }
}

function Read-Probe {
    param($Job, [int]$HardTimeoutSeconds = 60)

    $deadline = (Get-Date).AddSeconds($HardTimeoutSeconds)
    while (-not $Job.Proc.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }

    $timedOut = $false
    if (-not $Job.Proc.HasExited) {
        $timedOut = $true
        try { $Job.Proc.Kill() } catch { }
        try { $Job.Proc.WaitForExit(3000) | Out-Null } catch { }
    }

    $Job.Started.Stop()

    $code = -1
    try { $code = $Job.Proc.ExitCode } catch { }

    $stdout = ''
    $stderr = ''
    if (Test-Path $Job.Out) { $stdout = [string](Get-Content $Job.Out -Raw -ErrorAction SilentlyContinue) }
    if (Test-Path $Job.Err) { $stderr = [string](Get-Content $Job.Err -Raw -ErrorAction SilentlyContinue) }

    [pscustomobject]@{
        Name    = $Job.Name
        Code    = $code
        Stdout  = $stdout
        Stderr  = $stderr
        Text    = ($stdout + $stderr)
        TimedOut = $timedOut
        Seconds = [math]::Round($Job.Started.Elapsed.TotalSeconds, 1)
    }
}

# Runs one probe synchronously (used by a Check that needs a second probe, and
# by the serial crash-report case).
function Invoke-ProbeSync {
    param(
        [string]$Name, [string]$Url, [string]$Js = '',
        [int]$Wait = 300, [int]$Settle = 300, [string[]]$Extra = @(),
        [string]$Backend = 'process', [hashtable]$Env = @{}
    )

    $case = [pscustomobject]@{
        Name = $Name; Url = $Url; Js = $Js; Wait = $Wait; Settle = $Settle
        Extra = $Extra; Backend = $Backend; Env = $Env; Timeout = 25000
        Prepare = $null
    }
    $job = Start-Probe $case
    $result = Read-Probe $job
    [void](Assert-NoRenderer -Tag $job.Tag -What $Name)
    return $result
}

function Complete-Probe {
    param($Job)

    $r = Read-Probe $Job
    [void](Assert-NoRenderer -Tag $Job.Tag -What $Job.Name)

    $problem = $null
    if ($Job.Case.Check) {
        try { $problem = & $Job.Case.Check $r }
        catch { $problem = "check threw: $($_.Exception.Message)" }
    }

    if ($problem -or $r.TimedOut) {
        $script:failures++
        Write-Host ("FAIL  {0} ({1}s)" -f $Job.Name, $r.Seconds)
        if ($r.TimedOut) { Write-Host "      probe did not exit (hard timeout)" }
        if ($problem) {
            $text = if ($problem -is [string]) { $problem } else { ($problem | Out-String) }
            @($text -split "`n" | Where-Object { $_.Trim().Length -gt 0 } | Select-Object -First 4) |
                ForEach-Object { Write-Host ("      " + $_.TrimEnd()) }
        }
    } else {
        Write-Host ("PASS  {0} ({1}s)" -f $Job.Name, $r.Seconds)
    }

    [void]$results.Add([pscustomobject]@{
        Name = $Job.Name; Seconds = $r.Seconds; Failed = [bool]($problem -or $r.TimedOut)
    })
}

# =========================================================================
# Parallel cases: independent probes with disjoint profiles and tags.
# =========================================================================

Add-Case -Name 'load-title-timers' -Url (File-Url (Join-Path $pages 'cap_hidden_view.html')) `
    -Js 'probeExit("title=" + document.title + " ticks=" + window.__ticks);' -Wait 1500 -Settle 300 `
    -Check {
        param($r)
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        if ($r.Text -notmatch 'title=hidden view') { return "no title: $($r.Text)" }
        $ticks = if ($r.Text -match 'ticks=(\d+)') { [int]$Matches[1] } else { -1 }
        if ($ticks -lt 5) { return "ticks=$ticks" }
        return $null
    }

$shot = Join-Path $runRoot 'paint.bmp'
Add-Case -Name 'paint' -Url (File-Url (Join-Path $pages 'cap_hidden_view.html')) `
    -Wait 1500 -Settle 400 -Extra @('--shot', $shot) `
    -Check {
        param($r)
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        if (-not (Test-Path $shot)) { return "no shot file" }
        $pixels = & python $pixelCheck $shot '150,120,1478c8' --tol 12 2>&1
        if ($LASTEXITCODE -ne 0) { return (($pixels -split "`n") -join "`n") }
        return $null
    }

Add-Case -Name 'input' -Url (File-Url (Join-Path $pages 'cap_typing.html')) `
    -Wait 800 -Settle 300 -Extra @('--keys', 'ab1') `
    -Js 'probeExit("value=" + document.getElementById("text").value);' `
    -Check {
        param($r)
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        if ($r.Text -notmatch 'value=ab1') { return $r.Text }
        return $null
    }

# An injected asset larger than the inline arena: inspector.js is 32 KB, so it
# travels through a one-shot payload section. Before that channel existed the
# renderer cut every script at 4 KiB and the injection silently did nothing
# while the host still said "Injected ...".
Add-Case -Name 'inject-inspector' -Url (File-Url (Join-Path $pages 'inspector_demo.html')) `
    -Wait 400 -Settle 400 -Extra @('--inject', 'inspector') `
    -Js 'probeExit("ins=" + (window.__agaveInspector ? "yes" : "no") + " root=" + (document.getElementById("__agave_inspector__") ? "yes" : "no") + " hit=" + (window.__agaveInspector && window.__agaveInspector.inspectAt(120,140) ? "yes" : "no"));' `
    -Check {
        param($r)
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        if ($r.Text -notmatch 'ins=yes root=yes hit=yes') { return $r.Text }
        return $null
    }

# A 300 KiB --eval-file: the bytes must reach the renderer whole. The old path
# cut the file at 8 KiB in the shell, then at 32 KiB in the control; markers at
# both ends prove neither cut happens.
$bigScript = "window.__bigStart=1;`n// " + ('x' * 300000) + "`n;window.__bigEnd='tail-ok';probeExit('start=' + window.__bigStart + ' end=' + window.__bigEnd);"
Add-Case -Name 'large-eval' -Url (File-Url (Join-Path $pages 'blank.html')) `
    -Wait 300 -Settle 300 -Js $bigScript `
    -Check {
        param($r)
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        if ($r.Text -notmatch 'start=1 end=tail-ok') { return $r.Text }
        return $null
    }

# vConsole (286 KiB) injects and mounts. Besides the payload it covers the
# focus-after-DOM-mutation path: the shell focuses the view right after the
# eval, and Frame::setIsActive used to measure the selection on a dirty tree
# (ASSERT in RenderBlock::selectionGapRects, R170).
Add-Case -Name 'vconsole' -Url (File-Url (Join-Path $pages 'inspector_demo.html')) `
    -Wait 400 -Settle 2500 -Extra @('--inject', 'vconsole') `
    -Js 'probeExit("vc=" + !!window.__agaveVConsole + " root=" + !!document.getElementById("__vconsole") + " panel=" + !!(document.querySelector && document.querySelector(".vc-switch")));' `
    -Check {
        param($r)
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        if ($r.Text -notmatch 'vc=true root=true panel=true') { return $r.Text }
        return $null
    }

# Clipboard: a page-initiated copy (navigator.clipboard.writeText, the path
# inspector.js uses for "Copy element info") has to reach the OS clipboard
# through the host. The renderer used to have no clipboard callback at all, so
# the copy silently did nothing (R171).
Add-Case -Name 'clipboard' -Url (File-Url (Join-Path $pages 'blank.html')) `
    -Wait 300 -Settle 600 -Js "try { navigator.clipboard.writeText('process-copy'); } catch (e) {} probeExit('copied');" `
    -Prepare { Set-Clipboard -Value '' } `
    -Check {
        param($r)
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        $clipboard = Get-Clipboard -Raw
        if ($clipboard -ne 'process-copy') { return "clipboard=[$clipboard]" }
        return $null
    }

# History: after a link navigation the control must know the back count. The
# local backend gets it from cb_history_status; without the crossing the shell's
# back/forward buttons stayed disabled forever in process mode.
Add-Case -Name 'history' -Url (File-Url (Join-Path $pages 'window_links.html')) `
    -Wait 300 -Settle 1400 -Extra @('--click', '200,45') `
    -Js 'probeExit("url=" + location.pathname);' `
    -Env @{ AGB_HISTORY_DEBUG = '1' } `
    -Check {
        param($r)
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        if ($r.Text -notmatch '\[hist\] .*back=1') { return "no back=1: $($r.Text)" }
        if ($r.Text -notmatch 'url=/E:/mybrowsers/agave/tools/pages/window_target\.html') { return $r.Text }
        return $null
    }

Add-Case -Name 'default-backend' -Url (File-Url (Join-Path $pages 'cap_hidden_view.html')) `
    -Wait 1500 -Settle 300 -Backend '' `
    -Js 'probeExit("title=" + document.title);' `
    -Check {
        param($r)
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        if ($r.Text -notmatch 'title=hidden view') { return $r.Text }
        return $null
    }

Add-Case -Name 'backend-flag' -Url (File-Url (Join-Path $pages 'cap_hidden_view.html')) `
    -Wait 1500 -Settle 300 -Backend '' -Extra @('--backend', 'process') `
    -Js 'probeExit("title=" + document.title);' `
    -Check {
        param($r)
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        if ($r.Text -notmatch 'title=hidden view') { return $r.Text }
        if ($r.Text -notmatch 'process backend: view') { return "no renderer: $($r.Text)" }
        return $null
    }

Add-Case -Name 'unknown-backend' -Url (File-Url (Join-Path $pages 'blank.html')) `
    -Wait 300 -Settle 200 -Backend '' -Extra @('--backend', 'bogus') `
    -Check {
        param($r)
        if ($r.Code -eq 0) { return "exit=0 (expected a refusal)" }
        if ($r.Text -notmatch 'backend wants local or process') { return $r.Text }
        return $null
    }

# A command the host issues before there is a page is reported as dropped.
Add-Case -Name 'dropped-eval' -Url (File-Url (Join-Path $pages 'blank.html')) `
    -Wait 300 -Settle 200 -Extra @('--eval-before-nav', "document.title='before-nav'") `
    -Js 'probeExit("title=" + document.title);' `
    -Check {
        param($r)
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        if ($r.Text -match 'title=before-nav') { return "the dropped eval ran" }
        if ($r.Text -notmatch 'dropped=\d+ kind=eval reason=(no page yet|no live document)') { return $r.Text }
        return $null
    }

# "eval during a navigation": either it ran or the host was told it was
# dropped - never both missing.
Add-Case -Name 'eval-during-nav' -Url (File-Url (Join-Path $pages 'blank.html')) `
    -Wait 600 -Settle 300 -Extra @('--eval-early', "window.agavePostMessage('early-ran')") `
    -Js 'probeExit("done");' `
    -Check {
        param($r)
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        $ran = $r.Text -match 'early-ran'
        $dropped = $r.Text -match 'dropped=\d+ kind=eval'
        if ($ran -eq $dropped) { return "ran=$ran dropped=$dropped" }
        return $null
    }

# The renderer refuses a command stamped for a document it has left; the control
# turns that into on_command_dropped. The probe's own eval is the command that
# gets refused, so there is no probeExit(): the run ends on a short timeout and
# the check looks at the drop report, not at the exit code.
Add-Case -Name 'stale-epoch' -Url (File-Url (Join-Path $pages 'blank.html')) `
    -Wait 300 -Settle 200 -Timeout 5000 -Env @{ AGB_IPC_STALE_EPOCH = '1' } `
    -Js 'probeExit("should-not-run");' `
    -Check {
        param($r)
        if ($r.Text -match 'should-not-run') { return "the stale eval ran" }
        if ($r.Text -notmatch 'dropped=\d+ kind=eval reason=document replaced by a navigation') { return $r.Text }
        return $null
    }

# Right click: the request is raised in the renderer and crosses the boundary.
Add-Case -Name 'context-menu' -Url (File-Url (Join-Path $pages 'window_links.html')) `
    -Wait 300 -Settle 300 -Extra @('--right-click', '60,40') `
    -Js 'probeExit("done");' `
    -Check {
        param($r)
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        if ($r.Text -notmatch '\[ctx\] menu x=60 y=40 link=.*window_target\.html') { return $r.Text }
        return $null
    }

# An internal page (agave://) survives a reload in the renderer.
Add-Case -Name 'internal-page' -Url 'agave://downloads' `
    -Wait 500 -Settle 300 -Extra @('--reload', '2') `
    -Js 'probeExit("title=" + document.title);' `
    -Check {
        param($r)
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        if ($r.Text -match 'page crashed|fail resource') { return $r.Text }
        if ($r.Text -notmatch '\[exit\] title=下载') { return $r.Text }
        return $null
    }

# =========================================================================
# Serial cases: crash artifacts / timing measurements / second processes.
# =========================================================================

Add-Case -Name 'crash' -Serial -Url (File-Url (Join-Path $pages 'cap_hidden_view.html')) `
    -Wait 1200 -Settle 200 -Env @{ AGB_RENDER_CRASH_MS = '400' } `
    -Js 'probeExit("alive");' `
    -Check {
        param($r)
        if ($r.Text -notmatch '\[probe\] crash code=42') { return $r.Text }
        if ($r.Text -notmatch 'report_bytes=0') { return $r.Text }
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        return $null
    }

Add-Case -Name 'respawn' -Serial -Url (File-Url (Join-Path $pages 'cap_hidden_view.html')) `
    -Wait 1200 -Settle 300 -Env @{ AGB_RENDER_CRASH_MS = '400'; AGB_PROBE_RESPAWN = '1' } `
    -Js 'probeExit("title=" + document.title);' `
    -Check {
        param($r)
        if ($r.Text -notmatch 'respawn: loading') { return $r.Text }
        if ($r.Text -notmatch 'title=respawned') { return $r.Text }
        return $null
    }

# Protocol cost: AGB_IPC_STATS reports frame publish -> control poll latency and
# the mirror copy. Serial on purpose: parallel load makes the numbers
# meaningless.
Add-Case -Name 'protocol-stats' -Serial -Url (File-Url (Join-Path $pages 'cap_hidden_view.html')) `
    -Wait 1600 -Settle 200 -Env @{ AGB_IPC_STATS = '1' } `
    -Js 'probeExit("stats");' `
    -Check {
        param($r)
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        $copyMax = 0
        $copyMin = 0
        foreach ($m in [regex]::Matches($r.Text, 'copy_us=(\d+)')) {
            $v = [int]$m.Groups[1].Value
            if ($v -gt $copyMax) { $copyMax = $v }
            if ($copyMin -eq 0 -or $v -lt $copyMin) { $copyMin = $v }
        }
        $frames = [regex]::Matches($r.Text, 'frames=\d+ latency_avg_us=\d+ max_us=\d+')
        if ($frames.Count -eq 0 -or $copyMin -le 0 -or $copyMin -ge 2000 -or $copyMax -ge 8000) {
            return "frames=$($frames.Count) copyMin=$copyMin copyMax=$copyMax"
        }
        Write-Host ("      copy min {0} us / max {1} us" -f $copyMin, $copyMax)
        return $null
    }

# Crash report: the dying renderer writes environment + symbols into the shared
# section; the host shows it on its "Page crashed" page. The second probe loads
# that page back, so the assertion is about what a user would read.
$sadPage = Join-Path $runRoot 'sadpage.html'
Add-Case -Name 'crash-report' -Serial -Url (File-Url (Join-Path $pages 'blank.html')) `
    -Wait 900 -Settle 300 -Env @{ AGB_PROBE_CRASH_PAGE = $sadPage; AGB_RENDER_CRASH_AV = '600' } `
    -Check {
        param($r)
        $ok = $r.Code -eq 0 -and
              $r.Text -match '\[probe\] crash code=-1073741819' -and
              $r.Text -match 'exception: 0xc0000005 \(ACCESS_VIOLATION\)' -and
              $r.Text -match 'fault: write 0x0' -and
              $r.Text -match 'symbols: dbghelp' -and
              $r.Text -match '#00 agave_render_host\.exe!\S+\+0x[0-9a-f]+ \(' -and
              $r.Text -match 'AGB_VIEW_BACKEND=process'
        if (-not $ok) { return "crash report missing pieces: $($r.Text)" }

        if (-not (Test-Path $sadPage)) { return "no sad page at $sadPage" }
        $shown = Invoke-ProbeSync -Name 'crash-report-page' -Url (File-Url $sadPage) `
            -Wait 300 -Settle 200 `
            -Js 'probeExit("crashed=" + (document.title === "Page crashed") + " exception=" + (document.body.innerText.indexOf("exception: 0xc0000005") >= 0) + " frames=" + (document.body.innerText.indexOf(".c:") >= 0));'
        if ($shown.Code -ne 0) { return "sad page probe exit=$($shown.Code)" }
        if ($shown.Text -notmatch 'crashed=true exception=true frames=true') { return $shown.Text }
        return $null
    }

# P1-ac: an eval issued right after load_html (new document not committed yet)
# is reported as dropped, never falling into the legacy interpreter.
Add-Case -Name 'early-eval-after-load-html' -Serial -Url (File-Url (Join-Path $pages 'blank.html')) `
    -Wait 300 -Settle 800 `
    -Env @{ AGB_RENDER_CRASH_AV = '500'; AGB_PROBE_CRASH_EVAL = '1' } `
    -Check {
        param($r)
        if ($r.Text -match 'ASSERTION FAILED|\[crash\]') { return $r.Text }
        if ($r.Text -notmatch 'dropped=\d+ kind=eval reason=no live document') { return $r.Text }
        return $null
    }

# =========================================================================
# Run: parallel pool, then the serial cases and the external probes.
# =========================================================================

try {
    $parallelCases = @($cases | Where-Object { -not $_.Serial })
    $serialCases = @($cases | Where-Object { $_.Serial })

    $pending = [System.Collections.Generic.Queue[object]]::new()
    $parallelCases | ForEach-Object { $pending.Enqueue($_) }
    $running = @()

    while ($pending.Count -gt 0 -or $running.Count -gt 0) {
        while ($running.Count -lt $Parallel -and $pending.Count -gt 0) {
            $job = Start-Probe $pending.Dequeue()
            if ($job) { $running += $job }
        }

        $done = @($running | Where-Object { $_.Proc.HasExited })
        if ($done.Count -eq 0) {
            if ($running.Count -eq 0) { break }
            Start-Sleep -Milliseconds 100
            continue
        }

        foreach ($job in $done) {
            $running = @($running | Where-Object { $_ -ne $job })
            Complete-Probe $job
        }
    }

    foreach ($case in $serialCases) {
        $job = Start-Probe $case
        if (-not $job) { continue }
        $job.Serial = $true
        $job.Proc.WaitForExit()
        Complete-Probe $job
    }

    # ---- external probes (own executables / dry runs) -------------------
    $env:AGB_RENDER_TAG = "$runTag-external"
    $mv = Join-Path $build 'agave_multiview_probe.exe'
    if (-not (Test-Path $mv)) { throw "agave_multiview_probe.exe not found: $mv" }

    $mvOut = Join-Path $runRoot 'multiview_out.txt'
    $mvErr = Join-Path $runRoot 'multiview_err.txt'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $mv -NoNewWindow -PassThru -Wait `
         -ArgumentList @('--backend', 'process', '--interrupt-test') `
         -RedirectStandardOutput $mvOut -RedirectStandardError $mvErr
    $sw.Stop()
    $text = (Get-Content $mvOut -Raw) -as [string]
    $pong = if ($text -match 'pong_ms=(\d+)') { [int]$Matches[1] } else { -1 }
    $alive = if ($text -match 'alive_ms=(\d+)') { [int]$Matches[1] } else { -1 }
    if ($p.ExitCode -eq 0 -and $pong -ge 0 -and $pong -lt 200 -and $alive -ge 0 -and
        $alive -lt 300 -and $text -notmatch 'spin=0') {
        Write-Host ("PASS  multiview-process ({0}s)" -f [math]::Round($sw.Elapsed.TotalSeconds, 1))
        Write-Host ("      pong={0} ms, interrupt={1} ms" -f $pong, $alive)
    } else {
        $failures++
        Write-Host ("FAIL  multiview-process (exit {0}, pong={1}, alive={2})" -f $p.ExitCode, $pong, $alive)
        Write-Host (($text -split "`n" | Select-Object -Last 3) -join "`n")
    }
    [void](Assert-NoRenderer -Tag "$runTag-external" -What 'multiview-process')

    # The local baseline really serialises: two views, shared engine thread, B
    # never answers while A spins (the problem S3 exists for).
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $mv -NoNewWindow -PassThru -Wait `
         -ArgumentList @('--backend', 'local', '--allow-blocked', '--pong-wait', '2500') `
         -RedirectStandardOutput $mvOut -RedirectStandardError $mvErr
    $sw.Stop()
    $text = (Get-Content $mvOut -Raw) -as [string]
    if ($p.ExitCode -eq 0 -and $text -match 'blocked=1' -and $text -notmatch 'spin=0') {
        Write-Host ("PASS  local-baseline ({0}s)" -f [math]::Round($sw.Elapsed.TotalSeconds, 1))
    } else {
        $failures++
        Write-Host ("FAIL  local-baseline (exit {0}, expected blocked=1)" -f $p.ExitCode)
        Write-Host (($text -split "`n" | Select-Object -Last 3) -join "`n")
    }
    [void](Assert-NoRenderer -Tag "$runTag-external" -What 'local-baseline')

    # Closing a hung tab is immediate: the interrupt breaks the script, the
    # renderer exits, and the process is killed if it does not (no 2s wait).
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $mv -NoNewWindow -PassThru -Wait `
         -ArgumentList @('--backend', 'process', '--allow-blocked', '--pong-wait', '1200', '--destroy-test') `
         -RedirectStandardOutput $mvOut -RedirectStandardError $mvErr
    $sw.Stop()
    $text = (Get-Content $mvOut -Raw) -as [string]
    $destroy = if ($text -match 'destroy_ms=(\d+)') { [int]$Matches[1] } else { -1 }
    if ($p.ExitCode -eq 0 -and $destroy -ge 0 -and $destroy -lt 500) {
        Write-Host ("PASS  hung-tab-close ({0}s)" -f [math]::Round($sw.Elapsed.TotalSeconds, 1))
        Write-Host ("      destroy_ms={0}" -f $destroy)
    } else {
        $failures++
        Write-Host ("FAIL  hung-tab-close (exit {0}, destroy_ms={1})" -f $p.ExitCode, $destroy)
    }
    [void](Assert-NoRenderer -Tag "$runTag-external" -What 'hung-tab-close')

    # The launcher starts the GUI shell in process mode by default (and falls
    # back to local on request); -DryRun keeps the test headless.
    $launcher = Join-Path $root 'tools/agave.ps1'
    $dry = & pwsh -File $launcher -DryRun 2>&1 | Out-String
    $dryLocal = & pwsh -File $launcher -DryRun -Local 2>&1 | Out-String
    if ($dry -match '--backend process' -and $dryLocal -match '--backend local') {
        Write-Host "PASS  launcher"
    } else {
        $failures++
        Write-Host "FAIL  launcher dry run did not name the expected backend"
        Write-Host $dry
        Write-Host $dryLocal
    }
} finally {
    foreach ($name in $savedEnv.Keys) {
        if ($null -eq $savedEnv[$name]) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
        else { Set-Item "Env:$name" $savedEnv[$name] }
    }

    if ($failures -eq 0) {
        Remove-Item $runRoot -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        Write-Host ("logs kept under {0}" -f $runRoot)
    }
}

$total.Stop()

Write-Host ""
$slowest = @($results | Sort-Object Seconds -Descending | Select-Object -First 3)
if ($slowest.Count -gt 0) {
    Write-Host ("slowest: {0}" -f (($slowest | ForEach-Object { "$($_.Name)=$($_.Seconds)s" }) -join ', '))
}
$checked = $results.Count + 4   # probe cases + 3 external probes + launcher

if ($failures -gt 0) {
    Write-Host ("PROCESS BACKEND CHECKS FAILED ({0}) in {1}s" -f $failures, [math]::Round($total.Elapsed.TotalSeconds, 1))
    exit 1
}

Write-Host ("ALL PROCESS BACKEND CHECKS PASSED ({0} cases, {1}s, parallel={2})" -f
            $checked, [math]::Round($total.Elapsed.TotalSeconds, 1), $Parallel)
exit 0
