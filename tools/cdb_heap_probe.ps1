# tools/cdb_heap_probe.ps1
#
# Run a probe URL under cdb *from process start* (the only mode where the NT
# debug heap validates freed blocks) and, the moment the heap reports
#
#   HEAP[agave_shell.exe]: HEAP: Free Heap block 0000... modified at 0000... after it was freed
#
# dump that block and its would-be vtable.  The heap's complaint names the
# block; the faulting stack only names the allocation that happened to notice
# it, so this script closes the gap that tools/cdb_stack.ps1 cannot: it shows
# WHAT the freed block was (vtable -> class via `ln`, strings, pointers).
#
# cdb prints the address to a pipe we own, so the dump is issued live: the
# debugger is sitting at the break instruction when the message appears.
#
# Usage:
#   pwsh -File tools/cdb_heap_probe.ps1 -Url https://v.qq.com/ -Wait 60000
#   pwsh -File tools/cdb_heap_probe.ps1 -Url <url> -Timeout 60000 -Bytes 0xA0
#
# Exit: 0 when a block was found and dumped, 3 when the run ended without the
# heap message (no corruption this time), 1 on launch errors, 2 on timeout.

param(
    [Parameter(Mandatory = $true)][string]$Url,
    [string]$Script = '',
    [string]$ScriptFile = '',
    [int]$Wait = 3000,
    [int]$Settle = 1000,
    [int]$Timeout = 60000,
    [int]$Bytes = 0x80,
    [string]$Log = ''
)

$ErrorActionPreference = 'Stop'

$root    = Split-Path -Parent $PSScriptRoot
$cdb     = Join-Path $root 'tools\cdb.exe'
$exe     = Join-Path $root 'build-zig-native\agave_shell.exe'
$symDirs = @((Join-Path $root 'build-zig-native'), 'H:\ant-v16\build-zig')
$symPath = ($symDirs -join ';')

if (-not (Test-Path $cdb)) { throw "cdb not found: $cdb" }
if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }
if (-not $Log) {
    $Log = Join-Path $root ('debug\cdb_heap_{0}.log' -f (Get-Date -Format 'yyyyMMdd_HHmmss'))
}
$Log = [IO.Path]::GetFullPath($Log)

$probeArgs = @('--probe', $Url, '--wait', "$Wait", '--settle', "$Settle", '--timeout', "$Timeout")
if ($ScriptFile) { $probeArgs += @('--eval-file', $ScriptFile) }
elseif ($Script) { $probeArgs += @('--eval', $Script) }

function Quote-Arg([string]$a) {
    if ($a -match '[\s"]') { '"' + ($a -replace '"', '\"') + '"' } else { $a }
}

$cdbArgs = @(
    '-y', (Quote-Arg $symPath),
    '-c', (Quote-Arg 'sxe 80000003; g'),
    (Quote-Arg $exe)
) + ($probeArgs | ForEach-Object { Quote-Arg $_ })

$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName               = $cdb
$psi.Arguments              = ($cdbArgs -join ' ')
$psi.UseShellExecute        = $false
$psi.RedirectStandardInput  = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError  = $true
$psi.CreateNoWindow         = $true

$proc  = [Diagnostics.Process]::Start($psi)
$lines = New-Object System.Collections.ArrayList
$lock  = New-Object object
$found = $false
$block = $null
$cdbIn = $proc.StandardInput

function Add-Lines([string]$text) {
    if (-not $text) { return }
    [void]$lines.Add($text)
    Write-Host $text
}

# NOTE: no BeginOutputReadLine/DataReceived events here. Their handlers run on
# a thread-pool thread that has no PowerShell runspace, which kills the script
# ("There is no Runspace available"). Poll the async reads from this thread
# instead: one outstanding ReadLineAsync per stream, consumed when complete.
$outTask = $proc.StandardOutput.ReadLineAsync()
$errTask = $proc.StandardError.ReadLineAsync()
$outDone = $false
$errDone = $false

$deadline = [DateTime]::UtcNow.AddSeconds(($Timeout / 1000.0) + 120)
$dumpIssued = $false

while (-not $proc.HasExited -and [DateTime]::UtcNow -lt $deadline) {
    if (-not $outDone -and $outTask.IsCompleted) {
        $line = $outTask.Result
        if ($null -eq $line) { $outDone = $true }
        else {
            Add-Lines $line
            $outTask = $proc.StandardOutput.ReadLineAsync()
        }
    }
    if (-not $errDone -and $errTask.IsCompleted) {
        $line = $errTask.Result
        if ($null -eq $line) { $errDone = $true }
        else {
            Add-Lines $line
            $errTask = $proc.StandardError.ReadLineAsync()
        }
    }
    Start-Sleep -Milliseconds 30

    if ($dumpIssued) { continue }

    # The heap message carries both addresses: the freed block and the byte
    # inside it that changed.
    $hit = $null
    [System.Threading.Monitor]::Enter($lock)
    try {
        foreach ($l in $lines) {
            $m = [regex]::Match($l, 'Free Heap block\s+([0-9A-Fa-f`]+)\s+modified at\s+([0-9A-Fa-f`]+)')
            if ($m.Success) { $hit = $m; break }
        }
    } finally { [System.Threading.Monitor]::Exit($lock) }

    if (-not $hit) { continue }

    $blockAddr = $hit.Groups[1].Value -replace '`', ''
    $modAddr   = $hit.Groups[2].Value -replace '`', ''
    Write-Host ("[heap] freed block={0} modified-at={1} offset=0x{2:X}" -f
        $blockAddr, $modAddr, ([Convert]::ToUInt64($modAddr, 16) - [Convert]::ToUInt64($blockAddr, 16)))

    $block = $blockAddr
    $found = $true
    $dumpIssued = $true

    # Stop other threads first so the dump is not racing a live writer, then
    # dump the block and try to resolve its first qword as a vtable.
    $cmds = @(
        '.echo ===BLOCKDUMP===',
        ("db {0} L 0x{1:X}" -f $blockAddr, $Bytes),
        ("dq {0} L 0x10" -f $blockAddr),
        ("dps {0} L 0x20" -f $blockAddr),
        ("ln poi({0})" -f $blockAddr),
        'kb 40',
        '.echo ===END===',
        'q'
    )
    foreach ($c in $cmds) { $cdbIn.WriteLine($c) }
    $cdbIn.Flush()
}

if (-not $proc.HasExited) {
    Start-Sleep -Seconds 2
    if (-not $proc.HasExited) { try { $proc.Kill() } catch { } }
}

[System.Threading.Monitor]::Enter($lock)
try { $lines | Set-Content -Path $Log -Encoding utf8 } finally { [System.Threading.Monitor]::Exit($lock) }

Write-Host "log: $Log"
if ($found) {
    Write-Host "heap corruption block: $block"
    exit 0
}
Write-Host "no heap-corruption message in this run"
exit 3

