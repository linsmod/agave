# tools/run_window_tests.ps1
#
# Window lifecycle (<a target>, window.open, window.close) on BOTH render
# backends. The engine decides same-tab vs new window from the target name
# (HTMLAnchorElement: middle/ctrl/meta => _blank, otherwise the target
# attribute); the control owns the window registry, so the behaviour must not
# depend on where the page runs:
#
#   local   - one engine, every view in this process
#   process - every view in its own agave_render_host.exe (the request travels
#             as a SERVICE_REQUEST event)
#
# Cases (each on local and process):
#   1. plain link          -> same tab, no new window
#   2. target="_blank"     -> new tab, loaded, titled
#   3. window.open(url)    -> new tab, loaded, window object returned
#   4. named target x2     -> ONE new window, second click navigates it and asks
#                             the host to show that tab (activate=1)
#   5. window.close()      -> the popup asks the host to close it
#
# Usage:
#   pwsh -File tools/run_window_tests.ps1

param(
    [string]$BuildDir = 'build-zig-native'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir
$exe = Join-Path $build 'agave_shell.exe'
$pages = Join-Path $root 'tools/pages'

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }

$failures = 0
$out = Join-Path $build 'window_out.txt'
$err = Join-Path $build 'window_err.txt'

function Invoke-Probe {
    param(
        [string]$Backend,
        [string]$Page,
        [string]$Js = '',
        [int]$Wait = 700,
        [int]$Settle = 700,
        [string[]]$Extra = @()
    )

    $jsFile = Join-Path $build 'window_eval.js'
    if ($Js) { Set-Content -Path $jsFile -Value $Js -Encoding ascii }

    $saved = $env:AGB_VIEW_BACKEND
    if ($Backend) { $env:AGB_VIEW_BACKEND = $Backend }
    else { Remove-Item Env:AGB_VIEW_BACKEND -ErrorAction SilentlyContinue }

    try {
        $args = @('--probe', "file:///$($Page -replace '\\','/')",
                  '--wait', "$Wait", '--settle', "$Settle", '--timeout', '30000')
        if ($Js) { $args += @('--eval-file', $jsFile) }
        if ($Extra.Count -gt 0) { $args += $Extra }

        $p = Start-Process -FilePath $exe -NoNewWindow -PassThru -Wait `
            -ArgumentList $args -RedirectStandardOutput $out -RedirectStandardError $err

        [pscustomobject]@{
            Code   = $p.ExitCode
            Text   = ((Get-Content $out -Raw) -as [string]) + ((Get-Content $err -Raw) -as [string])
        }
    } finally {
        if ($null -eq $saved) { Remove-Item Env:AGB_VIEW_BACKEND -ErrorAction SilentlyContinue }
        else { $env:AGB_VIEW_BACKEND = $saved }
    }
}

# Renderers this run owns: the control passes AGB_RENDER_TAG to every renderer it
# spawns (--tag), and this run has its own tag. A renderer without it belongs to
# a developer's own shell - never counted, never killed.
$script:runTag = 'agvwin' + ([guid]::NewGuid().ToString('N').Substring(0, 12))
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

    for ($i = 0; $i -lt 30; ++$i) {
        if (-not (Get-StrayRenderers)) { return }
        Start-Sleep -Milliseconds 100
    }

    $left = @(Get-StrayRenderers)
    if ($left.Count -gt 0) {
        $script:failures++
        Write-Host ("FAIL  {0} left renderer process(es): {1}" -f $What,
                    (($left | ForEach-Object { $_.ProcessId }) -join ', '))
        $left | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    }
}

$links = Join-Path $pages 'window_links.html'

foreach ($backend in @('local', 'process')) {
    Write-Host "`n-- backend $backend --"

    # 1. plain link: same tab, no new window.
    $r = Invoke-Probe $backend $links -Js 'probeExit("url=" + document.URL);' `
        -Extra @('--click', '60,40')
    if ($r.Code -eq 0 -and $r.Text -match 'url=.*window_target\.html' -and
        $r.Text -notmatch 'new window requested') {
        Write-Host "PASS  $backend plain link: same tab"
    } else {
        $script:failures++
        Write-Host "FAIL  $backend plain link did not navigate in place"
        Write-Host (($r.Text -split "`n" | Where-Object { $_ -match 'url=|requested|exit' } | Select-Object -Last 3) -join "`n")
    }

    # 2. target=_blank: a new tab with the link, loaded and titled.
    $r = Invoke-Probe $backend $links -Js 'probeExit("main=" + document.URL);' `
        -Extra @('--click', '60,140')
    if ($r.Code -eq 0 -and $r.Text -match 'new window requested url=.*window_target\.html' -and
        $r.Text -match 'new window url=.*window_target\.html' -and
        $r.Text -match 'main=.*window_links\.html') {
        Write-Host "PASS  $backend target=_blank: new tab loaded, main tab unchanged"
    } else {
        $script:failures++
        Write-Host "FAIL  $backend target=_blank did not open a loaded tab"
        Write-Host (($r.Text -split "`n" | Where-Object { $_ -match 'new window|main=|exit' } | Select-Object -Last 3) -join "`n")
    }
    Assert-NoRenderer "$backend _blank"

    # 3. window.open(): same new-tab path, and the page gets a window object.
    $r = Invoke-Probe $backend $links -Settle 900 `
        -Js 'var w = window.open("window_target.html"); probeExit("opened=" + (w ? "obj" : "null"));'
    if ($r.Code -eq 0 -and $r.Text -match 'opened=obj' -and
        $r.Text -match 'new window url=.*window_target\.html') {
        Write-Host "PASS  $backend window.open: new tab loaded, window object returned"
    } else {
        $script:failures++
        Write-Host "FAIL  $backend window.open"
        Write-Host (($r.Text -split "`n" | Where-Object { $_ -match 'opened=|new window|exit' } | Select-Object -Last 3) -join "`n")
    }
    Assert-NoRenderer "$backend window.open"

    # 4. named target twice: one window, second click navigates it and asks the
    #    host to bring its tab forward.
    $r = Invoke-Probe $backend $links -Settle 1200 `
        -Js 'probeExit("main=" + document.URL);' `
        -Extra @('--click', '60,240', '--click', '60,340')
    $requested = ([regex]::Matches($r.Text, 'new window requested')).Count
    if ($r.Code -eq 0 -and $requested -eq 1 -and $r.Text -match 'activate=1' -and
        $r.Text -match 'new window url=.*window_target2\.html') {
        Write-Host "PASS  $backend named target: one window, reused and activated"
    } else {
        $script:failures++
        Write-Host "FAIL  $backend named target reuse (windows=$requested)"
        Write-Host (($r.Text -split "`n" | Where-Object { $_ -match 'requested|activate|new window url|exit' } | Select-Object -Last 4) -join "`n")
    }
    Assert-NoRenderer "$backend named"

    # 5. window.close() from the popup reaches the host (probe exits OK on it).
    $r = Invoke-Probe $backend $links -Settle 2500 `
        -Js 'window.open("window_closer.html");'
    if ($r.Code -eq 0 -and $r.Text -match '\[close\] window.close\(\) reached the host') {
        Write-Host "PASS  $backend window.close from a popup reaches the host"
    } else {
        $script:failures++
        Write-Host "FAIL  $backend window.close (exit $($r.Code))"
        Write-Host (($r.Text -split "`n" | Where-Object { $_ -match 'requested|close\]|exit' } | Select-Object -Last 3) -join "`n")
    }
    Assert-NoRenderer "$backend close"
}

Remove-Item $out, $err, (Join-Path $build 'window_eval.js') -ErrorAction SilentlyContinue

if ($failures -gt 0) {
    Write-Host "`nWINDOW CHECKS FAILED ($failures)"
    exit 1
}

Write-Host "`nALL WINDOW CHECKS PASSED"
exit 0
