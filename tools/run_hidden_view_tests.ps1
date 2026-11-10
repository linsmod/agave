# tools/run_hidden_view_tests.ps1
#
# Hidden views (a background tab, a minimized window) must keep running the page
# but stop laying out and painting (R150, the precondition for S3's per-view
# engine threads).
#
# Three cases against tools/pages/cap_hidden_view.html:
#
#   1. --hide-after: JS/timers keep running (ticks grow, document.hidden says
#      so) while the engine loop stops counting paints and starts counting
#      skips ([loop] view=... paints=N skips=M under AGB_LOOP_DEBUG=1).
#   2. Control run without hiding: paints keep growing, skips stay 0.
#   3. --hide-after + --show-after + --shot: the first frame after the view
#      comes back repaints the whole surface, so a colour change that happened
#      while it was hidden (blue -> green box) is on screen.
#
# The three probes run at once (own profiles). Expected wall time: ~4 s.
#
# Usage:
#   pwsh -File tools/run_hidden_view_tests.ps1
#   pwsh -File tools/run_hidden_view_tests.ps1 -BuildDir build-zig-native

param(
    [string]$BuildDir = 'build-zig-native'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir
$exe = Join-Path $build 'agave_shell.exe'
$page = Join-Path $root 'tools/pages/cap_hidden_view.html'
$pixelCheck = Join-Path $root 'tools/pixel_check.py'

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }
if (-not (Test-Path $page)) { throw "page not found: $page" }

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking
$run = New-TestRun -Suite 'hidden-view'

$failures = 0
$url = "file:///$($page -replace '\\','/')"

function Get-LoopPoints([string]$text) {
    $points = @()
    foreach ($m in [regex]::Matches($text, '\[loop\] view=\S+ paints=(\d+) skips=(\d+)')) {
        $points += [pscustomobject]@{ Paints = [int]$m.Groups[1].Value; Skips = [int]$m.Groups[2].Value }
    }
    return $points
}

# 1. hidden view: JS keeps running, paint stops
$hiddenJob = Start-TestProbe -Run $run -Name 'hidden' -Url $url `
    -Eval 'probeExit("ticks=" + window.__ticks + " hidden=" + (+!!document.hidden) + " seen=" + window.__hiddenSeen);' `
    -Extra @('--hide-after', '300') -Wait 2600 -Settle 300 -Timeout 30000 `
    -Env @{ AGB_LOOP_DEBUG = '1' }

# 2. control: a visible view keeps painting
$visibleJob = Start-TestProbe -Run $run -Name 'visible' -Url $url `
    -Eval 'probeExit("ticks=" + window.__ticks);' `
    -Wait 2600 -Settle 300 -Timeout 30000 `
    -Env @{ AGB_LOOP_DEBUG = '1' }

# 3. hide -> show: the changed content is on screen after coming back.
#    No eval here: the probe's shot path runs after --settle, and a probeExit()
#    would close the window before the capture.
$shotDir = Join-Path $run.Dir 'shot'
New-Item -ItemType Directory -Force -Path $shotDir | Out-Null
$shot = Join-Path $shotDir 'hidden_view.png'
$shownJob = Start-TestProbe -Run $run -Name 'shown' -Url $url `
    -RawArgs @('--probe', $url, '--wait', '1500', '--settle', '800', '--timeout', '30000',
               '--hide-after', '400', '--show-after', '900', '--shot', $shot) `
    -Env @{ AGB_LOOP_DEBUG = '1' }

$hidden = Complete-TestProbe -Job $hiddenJob
$visible = Complete-TestProbe -Job $visibleJob
$shown = Complete-TestProbe -Job $shownJob

$ticks = if ($hidden.Stdout -match 'ticks=(\d+)') { [int]$Matches[1] } else { -1 }
$points = Get-LoopPoints $hidden.Stderr
$first = $points | Select-Object -First 1
$last = $points | Select-Object -Last 1

$checks = @(
    @{ Name = 'hidden view still runs timers'; Want = 'ticks >= 100'; Ok = ($hidden.Code -eq 0 -and $ticks -ge 100) },
    @{ Name = 'document.hidden is true';       Want = 'hidden=1';   Ok = ($hidden.Stdout -match 'hidden=1') },
    @{ Name = 'visibilitychange fired';        Want = 'seen>=1';    Ok = ($hidden.Stdout -match 'seen=[1-9]') },
    @{ Name = 'loop reports paints and skips'; Want = 'paints=/skips='; Ok = ($null -ne $last) },
    @{ Name = 'paint stops while hidden';      Want = 'paints frozen'; Ok = ($null -ne $first -and $null -ne $last -and $last.Paints -eq $first.Paints) },
    @{ Name = 'skip counter grows';            Want = 'skips grow'; Ok = ($null -ne $first -and $null -ne $last -and ($last.Skips - $first.Skips) -ge 100) }
)

foreach ($c in $checks) {
    if ($c.Ok) {
        Write-Host ("PASS  hidden view: {0}" -f $c.Name)
    } else {
        $failures++
        Write-Host ("FAIL  hidden view: {0} (wanted {1})" -f $c.Name, $c.Want)
        Write-Host ("      ticks={0} code={1} points={2}" -f $ticks, $hidden.Code, $points.Count)
    }
}

$vPoints = Get-LoopPoints $visible.Stderr
$vFirst = $vPoints | Select-Object -First 1
$vLast = $vPoints | Select-Object -Last 1
if ($visible.Code -eq 0 -and $null -ne $vFirst -and $null -ne $vLast -and
    $vLast.Paints -gt $vFirst.Paints -and $vLast.Skips -eq 0) {
    Write-Host "PASS  visible view: paints keep growing, skips stay 0"
} else {
    $failures++
    Write-Host "FAIL  visible view: expected growing paints and no skips"
    Write-Host ("      code={0} first={1} last={2}" -f $visible.Code,
                ($(if ($vFirst) { "$($vFirst.Paints)/$($vFirst.Skips)" } else { 'none' })),
                ($(if ($vLast) { "$($vLast.Paints)/$($vLast.Skips)" } else { 'none' })))
}

if ($shown.Code -ne 0 -or -not (Test-Path $shot)) {
    $failures++
    Write-Host ("FAIL  hide/show: probe exit {0} shot={1}" -f $shown.Code, (Test-Path $shot))
} else {
    # rgb(30,180,60) = 1eb43c, inside the 300x200 box at the viewport origin.
    $pixels = & python $pixelCheck $shot '150,120,1eb43c' --tol 12 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "PASS  hide/show: first frame after showing repaints the changed box"
    } else {
        $failures++
        Write-Host "FAIL  hide/show: box is not the colour it changed to while hidden"
        Write-Host (($pixels -split "`n" | Select-Object -Last 4) -join "`n")
    }
}

Remove-TestRun $run $failures
if ($failures -gt 0) {
    Write-Host "`nHIDDEN VIEW CHECKS FAILED ($failures)"
    exit 1
}

Write-Host "`nALL HIDDEN VIEW CHECKS PASSED"
exit 0
