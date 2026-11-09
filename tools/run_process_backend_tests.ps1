# tools/run_process_backend_tests.ps1
#
# The `process` render backend (S3 B2): one agave_render_host.exe per view, the
# protocol over a named shared section (src/webview_control/AgwViewIpc.h).
#
# Every case runs the probe with AGB_VIEW_BACKEND=process and asserts the same
# things a local view answers, so the boundary cannot silently degrade:
#
#   1. load + title + timers   (page JS runs in the renderer; its title and
#                               progress cross as events)
#   2. paint                   (--shot: the canvas in shared memory lands in the
#                               control's mirror)
#   3. input                   (--keys goes control -> ring -> renderer -> DOM)
#   4. renderer crash          (renderer dies -> control reports the load error,
#                               probe exits 5 - not a hang)
#   5. default backend         (no env: the local backend still runs the page)
#
# After every case no agave_render_host.exe may be left behind.
#
# Usage:
#   pwsh -File tools/run_process_backend_tests.ps1

param(
    [string]$BuildDir = 'build-zig-native'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir
$exe = Join-Path $build 'agave_shell.exe'
$renderer = Join-Path $build 'agave_render_host.exe'
$pixelCheck = Join-Path $root 'tools/pixel_check.py'
$pages = Join-Path $root 'tools/pages'

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }
if (-not (Test-Path $renderer)) { throw "agave_render_host.exe not found: $renderer" }

$failures = 0

function Invoke-Probe {
    param(
        [string]$Page,
        [string]$Js,
        [int]$Wait = 1500,
        [int]$Settle = 300,
        [string[]]$Extra = @(),
        [string]$Backend = 'process'
    )

    $out = Join-Path $build 'process_backend_out.txt'
    $err = Join-Path $build 'process_backend_err.txt'
    $script = Join-Path $build 'process_backend_eval.js'

    if ($Js) { Set-Content -Path $script -Value $Js -Encoding ascii }

    $saved = $env:AGB_VIEW_BACKEND
    if ($Backend) { $env:AGB_VIEW_BACKEND = $Backend }
    else { Remove-Item Env:AGB_VIEW_BACKEND -ErrorAction SilentlyContinue }

    try {
        $args = @('--probe', "file:///$($Page -replace '\\','/')",
                  '--wait', "$Wait", '--settle', "$Settle", '--timeout', '25000')
        if ($Js) { $args += @('--eval-file', $script) }
        if ($Extra.Count -gt 0) { $args += $Extra }

        $p = Start-Process -FilePath $exe -NoNewWindow -PassThru -Wait `
            -ArgumentList $args -RedirectStandardOutput $out -RedirectStandardError $err

        [pscustomobject]@{
            Code   = $p.ExitCode
            Stdout = (Get-Content $out -Raw) -as [string]
            Stderr = (Get-Content $err -Raw) -as [string]
        }
    } finally {
        if ($null -eq $saved) { Remove-Item Env:AGB_VIEW_BACKEND -ErrorAction SilentlyContinue }
        else { $env:AGB_VIEW_BACKEND = $saved }
    }
}

function Assert-NoRenderer {
    param([string]$What)

    $left = @(Get-Process agave_render_host -ErrorAction SilentlyContinue)
    if ($left.Count -gt 0) {
        $failures++
        Write-Host ("FAIL  {0}: {1} renderer process(es) left behind" -f $What, $left.Count)
        $left | Stop-Process -Force
        return $false
    }
    return $true
}

# 1. load + title + timers
$load = Invoke-Probe -Page (Join-Path $pages 'cap_hidden_view.html') `
    -Js 'probeExit("title=" + document.title + " ticks=" + window.__ticks);'
$ticks = if ($load.Stdout -match 'ticks=(\d+)') { [int]$Matches[1] } else { -1 }

if ($load.Code -eq 0 -and $load.Stdout -match 'title=hidden view' -and $ticks -ge 5) {
    Write-Host ("PASS  process backend: page runs in the renderer (title crossed, ticks={0})" -f $ticks)
} else {
    $failures++
    Write-Host ("FAIL  process backend: load/title/timers (exit {0})" -f $load.Code)
    Write-Host (($load.Stdout -split "`n" | Select-Object -Last 3) -join "`n")
}
Assert-NoRenderer 'load/title/timers' | Out-Null

# 2. paint: the blue box in the shared canvas reaches the mirror
$shot = Join-Path $build 'process_backend_shot.bmp'
Remove-Item $shot -ErrorAction SilentlyContinue
$paint = Invoke-Probe -Page (Join-Path $pages 'cap_hidden_view.html') -Wait 1500 -Settle 400 `
    -Extra @('--shot', $shot)

if ($paint.Code -eq 0 -and (Test-Path $shot)) {
    $pixels = & python $pixelCheck $shot '150,120,1478c8' --tol 12 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "PASS  process backend: frames cross the boundary (canvas -> mirror)"
    } else {
        $failures++
        Write-Host "FAIL  process backend: frame pixels wrong"
        Write-Host (($pixels -split "`n" | Select-Object -Last 4) -join "`n")
    }
} else {
    $failures++
    Write-Host ("FAIL  process backend: shot (exit {0}, file={1})" -f $paint.Code, (Test-Path $shot))
}
Remove-Item $shot -ErrorAction SilentlyContinue
Assert-NoRenderer 'paint' | Out-Null

# 3. input: keys go control -> ring -> renderer -> DOM
$input = Invoke-Probe -Page (Join-Path $pages 'cap_typing.html') -Wait 800 -Settle 300 `
    -Js 'probeExit("value=" + document.getElementById("text").value);' `
    -Extra @('--keys', 'ab1')

if ($input.Code -eq 0 -and $input.Stdout -match 'value=ab1') {
    Write-Host "PASS  process backend: keyboard input reaches the renderer DOM"
} else {
    $failures++
    Write-Host ("FAIL  process backend: input (exit {0})" -f $input.Code)
    Write-Host (($input.Stdout -split "`n" | Select-Object -Last 2) -join "`n")
}
Assert-NoRenderer 'input' | Out-Null

# 4. crash: the renderer dies, the control reports an error instead of hanging
$env:AGB_RENDER_CRASH_MS = '400'
try {
    $crash = Invoke-Probe -Page (Join-Path $pages 'cap_hidden_view.html') -Wait 1200 -Settle 200 `
        -Js 'probeExit("alive");'
} finally {
    Remove-Item Env:AGB_RENDER_CRASH_MS -ErrorAction SilentlyContinue
}

if ($crash.Code -eq 5 -and ($crash.Stdout + $crash.Stderr) -match 'load error code=42') {
    Write-Host "PASS  process backend: renderer crash is reported as a load error (exit 5)"
} else {
    $failures++
    Write-Host ("FAIL  process backend: crash handling (exit {0})" -f $crash.Code)
    Write-Host (($crash.Stdout -split "`n" | Select-Object -Last 3) -join "`n")
}
Assert-NoRenderer 'crash' | Out-Null

# 5. default backend: the local path still runs the same page
$local = Invoke-Probe -Page (Join-Path $pages 'cap_hidden_view.html') `
    -Js 'probeExit("title=" + document.title);' -Backend $null

if ($local.Code -eq 0 -and $local.Stdout -match 'title=hidden view') {
    Write-Host "PASS  default backend: local still runs the page"
} else {
    $failures++
    Write-Host ("FAIL  default backend: local run (exit {0})" -f $local.Code)
}

Remove-Item (Join-Path $build 'process_backend_out.txt'),
            (Join-Path $build 'process_backend_err.txt'),
            (Join-Path $build 'process_backend_eval.js') -ErrorAction SilentlyContinue

if ($failures -gt 0) {
    Write-Host "`nPROCESS BACKEND CHECKS FAILED ($failures)"
    exit 1
}

Write-Host "`nALL PROCESS BACKEND CHECKS PASSED"
exit 0
