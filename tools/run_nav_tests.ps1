# run_nav_tests.ps1 - regression for the [nav] log channel.
#
# One probe run per automatic navigation action (link click, location change,
# history back, meta refresh, window.open, form submit, http 302) and one run
# with AGB_NAV_LOG=0 to prove the channel can be silenced.
#
#   pwsh -File tools/run_nav_tests.ps1
#
# The pages are in tools/pages/ and are served over http so form GET, cookies
# and redirects behave like a real site.
param(
    [int]$Port = 8973
)

$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$exe   = Join-Path $root 'build-zig-native\agave_shell.exe'
$pages = Join-Path $PSScriptRoot 'pages'
$base  = "http://127.0.0.1:$Port"

if (-not (Test-Path $exe)) {
    "missing $exe - build first (pwsh -File tools/build_project.ps1)"
    exit 1
}

$server = Start-Process -FilePath 'python' -ArgumentList @(
    (Join-Path $PSScriptRoot 'nav_redirect_server.py'), "$Port"
) -WindowStyle Hidden -PassThru

function Wait-Server {
    for ($i = 0; $i -lt 40; $i++) {
        try {
            Invoke-WebRequest -UseBasicParsing -Uri "$base/navcheck_target.html" -TimeoutSec 1 | Out-Null
            return $true
        } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    return $false
}

$failures = 0
$cases    = 0

function Run-Case {
    param(
        [string]$Label,
        [string]$Page,
        [string]$Eval,
        [string]$Expect,
        [string]$Expect2 = ''
    )

    $script:cases++
    $out  = & $exe --probe "$base/$Page" --wait 1200 --settle 300 --timeout 25000 --eval $Eval 2>&1
    $code = $LASTEXITCODE
    $text = ($out | Out-String)

    $ok = ($code -eq 0) -and ($text -match $Expect)
    if ($Expect2 -and ($text -notmatch $Expect2)) { $ok = $false }

    if ($ok) {
        "PASS  $Label"
    } else {
        $script:failures++
        "FAIL  $Label   exit=$code expected /$Expect/"
        ($text -split "`n" | Where-Object { $_ -match '\[nav\]|\[exit\]|exit code' }) -join "`n"
    }
}

try {
    if (-not (Wait-Server)) {
        "server did not come up on $base"
        exit 1
    }

    Run-Case 'link click'      'navcheck_link.html'    'probeExit(document.title)' 'request main type=link method=GET url=.*navcheck_target\.html' 'open url=.*navcheck_link\.html'
    Run-Case 'location.href'   'navcheck_loc.html'     'probeExit(document.title)' 'request main type=other method=GET url=.*navcheck_target\.html'
    Run-Case 'history back'    'navcheck_history.html' 'probeExit(document.title)' 'request main type=back-forward method=GET url=.*navcheck_history\.html' '\[exit\] BACK-OK'
    Run-Case 'meta refresh'    'navcheck_meta.html'    'probeExit(document.title)' 'client-redirect main url=.*navcheck_target\.html'
    Run-Case 'window.open'     'navcheck_popup.html'   'probeExit(document.title)' 'popup create url=.*navcheck_target\.html'
    Run-Case 'form GET submit' 'navcheck_form.html'    'probeExit(location.search)' 'request main type=form method=GET url=.*\?q=hello' 'submit main form=.* action=navcheck_target\.html'
    Run-Case 'http 302 redirect' 'redirect302'          'probeExit(document.title)' 'redirect main from=.*redirect302 to=.*navcheck_target\.html' '\[probe\] main url=.*navcheck_target\.html'

    # Internal pages (agave://) come from a loader-level scheme provider, so a
    # reload re-requests the URL through the same path as a navigation instead
    # of the network. Before R162 a reload of agave://downloads crashed the
    # renderer (process backend) and failed the load (local backend).
    foreach ($internal in @(
        @{ Label = 'internal page: navigate';    Extra = @();               Expect = '\[exit\] title=下载' },
        @{ Label = 'internal page: reload x2';   Extra = @('--reload','2'); Expect = '\[exit\] title=下载' }
    )) {
        $script:cases++
        $argv = @('--probe', 'agave://downloads')
        if ($internal.Extra.Count -gt 0) { $argv += $internal.Extra }
        $argv += @('--wait', '500', '--settle', '300', '--timeout', '25000',
                   '--eval', 'probeExit(''title=''+document.title);')
        $o = & $exe @argv 2>&1
        $code = $LASTEXITCODE
        $txt = ($o | Out-String)

        if ($code -eq 0 -and $txt -match $internal.Expect -and
            $txt -notmatch 'page crashed|ASSERTION FAILED|fail resource') {
            "PASS  $($internal.Label)"
        } else {
            $script:failures++
            "FAIL  $($internal.Label)   exit=$code"
            (($txt -split "`n" | Where-Object { $_ -match 'title=|crash|fail|ASSERT' } | Select-Object -First 4) -join "`n")
        }
    }

    # Opt-out: same link navigation, no [nav] line at all.
    $script:cases++
    $env:AGB_NAV_LOG = '0'
    $out  = & $exe --probe "$base/navcheck_link.html" --wait 800 --settle 200 --timeout 20000 --eval 'probeExit(1)' 2>&1
    $code = $LASTEXITCODE
    Remove-Item Env:\AGB_NAV_LOG
    $navLines = @($out | Where-Object { $_ -match '^\[nav\]' }).Count

    if (($code -eq 0) -and ($navLines -eq 0)) {
        "PASS  AGB_NAV_LOG=0 silences the channel"
    } else {
        $script:failures++
        "FAIL  AGB_NAV_LOG=0   exit=$code nav-lines=$navLines"
    }
} finally {
    if ($server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force
    }
}

""
if ($failures -eq 0) {
    "ALL NAV CHECKS PASSED ($cases cases)"
    exit 0
}

"$failures of $cases nav checks failed"
exit 1
