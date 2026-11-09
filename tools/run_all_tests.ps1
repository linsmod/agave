# tools/run_all_tests.ps1
#
# The whole offline regression batch in one command: every tools/run_*.ps1 that
# needs no network, no external workspace and no manual setup (run_layout_tests
# launches its own headless Chrome; the local http servers some suites start are
# part of their script).
#
# Excluded by default, because they need something the machine may not have:
#   run_site_smoke.ps1       real sites (network)            -IncludeNetwork
#   run_media_real_tests.ps1 ffmpeg + remote samples         -IncludeReal
#   run_c3.ps1               external workspace + 8899 port  (never; not a suite)
#
# Execution model (2026-11-09): suites run **in parallel** (default 4 at a time),
# each as its own pwsh process with its own AGB_USERDATA profile under
# %TEMP%\agb-suites-<id>\profile-<suite>. That keeps their cookie/cache stores
# from racing; the runners only share the build tree. A few suites cannot share
# the machine and are queued afterwards one at a time (see $serialOrder):
#
#   run_hang_recovery_tests   asserts that *no* agb_crash.txt / agb_watchdog.txt
#                             appears; another suite that intentionally crashes a
#                             renderer (process backend) would trip it
#   run_process_backend_tests crashes renderers on purpose (crash report case) and
#                             measures command latency, so it wants the machine
#   run_image_tests           asserts paint/rAF time limits (90/70 ms); parallel
#                             load makes those numbers meaningless
#   run_media_real_tests      ffmpeg decode + audio device, single-instance
#
# Every suite prints its own PASS/FAIL line; this script's exit code is the
# authority (0 = all green). Per-suite wall times are printed: the soft baseline
# table in docs/dev-tests.md §1 is measured **serially**, so a parallel run
# inflates them — compare with -Parallel 1 when a number looks off (AGENTS rule 9).
# -SlowSeconds is only a sanity cap (a suite that never finishes).
#
# Usage:
#   pwsh -File tools/run_all_tests.ps1
#   pwsh -File tools/run_all_tests.ps1 -Parallel 8
#   pwsh -File tools/run_all_tests.ps1 -Sequential        # old one-at-a-time order
#   pwsh -File tools/run_all_tests.ps1 -Only process      # name substring
#   pwsh -File tools/run_all_tests.ps1 -Skip run_layout_tests,run_media_tests
#   pwsh -File tools/run_all_tests.ps1 -IncludeNetwork -IncludeReal
#
# Expected duration: docs/dev-tests.md §1 (18 suites ≈ 4 minutes with the default
# parallelism; ≈6 minutes with -Sequential, on a 16-core machine).

param(
    [string]$Only = '',
    [string[]]$Skip = @(),
    [switch]$IncludeNetwork,
    [switch]$IncludeReal,
    [int]$SlowSeconds = 180,
    [int]$Parallel = 4,
    [switch]$Sequential,
    [switch]$KeepLogs
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $root 'tools'

# Broad/cheap first is less important now that they overlap; keep the list as
# the documentation of "what runs".
$suites = @(
    'run_process_backend_tests',
    'run_window_tests',
    'run_nav_tests',
    'run_hidden_view_tests',
    'run_realm_tests',
    'run_script_mode_tests',
    'run_tab_lifecycle_tests',
    'run_hang_recovery_tests',
    'run_inspector_tests',
    'run_jit_loop_tests',
    'run_typing_tests',
    'run_sticky_tests',
    'run_xhr_tests',
    'run_encoding_tests',
    'run_worker_tests',
    'run_image_tests',
    'run_layout_tests',
    'run_media_tests'
)

if ($IncludeNetwork) { $suites += 'run_site_smoke' }
if ($IncludeReal)    { $suites += 'run_media_real_tests' }

# Suites with a hard conflict (shared crash/watchdog files) or timing
# assertions. They run after the parallel phase, one at a time, in this order.
$serialOrder = @(
    'run_hang_recovery_tests',
    'run_process_backend_tests',
    'run_image_tests'
)
if ($IncludeReal) { $serialOrder += 'run_media_real_tests' }

$selected = @($suites | Where-Object {
    (-not $Only -or $_ -like "*$Only*") -and ($Skip -notcontains $_)
})
$serial = @($serialOrder | Where-Object { $selected -contains $_ })
$parallelList = @($selected | Where-Object { $serial -notcontains $_ })

if ($Sequential) { $Parallel = 1 }
$Parallel = [math]::Max(1, [math]::Min(16, $Parallel))

$runRoot = Join-Path ([IO.Path]::GetTempPath()) ('agb-suites-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null

$results = New-Object System.Collections.ArrayList
$total = [Diagnostics.Stopwatch]::StartNew()
$failed = 0
$savedUserData = $env:AGB_USERDATA

function Start-Suite([string]$name) {
    $script = Join-Path $tools "$name.ps1"
    if (-not (Test-Path $script)) {
        Write-Host ("SKIP  {0} (script missing)" -f $name)
        return $null
    }

    $profile = Join-Path $runRoot ("profile-" + $name)
    New-Item -ItemType Directory -Force -Path $profile | Out-Null
    $out = Join-Path $runRoot ("$name.out.txt")
    $err = Join-Path $runRoot ("$name.err.txt")

    # Start-Process copies the current environment into the child: give this
    # suite its own browser profile so parallel suites do not share cookies/cache.
    $env:AGB_USERDATA = $profile
    $proc = Start-Process -FilePath 'pwsh' -NoNewWindow -PassThru `
        -ArgumentList @('-NoProfile', '-File', ('"' + $script + '"')) `
        -RedirectStandardOutput $out -RedirectStandardError $err

    return [pscustomobject]@{
        Name    = $name
        Proc    = $proc
        Out     = $out
        Err     = $err
        Started = [Diagnostics.Stopwatch]::StartNew()
        Serial  = $false
    }
}

function Complete-Suite($job) {
    $job.Started.Stop()
    $seconds = [math]::Round($job.Started.Elapsed.TotalSeconds, 1)
    $code = 0

    try { $code = $job.Proc.ExitCode } catch { $code = -1 }

    $text = ''
    foreach ($path in @($job.Out, $job.Err)) {
        if (Test-Path $path) { $text += ((Get-Content $path -Raw) -as [string]) }
    }

    $mark = if ($job.Serial) { '  (serial)' } else { '' }
    if ($code -eq 0) {
        Write-Host ("PASS  {0,-30} {1,7}s{2}" -f $job.Name, $seconds, $mark)
    } else {
        $script:failed++
        Write-Host ("FAIL  {0,-30} {1,7}s  exit={2}{3}" -f $job.Name, $seconds, $code, $mark)
        @($text -split "`n" | Where-Object { $_ -match '^FAIL|FAILED|LAYOUT=FAIL' } |
            Select-Object -Last 4) | ForEach-Object { "      $($_.TrimEnd())" }
        if (-not $KeepLogs) {
            Write-Host ("      log: {0}" -f $job.Out)
        }
    }

    if ($seconds -gt $SlowSeconds) {
        Write-Host ("WARN  {0} took {1}s (over {2}s): check why before ignoring" -f $job.Name, $seconds, $SlowSeconds)
    }

    $results.Add([pscustomobject]@{ Name = $job.Name; Seconds = $seconds; Code = $code }) | Out-Null

    if ($code -ne 0) { return $false }
    return $true
}

try {
    # ---- parallel phase -------------------------------------------------
    $pending = [System.Collections.Generic.Queue[string]]::new()
    $parallelList | ForEach-Object { $pending.Enqueue($_) }
    $running = @()

    while ($pending.Count -gt 0 -or $running.Count -gt 0) {
        while ($running.Count -lt $Parallel -and $pending.Count -gt 0) {
            $name = $pending.Dequeue()
            $job = Start-Suite $name
            if ($job) { $running += $job }
        }

        $done = @($running | Where-Object { $_.Proc.HasExited })
        if ($done.Count -eq 0) {
            if ($running.Count -eq 0) { break }
            Start-Sleep -Milliseconds 200
            continue
        }

        foreach ($job in $done) {
            $running = @($running | Where-Object { $_ -ne $job })
            [void](Complete-Suite $job)
        }
    }

    # ---- serial phase (conflicts / timing-sensitive) --------------------
    foreach ($name in $serial) {
        if ($selected -notcontains $name) { continue }
        $job = Start-Suite $name
        if (-not $job) { continue }
        $job.Serial = $true
        $job.Proc.WaitForExit()
        [void](Complete-Suite $job)
    }
} finally {
    $env:AGB_USERDATA = $savedUserData

    # Profiles are throwaway; logs only stay for failures (or with -KeepLogs).
    $kept = @{}
    if ($KeepLogs -or $failed -gt 0) {
        $results | Where-Object { $_.Code -ne 0 } | ForEach-Object {
            $kept[$_.Name] = $true
        }
    }

    Get-ChildItem $runRoot -Force -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.Name -like 'profile-*') {
            Remove-Item $_.FullName -Recurse -Force -ErrorAction SilentlyContinue
        } elseif (-not $KeepLogs) {
            $suiteName = ($_.Name -replace '\.(out|err)\.txt$', '')
            if (-not $kept.ContainsKey($suiteName)) {
                Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue
            }
        }
    }

    if ($KeepLogs -or $failed -gt 0) {
        Write-Host ("logs kept under {0}" -f $runRoot)
    } else {
        Remove-Item $runRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$total.Stop()

Write-Host ""
Write-Host "slowest first:"
$results | Sort-Object Seconds -Descending | Select-Object -First 5 | ForEach-Object {
    Write-Host ("  {0,-30} {1,7}s" -f $_.Name, $_.Seconds)
}

Write-Host ""
if ($serial.Count -gt 0) {
    Write-Host ("serial: {0}" -f ($serial -join ', '))
}
Write-Host ("{0} suite(s), {1}s total, {2} failed (parallel={3})" -f $results.Count,
            [math]::Round($total.Elapsed.TotalSeconds, 1), $failed,
            $(if ($Sequential) { 1 } else { $Parallel }))

if ($failed -gt 0) {
    Write-Host "ALL SUITES: FAILED"
    exit 1
}

Write-Host "ALL SUITES PASSED"
exit 0
