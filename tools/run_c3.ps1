# Runs the local css3test mirror in the headless host and prints the probe output.
# Usage: pwsh tools\run_c3.ps1 [extra-url]
param([string]$Url = 'http://127.0.0.1:8899/c3local/index.html')

$ws = 'C:\Users\linswin\Documents\deepseek-harness\default-workspace'
$run = Join-Path $ws 'host\nativerun'

Copy-Item 'E:\mybrowsers\agave\src\javascript\ant_backend\polyfills.js' $run -Force

Get-Process agave_view_host -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$env:AGB_MAX_FRAMES = '30000'
$out = & (Join-Path $run 'agave_view_host.exe') $Url 'probe.bmp' 2>&1
Remove-Item Env:\AGB_MAX_FRAMES

$out | Select-String -Pattern '\[probe\]|\[agave\]|\[antjs\]' |
    Select-Object -First 12 |
    ForEach-Object { $_.Line.Substring(0, [Math]::Min(200, $_.Line.Length)) }
