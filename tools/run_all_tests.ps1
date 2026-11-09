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
# Every suite prints its own PASS/FAIL line; this script's exit code is the
# authority (0 = all green). Durations are printed per suite and summarised:
# compare them with the soft baseline table in docs/dev-tests.md §1 and report
# in the conversation when a suite is more than 30 s over it (AGENTS rule 9).
# -SlowSeconds is only a sanity cap (a suite that never finishes); it is not the
# baseline.
#
# Usage:
#   pwsh -File tools/run_all_tests.ps1
#   pwsh -File tools/run_all_tests.ps1 -Only process        # name substring
#   pwsh -File tools/run_all_tests.ps1 -Skip run_layout_tests,run_media_tests
#   pwsh -File tools/run_all_tests.ps1 -IncludeNetwork -IncludeReal
#
# Expected duration: docs/dev-tests.md §1 has the per-suite soft baseline
# (18 suites ≈ 360 s on a 16-core machine).

param(
    [string]$Only = '',
    [string[]]$Skip = @(),
    [switch]$IncludeNetwork,
    [switch]$IncludeReal,
    [int]$SlowSeconds = 180
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $root 'tools'

# Fixed order: cheap and broad first, so a broken build fails before the slow
# suites (layout, media) spend a minute each.
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

$results = @()
$total = [Diagnostics.Stopwatch]::StartNew()
$failed = 0

foreach ($name in $suites) {
    if ($Only -and $name -notlike "*$Only*") { continue }
    if ($Skip -contains $name) { continue }

    $script = Join-Path $tools "$name.ps1"
    if (-not (Test-Path $script)) {
        Write-Host ("SKIP  {0} (script missing)" -f $name)
        continue
    }

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $out = & pwsh -NoProfile -File $script 2>&1
    $code = $LASTEXITCODE
    $sw.Stop()

    $seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    $tail = @($out | Where-Object { "$_" -match 'FAIL|PASSED|FAILED|PASS|=' } | Select-Object -Last 1)

    if ($code -eq 0) {
        Write-Host ("PASS  {0,-30} {1,7}s" -f $name, $seconds)
    } else {
        $failed++
        Write-Host ("FAIL  {0,-30} {1,7}s  exit={2}" -f $name, $seconds, $code)
        ($out | Where-Object { "$_" -match '^FAIL|FAILED|LAYOUT=FAIL' } | Select-Object -Last 4) | ForEach-Object { "      $_" }
    }

    if ($seconds -gt $SlowSeconds) {
        Write-Host ("WARN  {0} took {1}s (over {2}s): check why before ignoring" -f $name, $seconds, $SlowSeconds)
    }

    $results += [pscustomobject]@{ Name = $name; Seconds = $seconds; Code = $code }
}

$total.Stop()

Write-Host ""
Write-Host "slowest first:"
$results | Sort-Object Seconds -Descending | Select-Object -First 5 | ForEach-Object {
    Write-Host ("  {0,-30} {1,7}s" -f $_.Name, $_.Seconds)
}

Write-Host ""
Write-Host ("{0} suite(s), {1}s total, {2} failed" -f $results.Count,
            [math]::Round($total.Elapsed.TotalSeconds, 1), $failed)

if ($failed -gt 0) {
    Write-Host "ALL SUITES: FAILED"
    exit 1
}

Write-Host "ALL SUITES PASSED"
exit 0
