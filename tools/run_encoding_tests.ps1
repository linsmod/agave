# tools/run_encoding_tests.ps1
#
# Regression for the document-encoding priority (hist-lessons R164):
#
#     BOM  >  HTTP Content-Type charset  >  <meta charset>  >  default
#
# tools/encoding_server.py serves tools/pages/encoding/*.html with the header
# deliberately contradicting the meta (and, for one route, the BOM
# contradicting both). Each page states its own expected title/charset in
# <meta name="expect-*">, and tools/pages/check_encoding.js (run through the
# probe) exits 7 when they do not match.
#
# Why this exists: the native (WinHTTP) backend never copied Content-Type's
# charset into the ResourceResponse, and TextResourceDecoder let <meta>
# override the HTTP header. Both 163 pages from the report rendered mojibake
# for exactly one of those two reasons.
#
# Usage:
#   pwsh -File tools/run_encoding_tests.ps1
#   pwsh -File tools/run_encoding_tests.ps1 -Port 8997
#
# Runtime: ~8-12 s (five probes against a local server).

param(
    [int]$Port = 8993,
    [string]$BuildDir = 'build-zig-native'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir
$exe = Join-Path $build 'agave_shell.exe'
$server = Join-Path $root 'tools/encoding_server.py'
$check = Join-Path $root 'tools/pages/check_encoding.js'

foreach ($p in @($exe, $server, $check)) {
    if (-not (Test-Path $p)) { throw "not found: $p" }
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ('agb-encoding-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

$serverOut = Join-Path $tmp 'server.out.txt'
$serverErr = Join-Path $tmp 'server.err.txt'
$watch = [Diagnostics.Stopwatch]::StartNew()
$serverProc = Start-Process -FilePath 'python' -PassThru -NoNewWindow `
    -ArgumentList @($server, '--port', $Port) `
    -RedirectStandardOutput $serverOut -RedirectStandardError $serverErr

$failures = 0

try {
    # Wait for the server: /health answers 200 once it is listening.
    $up = $false
    for ($i = 0; $i -lt 100 -and -not $up; $i++) {
        try {
            $r = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 1
            if ($r.StatusCode -eq 200) { $up = $true }
        } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    if (-not $up) {
        throw "encoding server did not start on port $Port (see $serverErr)"
    }

    $cases = @(
        @{ Name = 'header-utf8-beats-meta-gbk'; Path = '/header_utf8_meta_gbk.html' },
        @{ Name = 'header-gbk-beats-meta-utf8'; Path = '/header_gbk_meta_utf8.html' },
        @{ Name = 'header-only-no-meta';        Path = '/no_meta_utf8.html' },
        @{ Name = 'meta-when-header-is-silent'; Path = '/no_header_meta_gbk.html' },
        @{ Name = 'bom-beats-header-and-meta';  Path = '/bom_utf8.html' }
    )

    foreach ($case in $cases) {
        $url = "http://127.0.0.1:$Port$($case.Path)?t=$([guid]::NewGuid().ToString('N'))"
        $stdout = Join-Path $tmp ("$($case.Name).out.txt")
        $stderr = Join-Path $tmp ("$($case.Name).err.txt")

        $p = Start-Process -FilePath $exe -NoNewWindow -PassThru -Wait -ArgumentList @(
            '--probe', $url,
            '--eval-file', $check,
            '--wait', '1500', '--settle', '400', '--timeout', '30000'
        ) -RedirectStandardOutput $stdout -RedirectStandardError $stderr

        $err = (Get-Content $stderr -Raw) -as [string]
        $encLines = @($err -split "`n" | Where-Object { $_ -match 'ENC-' })
        $detail = ($encLines | Select-Object -Last 2) -join "`n"

        if ($p.ExitCode -ne 0) {
            $failures++
            Write-Host ("FAIL  {0} (probe exit {1})" -f $case.Name, $p.ExitCode)
            if ($detail) { Write-Host $detail }
        } else {
            Write-Host ("PASS  {0}" -f $case.Name)
        }
    }

    $watch.Stop()
    if ($failures -gt 0) {
        Write-Host ("ENCODING=FAIL ({0}/{1}) in {2:n1}s" -f $failures, $cases.Count, $watch.Elapsed.TotalSeconds)
    } else {
        Write-Host ("ALL ENCODING CHECKS PASSED ({0} cases) in {1:n1}s" -f $cases.Count, $watch.Elapsed.TotalSeconds)
    }
} finally {
    if ($serverProc -and -not $serverProc.HasExited) {
        Stop-Process -Id $serverProc.Id -Force -ErrorAction SilentlyContinue
    }
    # Any python child the launcher spawned (none expected, but be tidy).
    Get-CimInstance Win32_Process -Filter "Name='python.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine -like "*encoding_server.py*--port $Port*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

if ($failures -gt 0) { exit 1 }
exit 0
