# tools/build_project.ps1
#
# One-command build of agave with the zig toolchain: the configuration behind
# build-zig-native (native WinHTTP network backend + Ant JS engine + the Win32
# shell/embed demos).
#
# It sets up the same environment the manual build needs (msys2 ninja/ar on
# PATH, zig wrapped as cc/cxx) so nobody has to remember the six environment
# variables:
#
#   $env:PATH   = "C:\msys64\mingw64\bin;" + $env:PATH
#   $env:CC     = ...\zig-cc.cmd      (zig cc  -target x86_64-windows-gnu)
#   $env:CXX    = ...\zig-cxx.cmd     (zig c++ -target x86_64-windows-gnu)
#   $env:ASM    = ...\zig-cc.cmd
#   $env:AR     = C:/msys64/mingw64/bin/ar.exe
#   $env:RANLIB = C:/msys64/mingw64/bin/ranlib.exe
#   cmake --build build-zig-native --parallel 16
#
# Usage:
#   pwsh -File tools/build_project.ps1                 # incremental
#   pwsh -File tools/build_project.ps1 -Clean          # rebuild every target
#   pwsh -File tools/build_project.ps1 -Fresh          # wipe build dir, configure, build
#   pwsh -File tools/build_project.ps1 -Deploy $run    # ... and copy artifacts into $run
#
# -Fresh refuses to terminate anything by default: if a program started from the
# build dir (e.g. agave_shell, which is a real browser window) holds the files,
# the script lists it and stops. Add -StopLockingProcesses to let it kill them.
#
# Targets built: libagave.dll / libagave_webview.dll / agave_shell.exe /
# agave_embed.exe / agave_view_host.exe (headless test host) / touchweb.exe
# (built when OPT_TOUCHWEB was ON at configure time; this script never flips
# cached options - do that by hand with -Fresh if you really need to).
#
# Typical output:
#   build ok   1320/1320 targets in 120.1s (0 errors, 915 warnings)
#   libagave.dll        12476416 bytes  2026-11-03 01:04:06
#   ...
#
# The full compiler log is written to <repo>\build-native.log (git-ignored) and
# the tail is printed when the build fails.

param(
    [string]$BuildDir = 'build-zig-native',
    [switch]$Clean,                 # --clean-first: recompile everything, keep the cache
    [switch]$Fresh,                 # delete the build dir and configure from scratch
    [int]$Jobs = 0,                 # 0 = number of logical processors
    [string]$ZigPath = '',          # zig.exe; default: $env:AGB_ZIG, winget, PATH
    [string]$MinGWRoot = '',        # default: C:\msys64\mingw64
    [string]$Deploy = '',           # copy libagave*.dll / agave_*.exe here afterwards
    [switch]$StopLockingProcesses,  # -Fresh: stop processes running from the build dir if they block the delete
    [string]$Log = '',              # default: <repo>\build-native.log
    [switch]$NoLog                  # do not write a log file, just stream
)

$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot           # agave repo
$build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
$log   = if ($Log) { $Log } else { Join-Path $root 'build-native.log' }

$artifacts = @('libagave.dll', 'libagave_webview.dll', 'agave_shell.exe', 'agave_embed.exe',
               'agave_view_host.exe', 'touchweb.exe', 'polyfills.js')

function Fail([string]$message) {
    Write-Host "build failed: $message" -ForegroundColor Red
    exit 1
}

function Section([string]$text) {
    Write-Host ''
    Write-Host "== $text" -ForegroundColor Cyan
}

# ---------------------------------------------------------------- toolchain --

Section 'toolchain'

if (-not $ZigPath) {
    $ZigPath = $env:AGB_ZIG
}
if (-not $ZigPath) {
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages\zig.zig_Microsoft.Winget.Source_8wekyb3d8bbwe\zig-x86_64-windows-0.16.0\zig.exe'),
        'C:\zig\zig.exe',
        # Last resort: the winget Links shim is a symlink; some sandboxed hosts
        # (agent shells with untrusted-reparse-point / signature policies) refuse
        # to execute it even though it works fine in a normal terminal.
        (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\zig.exe')
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $ZigPath = $c; break }
    }
}
if (-not $ZigPath) {
    $found = Get-Command zig.exe -ErrorAction SilentlyContinue
    if ($found) { $ZigPath = $found.Source }
}
if (-not $ZigPath -or -not (Test-Path $ZigPath)) {
    Fail "zig.exe not found. Pass -ZigPath, set `$env:AGB_ZIG, or put zig on PATH."
}

if (-not $MinGWRoot) {
    $MinGWRoot = $env:AGB_MINGW
}
if (-not $MinGWRoot) {
    $MinGWRoot = 'C:\msys64\mingw64'
}
$mingwBin = Join-Path $MinGWRoot 'bin'
foreach ($tool in @('cmake.exe', 'ninja.exe', 'ar.exe', 'ranlib.exe')) {
    if (-not (Test-Path (Join-Path $mingwBin $tool))) {
        Fail "$tool not found in $mingwBin (pass -MinGWRoot, or install msys2 mingw64)."
    }
}

$zigVersion = (& $ZigPath version) 2>&1
Write-Host ("zig      : {0} ({1})" -f $ZigPath, $zigVersion)
Write-Host ("mingw    : {0}" -f $MinGWRoot)

# The cache is the source of truth for the compiler wrappers once the tree has
# been configured: pointing CMake at a different path would force a reconfigure
# (and a different toolchain fingerprint) for no reason.
$cc = ''; $cxx = ''
$cacheFile = Join-Path $build 'CMakeCache.txt'
if ((Test-Path $cacheFile) -and -not $Fresh) {
    foreach ($line in Get-Content $cacheFile) {
        if ($line -match '^CMAKE_C_COMPILER:(?:FILEPATH|STRING|PATH)=(.+)$')   { $cc  = $Matches[1].Trim() }
        if ($line -match '^CMAKE_CXX_COMPILER:(?:FILEPATH|STRING|PATH)=(.+)$') { $cxx = $Matches[1].Trim() }
    }
}
if (-not $cc -or -not (Test-Path $cc) -or -not $cxx -or -not (Test-Path $cxx)) {
    # Generate the wrappers the engine is known to build with (see
    # agave-zig-build-notes.md): plain `zig cc/c++ -target x86_64-windows-gnu`.
    # They live outside the build dir so -Fresh (which deletes that dir) cannot
    # pull the toolchain out from under the configure step.
    $toolDir = Join-Path $root '.zig-toolchain'
    New-Item -ItemType Directory -Force -Path $toolDir | Out-Null
    $cc  = Join-Path $toolDir 'zig-cc.cmd'
    $cxx = Join-Path $toolDir 'zig-cxx.cmd'
    $extra = if ($env:AGB_ZIG_FLAGS) { ' ' + $env:AGB_ZIG_FLAGS } else { '' }
    Set-Content -Path $cc  -Encoding ASCII -Value "@echo off`r`n`"$ZigPath`" cc  -target x86_64-windows-gnu$extra %*"
    Set-Content -Path $cxx -Encoding ASCII -Value "@echo off`r`n`"$ZigPath`" c++ -target x86_64-windows-gnu$extra %*"
}

$env:PATH   = "$mingwBin;" + $env:PATH
$env:CC     = $cc
$env:CXX    = $cxx
$env:ASM    = $cc
$env:AR     = (Join-Path $mingwBin 'ar.exe') -replace '\\', '/'
$env:RANLIB = (Join-Path $mingwBin 'ranlib.exe') -replace '\\', '/'

if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

Section 'configure'

if ($Fresh -and (Test-Path $build)) {
    Write-Host "removing $build"

    # A program started from the build dir (agave_shell is a real browser window,
    # not a test fixture) keeps its images mapped and Remove-Item then dies half
    # way through, leaving a broken tree. Never terminate anything on our own:
    # report what is holding the files and let the caller decide.
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        try {
            Remove-Item -Recurse -Force $build -ErrorAction Stop
            break
        } catch {
            $locked = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
                $_.Path -and $_.Path.StartsWith($build, [StringComparison]::OrdinalIgnoreCase)
            })

            if ($attempt -ge 3 -or -not $StopLockingProcesses) {
                $names = if ($locked.Count -gt 0) {
                    ($locked | ForEach-Object { "$($_.ProcessName) (pid $($_.Id))" }) -join ', '
                } else { 'a program outside this build dir' }
                $msg = "cannot delete $build - in use by: $names." + [Environment]::NewLine +
                       "Close it and retry, or pass -StopLockingProcesses to let the script stop them."
                Fail $msg
            }

            Write-Host ("stopping {0} running from the build dir (-StopLockingProcesses)" -f (($locked | ForEach-Object { $_.ProcessName }) -join ', '))
            $locked | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
            Start-Sleep -Milliseconds 500
        }
    }
    if (Test-Path $build) { Fail "cannot delete $build" }
}

# Reconfigure only when the cache is missing. Never flip cached options here:
# toggling one (e.g. OPT_TOUCHWEB) re-adds/removes whole target sets and makes
# ninja recompile everything under them.
$mustConfigure = $Fresh -or -not (Test-Path $cacheFile)
if ($mustConfigure) {
    $cmakeArgs = @(
        '-S', $root,
        '-B', $build,
        '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Debug',
        "-DCMAKE_MAKE_PROGRAM=$((Join-Path $mingwBin 'ninja.exe') -replace '\\','/')",
        "-DCMAKE_C_COMPILER=$cc",
        "-DCMAKE_CXX_COMPILER=$cxx",
        "-DCMAKE_AR=$env:AR",
        "-DCMAKE_RANLIB=$env:RANLIB",
        '-DOPT_NET_BACKEND=native',
        '-DOPT_JS_ANT=ON',
        '-DOPT_SHELL=ON'
    )
    Write-Host ("cmake {0}" -f ($cmakeArgs -join ' '))
    & (Join-Path $mingwBin 'cmake.exe') @cmakeArgs
    if ($LASTEXITCODE -ne 0) { Fail "cmake configure exited with $LASTEXITCODE" }
} else {
    Write-Host "reusing $build (pass -Fresh to wipe it)"
}

# -------------------------------------------------------------------- build --

Section 'build'

# Programs started from the build dir keep libagave*.dll and the exes mapped, so
# the relink that follows any engine change fails with "Permission denied".
# Never terminate them (agave_shell may be someone's browser window) - just say
# who is in the way before ninja does.
$runningFromBuild = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
    $_.Path -and $_.Path.StartsWith($build, [StringComparison]::OrdinalIgnoreCase)
})
if ($runningFromBuild.Count -gt 0) {
    $who = ($runningFromBuild | ForEach-Object { "$($_.ProcessName) (pid $($_.Id))" }) -join ', '
    Write-Host "note: running from the build dir: $who" -ForegroundColor Yellow
    Write-Host "      the relink will fail while they are open; close them if ninja reports 'Permission denied'." -ForegroundColor Yellow
}

$buildArgs = @('--build', $build, '--parallel', "$Jobs")
if ($Clean -or $Fresh) { $buildArgs += '--clean-first' }

$startedAt = Get-Date
$sw = [Diagnostics.Stopwatch]::StartNew()

$lines = @()
Push-Location $root
try {
    if ($NoLog) {
        & (Join-Path $mingwBin 'cmake.exe') @buildArgs 2>&1 | ForEach-Object { $lines += "$_"; $_ }
    } else {
        & (Join-Path $mingwBin 'cmake.exe') @buildArgs 2>&1 |
            Tee-Object -FilePath $log |
            ForEach-Object { $lines += "$_" }
    }
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
$sw.Stop()

if ($NoLog) { $null = $lines } else { Write-Host "log      : $log" }

if ($code -ne 0) {
    Write-Host ''
    Write-Host '---- last 40 lines ----' -ForegroundColor Yellow
    $lines | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }
    $errors = @($lines | Where-Object { $_ -match 'error:|error C[0-9]+|FAILED:' })
    if ($lines -match 'Permission denied') {
        Write-Host ''
        Write-Host 'hint: an output file is locked by a running program - close the windows listed above and rerun.' -ForegroundColor Yellow
    }
    Fail ("cmake --build exited with {0}; {1} error line(s)" -f $code, $errors.Count)
}

# --------------------------------------------------------------- reporting --

$warnings = @($lines | Where-Object { $_ -match 'warning:' }).Count
$targets  = 0
foreach ($l in $lines) {
    if ($l -match '\[(\d+)/(\d+)\]') { $targets = [int]$Matches[2] }
}
if ($targets -eq 0) { $targets = $artifacts.Count }

$seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1)
if ($lines -match 'ninja: no work to do') {
    Write-Host ''
    Write-Host ("build ok   nothing to do ({0}s) - sources already built" -f $seconds) -ForegroundColor Green
} else {
    Write-Host ''
    Write-Host ("build ok   {0} targets in {1}s ({2} warnings, 0 errors)" -f $targets, $seconds, $warnings) -ForegroundColor Green
}

Section 'artifacts'
$produced = 0
foreach ($name in $artifacts) {
    $path = Join-Path $build $name
    if (Test-Path $path) {
        $item = Get-Item $path
        Write-Host ("{0,-22} {1,10} bytes  {2}" -f $item.Name, $item.Length, $item.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))
        $produced++
    }
}
if ($produced -eq 0) { Fail "no artifacts in $build - did the build really run?" }

if (-not (Test-Path (Join-Path $build 'libagave.dll'))) {
    Fail "libagave.dll missing from $build"
}

# ------------------------------------------------------------------- deploy --

if ($Deploy) {
    Section "deploy -> $Deploy"
    if (-not (Test-Path $Deploy)) { Fail "deploy target does not exist: $Deploy" }
    foreach ($name in $artifacts) {
        $path = Join-Path $build $name
        if (Test-Path $path) {
            Copy-Item $path $Deploy -Force
            Write-Host "copied $name"
        }
    }
}

exit 0
