# run_worker_tests.ps1 - Web Worker regression (http worker, importScripts, blob,
# terminate, subclass, error events, structured clone).
#
#   pwsh -File tools/run_worker_tests.ps1              # start a server, run, stop
#   pwsh -File tools/run_worker_tests.ps1 -Port 8972   # another port
#   pwsh -File tools/run_worker_tests.ps1 -KeepServer   # leave the server running
#
# The page needs http semantics (worker scripts are fetched through the async
# bridge, so file:// would not exercise the real path). Exit code 0 = all checks
# passed, 1 = failures (printed), 2 = setup failed.
param(
    [int]$Port = 8971,
    [string]$Exe = (Join-Path $PSScriptRoot '..\build-zig-native\agave_shell.exe'),
    [int]$Wait = 3000,
    [int]$Settle = 2500,
    [switch]$KeepServer
)

$ErrorActionPreference = 'Stop'
$debugDir = (Resolve-Path (Join-Path $PSScriptRoot '..\debug')).Path
$Exe = (Resolve-Path $Exe).Path
$page = 'http://127.0.0.1:' + $Port + '/worker_support.html'

if (-not (Test-Path $Exe)) { Write-Output "missing binary: $Exe"; exit 2 }

$server = $null
try {
    $listening = $false
    try { Invoke-WebRequest -Uri $page -UseBasicParsing -TimeoutSec 2 | Out-Null; $listening = $true } catch {}
    if (-not $listening) {
        $server = Start-Process -WindowStyle Hidden python -ArgumentList '-m', 'http.server', $Port, '--directory', $debugDir -PassThru
        for ($i = 0; $i -lt 30 -and -not $listening; $i++) {
            Start-Sleep -Milliseconds 200
            try { Invoke-WebRequest -Uri $page -UseBasicParsing -TimeoutSec 2 | Out-Null; $listening = $true } catch {}
        }
    }
    if (-not $listening) { Write-Output "could not start a server on port $Port"; exit 2 }

    $eval = "probeExit('FAILS=' + JSON.stringify(window.__fail) + ' || ' + String(window.__t))"
    $out = & $Exe --probe $page --eval $eval --wait $Wait --settle $Settle --timeout 45000 2>&1
    $code = $LASTEXITCODE
    $line = ($out | Select-String '\[exit\]' | Select-Object -First 1)
    if (-not $line) { Write-Output "probe failed (exit=$code):"; $out | Select-Object -Last 10; exit 2 }

    $text = $line.Line -replace '^.*\[exit\]\s*', ''
    $fails = (($text -split '\|\|')[0] -replace '^FAILS=', '').Trim()
    $detail = ($text -split '\|\|', 2)
    Write-Output "exit=$code"
    Write-Output "fails=$fails"
    if ($detail.Count -gt 1) { Write-Output ('results=' + $detail[1].Trim()) }
    if ($fails -eq '[]' -and $code -eq 0) { Write-Output 'ALL WORKER CHECKS PASSED'; exit 0 }
    exit 1
}
finally {
    if ($server -and -not $KeepServer) {
        Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
    }
}
