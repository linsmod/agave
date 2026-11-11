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
# Execution model (2026-11-10): every suite runs **at the same time** (default
# = 16, or the CPU count when lower), each as its own pwsh process. Inside a
# suite the probes also run together (tools/TestParallel.psm1): every case gets
# its own AGB_USERDATA profile, cwd (agb_crash.txt), AGB_DIAG_DIR
# (agb_watchdog.txt / agb_busy_*.txt) and AGB_CLIPBOARD_FILE (a mock clipboard,
# so copy cases no longer race on Win32). Cross-process probe concurrency is
# bounded by AGB_TEST_MAX_PROBES (default 32) via a running-process count.
#
# There is no serial phase any more. The four suites that used to need one now
# isolate the resource instead of the machine:
#
#   run_hang_recovery_tests   per-case diag dir + cwd; only its own crash files
#                             are asserted, so another suite's crash is invisible
#   run_process_backend_tests per-case cwd/diag/clipboard and one aggregate
#                             "no orphaned renderer" check; latency limits are
#                             load-tolerant
#   run_image_tests           relative timing (sliced vs a one-shot control
#                             captured in the same wave), two waves so the timed
#                             probes do not run next to ten more decoders
#   run_selection_tests       mock clipboard file per case; the OS clipboard is
#                             neither read nor written
#
# Every suite prints its own PASS/FAIL line; this script's exit code is the
# authority (0 = all green). Per-suite wall times are printed: the table in
# docs/dev-tests.md §1 is a **single-suite** baseline, so a full parallel run
# inflates them (AGENTS rule 9 compares the per-suite line, or run -Only).
# -SlowSeconds is only a sanity cap (a suite that never finishes).
#
# Usage:
#   pwsh -File tools/run_all_tests.ps1
#   pwsh -File tools/run_all_tests.ps1 -Parallel 8
#   pwsh -File tools/run_all_tests.ps1 -Sequential        # suites one at a time (debug)
#   pwsh -File tools/run_all_tests.ps1 -Only process      # name substring
#   pwsh -File tools/run_all_tests.ps1 -Skip run_layout_tests,run_media_tests
#   pwsh -File tools/run_all_tests.ps1 -IncludeNetwork -IncludeReal
#
# Expected duration: docs/dev-tests.md §1 (23 suites ≈ 25-30 s wall clock on a
# 16-core machine; -Sequential runs the suites one at a time, ≈2 minutes).

param(
    [string]$Only = '',
    [string[]]$Skip = @(),
    [switch]$IncludeNetwork,
    [switch]$IncludeReal,
    [int]$SlowSeconds = 180,
    [int]$Parallel = 0,
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
    'run_singleton_tests',
    'run_nav_tests',
    'run_hidden_view_tests',
    'run_realm_tests',
    'run_script_mode_tests',
    'run_binding_semantics_tests',
    'run_shadow_tests',
    'run_webp_tests',
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
    'run_selection_tests',
    'run_media_tests'
)

if ($IncludeNetwork) { $suites += 'run_site_smoke' }
if ($IncludeReal)    { $suites += 'run_media_real_tests' }

$selected = @($suites | Where-Object {
    (-not $Only -or $_ -like "*$Only*") -and ($Skip -notcontains $_)
})
$parallelList = $selected

if ($Sequential) { $Parallel = 1 }
if ($Parallel -le 0) { $Parallel = [math]::Min(16, [Environment]::ProcessorCount) }
$Parallel = [math]::Max(1, [math]::Min(32, $Parallel))

# Bound the probes inside all suites together (TestParallel.psm1 reads this).
if (-not $env:AGB_TEST_MAX_PROBES) { $env:AGB_TEST_MAX_PROBES = '24' }

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

    if ($code -eq 0) {
        Write-Host ("PASS  {0,-30} {1,7}s" -f $job.Name, $seconds)
    } else {
        $script:failed++
        Write-Host ("FAIL  {0,-30} {1,7}s  exit={2}" -f $job.Name, $seconds, $code)
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
Write-Host ("{0} suite(s), {1}s total, {2} failed (parallel={3}, probes<={4})" -f $results.Count,
            [math]::Round($total.Elapsed.TotalSeconds, 1), $failed,
            $(if ($Sequential) { 1 } else { $Parallel }), $env:AGB_TEST_MAX_PROBES)

if ($failed -gt 0) {
    Write-Host "ALL SUITES: FAILED"
    exit 1
}

Write-Host "ALL SUITES PASSED"
exit 0
