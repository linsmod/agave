# run_tab_lifecycle_tests.ps1 - view/tab teardown lifecycle regression (R141).
#
# Every case is one headless probe run against a local http server; the shell
# destroys its views on the way out, which is the same agave_webview_destroy()
# path a tab close takes. Assertions are on the [evt] channel (AGB_EVT_DEBUG=1)
# and on the engine's own leak counter:
#
#   1. teardown order: pagehide -> visibilitychange -> unload -> realm stop
#   2. pinned nodes: a page that creates 300 elements from JS must not leak
#      its document (the binding used to take an untracked ref per node)
#   3. two views (a window.open popup): every realm start has its realm stop
#   4. font still downloading when the view is torn down: no crash, and the
#      new documentDetached() hook unregisters the font sources
#
# The four probes run at once (one shared server on a free port; each probe has
# its own profile and cwd, so agb_crash.txt cannot collide). Expected wall
# time: ~3 s.
#
#   pwsh -File tools/run_tab_lifecycle_tests.ps1
#
# The pages live in tools/pages/tab_lifecycle_*.html; the server
# (tools/tab_lifecycle_server.py) delays /slow.ttf so the font is in flight.
param(
    [int]$Port = 0
)

$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$exe   = Join-Path $root 'build-zig-native\agave_shell.exe'
$pages = Join-Path $PSScriptRoot 'pages'
$font  = Join-Path $root 'fonts\NotoSansSC-Regular.ttf'

if (-not (Test-Path $exe)) {
    "missing $exe - build first (pwsh -File tools/build_project.ps1)"
    exit 1
}

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking
$run = New-TestRun -Suite 'tab-lifecycle'

if ($Port -le 0) { $Port = Get-TestFreePort }
$base = "http://127.0.0.1:$Port"

$server = Start-Process -FilePath 'python' -ArgumentList @(
    (Join-Path $PSScriptRoot 'tab_lifecycle_server.py'), "$Port", $pages, $font, '1.5'
) -WindowStyle Hidden -PassThru

$script:failures = 0
$script:cases = 0

function Wait-Server {
    for ($i = 0; $i -lt 60; $i++) {
        try {
            Invoke-WebRequest -UseBasicParsing -Uri "$base/tab_lifecycle_events.html" -TimeoutSec 1 | Out-Null
            return $true
        } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    return $false
}

function Check {
    param([string]$Label, [bool]$Ok, [string]$Detail = '')

    $script:cases++
    if ($Ok) {
        "PASS  $Label"
    } else {
        $script:failures++
        "FAIL  $Label"
        if ($Detail) { $Detail -split "`n" | Select-Object -First 12 | ForEach-Object { "      $_" } }
    }
}

function Count-Of {
    param([string]$Text, [string]$Pattern)
    return ([regex]::Matches($Text, $Pattern)).Count
}

function Index-Of {
    param([string]$Text, [string]$Pattern)
    $m = [regex]::Match($Text, $Pattern)
    if ($m.Success) { return $m.Index }
    return -1
}

try {
    if (-not (Wait-Server)) {
        "server did not come up on $base"
        exit 1
    }

    $jobs = [ordered]@{}
    $jobs.events = Start-TestProbe -Run $run -Name 'events' -Url "$base/tab_lifecycle_events.html" `
        -Eval 'probeExit(document.title)' -Wait 600 -Settle 200 -Timeout 25000 `
        -Env @{ AGB_EVT_DEBUG = '1' }
    $jobs.pins = Start-TestProbe -Run $run -Name 'pins' -Url "$base/tab_lifecycle_pins.html" `
        -Eval 'probeExit(document.title)' -Wait 600 -Settle 200 -Timeout 25000 `
        -Env @{ AGB_EVT_DEBUG = '1' }
    # 3. two views: the popup from window.open is a second engine context and
    #    must go through the same teardown.
    $jobs.popup = Start-TestProbe -Run $run -Name 'popup' -Url "$base/tab_lifecycle_popup.html" `
        -Eval 'probeExit(document.title)' -Wait 900 -Settle 200 -Timeout 25000 `
        -Env @{ AGB_EVT_DEBUG = '1' }
    # 4. font still in flight when the view is destroyed.
    $jobs.font = Start-TestProbe -Run $run -Name 'font' -Url "$base/tab_lifecycle_font.html" `
        -Eval 'probeExit(document.title)' -Wait 300 -Settle 200 -Timeout 25000 `
        -Env @{ AGB_FONT_LOG = '1' }

    $res = @{}
    foreach ($name in @($jobs.Keys)) { $res[$name] = Complete-TestProbe -Job $jobs[$name] }

    function Crashed($Result) { return (Test-Path (Join-Path $Result.Dir 'agb_crash.txt')) }

    # 1. teardown order, on a page that registered pagehide/unload listeners.
    $r = $res.events
    $text = $r.Text
    $iPageHide = Index-Of $text '\[evt\] window event pagehide'
    $iVisibility = Index-Of $text '\[evt\] window event visibilitychange'
    $iUnload = Index-Of $text '\[evt\] window event unload'
    $iStop = Index-Of $text '\[evt\] realm stop'
    $ordered = ($iPageHide -ge 0) -and ($iPageHide -lt $iVisibility) -and
               ($iVisibility -lt $iUnload) -and ($iUnload -lt $iStop)
    Check 'teardown order pagehide -> visibilitychange -> unload -> realm stop' `
          (($r.Code -eq 0) -and $ordered -and (-not (Crashed $r))) $text

    Check 'every realm start has a realm stop' `
          ((Count-Of $text '\[evt\] realm start') -eq (Count-Of $text '\[evt\] realm stop gen=')) $text

    # 2. JS-created nodes must not pin the document after the view is gone.
    $r = $res.pins
    $text = $r.Text
    $pinsLine = ($text -split "`n" | Where-Object { $_ -match 'pins release .* nodes=' } | Select-Object -First 1)
    Check 'JS-created nodes are released on teardown (no LEAK line)' `
          (($r.Code -eq 0) -and ($pinsLine -ne $null) -and (-not ($text -match 'LEAK:\s+\d+ Node'))) $text

    # 3. popup view context.
    $r = $res.popup
    $text = $r.Text
    $starts = Count-Of $text '\[evt\] realm start .*\(view context\)'
    $stops = Count-Of $text '\[evt\] realm stop gen='
    Check 'window.open popup gets its own view context and is torn down' `
          (($r.Code -eq 0) -and ($starts -eq 2) -and ($stops -eq 2) -and (-not (Crashed $r))) $text

    # 4. font in flight during teardown.
    $r = $res.font
    $text = $r.Text
    Check 'font in flight during teardown: no crash, sources released' `
          (($r.Code -eq 0) -and (-not (Crashed $r)) -and ($text -match '\[fontsel\] documentDetached faces=')) $text
} finally {
    if ($server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-TestRun $run $failures
}

""
if ($script:failures -eq 0) {
    "ALL TAB LIFECYCLE CHECKS PASSED ($($script:cases) cases)"
    exit 0
}
"$($script:failures) of $($script:cases) tab lifecycle checks failed"
exit 1
