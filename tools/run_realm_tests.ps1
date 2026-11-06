# tools/run_realm_tests.ps1
#
# Regression for the per-frame realm work (docs/design-per-frame-realm.md).
#
# Two layers:
#   1. tools/ant_realm_probe.c on the bare bridge: one isolate, three realms,
#      each with its own global, its own `var` scope and its own intrinsics.
#   2. tools/pages/realm_parent.html + realm_child.html through the probe:
#      AGB_PER_FRAME_REALM=1 gives the iframe its own realm (its `var` no longer
#      leaks into the parent, `top.Array !== Array`), while parent globals stay
#      reachable from the child (parent.foo / parent.fn()) and the parent can
#      still reach into f.contentWindow.
#
# The default (flag off) path is checked too: it is the shipping behaviour until
# S2/S3 of the design land, and it must keep working.
#
# Usage:
#   pwsh -File tools/run_realm_tests.ps1
#   pwsh -File tools/run_realm_tests.ps1 -BuildDir build-zig-native

param(
    [string]$BuildDir = 'build-zig-native'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir
$exe = Join-Path $build 'agave_shell.exe'
$parentPage = Join-Path $root 'tools/pages/realm_parent.html'

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }
if (-not (Test-Path $parentPage)) { throw "page not found: $parentPage" }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ('agb-realm-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

# The probe script: what the parent sees of itself and of the iframe.
$checkJs = Join-Path $tmp 'realm_check.js'
@'
var f = document.getElementById('fr');
var cw = f ? f.contentWindow : null;
var cd = f ? f.contentDocument : null;

probeExit([
  'childTitle=' + (cd && cd.title ? cd.title : 'null'),
  'childVarLeak=' + (typeof window.childVar),
  'parentSelf=' + (window.parent === window),
  'parentTopArraySame=' + (window.top.Array === Array),
  'childReachable=' + (cw ? (typeof cw.childVar) : 'n/a'),
  'childTopIsParent=' + (cw ? (cw.top === window) : 'n/a'),
  'sameBody=' + (cd && cd.body && cw && cw.__selfBody
      ? (cd.body === cw.__selfBody) : 'n/a'),
  'childDcl=' + (cw ? cw.__childDcl : 'n/a'),
  'parentDcl=' + window.__parentDcl,
  'childTimer=' + (cw ? cw.__childTimer : 'n/a'),
  'childInterval=' + (cw ? cw.__childInterval : 'n/a'),
  'childTimerWorks=' + (cw ? cw.__childTimerWorks : 'n/a')
].join('|'));
'@ | Set-Content -Path $checkJs -Encoding ascii

$failures = 0

function Invoke-Probe {
    param([string]$PageEval)

    $stdout = Join-Path $tmp 'out.txt'
    $stderr = Join-Path $tmp 'err.txt'
    $old = $env:AGB_PER_FRAME_REALM
    if ($null -eq $PageEval) { Remove-Item Env:AGB_PER_FRAME_REALM -ErrorAction SilentlyContinue }
    else { $env:AGB_PER_FRAME_REALM = $PageEval }

    try {
        $p = Start-Process -FilePath $exe -NoNewWindow -PassThru -Wait -ArgumentList @(
            '--probe', $parentPage, '--eval-file', $checkJs,
            '--wait', '1500', '--settle', '500', '--timeout', '60000'
        ) -RedirectStandardOutput $stdout -RedirectStandardError $stderr

        [pscustomobject]@{
            Code   = $p.ExitCode
            Stdout = (Get-Content $stdout -Raw) -as [string]
        }
    } finally {
        if ($null -eq $old) { Remove-Item Env:AGB_PER_FRAME_REALM -ErrorAction SilentlyContinue }
        else { $env:AGB_PER_FRAME_REALM = $old }
    }
}

# 1. bare-bridge realm invariants
$pin = Join-Path $build 'ant_realm_probe.exe'
$pinSrc = Join-Path $root 'tools/ant_realm_probe.c'

if (-not (Test-Path $pin) -or ((Get-Item $pinSrc).LastWriteTime -gt (Get-Item $pin).LastWriteTime)) {
    Write-Host "building ant_realm_probe.exe"
    Push-Location $root
    try {
        & zig cc -target x86_64-windows-gnu -O1 -g -I '3rdparty/ant/include' $pinSrc `
            '3rdparty/ant/lib/antjsb.dll.a' -o $pin 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "zig cc failed for $pinSrc" }
    } finally { Pop-Location }
}

$realmOut = & $pin 2>&1 | Out-String
if ($LASTEXITCODE -ne 0 -or $realmOut -notmatch 'ALL REALM CHECKS PASSED') {
    $failures++
    Write-Host ("FAIL  bare realm invariants (exit {0})" -f $LASTEXITCODE)
    Write-Host (($realmOut -split "`n" | Select-Object -Last 8) -join "`n")
} else {
    Write-Host "PASS  bare realm invariants (isolated globals + intrinsics)"
}

# 2. shared realm (AGB_PER_FRAME_REALM=0): the old model, kept as A/B fallback
$shared = Invoke-Probe -PageEval '0'
$sharedText = $shared.Stdout

if ($shared.Code -ne 0) {
    $failures++
    Write-Host ("FAIL  shared realm baseline: exit {0}" -f $shared.Code)
} elseif ($sharedText -notmatch 'parentSelf=true' -or
          $sharedText -notmatch 'childReachable=string' -or
          $sharedText -notmatch 'parentFn=PF-ok') {
    $failures++
    Write-Host "FAIL  shared realm baseline: cross-frame access broken"
    Write-Host (($sharedText -split "`n" | Select-Object -Last 2) -join "`n")
} else {
    Write-Host "PASS  shared realm baseline (parent.foo / contentWindow)"
}

# 3. per-frame realms (now the default): isolation + cross-frame access both hold
$perFrame = Invoke-Probe -PageEval '1'
$perFrameText = $perFrame.Stdout

if ($perFrame.Code -ne 0) {
    $failures++
    Write-Host ("FAIL  per-frame realm: exit {0}" -f $perFrame.Code)
} else {
    $checks = @(
        @{ Name = 'child var does not leak';  Want = 'childVarLeak=undefined';   Ok = ($perFrameText -match 'childVarLeak=undefined') },
        @{ Name = 'child has own Array';      Want = 'topArraySame=false';       Ok = ($perFrameText -match 'topArraySame=false') },
        @{ Name = 'child top is parent';      Want = 'childTopIsParent=true';    Ok = ($perFrameText -match 'childTopIsParent=true') },
        @{ Name = 'parent globals reachable'; Want = 'parentFn=PF-ok';           Ok = ($perFrameText -match 'parentFn=PF-ok') },
        @{ Name = 'contentWindow reachable';  Want = 'childReachable=string';    Ok = ($perFrameText -match 'childReachable=string') },
        @{ Name = 'node wrappers per realm';  Want = 'sameBody=false';           Ok = ($perFrameText -match 'sameBody=false') },
        @{ Name = 'frame DOMContentLoaded';   Want = 'childDcl=1';               Ok = ($perFrameText -match 'childDcl=1') },
        @{ Name = 'parent DCL ran once';      Want = 'parentDcl=1';              Ok = ($perFrameText -match 'parentDcl=1') },
        @{ Name = 'child realm has timers';   Want = 'childTimer=function';      Ok = ($perFrameText -match 'childTimer=function') },
        @{ Name = 'child realm has interval'; Want = 'childInterval=function';   Ok = ($perFrameText -match 'childInterval=function') },
        @{ Name = 'child timer fires';        Want = 'childTimerWorks=1';        Ok = ($perFrameText -match 'childTimerWorks=1') }
    )

    foreach ($c in $checks) {
        if ($c.Ok) {
            Write-Host ("PASS  per-frame realm: {0}" -f $c.Name)
        } else {
            $failures++
            Write-Host ("FAIL  per-frame realm: {0} (wanted) {1}" -f $c.Name, $c.Want)
            Write-Host (($perFrameText -split "`n" | Select-Object -Last 2) -join "`n")
        }
    }
}

# 4. A frame realm must have the host API. A child page that calls fetch while
#    parsing used to hit "ReferenceError: __agbHttpRequestEx is not defined"
#    inside prelude's fetch when a realm ran prelude code without the API.
$fetchPage = Join-Path $root 'tools/pages/realm_parent_fetch.html'

if (Test-Path $fetchPage) {
    $fetchJs = Join-Path $tmp 'fetch_check.js'
    @'
var f = document.getElementById('fr');
var cw = f ? f.contentWindow : null;
probeExit('api=' + (cw ? typeof cw.__agbHttpRequestEx : 'n/a') +
          ' state=' + (cw ? String(cw.__fetchState) : 'n/a'));
'@ | Set-Content -Path $fetchJs -Encoding ascii

    $stdout = Join-Path $tmp 'fetch_out.txt'
    $stderr = Join-Path $tmp 'fetch_err.txt'
    $p = Start-Process -FilePath $exe -NoNewWindow -PassThru -Wait -ArgumentList @(
        '--probe', $fetchPage, '--eval-file', $fetchJs,
        '--wait', '1200', '--settle', '500', '--timeout', '60000'
    ) -RedirectStandardOutput $stdout -RedirectStandardError $stderr

    $text = (Get-Content $stdout -Raw) -as [string]

    if ($p.ExitCode -ne 0 -or $text -notmatch 'api=function') {
        $failures++
        Write-Host ("FAIL  frame realm has the host API (exit {0})" -f $p.ExitCode)
        Write-Host (($text -split "`n" | Select-Object -Last 2) -join "`n")
    } else {
        Write-Host "PASS  frame realm has the host API (fetch reachable)"
    }
}

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
if ($failures -gt 0) {
    Write-Host "`nREALM CHECKS FAILED ($failures)"
    exit 1
}

Write-Host "`nALL REALM CHECKS PASSED"
exit 0
