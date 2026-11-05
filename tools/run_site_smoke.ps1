# tools/run_site_smoke.ps1
#
# Real-site smoke test: does a handful of live pages still load, execute their
# JS and end up with a sane DOM? This is the regression that catches things the
# synthetic pages miss - realm changes, DCL ordering, loader changes, GC bugs.
#
# For each site it runs the headless probe with an embedded summary script and
# reports exit code, wall time, element count and title. Exit 0 + enough
# elements = pass.
#
# Requires network access. A site that is down or blocked shows up as exit 5/6
# (load failure / empty document) - that is a network problem, not an engine
# one; re-run before digging.
#
# v.qq.com runs with AGB_GC_OFF=1 on purpose: it still hits the superplayer AV
# tracked as P0-h/R68 (docs/plan-roadmap.md). Drop that when R68 is fixed.
#
# Usage:
#   pwsh -File tools/run_site_smoke.ps1
#   pwsh -File tools/run_site_smoke.ps1 -Sites deepseek,baidu
#   pwsh -File tools/run_site_smoke.ps1 -TimeoutSec 180

param(
    [string]$BuildDir = 'build-zig-native',
    [string[]]$Sites = @('deepseek', 'baidu', 'vqq'),
    [int]$TimeoutSec = 120
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root (Join-Path $BuildDir 'agave_shell.exe')
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('agb-site-smoke-' + [guid]::NewGuid().ToString('N'))

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

# The summary script runs after --wait; keep it quote-free for the probe parser.
$summaryJs = Join-Path $tmp 'summary.js'
@'
probeExit('all=' + document.querySelectorAll('*').length
  + ' title=' + String(document.title).slice(0, 40)
  + ' rs=' + document.readyState);
'@ | Set-Content -Path $summaryJs -Encoding ascii

$cases = @(
    @{ Name = 'deepseek'; Url = 'https://chat.deepseek.com/'; MinElements = 50;  Wait = 8000;  Extra = @{} }
    @{ Name = 'baidu';    Url = 'https://www.baidu.com/';    MinElements = 100; Wait = 6000;  Extra = @{} }
    @{ Name = 'vqq';      Url = 'https://v.qq.com/';         MinElements = 100; Wait = 20000;
       Extra = @{ AGB_GC_OFF = '1' } }   # P0-h/R68 workaround
)

$selected = @($cases | Where-Object { $Sites -contains $_.Name })

if ($selected.Count -eq 0) {
    throw ("no site matched -Sites {0} (known: {1})" -f ($Sites -join ','), (($cases | ForEach-Object { $_.Name }) -join ','))
}

$failures = 0

foreach ($case in $selected) {
    $stdout = Join-Path $tmp ($case.Name + '.out')
    $stderr = Join-Path $tmp ($case.Name + '.err')
    $saved = @{}
    $sw = [Diagnostics.Stopwatch]::StartNew()

    foreach ($key in $case.Extra.Keys) {
        $saved[$key] = [Environment]::GetEnvironmentVariable($key)
        Set-Item -Path ("Env:" + $key) -Value $case.Extra[$key]
    }

    try {
        $p = Start-Process -FilePath $exe -NoNewWindow -PassThru -Wait -ArgumentList @(
            '--probe', $case.Url, '--eval-file', $summaryJs,
            '--wait', "$($case.Wait)", '--settle', '1000', '--timeout', "$($TimeoutSec * 1000)"
        ) -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    } finally {
        foreach ($key in $case.Extra.Keys) {
            if ($null -eq $saved[$key]) { Remove-Item -Path ("Env:" + $key) -ErrorAction SilentlyContinue }
            else { Set-Item -Path ("Env:" + $key) -Value $saved[$key] }
        }
        $sw.Stop()
    }

    $outText = (Get-Content $stdout -Raw) -as [string]
    $text = (Get-Content $stderr -Raw) -as [string]

    # probeExit's payload goes to stdout; the engine's atexit/leak forensics
    # go to stderr.
    $payload = [regex]::Match($outText, '\[exit\]\s+(all=\d+[^\r\n]*)')
    $elements = 0

    if ($payload.Success -and $payload.Groups[1].Value -match 'all=(\d+)') {
        $elements = [int]$Matches[1]
    }

    $ok = ($p.ExitCode -eq 0) -and ($elements -ge $case.MinElements)

    if ($ok) {
        Write-Host ("PASS  {0,-9} {1,6:N1}s  {2}" -f $case.Name, $sw.Elapsed.TotalSeconds, $payload.Groups[1].Value)
    } else {
        $failures++
        $why = if ($p.ExitCode -ne 0) { "exit $($p.ExitCode)" } else { "elements $elements < $($case.MinElements)" }
        Write-Host ("FAIL  {0,-9} {1,6:N1}s  {2}" -f $case.Name, $sw.Elapsed.TotalSeconds, $why)
        if ($p.ExitCode -eq 5 -or $p.ExitCode -eq 6) {
            Write-Host "      load failure/empty document - check network reachability first"
        }
        if ($text) {
            Write-Host (($text -split "`n" | Select-Object -Last 4) -join "`n")
        }
    }
}

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

if ($failures -gt 0) {
    Write-Host "`nSITE SMOKE FAILED ($failures)"
    exit 1
}

Write-Host "`nALL SITE SMOKE PASSED"
exit 0
