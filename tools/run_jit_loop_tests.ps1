# tools/run_jit_loop_tests.ps1
#
# Regression for R61: long JIT loops (and any slow --eval script) must not take
# the probe down with 0xC0000005.
#
# The AV was never in the JIT. It was a lifetime bug in the view control: the
# host destroyed the view while the engine thread was still executing a queued
# EVAL, so macross_view_destroy() freed the engine under the running script.
# The probe made it easy to hit because --settle counted from "eval posted", so
# any script slower than --settle was still running when the probe tore down.
#
# Two properties are checked:
#   1. loops that outlive --settle finish and report their result (the probe now
#      starts the settle countdown when the eval returns);
#   2. a hard --timeout that fires *during* the loop still tears the view down
#      safely (the command pins the view; destroy is deferred to the engine
#      thread) - it must end with a probe exit code, never a raw AV.
#
# Usage:
#   pwsh -File tools/run_jit_loop_tests.ps1
#   pwsh -File tools/run_jit_loop_tests.ps1 -Repeat 5 -BuildDir build-zig-native

param(
    [string]$BuildDir = 'build-zig-native',
    [string]$Page = 'tools/pages/blank.html',
    [int]$Repeat = 3
)

$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$exe   = Join-Path $root (Join-Path $BuildDir 'agave_shell.exe')
$page  = Join-Path $root $Page
$tmp   = Join-Path ([IO.Path]::GetTempPath()) ('agb-jit-loop-' + [guid]::NewGuid().ToString('N'))

if (-not (Test-Path $exe))  { throw "agave_shell.exe not found: $exe" }
if (-not (Test-Path $page)) { throw "page not found: $page" }
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

function Invoke-Probe {
    param([string]$Script, [int]$Wait, [int]$Settle, [int]$Timeout)

    $file = Join-Path $tmp ('case-' + [guid]::NewGuid().ToString('N') + '.js')
    Set-Content -Path $file -Value $Script -Encoding ascii

    $stdout = Join-Path $tmp 'out.txt'
    $stderr = Join-Path $tmp 'err.txt'

    $proc = Start-Process -FilePath $exe -NoNewWindow -PassThru -Wait `
        -ArgumentList @('--probe', $page, '--eval-file', $file,
                        '--wait', "$Wait", '--settle', "$Settle", '--timeout', "$Timeout") `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr

    [pscustomobject]@{
        Code   = $proc.ExitCode
        Stdout = (Get-Content $stdout -Raw) -as [string]
        Stderr = (Get-Content $stderr -Raw) -as [string]
    }
}

$failures = 0
$cases = @(
    # Loops that finish after --settle used to crash; they must complete and
    # report the sum.
    @{ Name = 'empty 1M (1ms settle)';   Js = 'var s=0;for(var i=0;i<1000000;i++) s++;probeExit("s="+s);'; Exit = 0; Want = 's=1000000'; Settle = 1 }
    @{ Name = 'empty 1M (100ms settle)'; Js = 'var s=0;for(var i=0;i<1000000;i++) s++;probeExit("s="+s);'; Exit = 0; Want = 's=1000000'; Settle = 100 }
    @{ Name = 'empty 5M';                Js = 'var s=0;for(var i=0;i<5000000;i++) s++;probeExit("s="+s);'; Exit = 0; Want = 's=5000000'; Settle = 100 }
    @{ Name = 'dynamic key 200k';        Js = 'var o={a:1},s=0,k="a";for(var i=0;i<200000;i++) s+=o[k];probeExit("s="+s);'; Exit = 0; Want = 's=200000'; Settle = 100 }
    @{ Name = 'static key 600k';         Js = 'var o={a:1,b:2},s=0;for(var i=0;i<600000;i++) s+=o.a+o.b;probeExit("s="+s);'; Exit = 0; Want = 's=1800000'; Settle = 100 }
)

foreach ($case in $cases) {
    $codes = @()
    $last  = $null

    for ($i = 0; $i -lt $Repeat; $i++) {
        $last = Invoke-Probe -Script $case.Js -Wait 300 -Settle $case.Settle -Timeout 60000
        $codes += $last.Code
    }

    $bad = @($codes | Where-Object { $_ -ne $case.Exit })

    if ($bad.Count -gt 0) {
        $failures++
        $hex = ($bad | ForEach-Object { if ($_ -lt 0) { '0x{0:X8}' -f $_ } else { "$_" } }) -join ','
        Write-Host ("FAIL  {0}: exit {1} (wanted {2})" -f $case.Name, $hex, $case.Exit)
        if ($last.Stderr) { Write-Host (($last.Stderr -split "`n" | Select-Object -Last 6) -join "`n") }
    } elseif ($last.Stdout -notmatch [regex]::Escape($case.Want)) {
        $failures++
        Write-Host ("FAIL  {0}: exit ok but payload missing '{1}'" -f $case.Name, $case.Want)
    } else {
        Write-Host ("PASS  {0} ({1} runs)" -f $case.Name, $Repeat)
    }
}

# A --timeout that fires inside the loop destroys the view while the engine is
# executing the script. That must be a clean teardown: exit code 2..4, never
# 0xC0000005. (Without the command pin this was 6/6 AVs.)
$codes = @()
for ($i = 0; $i -lt $Repeat; $i++) {
    $r = Invoke-Probe -Script 'var s=0;for(var i=0;i<5000000;i++) s++;' -Wait 300 -Settle 5000 -Timeout 450
    $codes += $r.Code
}

if (@($codes | Where-Object { $_ -notin 2, 3, 4 }).Count -gt 0) {
    $failures++
    $hex = ($codes | ForEach-Object { if ($_ -lt 0) { '0x{0:X8}' -f $_ } else { "$_" } }) -join ','
    Write-Host ("FAIL  teardown during eval: exit {0} (want 2..4, no AV)" -f $hex)
} else {
    Write-Host ("PASS  teardown during eval ({0} runs, exit {1})" -f $Repeat, ($codes -join ','))
}

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

if ($failures -gt 0) {
    Write-Host "`nJIT LOOP CHECKS FAILED ($failures)"
    exit 1
}

Write-Host "`nALL JIT LOOP CHECKS PASSED"
exit 0
