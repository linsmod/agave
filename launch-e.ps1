# launch-e.ps1 - start agave_shell with the error-snippet switch on.
#
# AGB_ERR_SNIPPET makes the engine print
#   [snippet] line=2 col=<offset> msg=<message> | <source around the throw>
# for engine-thrown errors (ant src/errors.c), which is how you locate an error
# that a page swallows into console.error without a stack.
#
# Usage:
#   pwsh -File launch-e.ps1                                  # GUI, deepseek
#   pwsh -File launch-e.ps1 -Url file:///e:/x/page.html      # GUI, any page
#   pwsh -File launch-e.ps1 -Probe                           # headless probe + log
#   pwsh -File launch-e.ps1 -Probe -Url https://x/ -Wait 12000
#
# GUI mode keeps stderr on this console so [snippet] lines appear live; probe
# mode writes stderr to debug\shell_err.txt and prints the snippet lines.

param(
    [string]$Url = 'https://chat.deepseek.com/',
    [switch]$Probe,
    [string]$Eval = 'probeExit(1)',
    [int]$Wait = 8000,
    [int]$Settle = 6000,
    [switch]$CssDebug,
    [switch]$NoSnippet
)

$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
if (-not $root) { $root = Split-Path -Parent $MyInvocation.MyCommand.Path }
$exe = Join-Path $root 'build-zig-native\agave_shell.exe'

if (-not (Test-Path $exe)) {
    throw "agave_shell.exe not found at $exe (build it first: pwsh -File tools\build_project.ps1)"
}

# Only this process tree sees these; run the script in a fresh shell if you want
# the caller's environment untouched.
if (-not $NoSnippet) { $env:AGB_ERR_SNIPPET = '1' }
if ($CssDebug) { $env:AGB_CSS_DEBUG = '1' }

if ($Probe) {
    $log = Join-Path $root 'debug\shell_err.txt'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $log) | Out-Null

    & $exe --probe $Url --eval $Eval --wait $Wait --settle $Settle --timeout 90000 > $log
    "exit=$LASTEXITCODE  stderr -> $log"

    $hits = Select-String -Path $log -Pattern '\[snippet' -ErrorAction SilentlyContinue
    if ($hits) {
        '--- snippets ---'
        $hits | ForEach-Object { $_.Line }
    } else {
        'no [snippet] lines (nothing engine-thrown matched, or no error at all)'
    }
} else {
    $outLog = Join-Path $root 'launch.txt'
    $errLog = Join-Path $root 'launch.err.txt'
    $p = Start-Process -FilePath $exe -ArgumentList $Url `
         -RedirectStandardOutput $outLog `
         -RedirectStandardError  $errLog `
         -NoNewWindow -PassThru
    $p.WaitForExit()
    "exit=$($p.ExitCode)"
    "stdout -> $outLog"
    "stderr -> $errLog"
}
