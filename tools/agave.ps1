# tools/agave.ps1 - start the GUI browser shell.
#
# Defaults to the `process` render backend (S3): every tab's engine runs in its
# own agave_render_host.exe, so a stuck page no longer freezes the window. The
# shell itself still defaults to `local`; this launcher is what makes process
# mode the easy path.
#
# Usage:
#   pwsh -File tools/agave.ps1                       # process backend, blank tab
#   pwsh -File tools/agave.ps1 https://chat.deepseek.com/
#   pwsh -File tools/agave.ps1 -Local                # one engine thread (old path)
#   pwsh -File tools/agave.ps1 -DryRun -Url x        # print the command, do not run
#   pwsh -File tools/agave.ps1 -Help

param(
    [string]$BuildDir = 'build-zig-native',
    [string]$Backend = 'process',
    # Position 0 so a bare URL works; every other parameter is named-only.
    [Parameter(Position = 0)][string]$Url = '',
    [switch]$Local,
    [switch]$DryRun,
    [switch]$Help,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest = @()
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir
$exe = Join-Path $build 'agave_shell.exe'
$renderer = Join-Path $build 'agave_render_host.exe'

if ($Help) {
    Write-Host 'agave.ps1 - start the GUI shell'
    Write-Host ''
    Write-Host '  -BuildDir <dir>   where agave_shell.exe lives (default build-zig-native)'
    Write-Host '  -Backend <name>   local | process (default process)'
    Write-Host '  -Local            same as -Backend local'
    Write-Host '  -Url <url>        page to open (or pass it as the first spare argument)'
    Write-Host '  -DryRun           print the command line instead of starting it'
    Write-Host ''
    Write-Host '  `process` needs agave_render_host.exe next to agave_shell.exe'
    exit 0
}

if ($Local) { $Backend = 'local' }

if (-not (Test-Path $exe)) {
    Write-Host ("missing {0} - build first: pwsh -File tools/build_project.ps1" -f $exe)
    exit 1
}

if ($Backend -notin @('local', 'process')) {
    Write-Host ("unknown backend '{0}' (local|process)" -f $Backend)
    exit 2
}

if ($Backend -eq 'process' -and -not (Test-Path $renderer)) {
    Write-Host ("no agave_render_host.exe in {0} - falling back to the local backend" -f $build)
    $Backend = 'local'
}

# Not $args: that is PowerShell's automatic argument array (a previous script
# in this repo lost its arguments to exactly that).
$shellArgs = @('--backend', $Backend)
if ($Url) { $shellArgs += $Url }
if ($Rest.Count -gt 0) { $shellArgs += $Rest }

if ($DryRun) {
    Write-Host ("{0} {1}" -f $exe, ($shellArgs -join ' '))
    exit 0
}

Write-Host ("starting {0} (backend {1})" -f $exe, $Backend)
Start-Process -FilePath $exe -ArgumentList $shellArgs
exit 0
