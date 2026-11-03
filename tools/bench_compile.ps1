# tools/bench_compile.ps1
#
# A/B benchmark for script compile cost. Runs the headless probe on a page that
# loads a local bundle and reports wall time + CPU time of the process, so a
# compiler change can be compared on identical input without network noise.
#
# Usage:
#   pwsh -File tools\bench_compile.ps1 -Url file:///e:/mybrowsers/agave/debug/compile_bench.html -Runs 3

param(
    [Parameter(Mandatory = $true)][string]$Url,
    [int]$Runs = 3,
    [int]$Wait = 0,          # --wait ms after load before eval
    [int]$Settle = 1000,
    [string]$Shell = "$PSScriptRoot\..\build-zig-native\agave_shell.exe",
    [string]$ErrLog = "$PSScriptRoot\..\debug\bench_err.txt"
)

$ErrorActionPreference = 'Stop'

$times = @()
$cpus = @()

for ($i = 0; $i -lt $Runs; $i++) {
    Get-Process agave_shell -ErrorAction SilentlyContinue | Stop-Process -Force
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $Shell `
            -ArgumentList '--probe', $Url, '--eval', 'probeExit(1)', '--wait', "$Wait", '--settle', "$Settle", '--timeout', '120000' `
            -NoNewWindow -PassThru -RedirectStandardError $ErrLog
    $p.WaitForExit()
    $sw.Stop()
    $p.Refresh()
    $times += $sw.Elapsed.TotalSeconds
    $cpus += $p.TotalProcessorTime.TotalSeconds
}

$avgWall = [math]::Round(($times | Measure-Object -Average).Average, 3)
$avgCpu = [math]::Round(($cpus | Measure-Object -Average).Average, 3)
Write-Output ("wall avg={0}s [{1}]  cpu avg={2}s [{3}]" -f $avgWall, (($times | ForEach-Object { [math]::Round($_, 3) }) -join ', '),
    $avgCpu, (($cpus | ForEach-Object { [math]::Round($_, 3) }) -join ', '))
