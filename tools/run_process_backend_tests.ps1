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
# Execution model (2026-11-10, parallelised): every case runs at the same time,
# the same per-case isolation the other suites use:
#
#   - cwd is the case directory, so agb_crash.txt is per case
#   - AGB_DIAG_DIR is per case, so watchdog / busy-stack files never collide
#   - AGB_USERDATA and AGB_RENDER_TAG are per case (cookie/cache and the
#     "no orphaned renderer" assertion stay disjoint)
#   - AGB_CLIPBOARD_FILE is a per-case file, so the clipboard copy cannot race
#     with another suite or with the developer's clipboard
#
# Crash / respawn / crash-report therefore no longer need a serial phase, and
# protocol stats / multiview latency assertions use load-tolerant limits (they
# catch a broken or absent crossing, not a micro-benchmark).
#
# Environment is inherited by Start-Process at spawn time; the pool sets the
# per-case values immediately before each spawn (single-threaded spawn loop), so
# cases with different env/backend never race.
#
# Usage:
#   pwsh -File tools/run_process_backend_tests.ps1            # ~12 s
#   pwsh -File tools/run_process_backend_tests.ps1 -Parallel 16
#   pwsh -File tools/run_process_backend_tests.ps1 -Sequential  # debug order

param(
    [string]$BuildDir = 'build-zig-native',
    [int]$Parallel = 16,
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
$Parallel = [math]::Max(1, [math]::Min(24, $Parallel))

# The same cross-suite probe gate the other suites use (a process-backend probe
# also owns a renderer, so letting it exceed the global cap starves everyone).
Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking

$failures = 0
$total = [Diagnostics.Stopwatch]::StartNew()
$results = New-Object System.Collections.ArrayList

# Probe artifacts (profiles, shots, crash pages, per-case logs) live under the
# build tree, not %TEMP%: the probe process has to write them itself, and some
# sandboxes only grant writes inside the workspace. The directory is removed
# after a green run.
$runRoot = Join-Path $build ('process-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
# stdout/stderr/eval files go to %TEMP%: redirecting them into the workspace is
# several times slower (file-system filter / real-time scan), and 22 cases x 2
# files dominated the suite. The probe's own files (crash page, shot, profile)
# stay under the build tree, which it can write.
$logRoot = Join-Path ([IO.Path]::GetTempPath()) ('agb-process-logs-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $logRoot | Out-Null

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
    $logDir = Join-Path $logRoot $Case.Name
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $out = Join-Path $logDir 'out.txt'
    $err = Join-Path $logDir 'err.txt'
    $evalFile = Join-Path $logDir 'eval.js'

    if ($Case.Js) { Set-Content -Path $evalFile -Value $Case.Js -Encoding ascii }

    # Per-case environment snapshot. Start-Process copies the environment now,
    # so these values cannot race with the pool's other spawns.
    $profile = Join-Path $dir 'profile'
    $diag = Join-Path $dir 'diag'
    New-Item -ItemType Directory -Force -Path $profile | Out-Null
    New-Item -ItemType Directory -Force -Path $diag | Out-Null
    $env:AGB_USERDATA = $profile
    $env:AGB_DIAG_DIR = $diag
    $env:AGB_CLIPBOARD_FILE = (Join-Path $dir 'clipboard.txt')
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

    $slot = Wait-TestProbeSlot $null
    $proc = Start-Process -FilePath $exe -NoNewWindow -PassThru -WorkingDirectory $dir `
        -ArgumentList $args -RedirectStandardOutput $out -RedirectStandardError $err
    if ($slot) { Set-Content -Path $slot -Value $proc.Id -Encoding ascii }

    return [pscustomobject]@{
        Name = $Case.Name; Case = $Case; Proc = $proc; Out = $out; Err = $err
        Dir = $dir; ClipFile = (Join-Path $dir 'clipboard.txt'); Slot = $slot
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
    Release-TestProbeSlot $Job.Slot

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
        Dir     = $Job.Dir
        ClipFile = $Job.ClipFile
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

# R185: a status/progress callback that runs on the engine thread makes the host
# touch its window cross-thread (the shell's onStatus -> updateStatus crashed with
# a first-chance AV). The probe reports the thread it saw once, so the
# remote-backend path - which is where the crash was caught - is covered.
Add-Case -Name 'callback-thread' -Url (File-Url (Join-Path $pages 'cap_hidden_view.html')) `
    -Js 'probeExit("ok");' -Wait 1200 -Settle 300 `
    -Check {
        param($r)
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        if ($r.Text -match 'callback thread violation') { return "state callback ran on the engine thread: $($r.Text)" }
        if ($r.Text -notmatch 'callback thread ok') { return "probe did not report the callback thread: $($r.Text)" }
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
    -Wait 1000 -Settle 300 -Extra @('--keys', 'ab1') `
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

# vConsole (286 KiB) injects, mounts and opens its panel on a switch click.
# Besides the payload it covers the focus-after-DOM-mutation path (R170), the
# legacy event factory Svelte's createEventDispatcher needs (R172) and the
# scroller's layout (ResizeObserver poll + UA form margin, R173/R174).
Add-Case -Name 'vconsole' -Url (File-Url (Join-Path $pages 'inspector_demo.html')) `
    -Wait 400 -Settle 2500 -Extra @('--inject', 'vconsole') `
    -Js 'var root=document.getElementById("__vconsole"), sw=document.querySelector(".vc-switch"); if (sw) sw.click(); setTimeout(function(){ var f=document.querySelector(".vc-scroller-footer"), v=document.querySelector(".vc-scroller-viewport"); var fr=f?f.getBoundingClientRect():null, vr=v?v.getBoundingClientRect():null; var layout=!!(fr&&vr&&fr.height>0&&Math.abs(fr.bottom-vr.bottom)<=1); probeExit("vc=" + !!window.__agaveVConsole + " root=" + !!root + " panel=" + !!sw + " open=" + (root && String(root.className).indexOf("vc-toggle") >= 0) + " layout=" + layout); }, 700);' `
    -Check {
        param($r)
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        if ($r.Text -notmatch 'vc=true root=true panel=true open=true layout=true') { return $r.Text }
        return $null
    }

# Clipboard: a page-initiated copy (navigator.clipboard.writeText, the path
# inspector.js uses for "Copy element info") has to reach the OS clipboard
# through the host. The renderer used to have no clipboard callback at all, so
# the copy silently did nothing (R171).
Add-Case -Name 'clipboard' -Url (File-Url (Join-Path $pages 'blank.html')) `
    -Wait 300 -Settle 600 -Js "try { navigator.clipboard.writeText('process-copy'); } catch (e) {} probeExit('copied');" `
    -Check {
        param($r)
        if ($r.Code -ne 0) { return "exit=$($r.Code)" }
        # The copy crosses the renderer boundary; the host writes it through the
        # same callback the local backend uses, which AGB_CLIPBOARD_FILE redirects
        # into this case's file.
        $clipboard = if ($r.ClipFile -and (Test-Path $r.ClipFile)) { Get-Content $r.ClipFile -Raw } else { '' }
        if ($clipboard -ne 'process-copy') { return "clipboard=[$clipboard]" }
        return $null
    }

# History: after a link navigation the control must know the back count. The
# local backend gets it from cb_history_status; without the crossing the shell's
# back/forward buttons stayed disabled forever in process mode.
Add-Case -Name 'history' -Url (File-Url (Join-Path $pages 'window_links.html')) `
    -Wait 300 -Settle 1400 -Extra @('--click', '200,45', '--eval-retry') `
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
        # Load-tolerant limits: this catches a missing or pathologically slow
        # crossing (0 us / tens of ms), not a micro-benchmark of memcpy.
        if ($frames.Count -eq 0 -or $copyMin -le 0 -or $copyMin -ge 5000 -or $copyMax -ge 20000) {
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

# One wait for every renderer this run owns: teardown is asynchronous, so poll
# until the tag prefix disappears instead of paying a 3 s wait per case.
function Wait-NoTaggedRenderer {
    param([int]$Seconds = 10)

    $deadline = (Get-Date).AddSeconds($Seconds)
    while ($true) {
        $left = @(Get-CimInstance Win32_Process -Filter "Name='agave_render_host.exe'" -ErrorAction SilentlyContinue |
                  Where-Object { $_.CommandLine -and $_.CommandLine.Contains($runTag) })
        if ($left.Count -eq 0) { return @() }
        if ((Get-Date) -gt $deadline) { return $left }
        Start-Sleep -Milliseconds 100
    }
}

try {
    $mv = Join-Path $build 'agave_multiview_probe.exe'
    if (-not (Test-Path $mv)) { throw "agave_multiview_probe.exe not found: $mv" }

    # ---- external probes: start them now, they run next to the case pool ----
    $env:AGB_RENDER_TAG = "$runTag-external"
    $ext = [ordered]@{}
    $ext.mv = Start-Process -FilePath $mv -NoNewWindow -PassThru `
        -ArgumentList @('--backend', 'process', '--interrupt-test') `
        -RedirectStandardOutput (Join-Path $runRoot 'mv.out') -RedirectStandardError (Join-Path $runRoot 'mv.err')
    $ext.local = Start-Process -FilePath $mv -NoNewWindow -PassThru `
        -ArgumentList @('--backend', 'local', '--allow-blocked', '--pong-wait', '2500') `
        -RedirectStandardOutput (Join-Path $runRoot 'local.out') -RedirectStandardError (Join-Path $runRoot 'local.err')
    $ext.destroy = Start-Process -FilePath $mv -NoNewWindow -PassThru `
        -ArgumentList @('--backend', 'process', '--allow-blocked', '--pong-wait', '1200', '--destroy-test') `
        -RedirectStandardOutput (Join-Path $runRoot 'destroy.out') -RedirectStandardError (Join-Path $runRoot 'destroy.err')

    # The launcher starts the GUI shell in process mode by default (and falls
    # back to local on request); -DryRun keeps the test headless.
    $launcher = Join-Path $root 'tools/agave.ps1'
    $ext.dry = Start-Process -FilePath 'pwsh' -NoNewWindow -PassThru `
        -ArgumentList @('-NoProfile', '-File', $launcher, '-DryRun') `
        -RedirectStandardOutput (Join-Path $runRoot 'dry.out') -RedirectStandardError (Join-Path $runRoot 'dry.err')
    $ext.dryLocal = Start-Process -FilePath 'pwsh' -NoNewWindow -PassThru `
        -ArgumentList @('-NoProfile', '-File', $launcher, '-DryRun', '-Local') `
        -RedirectStandardOutput (Join-Path $runRoot 'drylocal.out') -RedirectStandardError (Join-Path $runRoot 'drylocal.err')

    # ---- case pool: all of them, same start time ---------------------------
    $pending = [System.Collections.Generic.Queue[object]]::new()
    $cases | ForEach-Object { $pending.Enqueue($_) }
    $running = @()

    while ($pending.Count -gt 0 -or $running.Count -gt 0) {
        while ($running.Count -lt $Parallel -and $pending.Count -gt 0) {
            $job = Start-Probe $pending.Dequeue()
            if ($job) { $running += $job }
        }

        $done = @($running | Where-Object { $_.Proc.HasExited })
        if ($done.Count -eq 0) {
            if ($running.Count -eq 0) { break }
            Start-Sleep -Milliseconds 50
            continue
        }

        foreach ($job in $done) {
            $running = @($running | Where-Object { $_ -ne $job })
            Complete-Probe $job
        }
    }

    # ---- external results --------------------------------------------------
    $mvOut = Join-Path $runRoot 'mv.out'
    $mvErr = Join-Path $runRoot 'mv.err'
    $ext.mv.WaitForExit()
    $text = (Get-Content $mvOut -Raw) -as [string]
    $pong = if ($text -match 'pong_ms=(\d+)') { [int]$Matches[1] } else { -1 }
    $alive = if ($text -match 'alive_ms=(\d+)') { [int]$Matches[1] } else { -1 }
    if ($ext.mv.ExitCode -eq 0 -and $pong -ge 0 -and $pong -lt 400 -and $alive -ge 0 -and
        $alive -lt 600 -and $text -notmatch 'spin=0') {
        Write-Host "PASS  multiview-process"
        Write-Host ("      pong={0} ms, interrupt={1} ms" -f $pong, $alive)
    } else {
        $failures++
        Write-Host ("FAIL  multiview-process (exit {0}, pong={1}, alive={2})" -f $ext.mv.ExitCode, $pong, $alive)
        Write-Host (($text -split "`n" | Select-Object -Last 3) -join "`n")
    }

    $ext.local.WaitForExit()
    $text = (Get-Content (Join-Path $runRoot 'local.out') -Raw) -as [string]
    if ($ext.local.ExitCode -eq 0 -and $text -match 'blocked=1' -and $text -notmatch 'spin=0') {
        Write-Host "PASS  local-baseline"
    } else {
        $failures++
        Write-Host ("FAIL  local-baseline (exit {0}, expected blocked=1)" -f $ext.local.ExitCode)
        Write-Host (($text -split "`n" | Select-Object -Last 3) -join "`n")
    }

    $ext.destroy.WaitForExit()
    $text = (Get-Content (Join-Path $runRoot 'destroy.out') -Raw) -as [string]
    $destroy = if ($text -match 'destroy_ms=(\d+)') { [int]$Matches[1] } else { -1 }
    if ($ext.destroy.ExitCode -eq 0 -and $destroy -ge 0 -and $destroy -lt 700) {
        Write-Host ("PASS  hung-tab-close (destroy_ms={0})" -f $destroy)
    } else {
        $failures++
        Write-Host ("FAIL  hung-tab-close (exit {0}, destroy_ms={1})" -f $ext.destroy.ExitCode, $destroy)
    }

    $ext.dry.WaitForExit()
    $ext.dryLocal.WaitForExit()
    $dry = Get-Content (Join-Path $runRoot 'dry.out') -Raw -ErrorAction SilentlyContinue
    $dryLocal = Get-Content (Join-Path $runRoot 'drylocal.out') -Raw -ErrorAction SilentlyContinue
    if ($dry -match '--backend process' -and $dryLocal -match '--backend local') {
        Write-Host "PASS  launcher"
    } else {
        $failures++
        Write-Host "FAIL  launcher dry run did not name the expected backend"
        Write-Host $dry
        Write-Host $dryLocal
    }

    # ---- one aggregate orphan-renderer check -------------------------------
    $left = Wait-NoTaggedRenderer
    if ($left.Count -gt 0) {
        $failures++
        Write-Host ("FAIL  orphaned renderer process(es): {0}" -f (($left | ForEach-Object {
            "{0} [{1}]" -f $_.ProcessId, (($_.CommandLine -split '--tag ')[-1] -split ' ')[0] }) -join ', '))
        $left | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    }
} finally {
    foreach ($name in $savedEnv.Keys) {
        if ($null -eq $savedEnv[$name]) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
        else { Set-Item "Env:$name" $savedEnv[$name] }
    }

    if ($failures -eq 0) {
        Remove-Item $runRoot -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item $logRoot -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        Write-Host ("logs kept under {0}" -f $runRoot)
        Write-Host ("             and {0}" -f $logRoot)
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
