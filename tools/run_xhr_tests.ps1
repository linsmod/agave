# tools/run_xhr_tests.ps1
#
# XHR delivery regression: the delivery loop calls back into page JS, so it has
# to survive its own re-entrancy - and the body it hands over has to be decoded
# as UTF-8, not passed through as raw bytes.
#
#   * xhr_reentry.html - two requests finish in the same frame and the first
#     callback runs a nested evaluation (an inline <script>). collectXhrJobs()
#     used to be re-entered while the outer loop still held an index into
#     xhrJobs(): the nested frame destroyed the job the outer frame was
#     delivering, then `jobs[i]` was past the end -> "ASSERTION FAILED:
#     i < size()" (v.qq.com, 0xc0000005, write 0xbbadbeef).
#
#   * cap_xhr_text.html - an async XHR whose response is UTF-8 JSON with Chinese
#     text, read back through responseText and JSON.parse. The async path always
#     delivers bytes (base64), and _finish() used to leave responseText empty for
#     the default responseType, so every JSON endpoint threw
#     "JSON.parse: unexpected character" (v.qq.com's remote-config fetch).
#
# The pages need a real origin (XHR to file:// has no transport), so this runs a
# throwaway python http.server over tools/pages. All probes run at once: each
# has its own profile, and neither the server nor the pages share state.
#
# Usage:
#   pwsh -File tools/run_xhr_tests.ps1
#   pwsh -File tools/run_xhr_tests.ps1 -Port 8991 -BuildDir build-zig-native
#
# Expected wall time: ~4 s (one http server + two slow-xhr probes in flight).

param(
    [string]$BuildDir = 'build-zig-native',
    [int]$Port = 0
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root (Join-Path $BuildDir 'agave_shell.exe')
$pageDir = Join-Path $root 'tools/pages'

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }
if (-not (Get-Command python -ErrorAction SilentlyContinue)) { throw 'python not found (needed for the test server)' }

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force
$run = New-TestRun -Suite 'xhr'

if ($Port -le 0) { $Port = Get-TestFreePort }
$slowPort = Get-TestFreePort

$server = Start-Process -FilePath 'python' -WindowStyle Hidden -PassThru `
    -ArgumentList '-m', 'http.server', "$Port", '--bind', '127.0.0.1', '--directory', $pageDir
$slow = Start-Process -FilePath 'python' -WindowStyle Hidden -PassThru `
    -ArgumentList (Join-Path $root 'tools/slow_xhr_server.py'), "$slowPort", $pageDir

$failed = 0

function Stop-Servers {
    foreach ($p in @($server, $slow)) {
        if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
}

function Wait-Server([int]$At, [string]$Path) {
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 100
        try {
            $r = Invoke-WebRequest -Uri "http://127.0.0.1:$At/$Path" -UseBasicParsing -TimeoutSec 2
            if ($r.StatusCode -eq 200) { return $true }
        } catch { }
    }
    return $false
}

function Check-Case {
    param($Case, $Result)

    if ($Result.Code -ne 0) {
        $failed++
        Write-Host "FAIL  $($Case.Page): probe exit $($Result.Code)"
        return
    }
    $missing = 0
    foreach ($needle in $Case.Needles) {
        if ($Result.Text -notmatch [regex]::Escape($needle)) {
            Write-Host "FAIL  $($Case.Page): missing '$needle'"
            $missing++
        }
    }
    if ($Case.ContainsKey('Forbid') -and $Result.Text -match $Case.Forbid) {
        Write-Host "FAIL  $($Case.Page): matched forbidden '$($Case.Forbid)'"
        $missing++
    }
    if ($missing) { $failed++ } else { Write-Host "PASS  $($Case.Page) ($($Case.What))" }
}

try {
    if (-not (Wait-Server $Port 'xhr_reentry.html')) { throw "test server did not come up on port $Port" }
    if (-not (Wait-Server $slowPort 'xhr_frame_cancel.html')) { throw "slow xhr server did not come up on port $slowPort" }

    $cases = @(
        @{ Page = 'xhr_reentry.html'; Check = 'check_xhr_reentry.js'; Wait = 1200; Settle = 300
           What = 'delivery survives re-entrancy'
           Forbid = 'ASSERTION FAILED|0xbbadbeef'
           Needles = @('done=2', 'nested=1', 'log=cb1@', 'cb2@', 'settled') }

        @{ Page = 'cap_xhr_text.html'; Check = 'check_xhr_text.js'; Wait = 1800; Settle = 400
           What = 'responseText is decoded UTF-8'
           Needles = @('status=200', 'rtHas=yes', 'rt0=9884', 'json.ok=true',
                       'json.msgHas=yes', 'json.list2=yes') }

        # Every needle below is what Chrome's --headless=new --dump-dom prints for
        # the same page; see the comment in cap_xhr_headers.html.
        @{ Page = 'cap_xhr_headers.html'; Check = 'check_xhr_headers.js'; Wait = 2000; Settle = 400
           What = 'getResponseHeader/getAllResponseHeaders match Chrome'
           Needles = @('states=1,2,3,4', 'statusText="OK"', 'preHeader=null', 'preAll=""',
                       'all-endCRLF=yes', 'allHasSetCookie=no', 'getJson="application/json"',
                       'getLower="application/json"', 'getMissing=null', 'getSetCookie=null',
                       'jsonRT=THREW InvalidStateError', 'jsonResp=true') }

        @{ Page = 'cap_xhr_mime.html'; Check = 'check_xhr_mime.js'; Wait = 2000; Settle = 400
           What = 'overrideMimeType charset wins over Content-Type'
           Needles = @('plain="预 caf', 'override="é¢„ café"', 'overrideCT="text/plain"',
                       'plainStatus=200') }

        @{ Page = 'cap_xhr_accessors.html'; Check = 'check_xhr_accessors.js'; Wait = 2000; Settle = 400
           What = 'accessor edge cases (reset, missing, sorted, text type)'
           Needles = @('earlyHeader=null', 'earlyAll=""', 'afterOpenStatus=0', 'afterOpenAll=""',
                       'missing=null', 'allSorted=yes', 'reopenStatus=0', 'reopenAll=""',
                       'reopenRT=""', 'textTypeRT=string', 'textTypeHas=yes', 'textTypeResp=same') }

        # ant has no gbk/big5/shift_jis tables; the browser hands it its own through
        # antb_set_text_codec_provider. Every needle is Chrome's answer except
        # shift_jis, which is a KNOWN DIVERGENCE in WebCore's table lookup
        # (shiftjis_conv_to_uchar) and is therefore not asserted here.
        @{ Page = 'cap_textdecoder_legacy.html'; Check = 'check_textdecoder_legacy.js'; Wait = 2000; Settle = 300
           What = 'gbk/gb18030/big5/euc-kr come from the host provider'
           Needles = @('gbk=预约', 'gbk=预约 A', 'gb2312=预约', 'gb18030=预约',
                       'big5=頌地', 'euc-kr=예한', 'gbk=gbk', 'big5=big5',
                       'utf-8=预', 'windows-1252=é', 'bogus=rejected') }

        # S2: a frame document going away cancels the work it started (own server:
        # /slow-xhr takes 1.2 s).
        @{ Page = 'xhr_frame_cancel.html'; Check = 'check_xhr_frame_cancel.js'; Wait = 3000; Settle = 300
           Port = $slowPort; What = 'frame work is cancelled with its document'
           Needles = @('fired=0') }
    )

    $jobs = @()
    $casesUsed = @()
    foreach ($c in $cases) {
        $at = if ($c.ContainsKey('Port')) { $c.Port } else { $Port }
        $jobs += Start-TestProbe -Run $run -Name ($c.Page -replace '\.html$', '') `
            -Url "http://127.0.0.1:$at/$($c.Page)" `
            -EvalFile (Join-Path $pageDir $c.Check) `
            -Wait $c.Wait -Settle $c.Settle -Timeout 30000
        $casesUsed += $c
    }

    $results = Complete-TestProbes -Jobs $jobs
    for ($i = 0; $i -lt $casesUsed.Count; $i++) {
        Check-Case $casesUsed[$i] $results[$i]
    }

    # The shift_jis divergence guard stays informational, as before.
    $legacy = $results[5]
    if ($legacy.Text -notmatch [regex]::Escape('shift_jis=椿Δ')) {
        Write-Host 'NOTE  cap_textdecoder_legacy.html: shift_jis changed - check the R97 known divergence'
        $failed++
    }
} finally {
    Stop-Servers
    Remove-TestRun $run $failed
}

if ($failed) {
    Write-Host "`nXHR CHECKS FAILED ($failed)"
    exit 1
}

Write-Host "`nALL XHR CHECKS PASSED"
exit 0
