# tools/build_ant_replay.ps1
#
# Builds tools/ant-replay/ant_replay.c: a standalone embedder that proves (or
# disproves) that one call from C into JavaScript runs the JS body more than
# once. It links the very same libant.a the browser bridge uses, so the answer
# covers the shipped Ant, not a rebuild.
#
# Usage:  pwsh -File tools\build_ant_replay.ps1 [-AntRoot H:\ant-v16] [-Run]
#
# Why this exists: agave's load event ran a page's onload handler 2-6 times per
# dispatch while the dispatch itself happened once. This program calls closures
# from C (directly, nested through a native callback, and with arguments) and
# prints how many times the JS body ran. All cases print runs=1, which means the
# replay is not in Ant and not in the bridge's js_call_value() wrapper - it needs
# agave's runtime state to show up.

param(
    [string]$AntRoot = 'H:\ant-v16',
    [string]$OutDir = '',
    [switch]$Run
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $root 'build-ant-replay' }

$zigCandidates = @(
    (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\zig.exe'),
    'C:\zig\zig.exe'
)
$zig = $env:AGB_ZIG
if (-not $zig) {
    foreach ($c in $zigCandidates) { if (Test-Path $c) { $zig = $c; break } }
}
if (-not $zig -or -not (Test-Path $zig)) { throw 'zig.exe not found (pass $env:AGB_ZIG)' }

$ant     = (Resolve-Path $AntRoot).Path
$src     = Join-Path $root 'tools\ant-replay\ant_replay.c'
$libant  = Join-Path $ant 'build-zig\libant.a'
$vendor  = Join-Path $ant 'build-zig\vendor'
$mingwLib = 'C:/msys64/mingw64/lib'

foreach ($p in @($src, $libant, (Join-Path $ant 'include\ant.h'))) {
    if (-not (Test-Path $p)) { throw "missing input: $p" }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$obj = Join-Path $OutDir 'ant_replay.obj'
$exe = Join-Path $OutDir 'ant_replay.exe'

$includes = @(
    (Join-Path $ant 'include'),
    (Join-Path $root '3rdparty\ant\include'),
    (Join-Path $ant 'vendor\uthash-2.3.0\src'),
    (Join-Path $ant 'vendor\libuv-v1.53.0\include')
)

Write-Host "compile..."
$ccArgs = @('cc', '-target', 'x86_64-windows-gnu', '-O1', '-std=gnu23')
foreach ($i in $includes) { $ccArgs += @('-I', $i) }
$ccArgs += @('-c', $src, '-o', $obj)
& $zig @ccArgs
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }

$vendorLibs = Get-ChildItem $vendor -Recurse -Filter '*.a' | ForEach-Object { $_.FullName }
$systemLibs = @(
    '-lpsapi', '-luser32', '-ladvapi32', '-liphlpapi', '-luserenv', '-lws2_32',
    '-ldbghelp', '-lole32', '-luuid', '-lshell32', '-lm', '-lsecur32', '-lntdll',
    '-lcrypt32', '-loleaut32', '-lwininet', '-lwinspool', '-lncrypt', '-lsynchronization'
)

Write-Host "link ($($vendorLibs.Count) vendor libs)..."
$ldArgs = @('c++', '-target', 'x86_64-windows-gnu', '-o', $exe, $obj, $libant, '-Wl,--start-group')
$ldArgs += $vendorLibs
$ldArgs += @('-Wl,--end-group', '-L', $mingwLib)
$ldArgs += $systemLibs
$ldArgs += @((Join-Path $mingwLib 'libpathcch.a'), (Join-Path $mingwLib 'libtaskschd.a'))
& $zig @ldArgs
if ($LASTEXITCODE -ne 0) { throw "link failed ($LASTEXITCODE)" }

Write-Host "built $exe"
if ($Run) {
    & $exe
} else {
    Write-Host "run it with: $exe   (expect every case to print runs=1)"
}

# ---------------------------------------------------------------------------
# Second program: the same test through agave's own bridge (antjsb.dll), i.e.
# antb_engine_create + antb_eval + antb_pump + antb_set_native + antb_call.
# ---------------------------------------------------------------------------
$bridgeSrc = Join-Path $root 'tools\ant-replay\ant_replay_bridge.c'
$bridgeLib = Join-Path $root '3rdparty\ant\lib\antjsb.dll.a'
$bridgeDll = Join-Path $root '3rdparty\ant\bin\antjsb.dll'

if ((Test-Path $bridgeSrc) -and (Test-Path $bridgeLib)) {
    $bobj = Join-Path $OutDir 'ant_replay_bridge.obj'
    $bexe = Join-Path $OutDir 'ant_replay_bridge.exe'

    Write-Host "compile (bridge variant)..."

    $bccArgs = @('cc', '-target', 'x86_64-windows-gnu', '-O1', '-std=gnu23')
    foreach ($i in $includes) { $bccArgs += @('-I', $i) }
    $bccArgs += @('-I', (Join-Path $root 'src\javascript\ant_backend'), '-c', $bridgeSrc, '-o', $bobj)
    & $zig @bccArgs
    if ($LASTEXITCODE -ne 0) { throw "bridge compile failed ($LASTEXITCODE)" }

    $bldArgs = @('c++', '-target', 'x86_64-windows-gnu', '-o', $bexe, $bobj, $bridgeLib, '-L', $mingwLib)
    $bldArgs += $systemLibs
    & $zig @bldArgs
    if ($LASTEXITCODE -ne 0) { throw "bridge link failed ($LASTEXITCODE)" }

    Copy-Item $bridgeDll $OutDir -Force
    Write-Host "built $bexe"
    if ($Run) { & $bexe }
} else {
    Write-Host 'bridge variant skipped (antjsb.dll.a missing; run tools\build_antjsb.ps1)'
}


