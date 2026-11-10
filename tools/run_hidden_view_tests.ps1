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

$tmp = Join-Path ([IO.Path]::GetTempPath()) ('agb-hidden-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

$failures = 0

function Invoke-Probe {
    param(
        [int]$Wait = 2600,
        [int]$Settle = 300,
        [string]$Js,
        [string[]]$Extra = @()
    )

    $stdout = Join-Path $tmp 'out.txt'
    $stderr = Join-Path $tmp 'err.txt'
    $script = Join-Path $tmp ('js-' + [guid]::NewGuid().ToString('N') + '.js')

    if ($Js) { Set-Content -Path $script -Value $Js -Encoding ascii }

    $old = $env:AGB_LOOP_DEBUG
    $env:AGB_LOOP_DEBUG = '1'

    try {
        $cmdArgs = @('--probe', "file:///$($page -replace '\\','/')", '--wait', "$Wait",
                     '--settle', "$Settle", '--timeout', '30000')
        if ($Js) { $cmdArgs += @('--eval-file', $script) }
        if ($Extra.Count -gt 0) { $cmdArgs += $Extra }

        $p = Start-Process -FilePath $exe -NoNewWindow -PassThru -Wait `
             -ArgumentList $cmdArgs `
             -RedirectStandardOutput $stdout -RedirectStandardError $stderr

        $text = (Get-Content $stdout -Raw) -as [string]
        $errText = (Get-Content $stderr -Raw) -as [string]

        # [loop] view=<ptr> paints=<n> skips=<m>: the per-view accounting.
        $points = @()
        foreach ($m in [regex]::Matches($errText, '\[loop\] view=\S+ paints=(\d+) skips=(\d+)')) {
            $points += [pscustomobject]@{ Paints = [int]$m.Groups[1].Value; Skips = [int]$m.Groups[2].Value }
        }

        [pscustomobject]@{
            Code   = $p.ExitCode
            Stdout = $text
            Stderr = $errText
            Points = $points
        }
    } finally {
        if ($null -eq $old) { Remove-Item Env:AGB_LOOP_DEBUG -ErrorAction SilentlyContinue }
        else { $env:AGB_LOOP_DEBUG = $old }
    }
}

# 1. hidden view: JS keeps running, paint stops
$hidden = Invoke-Probe -Js 'probeExit("ticks=" + window.__ticks + " hidden=" + (+!!document.hidden) + " seen=" + window.__hiddenSeen);' -Extra @('--hide-after', '300')

$ticks = if ($hidden.Stdout -match 'ticks=(\d+)') { [int]$Matches[1] } else { -1 }
$first = $hidden.Points | Select-Object -First 1
$last = $hidden.Points | Select-Object -Last 1

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
        Write-Host ("      ticks={0} code={1} points={2}" -f $ticks, $hidden.Code, ($hidden.Points.Count))
        Write-Host (($hidden.Stdout -split "`n" | Select-String -Pattern 'ticks=|exit code' | Select-Object -Last 2) -join "`n")
    }
}

# 2. control: a visible view keeps painting
$visible = Invoke-Probe -Js 'probeExit("ticks=" + window.__ticks);'
$first = $visible.Points | Select-Object -First 1
$last = $visible.Points | Select-Object -Last 1

if ($visible.Code -eq 0 -and $null -ne $first -and $null -ne $last -and $last.Paints -gt $first.Paints -and $last.Skips -eq 0) {
    Write-Host "PASS  visible view: paints keep growing, skips stay 0"
} else {
    $failures++
    Write-Host "FAIL  visible view: expected growing paints and no skips"
    Write-Host ("      code={0} first={1} last={2}" -f $visible.Code,
                ($(if ($first) { "$($first.Paints)/$($first.Skips)" } else { 'none' })),
                ($(if ($last) { "$($last.Paints)/$($last.Skips)" } else { 'none' })))
}

# 3. hide -> show: the changed content is on screen after coming back.
#    No eval here: the probe's shot path runs after --settle, and a probeExit()
#    would close the window before the capture. The capture path is written
#    next to the build output (the probe cannot fopen() a file under %TEMP%).
$shot = Join-Path $build 'hidden_view_shot.png'
Remove-Item $shot -ErrorAction SilentlyContinue
$shown = Invoke-Probe -Wait 1500 -Settle 800 -Js $null `
    -Extra @('--hide-after', '400', '--show-after', '900', '--shot', $shot)

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

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
Remove-Item $shot -ErrorAction SilentlyContinue
if ($failures -gt 0) {
    Write-Host "`nHIDDEN VIEW CHECKS FAILED ($failures)"
    exit 1
}

Write-Host "`nALL HIDDEN VIEW CHECKS PASSED"
exit 0
