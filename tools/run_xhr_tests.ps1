# tools/run_xhr_tests.ps1
#
# XHR delivery regression: the delivery loop calls back into page JS, so it has
# to survive its own re-entrancy.
#
#   * xhr_reentry.html - two requests finish in the same frame and the first
#     callback runs a nested evaluation (an inline <script>). collectXhrJobs()
#     used to be re-entered while the outer loop still held an index into
#     xhrJobs(): the nested frame destroyed the job the outer frame was
#     delivering, then `jobs[i]` was past the end -> "ASSERTION FAILED:
#     i < size()" (v.qq.com, 0xc0000005, write 0xbbadbeef).
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
} finally {
    Stop-Server
}

if ($failed) {
    Write-Host "`nXHR CHECKS FAILED ($failed)"
    exit 1
}

Write-Host "`nALL XHR CHECKS PASSED"
exit 0
