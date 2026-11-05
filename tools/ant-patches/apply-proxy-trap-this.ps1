# tools/ant-patches/apply-proxy-trap-this.ps1
#
# Ant calls every Proxy trap with `this === undefined`:
#
#     sv_vm_call(js->vm, js, get_trap, js_mkundef(), args, 3, NULL, js_mkundef());
#
# The spec (and every other engine) calls the trap with `this` bound to the
# handler object. Vue 3's reactivity reads `this._isReadonly` inside its `get`
# trap, so browserscore.dev died with
#   TypeError: Cannot read properties of undefined (reading '_isReadonly')
# the moment it touched a reactive object.
#
# Usage:  pwsh -File tools\ant-patches\apply-proxy-trap-this.ps1 [-AntRoot H:\ant-v16] [-Build]
#   -Build  also rebuild libant.a (ninja -C build-zig libant.a)

param(
    [string]$AntRoot = 'H:\ant-v16',
    [switch]$Build
)

$ErrorActionPreference = 'Stop'

$src = Join-Path $AntRoot 'src\ant.c'
if (-not (Test-Path $src)) { throw "ant.c not found at $src" }

$text = [IO.File]::ReadAllText($src)
$before = ([regex]::Matches($text, '_trap, js_mkundef\(\), args,')).Count

if ($before -eq 0) {
    Write-Host 'already patched (no trap call sites left with an undefined receiver)'
} else {
    $text = $text.Replace('_trap, js_mkundef(), args,', '_trap, handler, args,')
    [IO.File]::WriteAllText($src, $text)
    Write-Host "patched $before trap call site(s): this = handler"
}

# proxy_set_sym() has no local `handler`, only data->handler
$text = [IO.File]::ReadAllText($src)
if ($text.Contains('set_trap, handler, args, 4') -and $text.Contains('static ant_value_t proxy_set_sym(')) {
    $idx = $text.IndexOf('static ant_value_t proxy_set_sym(')
    $tail = $text.Substring($idx)
    $tail = $tail.Replace('set_trap, handler, args, 4', 'set_trap, data->handler, args, 4')
    $text = $text.Substring(0, $idx) + $tail
    [IO.File]::WriteAllText($src, $text)
    Write-Host 'proxy_set_sym: this = data->handler'
}

if ($Build) {
    $env:PATH = "C:\msys64\usr\bin;C:\msys64\mingw64\bin;" + $env:PATH
    & 'C:\msys64\mingw64\bin\ninja.exe' -C (Join-Path $AntRoot 'build-zig') libant.a
    if ($LASTEXITCODE -ne 0) { throw "libant build failed ($LASTEXITCODE)" }
    Write-Host 'libant.a rebuilt - now run tools\build_antjsb.ps1 -Force and tools\build_project.ps1'
}
