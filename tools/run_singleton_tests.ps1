# tools/run_singleton_tests.ps1
#
# Single-instance shell (Chrome's model): one browser process per profile root;
# a second launch hands its URL to the running shell and exits. These are
# **logic** tests, not UI tests - every case is a normal `--probe` process and
# no window, console or shell process is ever created:
#
#   * profile identity: one hash per normalized profile root, so "X", "X\" and
#     "X\." all name the same instance (and a different dir does not);
#   * hand-off decision: bare URL / --new-tab -> new tab in the running window,
#     --new-window -> another window in the same process, no URL -> activate;
#   * ack handling: ok / "starting" (retry) / error / no answer (fail fast).
#
# The running shell is mocked with files, the same way AGB_CLIPBOARD_FILE mocks
# the clipboard: AGB_PROBE_HANDOFF=1 makes the probe print/write the hand-off
# request and read <mock>\ack.txt instead of talking to a real window. Every
# case is a normal `--probe` process started through tools/TestParallel.psm1:
# no window, no console and no shell process is ever created, and nothing on
# the desktop (a developer's shell, the clipboard) is touched. The real
# WM_COPYDATA/UI path is not covered here - see design-shell-singleton.md §9.
#
# Cases run in two waves through tools/TestParallel.psm1 (per-case
# AGB_USERDATA / cwd / diag / logs, bounded by the shared AGB_TEST_MAX_PROBES
# gate). Expected wall time: ~2-3 s.
#
# Usage:
#   pwsh -File tools/run_singleton_tests.ps1

param(
    [string]$BuildDir = 'build-zig-native'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir
$exe = Join-Path $build 'agave_shell.exe'
$page = Join-Path $root 'tools/pages/window_links.html'
$pageUrl = 'file:///' + ($page -replace '\\', '/')

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }
if (-not (Test-Path $page)) { throw "test page not found: $page" }

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking

$run = New-TestRun -Suite 'singleton'
$failures = 0
$names = New-Object System.Collections.Generic.List[string]
$jobs = @{}
$mockDirs = @{}

$baseEnv = @{ AGB_PROBE_HANDOFF = '1'; AGB_HANDOFF_WAIT_MS = '1500' }

function Add-Case {
    param([string]$Name, [string[]]$RawArgs, [hashtable]$Env)

    $envMap = @{}
    foreach ($key in $baseEnv.Keys) { $envMap[$key] = $baseEnv[$key] }
    foreach ($key in $Env.Keys) { $envMap[$key] = $Env[$key] }

    $script:jobs[$Name] = Start-TestProbe -Run $run -Name $Name -RawArgs $RawArgs `
        -Env $envMap -Timeout 30000
    $script:names.Add($Name)
}

# ---- pure decision cases (no mock needed) ---------------------------------
Add-Case 'op-focus' @('--probe') @{}
Add-Case 'op-url' @('--probe', $pageUrl) @{}
Add-Case 'op-new-tab' @('--probe', $pageUrl, '--new-tab') @{}
Add-Case 'op-new-window' @('--probe', $pageUrl, '--new-window') @{}

# Profile identity: three spellings of one root must hash the same, and an
# unrelated dir must not. AGB_USERDATA is overridden per case on purpose.
$normRoot = Join-Path $run.Dir 'norm-root'
New-Item -ItemType Directory -Force -Path $normRoot | Out-Null
Add-Case 'norm-plain' @('--probe') @{ AGB_USERDATA = $normRoot }
Add-Case 'norm-slash' @('--probe') @{ AGB_USERDATA = ($normRoot + '\') }
Add-Case 'norm-dot' @('--probe') @{ AGB_USERDATA = ($normRoot + '\.') }
Add-Case 'norm-other' @('--probe') @{ AGB_USERDATA = ($normRoot + '-other') }

# Finish this wave before starting the ack cases: the probe gate
# (AGB_TEST_MAX_PROBES) is shared across every suite in the batch, so keep the
# peak this suite occupies small (8 probes here, 5 below).
$wave1End = $names.Count
$results = @{}
$wave1 = @(for ($i = 0; $i -lt $wave1End; $i++) { $names[$i] })
$done1 = Complete-TestProbes -Jobs @($wave1 | ForEach-Object { $jobs[$_] }) -HardTimeoutSeconds 60
for ($i = 0; $i -lt $wave1.Count; $i++) { $results[$wave1[$i]] = $done1[$i] }

# ---- mock shell: ack handling --------------------------------------------
function New-MockDir {
    param([string]$Name)
    $dir = Join-Path $run.Dir ('mock-' + $Name)
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    return $dir
}

# The mock shell answers through files. Write every ack BEFORE the probes start:
# under a loaded batch, spawning 13 probes can take longer than a short wait cap,
# and an ack written afterwards would race the probe's deadline.
$mockDirs['ack-ok'] = New-MockDir 'ack-ok'
$mockDirs['ack-error'] = New-MockDir 'ack-error'
$mockDirs['ack-starting'] = New-MockDir 'ack-starting'
$mockDirs['ack-timeout'] = New-MockDir 'ack-timeout'

[IO.File]::WriteAllText((Join-Path $mockDirs['ack-ok'] 'ack.txt'),
    "state=ok pid=4242 op=new-tab windows=1 target=0x1 tabs=2`n")
[IO.File]::WriteAllText((Join-Path $mockDirs['ack-error'] 'ack.txt'),
    "state=error err=version`n")
[IO.File]::WriteAllText((Join-Path $mockDirs['ack-starting'] 'ack.txt'),
    "state=starting`n")

Add-Case 'ack-ok' @('--probe', $pageUrl) @{
    AGB_USERDATA = (Join-Path $run.Dir 'prof-ack-ok')
    AGB_HANDOFF_MOCK_DIR = $mockDirs['ack-ok']
}

Add-Case 'ack-error' @('--probe', $pageUrl) @{
    AGB_USERDATA = (Join-Path $run.Dir 'prof-ack-error')
    AGB_HANDOFF_MOCK_DIR = $mockDirs['ack-error']
    AGB_HANDOFF_WAIT_MS = '3000'
}

Add-Case 'ack-starting' @('--probe', $pageUrl) @{
    AGB_USERDATA = (Join-Path $run.Dir 'prof-ack-starting')
    AGB_HANDOFF_MOCK_DIR = $mockDirs['ack-starting']
    AGB_HANDOFF_WAIT_MS = '4000'
}

Add-Case 'ack-timeout' @('--probe', $pageUrl) @{
    AGB_USERDATA = (Join-Path $run.Dir 'prof-ack-timeout')
    AGB_HANDOFF_MOCK_DIR = $mockDirs['ack-timeout']
    AGB_HANDOFF_WAIT_MS = '600'
}

# ---- the hook is opt-in: a normal probe must not mention hand-off ----------
$plainEval = Join-Path $run.Dir 'plain-eval.js'
Set-Content -Path $plainEval -Value 'probeExit("ok")' -Encoding utf8
Add-Case 'plain-probe' @('--probe', $pageUrl, '--eval-file', $plainEval,
                         '--wait', '100', '--settle', '50', '--timeout', '20000') @{ AGB_PROBE_HANDOFF = $null }

# "starting" is replaced 250 ms later to exercise the retry (the probes are
# already running; the ones that got started early will see "starting" first).
Start-Sleep -Milliseconds 250
[IO.File]::WriteAllText((Join-Path $mockDirs['ack-starting'] 'ack.txt'),
    "state=ok pid=4243 op=new-tab windows=1 tabs=2`n")

$ordered = @(for ($i = $wave1End; $i -lt $names.Count; $i++) { $names[$i] })
$done2 = Complete-TestProbes -Jobs @($ordered | ForEach-Object { $jobs[$_] }) -HardTimeoutSeconds 60
for ($i = 0; $i -lt $ordered.Count; $i++) { $results[$ordered[$i]] = $done2[$i] }

function Check {
    param([string]$Name, [bool]$Ok, [string]$Detail)

    if ($Ok) {
        Write-Host "PASS  $Name"
    } else {
        $script:failures++
        Write-Host "FAIL  $Name"
        if ($Detail) {
            $Detail -split "`n" | Select-Object -Last 5 | ForEach-Object { Write-Host ("      " + $_.TrimEnd()) }
        }
    }
}

function Get-AckKey {
    param($Result)
    $m = [regex]::Match($Result.Text, '\[handoff\] key=([0-9a-f]{16})')
    if ($m.Success) { return $m.Groups[1].Value }
    return ''
}

function Read-Request {
    param([string]$MockDir)
    $path = Join-Path $MockDir 'request.txt'
    if (-not (Test-Path $path)) { return @{} }
    $map = @{}
    foreach ($line in Get-Content $path) {
        $eq = $line.IndexOf('=')
        if ($eq -gt 0) { $map[$line.Substring(0, $eq)] = $line.Substring($eq + 1) }
    }
    return $map
}

$r = $results['op-focus']
Check 'no URL means "only activate" (op=focus)' `
    ($r.Code -eq 0 -and $r.Text -match 'op=focus' -and $r.Text -match 'url=\(none\)') $r.Text

$r = $results['op-url']
Check 'a bare URL means "new tab"' `
    ($r.Code -eq 0 -and $r.Text -match 'op=new-tab' -and $r.Text -match [regex]::Escape('url=' + $pageUrl)) $r.Text

$r = $results['op-new-tab']
Check '--new-tab means "new tab"' ($r.Code -eq 0 -and $r.Text -match 'op=new-tab') $r.Text

$r = $results['op-new-window']
Check '--new-window means "new window in the same process"' `
    ($r.Code -eq 0 -and $r.Text -match 'op=new-window') $r.Text

# ---- profile identity ------------------------------------------------------
$keyPlain = Get-AckKey $results['norm-plain']
$keySlash = Get-AckKey $results['norm-slash']
$keyDot = Get-AckKey $results['norm-dot']
$keyOther = Get-AckKey $results['norm-other']

Check 'profile key is the same for "X", "X\" and "X\."' `
    ($keyPlain -and $keyPlain -eq $keySlash -and $keyPlain -eq $keyDot) `
    "plain=$keyPlain slash=$keySlash dot=$keyDot"

Check 'a different profile root gets a different key' `
    ($keyPlain -and $keyOther -and $keyPlain -ne $keyOther) "plain=$keyPlain other=$keyOther"

# ---- mock shell: request marshalling + ack handling ------------------------
$r = $results['ack-ok']
$request = Read-Request $mockDirs['ack-ok']
$requestText = (($request.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) | Sort-Object) -join ' '
Check 'ack ok: request marshalled, ack printed, exit 0' `
    ($r.Code -eq 0 -and $r.Stdout -match 'state=ok' -and
     $request['op'] -eq '1' -and $request['opname'] -eq 'new-tab' -and
     $request['url'] -eq $pageUrl -and $request['key'] -match '^[0-9a-f]{16}$') `
    ($r.Text + "`nrequest: " + $requestText)

$r = $results['ack-error']
Check 'ack error: reported, exit 3' ($r.Code -eq 3 -and $r.Text -match 'rejected') $r.Text

$r = $results['ack-starting']
Check 'ack "starting" is retried until the shell answers' `
    ($r.Code -eq 0 -and $r.Stdout -match 'state=ok') $r.Text

$r = $results['ack-timeout']
Check 'no ack fails fast instead of hanging' `
    ($r.Code -eq 3 -and $r.Text -match 'no ack') ("exit=$($r.Code) " + $r.Text)

$r = $results['plain-probe']
Check 'the hook is opt-in: a plain probe never mentions hand-off' `
    ($r.Code -eq 0 -and $r.Text -notmatch '\[handoff\]') $r.Text

Remove-TestRun -Run $run -Failures $failures

Write-Host ''
if ($failures -gt 0) {
    Write-Host ("SINGLETON=FAIL ({0} check(s) failed)" -f $failures)
    exit 1
}

Write-Host 'SINGLETON=PASS'
exit 0
