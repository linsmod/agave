# tools/run_shell_ui_tests.ps1
#
# Regression for the browser shell's chrome (design-shell-ui.md):
#   * light and dark renders of the offscreen DIB, written as PNG;
#   * layout/hit-test/dpi assertions reported by `--ui-shot`;
#   * every `ui-check` line and the aggregate `UI_SHOT PASS/FAIL`.
#
# No window, no engine, no profile and no desktop interaction: `--ui-shot`
# builds a DIB, paints the same UiChrome the GUI uses and exits. The two theme
# runs go in parallel; expected wall time < 3 s.
#
# Usage:
#   pwsh -File tools/run_shell_ui_tests.ps1
#   pwsh -File tools/run_shell_ui_tests.ps1 -BuildDir build-zig-native

param(
    [string]$BuildDir = 'build-zig-native'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path (Join-Path $root $BuildDir) 'agave_shell.exe'

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking
$run = New-TestRun -Suite 'shell-ui'

$failures = 0
$cases = @(
    [pscustomobject]@{ Name = 'light'; Theme = 'light'; Dpi = 96;  Png = (Join-Path $run.Dir 'ui-light.png');
                       Out = ''; Err = ''; Proc = $null; Started = $null; Timeout = $false },
    [pscustomobject]@{ Name = 'dark';  Theme = 'dark';  Dpi = 96;  Png = (Join-Path $run.Dir 'ui-dark.png');
                       Out = ''; Err = ''; Proc = $null; Started = $null; Timeout = $false },
    [pscustomobject]@{ Name = 'light-150'; Theme = 'light'; Dpi = 144; Png = (Join-Path $run.Dir 'ui-light-150.png');
                       Out = ''; Err = ''; Proc = $null; Started = $null; Timeout = $false }
)

# The mock table proves nothing in the chrome render depends on the developer's
# real profile/registry; --ui-theme has already selected the palette.
$savedOps = $env:AGB_SHELL_OPS
$env:AGB_SHELL_OPS = 'mock'

foreach ($case in $cases) {
    $case.Out = Join-Path $run.Dir ("ui-{0}.out.txt" -f $case.Name)
    $case.Err = Join-Path $run.Dir ("ui-{0}.err.txt" -f $case.Name)
    $case.Started = [Diagnostics.Stopwatch]::StartNew()
    $case.Proc = Start-Process -FilePath $exe -NoNewWindow -PassThru `
        -WorkingDirectory $run.Dir `
        -ArgumentList ("--ui-shot `"{0}`" --ui-theme {1} --ui-dpi {2} --ui-size 1100x760" -f `
            $case.Png, $case.Theme, $case.Dpi) `
        -RedirectStandardOutput $case.Out `
        -RedirectStandardError $case.Err
}

try {
    foreach ($case in $cases) {
        # An --ui-shot run is over in ~0.3 s; the other theme's process may
        # already be gone by the time we get here, and Wait-Process throws
        # "process not found" for that (not a timeout).
        $case.Proc.Refresh()
        if (-not $case.Proc.HasExited) {
            try {
                Wait-Process -Id $case.Proc.Id -Timeout 30 -ErrorAction Stop
            } catch {
                $case.Proc.Refresh()
        if (-not $case.Proc.HasExited) {
                    try { Stop-Process -Id $case.Proc.Id -Force -ErrorAction SilentlyContinue } catch { }
                    $case.Timeout = $true
                }
            }
        }
        $case.Proc.Refresh()
        $case.Started.Stop()
    }
} finally {
    if ($null -eq $savedOps) { Remove-Item Env:AGB_SHELL_OPS -ErrorAction SilentlyContinue }
    else { $env:AGB_SHELL_OPS = $savedOps }
}

foreach ($case in $cases) {
    $stdout = if (Test-Path $case.Out) { Get-Content $case.Out -Raw } else { '' }
    $stderr = if (Test-Path $case.Err) { Get-Content $case.Err -Raw } else { '' }
    $code = if ($case.Timeout) { -1 } else { $case.Proc.ExitCode }
    $passed = $false
    $checks = 0

    if ($code -eq 0 -and $stdout -match 'UI_SHOT PASS checks=(\d+) fails=(\d+)') {
        $checks = [int]$Matches[1]
        $passed = ([int]$Matches[2] -eq 0) -and ($stdout -notmatch 'ui-check .* ok=0')
    }

    if (Test-Path $case.Png) {
        $size = (Get-Item $case.Png).Length
        if ($size -lt 5000) {
            $passed = $false
            Write-Host ("FAIL  {0}: PNG too small ({1} bytes)" -f $case.Name, $size)
        }
    } else {
        $passed = $false
        Write-Host ("FAIL  {0}: PNG missing" -f $case.Name)
    }

    if ($passed) {
        Write-Host ("PASS  shell-ui/{0} ({1} checks, {2:N0} ms, {3:N1} KB)" -f `
            $case.Name, $checks, $case.Started.Elapsed.TotalMilliseconds,
            ((Get-Item $case.Png).Length / 1024.0))
    } else {
        ++$failures
        Write-Host ("FAIL  shell-ui/{0}: exit={1} timeout={2}" -f `
            $case.Name, $code, [bool]$case.Timeout)
        if ($stdout) {
            ($stdout -split "`n" | Where-Object { $_ -match 'ui-check|UI_SHOT|ui-part' } |
                Select-Object -Last 12) | ForEach-Object { Write-Host ("      " + $_.TrimEnd()) }
        }
        if ($stderr -and $stderr.Trim().Length -gt 0) {
            Write-Host ("      stderr: " + (($stderr -split "`n" | Select-Object -First 3) -join ' '))
        }
    }
}

Remove-TestRun $run $failures

if ($failures -ne 0) {
    Write-Host ("SHELL UI CHECKS FAILED ({0})" -f $failures)
    exit 1
}

Write-Host "ALL SHELL UI CHECKS PASSED"
exit 0
