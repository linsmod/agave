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
#   5. document.currentScript (R95): the <script> element while its code runs
#      (inline, external, defer, async, dynamic, nested), null for modules and
#      for anything that runs after the script returned.
#   6. per-frame module entry + per-realm import() (P1-e);
#   7. a classic script whose first token is `import(` is not a module;
#   8. Node.baseURI / document.baseURI (R176): <base href> applied, every node
#      reports the document base URL;
#   9. HTML import maps (R176): bare specifiers resolve through
#      <script type="importmap"> (exact + trailing-slash prefix), relative
#      imports still work, an unmapped bare specifier still fails.
#  10. browser shims (R176): the URLSearchParams wrapper iterates / reports
#      size + toStringTag, and a frame realm gets the web globals (URL,
#      TextEncoder, Blob, Headers, DOMException, ...) the main realm has.
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
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('agb-script-mode-' + [guid]::NewGuid().ToString('N'))

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }
if (-not (Test-Path $page)) { throw "page not found: $page" }
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

$failures = 0

function Invoke-Probe {
    param([string]$Url, [string]$ScriptFile, [string]$Js, [int]$Wait = 300, [int]$Settle = 1000,
          [int]$Timeout = 60000, [string]$PageEval = $null)

    if ($Js) {
        $ScriptFile = Join-Path $tmp ('js-' + [guid]::NewGuid().ToString('N') + '.js')
        Set-Content -Path $ScriptFile -Value $Js -Encoding ascii
    }

    # Not $args: that is PowerShell's automatic argument array, and assigning to
    # it then splatting it silently loses the assignments (the bench case used to
    # start the probe with no arguments at all and report an empty exit code).
    $cmdArgs = @('--probe', $Url)
    if ($ScriptFile) { $cmdArgs += @('--eval-file', $ScriptFile) }
    $cmdArgs += @('--wait', "$Wait", '--settle', "$Settle", '--timeout', "$Timeout")

    $stdout = Join-Path $tmp 'out.txt'
    $stderr = Join-Path $tmp 'err.txt'

    $old = $env:AGB_PAGE_EVAL
    if ($null -eq $PageEval) { Remove-Item Env:AGB_PAGE_EVAL -ErrorAction SilentlyContinue }
    else { $env:AGB_PAGE_EVAL = $PageEval }

    try {
        $p = Start-Process -FilePath $exe -NoNewWindow -PassThru -Wait -ArgumentList $cmdArgs `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        [pscustomobject]@{
            Code   = $p.ExitCode
            Stdout = (Get-Content $stdout -Raw) -as [string]
            Stderr = (Get-Content $stderr -Raw) -as [string]
        }
    } finally {
        if ($null -eq $old) { Remove-Item Env:AGB_PAGE_EVAL -ErrorAction SilentlyContinue }
        else { $env:AGB_PAGE_EVAL = $old }
    }
}

# 1. antb_retain on a function value.
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

$pinOut = & $pin 2>&1 | Out-String
$pinCode = $LASTEXITCODE

if ($pinCode -ne 0 -or $pinOut -notmatch 'result: 42') {
    $failures++
    Write-Host ("FAIL  retain a function across GC (exit {0})" -f $pinCode)
    Write-Host (($pinOut -split "`n" | Select-Object -Last 6) -join "`n")
} else {
    Write-Host "PASS  retain a function across GC"
}

# 2. classic-script semantics of page scripts (the default now).
# No string literals in the probe script: the shell parses its own command line
# and treats quotes as syntax, so keep the payload quote-free.
$semJs = 'probeExit([typeof pageVar, this === window, topThis, topGlobalThis, hoisted, typeof earlyFn, pageFn()].join(String.fromCharCode(124)))'
$sem = Invoke-Probe -Url (Join-Path $root 'tools/pages/cap_script_semantics.html') -Js $semJs

if ($sem.Code -ne 0) {
    $failures++
    Write-Host ("FAIL  classic script semantics: exit {0}" -f $sem.Code)
    Write-Host (($sem.Stderr -split "`n" | Select-Object -Last 8) -join "`n")
} elseif ($sem.Stdout -notmatch 'number\|true\|true\|true\|function\|function\|51') {
    $failures++
    Write-Host "FAIL  classic script semantics: wrong payload"
    Write-Host (($sem.Stdout -split "`n" | Select-Object -Last 4) -join "`n")
} else {
    Write-Host "PASS  classic script semantics (window.pageVar / this / hoisting)"
}

# 3. AGB_PAGE_EVAL=eval is still selectable.
$evJs = 'probeExit([typeof pageVar, this === window, pageFn()].join(String.fromCharCode(124)))'
$ev = Invoke-Probe -Url (Join-Path $root 'tools/pages/cap_script_semantics.html') -Js $evJs -PageEval 'eval'

if ($ev.Code -ne 0 -or $ev.Stdout -notmatch 'number\|true\|51') {
    $failures++
    Write-Host ("FAIL  AGB_PAGE_EVAL=eval fallback (exit {0})" -f $ev.Code)
    Write-Host (($ev.Stderr -split "`n" | Select-Object -Last 8) -join "`n")
} else {
    Write-Host "PASS  AGB_PAGE_EVAL=eval fallback"
}

# 4. Hot-loop cost: the point of R66 was 20-60x on these.
$bench = Invoke-Probe -Url $page -ScriptFile (Join-Path $root 'debug/bench_hot.js') -Wait 2000 -Settle 1000 -Timeout 120000
$benchText = $bench.Stdout + $bench.Stderr
$limits = @{ 'vm-dispatch' = 50; 'static-key' = 50; 'dense-read' = 50 }

if ($bench.Code -ne 0) {
    $failures++
    Write-Host ("FAIL  bench_hot: exit {0}" -f $bench.Code)
} else {
    foreach ($name in $limits.Keys) {
        $m = [regex]::Match($benchText, "$name=(\d+)ms \(([\d.]+) ns/iter\)")

        if (-not $m.Success) {
            $failures++
            Write-Host ("FAIL  bench_hot: no measurement for {0}" -f $name)
            continue
        }

        $ns = [double]$m.Groups[2].Value
        if ($ns -gt $limits[$name]) {
            $failures++
            Write-Host ("FAIL  bench_hot {0}: {1} ns/iter (limit {2})" -f $name, $ns, $limits[$name])
        } else {
            Write-Host ("PASS  bench_hot {0}: {1} ns/iter" -f $name, $ns)
        }
    }
}

# 5. document.currentScript (R95). The page writes `key=value` into #out and the
# observer reports that text, so the needles below are the page's own answers.
# Chrome agrees on every one of them except the microtask case: its microtask
# checkpoint runs inside "run a classic script", before currentScript is
# cleared, so Chrome reports inline1 there and agave reports null.
$cs = Invoke-Probe -Url (Join-Path $root 'tools/pages/cap_currentscript.html') `
    -ScriptFile (Join-Path $root 'tools/pages/check_currentscript.js') -Wait 2500 -Settle 300

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
        if ($cs.Stdout -notmatch [regex]::Escape($needle)) {
            $failures++
            $csMissing++
            Write-Host ("FAIL  document.currentScript: missing '{0}'" -f $needle)
        }
    }
    if (-not $csMissing) { Write-Host 'PASS  document.currentScript (inline/external/defer/async/nested, null elsewhere)' }
}

# 6. Module semantics come from the parser (P1-e): the frame's inline module
#    runs, a frame's classic script can import(), and each realm evaluates the
#    shared module once (its own window.__sharedLoads). Before P1-e the frame's
#    module never ran, the frame's import() rejected with
#    ERR_VM_DYNAMIC_IMPORT_CALLBACK_MISSING, and a classic script starting with
#    `import(` was rerouted through the module queue by a source sniff.
$modUrl = 'file:///' + ((Join-Path $root 'tools/pages/cap_module_import_frame.html') -replace '\\', '/')
$modJs = "var f=document.getElementById('f');probeExit([f.contentWindow.__dynImport, window.__ownImport, f.contentWindow.__inlineModule].join(String.fromCharCode(124)))"
$mod = Invoke-Probe -Url $modUrl -Js $modJs -Wait 2000 -Settle 300

if ($mod.Code -ne 0) {
    $failures++
    Write-Host ("FAIL  frame modules / per-realm import: exit {0}" -f $mod.Code)
    Write-Host (($mod.Stderr -split "`n" | Select-Object -Last 8) -join "`n")
} elseif ($mod.Stdout -notmatch '1\|1\|1') {
    $failures++
    Write-Host "FAIL  frame modules / per-realm import: wrong payload"
    Write-Host (($mod.Stdout -split "`n" | Select-Object -Last 4) -join "`n")
} else {
    Write-Host 'PASS  frame module entry + per-realm import()'
}

# 7. A classic <script> whose first token is `import(` is a classic script with a
#    dynamic import - not module syntax.
$firstUrl = 'file:///' + ((Join-Path $root 'tools/pages/cap_module_import_first_token.html') -replace '\\', '/')
$first = Invoke-Probe -Url $firstUrl -Js 'probeExit(String(window.__firstToken))' -Wait 1500 -Settle 300

if ($first.Code -ne 0 -or $first.Stdout -notmatch '1') {
    $failures++
    Write-Host ("FAIL  classic script starting with import(): exit {0}" -f $first.Code)
    Write-Host (($first.Stderr -split "`n" | Select-Object -Last 6) -join "`n")
    Write-Host (($first.Stdout -split "`n" | Select-Object -Last 4) -join "`n")
} else {
    Write-Host 'PASS  classic script starting with import( (dynamic import, not a module)'
}

# 8. Node.baseURI / document.baseURI (R176). monaco's playground evaluates
#    `document.baseURI.toString()` at the top of its bundle; the property did
#    not exist on the bridge, so the template threw and the playground module
#    aborted. The page writes its four checks into window.__baseuri.
$buUrl = 'file:///' + ((Join-Path $root 'tools/pages/cap_baseuri.html') -replace '\\', '/')
$bu = Invoke-Probe -Url $buUrl -Js 'probeExit(String(window.__baseuri))' -Wait 1500 -Settle 300

if ($bu.Code -ne 0 -or $bu.Stdout -notmatch '1\|1\|1\|1') {
    $failures++
    Write-Host ("FAIL  document.baseURI: exit {0}" -f $bu.Code)
    Write-Host (($bu.Stdout + $bu.Stderr) -split "`n" | Select-Object -Last 8 | Out-String)
} else {
    Write-Host 'PASS  document.baseURI / Node.baseURI (with <base href>)'
}

# 9. HTML import maps (R176). GitHub ships <script type="importmap"> for
#    react/react-dom/...; without it the bare specifier was opened as a file
#    ("Cannot open module: react") and every React entry module failed. The
#    page writes "<exact>|<prefix>|<relative>" and the rejection of an unmapped
#    bare specifier.
$imUrl = 'file:///' + ((Join-Path $root 'tools/pages/importmap/index.html') -replace '\\', '/')
$im = Invoke-Probe -Url $imUrl -Wait 2500 -Settle 800 `
    -Js 'probeExit(String(window.__imap) + "/" + String(window.__imapDynamic))'

if ($im.Code -ne 0 -or $im.Stdout -notmatch 'hello\|helper\|relative/rejected') {
    $failures++
    Write-Host ("FAIL  import map: exit {0}" -f $im.Code)
    Write-Host (($im.Stdout + $im.Stderr) -split "`n" | Select-Object -Last 8 | Out-String)
} else {
    Write-Host 'PASS  import map (exact + prefix bare specifiers, relative, unmapped rejects)'
}

# 10. Browser shims (R176): the URLSearchParams wrapper is a plain JS object
#     that delegates to the native one, so it has to forward every member -
#     `for (const [k, v] of params)` reads Symbol.iterator (monaco's playground
#     died with "not iterable" without it). A frame realm is created by the
#     bridge, which only installs ant's intrinsics + timers; the web globals are
#     copied from the parent realm (monaco's isolated playground iframe threw
#     "URLSearchParams is not defined").
$polyUrl = 'file:///' + ((Join-Path $root 'tools/pages/cap_polyfills.html') -replace '\\', '/')
$poly = Invoke-Probe -Url $polyUrl -Js 'probeExit(String(window.__poly))' -Wait 2500 -Settle 1500

if ($poly.Code -ne 0 -or $poly.Stdout -notmatch 'usp:function,2,a=1,2,\[object URLSearchParams\]\|frameMissing:\|frameUsp:function') {
    $failures++
    Write-Host ("FAIL  browser shims: exit {0}" -f $poly.Code)
    Write-Host (($poly.Stdout + $poly.Stderr) -split "`n" | Select-Object -Last 8 | Out-String)
} else {
    Write-Host 'PASS  URLSearchParams wrapper iteration + frame-realm web globals'
}

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

if ($failures -gt 0) {
    Write-Host "`nSCRIPT MODE CHECKS FAILED ($failures)"
    exit 1
}

Write-Host "`nALL SCRIPT MODE CHECKS PASSED"
exit 0

