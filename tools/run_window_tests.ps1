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
# All ten probes run at once. Each has its own AGB_RENDER_TAG, so the "no
# orphaned renderer" check is per case even while the other nine are still
# working; one tag-prefix query covers them all at the end.
#
# Usage:
#   pwsh -File tools/run_window_tests.ps1
# Expected wall time: ~5 s.

param(
    [string]$BuildDir = 'build-zig-native'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir
$exe = Join-Path $build 'agave_shell.exe'
$pages = Join-Path $root 'tools/pages'

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking
$run = New-TestRun -Suite 'window'

$failures = 0
$runTag = 'agvwin' + ([guid]::NewGuid().ToString('N').Substring(0, 12))
$links = Join-Path $pages 'window_links.html'
$jobs = @()

function Add-Case {
    param([string]$Name, [string]$Backend, [string]$Js, [int]$Wait, [int]$Settle, [string[]]$Extra)

    $script:jobs += Start-TestProbe -Run $run -Name "$Backend-$Name" -Url $links `
        -Eval $Js -Wait $Wait -Settle $Settle -Timeout 30000 -Extra $Extra `
        -Backend $Backend -RenderTag "$runTag-$Backend-$Name"
}

foreach ($backend in @('local', 'process')) {
    # --eval-retry: the plain/named cases navigate as a result of the click,
    # and under load the eval can be dispatched while that navigation is
    # committing (the control drops it; the retry runs it on the new document).
    Add-Case 'plain' $backend 'probeExit("url=" + document.URL);' 700 700 @('--click', '60,40', '--eval-retry')
    Add-Case 'blank' $backend 'probeExit("main=" + document.URL);' 700 700 @('--click', '60,140', '--eval-retry')
    Add-Case 'open' $backend 'var w = window.open("window_target.html"); probeExit("opened=" + (w ? "obj" : "null"));' 700 900 @()
    Add-Case 'named' $backend 'probeExit("main=" + document.URL);' 700 1200 @('--click', '60,240', '--click', '60,340', '--eval-retry')
    Add-Case 'close' $backend 'window.open("window_closer.html");' 700 2500 @()
}

$results = @{}
for ($i = 0; $i -lt $jobs.Count; $i++) {
    $r = Complete-TestProbe -Job $jobs[$i]
    $results[$jobs[$i].Name] = $r
}

function Check {
    param([string]$Name, [bool]$Ok, [string]$Detail)

    if ($Ok) {
        Write-Host "PASS  $Name"
    } else {
        $script:failures++
        Write-Host "FAIL  $Name"
        if ($Detail) {
            $Detail -split "`n" | Where-Object { $_ -match 'url=|requested|opened=|activate|close\]|exit' } |
                Select-Object -Last 3 | ForEach-Object { Write-Host ("      " + $_.TrimEnd()) }
        }
    }
}

foreach ($backend in @('local', 'process')) {
    Write-Host "`n-- backend $backend --"

    $r = $results["$backend-plain"]
    Check "$backend plain link: same tab" `
        ($r.Code -eq 0 -and $r.Text -match 'url=.*window_target\.html' -and $r.Text -notmatch 'new window requested') $r.Text

    $r = $results["$backend-blank"]
    Check "$backend target=_blank: new tab loaded, main tab unchanged" `
        ($r.Code -eq 0 -and $r.Text -match 'new window requested url=.*window_target\.html' -and
         $r.Text -match 'new window url=.*window_target\.html' -and
         $r.Text -match 'main=.*window_links\.html') $r.Text

    $r = $results["$backend-open"]
    Check "$backend window.open: new tab loaded, window object returned" `
        ($r.Code -eq 0 -and $r.Text -match 'opened=obj' -and $r.Text -match 'new window url=.*window_target\.html') $r.Text

    $r = $results["$backend-named"]
    $requested = ([regex]::Matches($r.Text, 'new window requested')).Count
    Check "$backend named target: one window, reused and activated" `
        ($r.Code -eq 0 -and $requested -eq 1 -and $r.Text -match 'activate=1' -and
         $r.Text -match 'new window url=.*window_target2\.html') $r.Text

    $r = $results["$backend-close"]
    Check "$backend window.close from a popup reaches the host" `
        ($r.Code -eq 0 -and $r.Text -match '\[close\] window.close\(\) reached the host') $r.Text
}

# One aggregate orphan check for all ten tags (teardown is asynchronous).
$left = @()
$deadline = (Get-Date).AddSeconds(10)
while ($true) {
    $left = @(Get-CimInstance Win32_Process -Filter "Name='agave_render_host.exe'" -ErrorAction SilentlyContinue |
              Where-Object { $_.CommandLine -and $_.CommandLine.Contains($runTag) })
    if ($left.Count -eq 0 -or (Get-Date) -gt $deadline) { break }
    Start-Sleep -Milliseconds 100
}
if ($left.Count -gt 0) {
    $failures++
    Write-Host ("FAIL  leftover renderer process(es): {0}" -f (($left | ForEach-Object { $_.ProcessId }) -join ', '))
    $left | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

Remove-TestRun $run $failures

if ($failures -gt 0) {
    Write-Host "`nWINDOW CHECKS FAILED ($failures)"
    exit 1
}

Write-Host "`nALL WINDOW CHECKS PASSED"
exit 0
