# tools/run_script_mode_tests.ps1
#
# Regression for R66/R67: page scripts run as classic scripts (top-level
# var/function land on the global object) and the hot loops that motivated the
# switch stay fast.
#
# R67 was the bug this suite exists for now: the bridge keeps callbacks in C++
# and roots them with antb_retain() -> gc_pin_permanent(), which silently
# refused kTypeFunction values. A listener held only from C was collected and
# the next call read a freed closure - chat.deepseek.com AV'd 3/4 in script
# mode while eval mode happened to stay alive. ant_pin_repro.exe reproduces it
# with the bare engine (no browser), so it is the first case here.
#
# Checks:
#   1. antb_retain() keeps a function that nothing else references alive
#      across forced GC, and it still returns 42;
#   2. a page's top-level var/function/this still look like classic script;
#   3. `AGB_PAGE_EVAL=eval` still opts back into the old semantics;
#   4. bench_hot.js stays in single-digit..tens of ns per iteration (eval mode
#      was 100-600 ns);
#   5. document.currentScript (R95): the <script> element while its code runs;
#   6. per-frame module entry + per-realm import() (P1-e);
#   7. a classic script whose first token is `import(` is not a module;
#   8. Node.baseURI / document.baseURI (R176);
#   9. HTML import maps (R176);
#  10. browser shims (R176): URLSearchParams iteration + frame realm web globals.
#  11. probe lifecycle: --wait-dcl arms --eval at DOMContentLoaded (marker from
#      prelude.js), and stays invisible/opt-in for a normal load-armed probe.
#
# All probes run at once (own profiles); the bench keeps a clean retry if its
# per-iteration budget is exceeded while the batch is loading the machine.
# Expected wall time: ~4 s.
#
# Usage:
#   pwsh -File tools/run_script_mode_tests.ps1
#   pwsh -File tools/run_script_mode_tests.ps1 -BuildDir build-zig-native

param(
    [string]$BuildDir = 'build-zig-native',
    [string]$Page = 'tools/pages/blank.html'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir
$exe = Join-Path $build 'agave_shell.exe'
$page = Join-Path $root $Page

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }
if (-not (Test-Path $page)) { throw "page not found: $page" }

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking
$run = New-TestRun -Suite 'script-mode'

$failures = 0

# ---- 1. antb_retain on a function value (bare engine, own process) ---------
$pin = Join-Path $build 'ant_pin_repro.exe'
$pinSrc = Join-Path $root 'tools/ant_pin_repro.c'

if (-not (Test-Path $pin) -or ((Get-Item $pinSrc).LastWriteTime -gt (Get-Item $pin).LastWriteTime)) {
    Write-Host "building ant_pin_repro.exe"
    Push-Location $root
    try {
        & zig cc -target x86_64-windows-gnu -O1 -g -I '3rdparty/ant/include' $pinSrc `
            '3rdparty/ant/lib/antjsb.dll.a' -o $pin 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "zig cc failed for $pinSrc" }
    } finally { Pop-Location }
}
$pinOutFile = Join-Path $run.Dir 'pin.out'
$pinErrFile = Join-Path $run.Dir 'pin.err'
$pinProc = Start-Process -FilePath $pin -NoNewWindow -PassThru `
    -RedirectStandardOutput $pinOutFile -RedirectStandardError $pinErrFile

$jobs = [ordered]@{}

# 2. classic-script semantics of page scripts (the default now).
# No string literals in the probe script: the shell parses its own command line
# and treats quotes as syntax, so keep the payload quote-free. The helper writes
# it to --eval-file, so spaces are fine.
$semJs = 'probeExit([typeof pageVar, this === window, topThis, topGlobalThis, hoisted, typeof earlyFn, pageFn()].join(String.fromCharCode(124)))'
$jobs.sem = Start-TestProbe -Run $run -Name 'classic-semantics' `
    -Url (Join-Path $root 'tools/pages/cap_script_semantics.html') `
    -Eval $semJs -Wait 300 -Settle 1000 -Timeout 60000

# 3. AGB_PAGE_EVAL=eval is still selectable.
$evJs = 'probeExit([typeof pageVar, this === window, pageFn()].join(String.fromCharCode(124)))'
$jobs.eval = Start-TestProbe -Run $run -Name 'page-eval-eval' `
    -Url (Join-Path $root 'tools/pages/cap_script_semantics.html') `
    -Eval $evJs -Wait 300 -Settle 1000 -Timeout 60000 `
    -Env @{ AGB_PAGE_EVAL = 'eval' }

# 4. Hot-loop cost: the point of R66 was 20-60x on these.
$jobs.bench = Start-TestProbe -Run $run -Name 'bench-hot' -Url $page `
    -EvalFile (Join-Path $root 'debug/bench_hot.js') -Wait 2000 -Settle 1000 -Timeout 120000

# 5. document.currentScript (R95).
$jobs.current = Start-TestProbe -Run $run -Name 'current-script' `
    -Url (Join-Path $root 'tools/pages/cap_currentscript.html') `
    -EvalFile (Join-Path $root 'tools/pages/check_currentscript.js') -Wait 2500 -Settle 300

# 6. Module semantics come from the parser (P1-e).
$modUrl = 'file:///' + ((Join-Path $root 'tools/pages/cap_module_import_frame.html') -replace '\\', '/')
$modJs = "var f=document.getElementById('f');probeExit([f.contentWindow.__dynImport, window.__ownImport, f.contentWindow.__inlineModule].join(String.fromCharCode(124)))"
$jobs.modules = Start-TestProbe -Run $run -Name 'frame-modules' -Url $modUrl -Eval $modJs -Wait 2000 -Settle 300

# 7. A classic <script> whose first token is `import(` is a classic script with
#    a dynamic import - not module syntax.
$firstUrl = 'file:///' + ((Join-Path $root 'tools/pages/cap_module_import_first_token.html') -replace '\\', '/')
$jobs.first = Start-TestProbe -Run $run -Name 'import-first-token' -Url $firstUrl `
    -Eval 'probeExit(String(window.__firstToken))' -Wait 1500 -Settle 300

# 8. Node.baseURI / document.baseURI (R176).
$buUrl = 'file:///' + ((Join-Path $root 'tools/pages/cap_baseuri.html') -replace '\\', '/')
$jobs.baseuri = Start-TestProbe -Run $run -Name 'baseuri' -Url $buUrl `
    -Eval 'probeExit(String(window.__baseuri))' -Wait 1500 -Settle 300

# 9. HTML import maps (R176).
$imUrl = 'file:///' + ((Join-Path $root 'tools/pages/importmap/index.html') -replace '\\', '/')
$jobs.importmap = Start-TestProbe -Run $run -Name 'importmap' -Url $imUrl `
    -Eval 'probeExit(String(window.__imap) + "/" + String(window.__imapDynamic))' -Wait 2500 -Settle 800

# 10. Browser shims (R176).
$polyUrl = 'file:///' + ((Join-Path $root 'tools/pages/cap_polyfills.html') -replace '\\', '/')
$jobs.polyfills = Start-TestProbe -Run $run -Name 'polyfills' -Url $polyUrl `
    -Eval 'probeExit(String(window.__poly))' -Wait 2500 -Settle 500

# 11. --wait-dcl (probe lifecycle): the eval can be armed at DOMContentLoaded
#     instead of load. Prelude.js (bound before the page's scripts) reports the
#     event; a probe without the flag must not see the marker at all.
$dclUrl = 'file:///' + ((Join-Path $root 'tools/pages/dcl_probe.html') -replace '\\', '/')
$dclEval = 'probeExit(String(window.__phase) + String.fromCharCode(124) + ' +
           '(window.__dclSeen ? 1 : 0) + String.fromCharCode(124) + (window.__loadSeen ? 1 : 0))'
$jobs.dcl = Start-TestProbe -Run $run -Name 'wait-dcl' -Url $dclUrl `
    -Eval $dclEval -Extra @('--wait-dcl') -Wait 0 -Settle 300
$jobs.nodcl = Start-TestProbe -Run $run -Name 'wait-load' -Url $dclUrl `
    -Eval $dclEval -Wait 500 -Settle 300

$res = @{}
foreach ($name in @($jobs.Keys)) { $res[$name] = Complete-TestProbe -Job $jobs[$name] }
$pinProc.WaitForExit()
$pinOut = (Get-Content $pinOutFile -Raw -ErrorAction SilentlyContinue) +
          (Get-Content $pinErrFile -Raw -ErrorAction SilentlyContinue)

if ($pinProc.ExitCode -ne 0 -or $pinOut -notmatch 'result: 42') {
    $failures++
    Write-Host ("FAIL  retain a function across GC (exit {0})" -f $pinProc.ExitCode)
    Write-Host (($pinOut -split "`n" | Select-Object -Last 6) -join "`n")
} else {
    Write-Host "PASS  retain a function across GC"
}

if ($res.sem.Code -ne 0) {
    $failures++
    Write-Host ("FAIL  classic script semantics: exit {0}" -f $res.sem.Code)
    Write-Host (($res.sem.Stderr -split "`n" | Select-Object -Last 8) -join "`n")
} elseif ($res.sem.Text -notmatch 'number\|true\|true\|true\|function\|function\|51') {
    $failures++
    Write-Host "FAIL  classic script semantics: wrong payload"
    Write-Host (($res.sem.Text -split "`n" | Select-Object -Last 4) -join "`n")
} else {
    Write-Host "PASS  classic script semantics (window.pageVar / this / hoisting)"
}

if ($res.eval.Code -ne 0 -or $res.eval.Text -notmatch 'number\|true\|51') {
    $failures++
    Write-Host ("FAIL  AGB_PAGE_EVAL=eval fallback (exit {0})" -f $res.eval.Code)
    Write-Host (($res.eval.Stderr -split "`n" | Select-Object -Last 8) -join "`n")
} else {
    Write-Host "PASS  AGB_PAGE_EVAL=eval fallback"
}

# ---- bench: retry once on its own if the batch load polluted the numbers ---
$limits = @{ 'vm-dispatch' = 50; 'static-key' = 50; 'dense-read' = 50 }
function Test-Bench($Result) {
    if ($Result.Code -ne 0) { return @{ Ok = $false; Why = "exit $($Result.Code)" } }
    foreach ($name in $limits.Keys) {
        $m = [regex]::Match($Result.Text, "$name=(\d+)ms \(([\d.]+) ns/iter\)")
        if (-not $m.Success) { return @{ Ok = $false; Why = "no measurement for $name" } }
        $ns = [double]$m.Groups[2].Value
        if ($ns -gt $limits[$name]) { return @{ Ok = $false; Why = "$name $ns ns/iter (limit $($limits[$name]))" } }
    }
    return @{ Ok = $true; Why = '' }
}

$bench = Test-Bench $res.bench
if (-not $bench.Ok) {
    $res.bench = Complete-TestProbe -Job (Start-TestProbe -Run $run -Name 'bench-hot-retry' -Url $page `
        -EvalFile (Join-Path $root 'debug/bench_hot.js') -Wait 2000 -Settle 1000 -Timeout 120000)
    $bench = Test-Bench $res.bench
}
if (-not $bench.Ok) {
    $failures++
    Write-Host ("FAIL  bench_hot: {0}" -f $bench.Why)
} else {
    foreach ($name in $limits.Keys) {
        $m = [regex]::Match($res.bench.Text, "$name=(\d+)ms \(([\d.]+) ns/iter\)")
        Write-Host ("PASS  bench_hot {0}: {1} ns/iter" -f $name, $m.Groups[2].Value)
    }
}

$cs = $res.current
if ($cs.Code -ne 0) {
    $failures++
    Write-Host ("FAIL  document.currentScript: exit {0}" -f $cs.Code)
    Write-Host (($cs.Stderr -split "`n" | Select-Object -Last 8) -join "`n")
} else {
    $csMissing = 0
    foreach ($needle in @('inline.self=inline1', 'inline.data=one', 'inline.src=""',
                          'fn=inline1', 'eval=inline1', 'newFunction=inline1',
                          'nested.id=nested', 'inline.afterNested=inline1',
                          'listener=inline1', 'inline2.self=inline2',
                          'timer=null', 'microtask=null', 'module=null', 'afterLoad=null',
                          'ext.seen=ext', 'ext.data=ext', 'defer.seen=defer',
                          'async.seen=async', 'module.inDom=yes')) {
        if ($cs.Text -notmatch [regex]::Escape($needle)) {
            $failures++
            $csMissing++
            Write-Host ("FAIL  document.currentScript: missing '{0}'" -f $needle)
        }
    }
    if (-not $csMissing) { Write-Host 'PASS  document.currentScript (inline/external/defer/async/nested, null elsewhere)' }
}

if ($res.modules.Code -ne 0) {
    $failures++
    Write-Host ("FAIL  frame modules / per-realm import: exit {0}" -f $res.modules.Code)
    Write-Host (($res.modules.Stderr -split "`n" | Select-Object -Last 8) -join "`n")
} elseif ($res.modules.Text -notmatch '1\|1\|1') {
    $failures++
    Write-Host "FAIL  frame modules / per-realm import: wrong payload"
    Write-Host (($res.modules.Text -split "`n" | Select-Object -Last 4) -join "`n")
} else {
    Write-Host 'PASS  frame module entry + per-realm import()'
}

if ($res.first.Code -ne 0 -or $res.first.Text -notmatch '1') {
    $failures++
    Write-Host ("FAIL  classic script starting with import(): exit {0}" -f $res.first.Code)
    Write-Host (($res.first.Stderr -split "`n" | Select-Object -Last 6) -join "`n")
    Write-Host (($res.first.Text -split "`n" | Select-Object -Last 4) -join "`n")
} else {
    Write-Host 'PASS  classic script starting with import( (dynamic import, not a module)'
}

if ($res.baseuri.Code -ne 0 -or $res.baseuri.Text -notmatch '1\|1\|1\|1') {
    $failures++
    Write-Host ("FAIL  document.baseURI: exit {0}" -f $res.baseuri.Code)
    Write-Host (($res.baseuri.Text + $res.baseuri.Stderr) -split "`n" | Select-Object -Last 8 | Out-String)
} else {
    Write-Host 'PASS  document.baseURI / Node.baseURI (with <base href>)'
}

if ($res.importmap.Code -ne 0 -or $res.importmap.Text -notmatch 'hello\|helper\|relative/rejected') {
    $failures++
    Write-Host ("FAIL  import map: exit {0}" -f $res.importmap.Code)
    Write-Host (($res.importmap.Text + $res.importmap.Stderr) -split "`n" | Select-Object -Last 8 | Out-String)
} else {
    Write-Host 'PASS  import map (exact + prefix bare specifiers, relative, unmapped rejects)'
}

if ($res.polyfills.Code -ne 0 -or $res.polyfills.Text -notmatch 'usp:function,2,a=1,2,\[object URLSearchParams\]\|frameMissing:\|frameUsp:function') {
    $failures++
    Write-Host ("FAIL  browser shims: exit {0}" -f $res.polyfills.Code)
    Write-Host (($res.polyfills.Text + $res.polyfills.Stderr) -split "`n" | Select-Object -Last 8 | Out-String)
} else {
    Write-Host 'PASS  URLSearchParams wrapper iteration + frame-realm web globals'
}

# 11. --wait-dcl: marker consumed only when asked, and the eval is armed by DCL.
$dclArmed = $res.dcl.Text.IndexOf('domcontentloaded')
$dclEvalAt = $res.dcl.Text.IndexOf('running eval')
if ($res.dcl.Code -ne 0 -or $dclArmed -lt 0 -or $dclEvalAt -lt 0 -or $dclArmed -gt $dclEvalAt) {
    $failures++
    Write-Host ("FAIL  --wait-dcl: exit {0}, marker/index {1}/{2}" -f $res.dcl.Code, $dclArmed, $dclEvalAt)
    Write-Host (($res.dcl.Text + $res.dcl.Stderr) -split "`n" | Select-Object -Last 8 | Out-String)
} elseif ($res.dcl.Text -notmatch '\|1\|') {
    $failures++
    Write-Host 'FAIL  --wait-dcl: eval ran before the page saw DOMContentLoaded'
    Write-Host (($res.dcl.Text) -split "`n" | Select-Object -Last 4 | Out-String)
} else {
    Write-Host 'PASS  --wait-dcl: eval armed at DOMContentLoaded (page saw it)'
}

if ($res.nodcl.Code -ne 0 -or $res.nodcl.Text -match 'domcontentloaded' -or $res.nodcl.Text -notmatch '\|1\|1') {
    $failures++
    Write-Host ("FAIL  load-armed eval is unchanged / marker is opt-in: exit {0}" -f $res.nodcl.Code)
    Write-Host (($res.nodcl.Text + $res.nodcl.Stderr) -split "`n" | Select-Object -Last 8 | Out-String)
} else {
    Write-Host 'PASS  without --wait-dcl the eval still starts at load and no DCL marker leaks'
}

Remove-TestRun $run $failures

if ($failures -gt 0) {
    Write-Host "`nSCRIPT MODE CHECKS FAILED ($failures)"
    exit 1
}

Write-Host "`nALL SCRIPT MODE CHECKS PASSED"
exit 0
