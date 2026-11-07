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
# throwaway python http.server over tools/pages.
#
# Usage:
#   pwsh -File tools/run_xhr_tests.ps1
#   pwsh -File tools/run_xhr_tests.ps1 -Port 8991 -BuildDir build-zig-native

param(
    [string]$BuildDir = 'build-zig-native',
    [int]$Port = 8991
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root (Join-Path $BuildDir 'agave_shell.exe')
$pageDir = Join-Path $root 'tools/pages'

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }
if (-not (Get-Command python -ErrorAction SilentlyContinue)) { throw 'python not found (needed for the test server)' }

$server = Start-Process -FilePath 'python' -WindowStyle Hidden -PassThru `
    -ArgumentList '-m', 'http.server', "$Port", '--bind', '127.0.0.1', '--directory', $pageDir

$failed = 0

function Stop-Server {
    if ($server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue }
}

try {
    $ready = $false
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 150
        try {
            $r = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/xhr_reentry.html" -UseBasicParsing -TimeoutSec 2
            if ($r.StatusCode -eq 200) { $ready = $true; break }
        } catch { }
    }
    if (-not $ready) { throw "test server did not come up on port $Port" }

    $url = "http://127.0.0.1:$Port/xhr_reentry.html"
    $out = & $exe --probe $url `
        --eval-file (Join-Path $pageDir 'check_xhr_reentry.js') `
        --wait 1500 --settle 300 --timeout 30000 2>&1
    $code = $LASTEXITCODE

    $text = $out -join "`n"
    $missing = 0

    if ($code -ne 0) {
        Write-Host "FAIL  xhr_reentry.html: probe exit $code"
        $failed++
    } else {
        foreach ($needle in @('done=2', 'nested=1', 'log=cb1@', 'cb2@', 'settled')) {
            if ($text -notmatch [regex]::Escape($needle)) {
                Write-Host "FAIL  xhr_reentry.html: missing '$needle'"
                $missing++
            }
        }
        if ($text -match 'ASSERTION FAILED|0xbbadbeef') {
            Write-Host 'FAIL  xhr_reentry.html: assertion / poison write'
            $missing++
        }
        if ($missing) { $failed++ } else { Write-Host 'PASS  xhr_reentry.html (delivery survives re-entrancy)' }
    }

    # ---- UTF-8 responseText -------------------------------------------------
    $url = "http://127.0.0.1:$Port/cap_xhr_text.html"
    $out = & $exe --probe $url `
        --eval-file (Join-Path $pageDir 'check_xhr_text.js') `
        --wait 2500 --settle 500 --timeout 30000 2>&1
    $code = $LASTEXITCODE

    $text = $out -join "`n"
    $missing = 0

    if ($code -ne 0) {
        Write-Host "FAIL  cap_xhr_text.html: probe exit $code"
        $failed++
    } else {
        foreach ($needle in @('status=200', 'rtHas=yes', 'rt0=9884', 'json.ok=true',
                              'json.msgHas=yes', 'json.list2=yes')) {
            if ($text -notmatch [regex]::Escape($needle)) {
                Write-Host "FAIL  cap_xhr_text.html: missing '$needle'"
                $missing++
            }
        }
        if ($missing) { $failed++ } else { Write-Host 'PASS  cap_xhr_text.html (responseText is decoded UTF-8)' }
    }

    # ---- response header API ------------------------------------------------
    # Every needle below is what Chrome's --headless=new --dump-dom prints for
    # the same page; see the comment in cap_xhr_headers.html.
    $cases = @(
        @{ Page = 'cap_xhr_headers.html'
           Check = 'check_xhr_headers.js'
           What  = 'getResponseHeader/getAllResponseHeaders match Chrome'
           Needles = @('states=1,2,3,4', 'statusText="OK"', 'preHeader=null', 'preAll=""',
                       'all-endCRLF=yes', 'allHasSetCookie=no', 'getJson="application/json"',
                       'getLower="application/json"', 'getMissing=null', 'getSetCookie=null',
                       'jsonRT=THREW InvalidStateError', 'jsonResp=true') }
        @{ Page = 'cap_xhr_mime.html'
           Check = 'check_xhr_mime.js'
           What  = 'overrideMimeType charset wins over Content-Type'
           Needles = @('plain="预 caf', 'override="é¢„ café"', 'overrideCT="text/plain"',
                       'plainStatus=200') }
        @{ Page = 'cap_xhr_accessors.html'
           Check = 'check_xhr_accessors.js'
           What  = 'accessor edge cases (reset, missing, sorted, text type)'
           Needles = @('earlyHeader=null', 'earlyAll=""', 'afterOpenStatus=0', 'afterOpenAll=""',
                       'missing=null', 'allSorted=yes', 'reopenStatus=0', 'reopenAll=""',
                       'reopenRT=""', 'textTypeRT=string', 'textTypeHas=yes', 'textTypeResp=same') }
    )

    foreach ($case in $cases) {
        $out = & $exe --probe "http://127.0.0.1:$Port/$($case.Page)" `
            --eval-file (Join-Path $pageDir $case.Check) `
            --wait 3000 --settle 500 --timeout 30000 2>&1
        $code = $LASTEXITCODE
        $text = $out -join "`n"
        $missing = 0

        if ($code -ne 0) {
            Write-Host "FAIL  $($case.Page): probe exit $code"
            $failed++
            continue
        }

        foreach ($needle in $case.Needles) {
            if ($text -notmatch [regex]::Escape($needle)) {
                Write-Host "FAIL  $($case.Page): missing '$needle'"
                $missing++
            }
        }

        if ($missing) { $failed++ } else { Write-Host "PASS  $($case.Page) ($($case.What))" }
    }

    # ---- legacy TextDecoder codecs ------------------------------------------
    # ant has no gbk/big5/shift_jis tables; the browser hands it its own through
    # antb_set_text_codec_provider. Every needle is Chrome's answer except
    # shift_jis, which is a KNOWN DIVERGENCE in WebCore's table lookup
    # (shiftjis_conv_to_uchar) and is therefore not asserted here.
    $out = & $exe --probe "http://127.0.0.1:$Port/cap_textdecoder_legacy.html" `
        --eval-file (Join-Path $pageDir 'check_textdecoder_legacy.js') `
        --wait 2000 --settle 300 --timeout 30000 2>&1
    $code = $LASTEXITCODE
    $text = $out -join "`n"
    $missing = 0

    if ($code -ne 0) {
        Write-Host "FAIL  cap_textdecoder_legacy.html: probe exit $code"
        $failed++
    } else {
        foreach ($needle in @(
            'gbk=预约', 'gbk=预约 A', 'gb2312=预约', 'gb18030=预约',
            'big5=頌地', 'euc-kr=예한', 'gbk=gbk', 'big5=big5',
            'utf-8=预', 'windows-1252=é', 'bogus=rejected')) {
            if ($text -notmatch [regex]::Escape($needle)) {
                Write-Host "FAIL  cap_textdecoder_legacy.html: missing '$needle'"
                $missing++
            }
        }
        # Guarded so the known shift_jis divergence cannot silently spread.
        if ($text -notmatch [regex]::Escape('shift_jis=椿Δ')) {
            Write-Host "NOTE  cap_textdecoder_legacy.html: shift_jis changed - check the R97 known divergence"
            $missing++
        }
        if ($missing) { $failed++ } else {
            Write-Host 'PASS  cap_textdecoder_legacy.html (gbk/gb18030/big5/euc-kr come from the host provider)'
        }
    }
} finally {
    Stop-Server
}

if ($failed) {
    Write-Host "`nXHR CHECKS FAILED ($failed)"
    exit 1
}

Write-Host "`nALL XHR CHECKS PASSED"
exit 0
