#!/usr/bin/env pwsh
# tools/build_antjsb.ps1
#
# Builds antjsb.dll from the ant checkout, then installs the DLL, import
# library and public header into 3rdparty/ant.
#
# The bridge used to be compiled by tools/build_antbridge.ps1 from agave's
# AntBridge.c. It now lives in the ant tree (bridge/) and is built by ant's
# Meson target. See docs/ref-ant-engine.md.
#
# Usage:
#   pwsh -File tools/build_antjsb.ps1
#   pwsh -File tools/build_antjsb.ps1 -AntRoot D:\src\ant -Force
#   pwsh -File tools/build_antjsb.ps1 -NoPublish

param(
    [string]$AntRoot = 'H:\ant-v16',
    [string]$BuildDir = '',
    [string]$OutDir = '',
    [switch]$Force,
    [switch]$NoPublish
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $AntRoot 'build-zig' }
if (-not $OutDir)   { $OutDir   = Join-Path $root '3rdparty\ant' }

function Require($path, $what) {
    if (-not (Test-Path $path)) { throw "$what not found: $path" }
}

$bridge = Join-Path $AntRoot 'bridge\AntBridge.c'
Require $bridge 'bridge/AntBridge.c (ant checkout)'
Require (Join-Path $AntRoot 'meson.build') 'ant meson.build'

if (Get-Process agave_shell -ErrorAction SilentlyContinue) {
    throw 'agave_shell is running; stop it before replacing antjsb.dll'
}

$meson = Get-Command meson -ErrorAction SilentlyContinue
if (-not $meson) { throw 'meson not found in PATH' }
$ninja = Get-Command ninja -ErrorAction SilentlyContinue
if (-not $ninja) { throw 'ninja not found in PATH' }

Write-Host "== antjsb"
Write-Host "  ant root : $AntRoot"
Write-Host "  build dir: $BuildDir"
Write-Host "  output   : $OutDir"

if ($Force -or -not (Test-Path (Join-Path $BuildDir 'build.ninja'))) {
    Push-Location $AntRoot
    try {
        & $meson.Source setup --reconfigure $BuildDir
        if ($LASTEXITCODE -ne 0) { throw "meson setup failed ($LASTEXITCODE)" }
    } finally { Pop-Location }
}

Push-Location $AntRoot
try {
    & $meson.Source compile -C $BuildDir antjsb
    if ($LASTEXITCODE -ne 0) { throw "meson compile failed ($LASTEXITCODE)" }
} finally { Pop-Location }

$dll = Join-Path $BuildDir 'antjsb.dll'
$lib = Join-Path $BuildDir 'antjsb.dll.a'
$pdb = Join-Path $BuildDir 'antjsb.pdb'
Require $dll 'antjsb.dll'
Require $lib 'antjsb.dll.a'

if ($NoPublish) {
    Write-Host "== built (not published)"
    return
}

$bin = Join-Path $OutDir 'bin'
$libDir = Join-Path $OutDir 'lib'
$incDir = Join-Path $OutDir 'include'
New-Item -ItemType Directory -Force -Path $bin, $libDir, $incDir | Out-Null

Copy-Item $dll $bin -Force
Copy-Item $lib $libDir -Force
Copy-Item (Join-Path $AntRoot 'bridge\AntBridge.h') $incDir -Force
if (Test-Path $pdb) { Copy-Item $pdb $bin -Force }

Write-Host "== published"
Write-Host "  $bin\antjsb.dll"
Write-Host "  $libDir\antjsb.dll.a"
Write-Host "  $incDir\AntBridge.h"
