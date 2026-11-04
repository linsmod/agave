# tools/build_antbridge.ps1
#
# Rebuilds antbridge.dll from src/javascript/ant_backend/AntBridge.c.
#
# The DLL that ships in 3rdparty/ant/bin is a prebuilt binary: agave's CMake only
# copies it. Any change to AntBridge.c therefore needs this script, followed by a
# normal `cmake --build build-zig-native` so libagave relinks against it.
#
# Usage:  pwsh -File tools/build_antbridge.ps1 [-Check] [-Force] [-Restore]
#   The DLL is always built `-O2 -g` (release with debug info): same code as a
#   pure release build, plus symbols, so cdb / tools/cdb_stack.ps1 can resolve
#   antbridge and ant engine frames. antbridge.pdb is installed next to every
#   DLL copy, and src/src.cmake copies it into the build dir.
#   -Check    only verify that a rebuild is needed / all inputs exist
#   -Debug    accepted for compatibility; it is the default now (no-op)
#   -Restore  put the saved ship DLL back (H:\ant-v16\bridge\antbridge_ship.dll)

param(
    [switch]$Check,
    [switch]$Force,
    [switch]$Debug,
    [switch]$Restore
)

$ErrorActionPreference = 'Stop'

$root      = Split-Path -Parent $PSScriptRoot          # agave repo
$antRoot   = 'H:\ant-v16'
$ws        = 'C:\Users\linswin\Documents\deepseek-harness\default-workspace'
$zig       = Join-Path $ws 'zigspike'
$mingwBin  = 'C:\msys64\mingw64\bin'
$mingwLib  = 'C:\msys64\mingw64\lib'

$src       = Join-Path $root 'src\javascript\ant_backend\AntBridge.c'
$header    = Join-Path $root 'src\javascript\ant_backend\AntBridge.h'
$bridgeDir = Join-Path $antRoot 'bridge'
$cssrc     = Join-Path $bridgeDir 'AntBridge.c'
$cshdr     = Join-Path $bridgeDir 'AntBridge.h'
$obj       = Join-Path $bridgeDir 'AntBridge.obj'
$outDll    = Join-Path $bridgeDir 'antbridge.dll'
$outLib    = Join-Path $bridgeDir 'antbridge.dll.a'
$outPdb    = Join-Path $bridgeDir 'antbridge.pdb'
$shipDll   = Join-Path $bridgeDir 'antbridge_ship.dll'
$install   = Join-Path $root 'build-zig-native'

$libant    = Join-Path $antRoot 'build-zig\libant.a'
$vendorDir = Join-Path $antRoot 'build-zig\vendor'

$includeDirs = @(
    (Join-Path $antRoot 'include'),
    (Join-Path $antRoot 'vendor\uthash-2.3.0\src'),
    (Join-Path $antRoot 'vendor\libuv-v1.53.0\include'),
    (Join-Path $root 'src\webview')
)

$systemLibs = @(
    '-lpsapi', '-luser32', '-ladvapi32', '-liphlpapi', '-luserenv', '-lws2_32',
    '-ldbghelp', '-lole32', '-luuid', '-lshell32', '-lm', '-lsecur32', '-lntdll',
    '-lcrypt32', '-loleaut32', '-lwininet', '-lwinspool', '-lncrypt',
    # libuv needs WaitOnAddress/WakeByAddressSingle from synchronization.lib
    '-lsynchronization'
)

function Require($path, $what) {
    if (-not (Test-Path $path)) { throw "$what not found: $path" }
}

function AssertShellStopped {
    $p = Get-Process agave_shell -ErrorAction SilentlyContinue
    if ($p) {
        throw "agave_shell is running (pid $($p.Id -join ',')); stop it before swapping antbridge.dll"
    }
}

if ($Restore) {
    Require $shipDll 'antbridge_ship.dll (nothing was saved)'
    AssertShellStopped
    Copy-Item $shipDll (Join-Path $install 'antbridge.dll') -Force
    Write-Host "== restored ship antbridge.dll =="
    return
}

if ($Debug) {
    Write-Host "== -Debug is the default now: antbridge is always built -O2 -g =="
}

Require $src    'AntBridge.c'
Require $libant 'libant.a'
Require (Join-Path $zig 'zig-cc.cmd')  'zig-cc.cmd'
Require (Join-Path $zig 'zig-cxx.cmd') 'zig-cxx.cmd'

$vendorLibs = Get-ChildItem $vendorDir -Recurse -Filter '*.a' | ForEach-Object { $_.FullName }

Write-Host "== inputs =="
Write-Host "  source     : $src"
Write-Host "  libant.a   : $libant"
Write-Host "  vendor libs: $($vendorLibs.Count)"
Write-Host "  output     : $outDll (+ $(Split-Path -Leaf $outPdb))"

# Only rebuild when the source is newer than the DLL (and the PDB is there),
# unless -Force is given.
if ((Test-Path $outDll) -and (Test-Path $outPdb) -and -not $Force) {
    $sourceTime = (Get-Item $src).LastWriteTimeUtc
    $dllTime    = (Get-Item $outDll).LastWriteTimeUtc
    if ($dllTime -gt $sourceTime) {
        Write-Host "== up to date (dll is newer than source); nothing to do =="
        if ($Check) { return }
        Write-Host "   touch the source or pass -Force to rebuild"
        return
    }
}

if ($Check) {
    Write-Host "== rebuild required =="
    return
}

$env:PATH = "$mingwBin;" + $env:PATH

Write-Host "== 1/3 sync sources =="
Copy-Item $src    $cssrc  -Force
Copy-Item $header $cshdr  -Force

Write-Host "== 2/3 compile =="
# -O2 -g: release code with debug info (this is what makes cdb frames resolve).
$ccArgs = @('-target', 'x86_64-windows-gnu', '-std=gnu23', '-O2', '-g',
            '-DAGB_BUILD_DLL', '-c', $cssrc, '-o', $obj)
foreach ($inc in $includeDirs) { $ccArgs += @('-I', $inc) }

& (Join-Path $zig 'zig-cc.cmd') @ccArgs
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }

Write-Host "== 3/3 link =="
$ldArgs = @('-target', 'x86_64-windows-gnu', '-shared',
            '-Wl,--stack,134217728',
            '-L', $mingwLib,
            '-Wl,--start-group', $obj, $libant)
$ldArgs += $vendorLibs
$ldArgs += @('-Wl,--end-group')
$ldArgs += $systemLibs
$ldArgs += @((Join-Path $mingwLib 'libpathcch.a'), (Join-Path $mingwLib 'libtaskschd.a'))
$ldArgs += @('-Wl,--out-implib=' + $outLib)
$ldArgs += '-g'
$ldArgs += @('-o', $outDll)

& (Join-Path $zig 'zig-cxx.cmd') @ldArgs
if ($LASTEXITCODE -ne 0) { throw "link failed ($LASTEXITCODE)" }

if (-not (Test-Path $outPdb)) { throw "link did not produce $outPdb" }

Write-Host "== install =="
AssertShellStopped

# Keep one copy of the DLL that was installed before, so -Restore can undo an
# experiment without a rebuild.
if (Test-Path (Join-Path $install 'antbridge.dll')) {
    Copy-Item (Join-Path $install 'antbridge.dll') $shipDll -Force
    Write-Host "  saved previous DLL -> $shipDll"
}

$targets = @(
    (Join-Path $root '3rdparty\ant\bin'),
    (Join-Path $root '3rdparty\ant\lib'),
    $install,
    (Join-Path $ws   'host\nativerun')
)
foreach ($dir in $targets) {
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    Copy-Item $outDll $dir -Force
    Copy-Item $outPdb $dir -Force
    if (Test-Path $outLib) { Copy-Item $outLib $dir -Force }
    Write-Host "  -> $dir"
}

Write-Host "== done (release + debug info) =="
Write-Host "  $outDll + $(Split-Path -Leaf $outPdb)"
Write-Host "== now re-link libagave =="
Write-Host "  cmake --build $root\build-zig-native --parallel 16"
