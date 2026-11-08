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
#   pwsh -File tools/run_tab_lifecycle_tests.ps1
#
# The pages live in tools/pages/tab_lifecycle_*.html; the server
# (tools/tab_lifecycle_server.py) delays /slow.ttf so the font is in flight.
param(
    [int]$Port = 8979
)

$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$exe   = Join-Path $root 'build-zig-native\agave_shell.exe'
$pages = Join-Path $PSScriptRoot 'pages'
$font  = Join-Path $root 'fonts\NotoSansSC-Regular.ttf'
$base  = "http://127.0.0.1:$Port"
$work  = Join-Path $env:TEMP ("agave_tab_lifecycle_" + [guid]::NewGuid().ToString('N').Substring(0, 8))

if (-not (Test-Path $exe)) {
    "missing $exe - build first (pwsh -File tools/build_project.ps1)"
    exit 1
}

New-Item -ItemType Directory -Force -Path $work | Out-Null

$server = Start-Process -FilePath 'python' -ArgumentList @(
    (Join-Path $PSScriptRoot 'tab_lifecycle_server.py'), "$Port", $pages, $font, '1.5'
) -WindowStyle Hidden -PassThru

$script:failures = 0
$script:cases = 0

function Wait-Server {
    for ($i = 0; $i -lt 40; $i++) {
        try {
            Invoke-WebRequest -UseBasicParsing -Uri "$base/tab_lifecycle_events.html" -TimeoutSec 1 | Out-Null
            return $true
        } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    return $false
}

function Run-Probe {
    param(
        [string]$Page,
        [int]$Wait = 600,
        [hashtable]$Env = @{}
    )

    $saved = @{}
    foreach ($k in $Env.Keys) {
        $saved[$k] = [Environment]::GetEnvironmentVariable($k)
        [Environment]::SetEnvironmentVariable($k, $Env[$k])
    }

    $crash = Join-Path $work 'agb_crash.txt'
    Remove-Item $crash -ErrorAction SilentlyContinue

    Push-Location $work
    try {
        $out = & $exe --probe "$base/$Page" --wait $Wait --settle 200 --timeout 25000 `
                     --eval "probeExit(document.title)" 2>&1
        $code = $LASTEXITCODE
    } finally {
        Pop-Location
        foreach ($k in $saved.Keys) {
            [Environment]::SetEnvironmentVariable($k, $saved[$k])
        }
    }

    [pscustomobject]@{
        Text   = ($out | Out-String)
        Code   = $code
        Crashed = (Test-Path $crash)
    }
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

    # 1. teardown order, on a page that registered pagehide/unload listeners.
    $r = Run-Probe 'tab_lifecycle_events.html' -Env @{ AGB_EVT_DEBUG = '1' }
    $text = $r.Text
    $iPageHide = Index-Of $text '\[evt\] window event pagehide'
    $iVisibility = Index-Of $text '\[evt\] window event visibilitychange'
    $iUnload = Index-Of $text '\[evt\] window event unload'
    $iStop = Index-Of $text '\[evt\] realm stop'
    $ordered = ($iPageHide -ge 0) -and ($iPageHide -lt $iVisibility) -and
               ($iVisibility -lt $iUnload) -and ($iUnload -lt $iStop)
    Check 'teardown order pagehide -> visibilitychange -> unload -> realm stop' `
          (($r.Code -eq 0) -and $ordered -and (-not $r.Crashed)) $text

    Check 'every realm start has a realm stop' `
          ((Count-Of $text '\[evt\] realm start') -eq (Count-Of $text '\[evt\] realm stop gen=')) $text

    # 2. JS-created nodes must not pin the document after the view is gone.
    $r = Run-Probe 'tab_lifecycle_pins.html' -Env @{ AGB_EVT_DEBUG = '1' }
    $text = $r.Text
    $pinsLine = ($text -split "`n" | Where-Object { $_ -match 'pins release .* nodes=' } | Select-Object -First 1)
    Check 'JS-created nodes are released on teardown (no LEAK line)' `
          (($r.Code -eq 0) -and ($pinsLine -ne $null) -and (-not ($text -match 'LEAK:\s+\d+ Node'))) $text

    # 3. two views: the popup from window.open is a second engine context and
    #    must go through the same teardown.
    $r = Run-Probe 'tab_lifecycle_popup.html' -Wait 900 -Env @{ AGB_EVT_DEBUG = '1' }
    $text = $r.Text
    $starts = Count-Of $text '\[evt\] realm start .*\(view context\)'
    $stops = Count-Of $text '\[evt\] realm stop gen='
    Check 'window.open popup gets its own view context and is torn down' `
          (($r.Code -eq 0) -and ($starts -eq 2) -and ($stops -eq 2) -and (-not $r.Crashed)) $text

    # 4. font still in flight when the view is destroyed: the download must not
    #    come back into a document without a render tree.
    $r = Run-Probe 'tab_lifecycle_font.html' -Wait 300 -Env @{ AGB_FONT_LOG = '1' }
    $text = $r.Text
    Check 'font in flight during teardown: no crash, sources released' `
          (($r.Code -eq 0) -and (-not $r.Crashed) -and ($text -match '\[fontsel\] documentDetached faces=')) $text
} finally {
    if ($server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}

""
if ($script:failures -eq 0) {
    "ALL TAB LIFECYCLE CHECKS PASSED ($($script:cases) cases)"
    exit 0
}
"$($script:failures) of $($script:cases) tab lifecycle checks failed"
exit 1
