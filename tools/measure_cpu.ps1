# tools/measure_cpu.ps1
#
# Measures how much CPU the headless probe burns on a page. Starts
# agave_shell --probe, lets the page settle, then samples process CPU time over
# a fixed window and prints the percentage of one core.
#
# Usage:
#   pwsh -File tools\measure_cpu.ps1 -Url https://chat.deepseek.com/ -Wait 20000
#   pwsh -File tools\measure_cpu.ps1 -Url file:///e:/mybrowsers/agave/debug/idle.html -Wait 8000 -Window 5

param(
    [Parameter(Mandatory = $true)][string]$Url,
    [int]$Wait = 15000,      # --wait passed to the probe (ms)
    [int]$Settle = 0,        # --settle window (ms), 0 = omit
    [int]$Warmup = 6,        # seconds before the first CPU reading
    [int]$Window = 5,        # seconds between readings
    [string]$Shell = "$PSScriptRoot\..\build-zig-native\agave_shell.exe",
    [string]$ErrLog = "$PSScriptRoot\..\debug\cpu_err.txt"
)

$ErrorActionPreference = 'Stop'

$argList = @('--probe', $Url, '--wait', "$Wait", '--timeout', '120000')
if ($Settle -gt 0) { $argList += @('--settle', "$Settle") }

$p = Start-Process -FilePath $Shell -ArgumentList $argList -NoNewWindow -PassThru `
        -RedirectStandardError $ErrLog

Start-Sleep -Seconds $Warmup
$p.Refresh()
if ($p.HasExited) {
    Write-Output "exited before first sample (code $($p.ExitCode))"
    return
}

$c1 = $p.TotalProcessorTime.TotalSeconds
Start-Sleep -Seconds $Window
$p.Refresh()
if ($p.HasExited) {
    Write-Output "exited during window (code $($p.ExitCode))"
    return
}
$c2 = $p.TotalProcessorTime.TotalSeconds

$pct = [math]::Round((($c2 - $c1) / $Window) * 100, 1)
$threads = $p.Threads.Count
$ws = [math]::Round($p.WorkingSet64 / 1MB, 1)
Write-Output ("cpu={0}% threads={1} ws={2}MB stderr={3}" -f $pct, $threads, $ws, $ErrLog)

$p.Kill()
