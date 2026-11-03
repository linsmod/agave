# tools/ant-patches/apply-browser-rejection-policy.ps1
#
# Ant follows Node's semantics for unhandled promise rejections: with no
# `unhandledRejection` listener installed, report_unhandled_rejection() prints
# the error and calls process_exit_with(EXIT_FAILURE) (src/ant.c). For a script
# runner that is right; for a browser it means one unhandled page promise closes
# the whole application - which is exactly what happened with browserscore.dev
# ("Error: network error: undefined" -> exit code 1).
#
# The policy is therefore configured at build time, not through an environment
# variable: gc_state_init() (src/gc/gc.c) sets uncaught_nonfatal on every ant_t
# instance, including realms created later in the session.
#
# Usage:  pwsh -File tools\ant-patches\apply-browser-rejection-policy.ps1 [-AntRoot H:\ant-v16] [-Build]
param(
    [string]$AntRoot = 'H:\ant-v16',
    [switch]$Build
)

$ErrorActionPreference = 'Stop'
$gc = Join-Path $AntRoot 'src\gc\gc.c'
if (-not (Test-Path $gc)) { throw "not found: $gc" }

$text = [IO.File]::ReadAllText($gc)
if ($text -notmatch 'js->uncaught_nonfatal = true') {
    $anchor = "void gc_state_init(ant_t *js) {"
    if ($text.IndexOf($anchor) -lt 0) { throw 'gc_state_init not found' }
    $addition = @"
void gc_state_init(ant_t *js) {
  /* Browser embedding: log and report unhandled promise rejections, never
   * terminate the process (Node's default, which Ant follows upstream). */
  js->uncaught_nonfatal = true;
"@
    $text = $text.Replace($anchor, $addition.Replace("`r`n", "`n"))
    [IO.File]::WriteAllText($gc, $text)
    Write-Host 'gc_state_init: uncaught_nonfatal defaults to true'
} else {
    Write-Host 'already configured'
}

if ($Build) {
    $env:PATH = "C:\msys64\usr\bin;C:\msys64\mingw64\bin;" + $env:PATH
    & 'C:\msys64\mingw64\bin\ninja.exe' -C (Join-Path $AntRoot 'build-zig') libant.a
    if ($LASTEXITCODE -ne 0) { throw "libant build failed ($LASTEXITCODE)" }
    Write-Host 'libant.a rebuilt - now run tools\build_antbridge.ps1 -Force'
}
