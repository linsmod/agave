# run_nav_tests.ps1 - regression for the [nav] log channel.
#
# One probe run per automatic navigation action (link click, location change,
# history back, meta refresh, window.open, form submit, http 302) and one run
# with AGB_NAV_LOG=0 to prove the channel can be silenced.
#
#   pwsh -File tools/run_nav_tests.ps1
#
# The pages are in tools/pages/ and are served over http so form GET, cookies
# and redirects behave like a real site. All cases run at once (own profiles,
# free port for the server). Expected wall time: ~3 s.
param(
    [int]$Port = 0
)

$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$exe   = Join-Path $root 'build-zig-native\agave_shell.exe'
$pages = Join-Path $PSScriptRoot 'pages'

if (-not (Test-Path $exe)) {
    "missing $exe - build first (pwsh -File tools/build_project.ps1)"
    exit 1
}

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking
$run = New-TestRun -Suite 'nav'

if ($Port -le 0) { $Port = Get-TestFreePort }
$base = "http://127.0.0.1:$Port"

$server = Start-Process -FilePath 'python' -ArgumentList @(
    (Join-Path $PSScriptRoot 'nav_redirect_server.py'), "$Port"
) -WindowStyle Hidden -PassThru

function Wait-Server {
    for ($i = 0; $i -lt 60; $i++) {
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

function Check {
    param([string]$Label, [bool]$Ok, [string]$Detail)

    $script:cases++
    if ($Ok) {
        "PASS  $Label"
    } else {
        $script:failures++
        "FAIL  $Label"
        if ($Detail) {
            $Detail -split "`n" | Where-Object { $_ -match '\[nav\]|\[exit\]|exit code|title=' } |
                Select-Object -First 6 | ForEach-Object { "      $_" }
        }
    }
}

try {
    if (-not (Wait-Server)) {
        "server did not come up on $base"
        exit 1
    }

    $list = @(
        @{ Name = 'link click';       Page = 'navcheck_link.html';    Eval = 'probeExit(document.title)'; Expect = 'request main type=link method=GET url=.*navcheck_target\.html'; Expect2 = 'open url=.*navcheck_link\.html' },
        @{ Name = 'location.href';    Page = 'navcheck_loc.html';     Eval = 'probeExit(document.title)'; Expect = 'request main type=other method=GET url=.*navcheck_target\.html'; Expect2 = '' },
        @{ Name = 'history back';     Page = 'navcheck_history.html'; Eval = 'probeExit(document.title)'; Expect = 'request main type=back-forward method=GET url=.*navcheck_history\.html'; Expect2 = '\[exit\] BACK-OK' },
        @{ Name = 'meta refresh';     Page = 'navcheck_meta.html';    Eval = 'probeExit(document.title)'; Expect = 'client-redirect main url=.*navcheck_target\.html'; Expect2 = '' },
        @{ Name = 'window.open';      Page = 'navcheck_popup.html';   Eval = 'probeExit(document.title)'; Expect = 'popup create url=.*navcheck_target\.html'; Expect2 = '' },
        @{ Name = 'form GET submit';  Page = 'navcheck_form.html';    Eval = 'probeExit(location.search)'; Expect = 'request main type=form method=GET url=.*\?q=hello'; Expect2 = 'submit main form=.* action=navcheck_target\.html' },
        @{ Name = 'http 302 redirect'; Page = 'redirect302';          Eval = 'probeExit(document.title)'; Expect = 'redirect main from=.*redirect302 to=.*navcheck_target\.html'; Expect2 = '\[probe\] main url=.*navcheck_target\.html' }
    )

    $jobs = @()
    foreach ($c in $list) {
        # --eval-retry: the eval is armed a fixed time after load, but these
        # pages navigate as the result of the probe's own click/submit. Under
        # load the navigation can be mid-commit when the eval is dispatched;
        # the control drops it, and the retry runs it against the new document.
        $jobs += Start-TestProbe -Run $run -Name $c.Name -Url "$base/$($c.Page)" `
            -Eval $c.Eval -Wait 1200 -Settle 300 -Timeout 25000 -Extra @('--eval-retry')
    }

    # Internal pages (agave://) come from a loader-level scheme provider, so a
    # reload re-requests the URL through the same path as a navigation instead
    # of the network.
    $internal = @(
        @{ Name = 'internal page: navigate';  Extra = @();               Expect = '\[exit\] title=下载' },
        @{ Name = 'internal page: reload x2'; Extra = @('--reload','2'); Expect = '\[exit\] title=下载' }
    )
    $internalJobs = @()
    foreach ($c in $internal) {
        $internalJobs += Start-TestProbe -Run $run -Name $c.Name -Url 'agave://downloads' `
            -Eval "probeExit('title='+document.title)" -Wait 500 -Settle 300 -Timeout 25000 -Extra $c.Extra
    }

    # Opt-out: same link navigation, no [nav] line at all.
    $optJob = Start-TestProbe -Run $run -Name 'nav-log-off' -Url "$base/navcheck_link.html" `
        -Eval 'probeExit(1)' -Wait 800 -Settle 200 -Timeout 20000 -Env @{ AGB_NAV_LOG = '0' }

    $results = Complete-TestProbes -Jobs $jobs
    for ($i = 0; $i -lt $list.Count; $i++) {
        $c = $list[$i]
        $r = $results[$i]
        $ok = ($r.Code -eq 0) -and ($r.Text -match $c.Expect)
        if ($c.Expect2 -and ($r.Text -notmatch $c.Expect2)) { $ok = $false }
        Check $c.Name $ok $r.Text
    }

    $internalRes = Complete-TestProbes -Jobs $internalJobs
    for ($i = 0; $i -lt $internal.Count; $i++) {
        $c = $internal[$i]
        $r = $internalRes[$i]
        $ok = ($r.Code -eq 0) -and ($r.Text -match $c.Expect) -and
              ($r.Text -notmatch 'page crashed|ASSERTION FAILED|fail resource')
        Check $c.Name $ok $r.Text
    }

    $opt = Complete-TestProbe -Job $optJob
    $navLines = @($opt.Text -split "`n" | Where-Object { $_ -match '^\[nav\]' }).Count
    Check 'AGB_NAV_LOG=0 silences the channel' ($opt.Code -eq 0 -and $navLines -eq 0) $opt.Text
} finally {
    if ($server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-TestRun $run $failures
}

""
if ($failures -eq 0) {
    "ALL NAV CHECKS PASSED ($cases cases)"
    exit 0
}

"$failures of $cases nav checks failed"
exit 1
