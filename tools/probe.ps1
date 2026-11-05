<#
.SYNOPSIS
  Run agave_shell in headless probe mode (one command, no quoting traps).

.DESCRIPTION
  agave_shell parses its own command line and treats " and \ as syntax, so a
  --eval script holding JS string literals or a regexp arrives mangled. Pass
  the script with -ScriptFile (preferred) and it is handed over as a file.

  Exit code is the authoritative verdict (see docs/dev-debugging.md):
    0 OK, 1 usage, 2 load timeout, 3 no [exit] after load,
    4 eval ran but no [exit], 5 navigation failed, 6 empty document.

.EXAMPLE
  pwsh -File tools/probe.ps1 -Url https://chat.deepseek.com/sign_in `
       -ScriptFile debug/wxlayout.js -Wait 4000

.EXAMPLE
  pwsh -File tools/probe.ps1 -Url file:///E:/mybrowsers/agave/tools/pages/cap_dom.html `
       -Script "probeExit(document.title)"
#>
param(
    [Parameter(Mandatory = $true)][string]$Url,
    [string]$Script,
    [string]$ScriptFile,
    [int]$Wait = 3000,
    [int]$Settle = 1000,
    [int]$Timeout = 60000,
    [int]$Reload = 0,
    [string]$UserData,
    [switch]$Offline,
    [switch]$Show
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build-zig-native\agave_shell.exe'
if (-not (Test-Path $exe)) {
    throw "missing $exe - run tools/build_project.ps1 first"
}
if ($Script -and $ScriptFile) {
    throw '-Script and -ScriptFile are mutually exclusive'
}

$cliArgs = @('--probe', $Url)
if ($ScriptFile) {
    if (-not (Test-Path $ScriptFile)) { throw "missing script file $ScriptFile" }
    $cliArgs += @('--eval-file', (Resolve-Path $ScriptFile).Path)
} elseif ($Script) {
    $cliArgs += @('--eval', $Script)
}
$cliArgs += @('--wait', "$Wait", '--settle', "$Settle", '--timeout', "$Timeout")
if ($Reload -gt 0) { $cliArgs += @('--reload', "$Reload") }
if ($UserData) { $cliArgs += @('--userdata', $UserData) }
if ($Offline) { $cliArgs += '--offline' }

if ($Show) { $env:AGB_PROBE_SHOW = '1' }
else { Remove-Item Env:AGB_PROBE_SHOW -ErrorAction SilentlyContinue }

Push-Location (Split-Path -Parent $exe)
try {
    & $exe @cliArgs
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
Write-Host "[probe.ps1] exit code $code"
exit $code
