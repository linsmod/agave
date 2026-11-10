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
# The bare-bridge probe and all page probes start together (own profiles);
# expected wall time ~4 s.
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

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking
$run = New-TestRun -Suite 'realm'

$failures = 0

# ---- 1. bare-bridge realm invariants (own exe, started concurrently) ------
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
$pinOutFile = Join-Path $run.Dir 'pin.out'
$pinErrFile = Join-Path $run.Dir 'pin.err'
$pinProc = Start-Process -FilePath $pin -NoNewWindow -PassThru `
    -RedirectStandardOutput $pinOutFile -RedirectStandardError $pinErrFile

# The probe script: what the parent sees of itself and of the iframe.
$checkJs = Join-Path $run.Dir 'realm_check.js'
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
  'framesIndexSame=' + (cw ? (window.frames[0] === cw) : 'n/a'),
  'framesNameSame=' + (cw ? (window.frames['fr'] === cw) : 'n/a'),
  'winIndexSame=' + (cw ? (window[0] === cw) : 'n/a'),
  'framesLen=' + window.length,
  'childFramesByIndex=' + (cw && cw.__framesMe ? cw.__framesMe.byIndex : 'n/a'),
  'childFramesByName=' + (cw && cw.__framesMe ? cw.__framesMe.byName : 'n/a'),
  'childFramesByOwner=' + (cw && cw.__framesMe ? cw.__framesMe.byOwner : 'n/a'),
  'childOwnFrames=' + (cw && cw.__framesMe
      ? cw.__framesMe.ownLen + ':' + cw.__framesMe.ownIndex : 'n/a'),
  'sameBody=' + (cd && cd.body && cw && cw.__selfBody
      ? (cd.body === cw.__selfBody) : 'n/a'),
  'childDcl=' + (cw ? cw.__childDcl : 'n/a'),
  'parentDcl=' + window.__parentDcl,
  'childTimer=' + (cw ? cw.__childTimer : 'n/a'),
  'childInterval=' + (cw ? cw.__childInterval : 'n/a'),
  'childTimerWorks=' + (cw ? cw.__childTimerWorks : 'n/a'),
  'childTimerUrl=' + (cw ? cw.__childTimerUrl : 'n/a'),
  'childTimerMarker=' + (cw ? cw.__childTimerMarker : 'n/a'),
  'childTimerDocSame=' + (cw ? cw.__childTimerDocSame : 'n/a'),
  'childCtorDiff=' + (cw ? (cw.HTMLElement !== HTMLElement) : 'n/a'),
  'childProtoDiff=' + (cw ? (cw.HTMLElement.prototype !== HTMLElement.prototype) : 'n/a')
].join('|'));
'@ | Set-Content -Path $checkJs -Encoding ascii

$jobs = @{}
# 2. shared realm (AGB_PER_FRAME_REALM=0): the old model, kept as A/B fallback
$jobs.shared = Start-TestProbe -Run $run -Name 'shared-realm' -Url $parentPage `
    -EvalFile $checkJs -Wait 1500 -Settle 500 -Timeout 60000 `
    -Env @{ AGB_PER_FRAME_REALM = '0'; AGB_REALM_CHECK = '1' }
# 3. per-frame realms (now the default).
$jobs.perFrame = Start-TestProbe -Run $run -Name 'per-frame-realm' -Url $parentPage `
    -EvalFile $checkJs -Wait 1500 -Settle 500 -Timeout 60000 `
    -Env @{ AGB_PER_FRAME_REALM = '1'; AGB_REALM_CHECK = '1' }

# 4. Document-scoped window objects (R131): location/history must answer for
#    the document they belong to, in both per-frame and shared-realm modes.
$crossPage = Join-Path $root 'tools/pages/cap_realm_cross.html'
$crossJs = Join-Path $run.Dir 'cross_check.js'
if (Test-Path $crossPage) {
    @'
var f = document.getElementById('fr');
var w = f ? f.contentWindow : null;
var d = f ? f.contentDocument : null;
var x = w ? w.__x : null;
probeExit([
  'childSearch=' + (x ? x.search : 'n/a'),
  'childLocAfter=' + (x ? x.locAfter : 'n/a'),
  'childDocLocHref=' + (x ? x.docLocHref : 'n/a'),
  'locIsWinLoc=' + (x ? x.locIsWinLoc : 'n/a'),
  'imgDoc=' + (x ? x.imgDoc : 'n/a'),
  'parentHref=' + (x ? x.parentHref : 'n/a'),
  'childTitle=' + (x ? x.title : 'n/a'),
  'parentTitle=' + document.title,
  'histBefore=' + (x ? x.histBefore : 'n/a'),
  'histAfter=' + (x ? x.histAfter : 'n/a'),
  'parentHist=' + window.__parentHist,
  'childParentHist=' + (x ? x.parentHist : 'n/a'),
  'contentDoc=' + (d && d.URL.indexOf('cap_realm_cross_child') >= 0)
].join('|'));
'@ | Set-Content -Path $crossJs -Encoding ascii

    $jobs.crossPer = Start-TestProbe -Run $run -Name 'cross-per-frame' -Url $crossPage `
        -EvalFile $crossJs -Wait 1500 -Settle 500 -Timeout 60000 `
        -Env @{ AGB_PER_FRAME_REALM = '1'; AGB_REALM_CHECK = '1' }
    $jobs.crossShared = Start-TestProbe -Run $run -Name 'cross-shared' -Url $crossPage `
        -EvalFile $crossJs -Wait 1500 -Settle 500 -Timeout 60000 `
        -Env @{ AGB_PER_FRAME_REALM = '0'; AGB_REALM_CHECK = '1' }
}

# 5. A frame realm must have the host API.
$fetchPage = Join-Path $root 'tools/pages/realm_parent_fetch.html'
$fetchJs = Join-Path $run.Dir 'fetch_check.js'
if (Test-Path $fetchPage) {
    @'
var f = document.getElementById('fr');
var cw = f ? f.contentWindow : null;
probeExit('api=' + (cw ? typeof cw.__agbHttpRequestEx : 'n/a') +
          ' state=' + (cw ? String(cw.__fetchState) : 'n/a'));
'@ | Set-Content -Path $fetchJs -Encoding ascii
    $jobs.fetch = Start-TestProbe -Run $run -Name 'frame-host-api' -Url $fetchPage `
        -EvalFile $fetchJs -Wait 1200 -Settle 500 -Timeout 60000
}

$res = @{}
foreach ($name in @($jobs.Keys)) { $res[$name] = Complete-TestProbe -Job $jobs[$name] }
$pinProc.WaitForExit()
$realmOut = ((Get-Content $pinOutFile -Raw -ErrorAction SilentlyContinue) +
             (Get-Content $pinErrFile -Raw -ErrorAction SilentlyContinue))

function Mismatches($Result) {
    if ($Result.Stderr -match 'realm-mismatch') {
        return ([regex]::Matches($Result.Stderr, 'realm-mismatch')).Count
    }
    return 0
}

if ($pinProc.ExitCode -ne 0 -or $realmOut -notmatch 'ALL REALM CHECKS PASSED') {
    $failures++
    Write-Host ("FAIL  bare realm invariants (exit {0})" -f $pinProc.ExitCode)
    Write-Host (($realmOut -split "`n" | Select-Object -Last 8) -join "`n")
} else {
    Write-Host "PASS  bare realm invariants (isolated globals + intrinsics)"
}

# ---- shared baseline ------------------------------------------------------
$shared = $res.shared
$sharedText = $shared.Stdout
if ($shared.Code -ne 0) {
    $failures++
    Write-Host ("FAIL  shared realm baseline: exit {0}" -f $shared.Code)
} elseif ($sharedText -notmatch 'parentSelf=true' -or
          $sharedText -notmatch 'childReachable=string' -or
          $sharedText -notmatch 'parentFn=PF-ok' -or
          $sharedText -notmatch 'framesIndexSame=true' -or
          $sharedText -notmatch 'framesNameSame=true' -or
          $sharedText -notmatch 'winIndexSame=true' -or
          $sharedText -notmatch 'framesLen=1' -or
          $sharedText -notmatch 'childTimerUrl=own' -or
          $sharedText -notmatch 'childTimerMarker=found' -or
          (Mismatches $shared) -gt 0) {
    $failures++
    Write-Host "FAIL  shared realm baseline: cross-frame access broken"
    Write-Host (($sharedText -split "`n" | Select-Object -Last 2) -join "`n")
} else {
    Write-Host "PASS  shared realm baseline (parent.foo / contentWindow)"
}

# ---- per-frame realms -----------------------------------------------------
$perFrame = $res.perFrame
$perFrameText = $perFrame.Stdout
if ($perFrame.Code -ne 0) {
    $failures++
    Write-Host ("FAIL  per-frame realm: exit {0}" -f $perFrame.Code)
} else {
    if ((Mismatches $perFrame) -gt 0) {
        $failures++
        Write-Host ("FAIL  per-frame realm: realm-mismatch guard ({0} lines)" -f (Mismatches $perFrame))
    } else {
        Write-Host "PASS  per-frame realm: no document mismatch on the write path"
    }
    $checks = @(
        @{ Name = 'child var does not leak';  Ok = ($perFrameText -match 'childVarLeak=undefined') },
        @{ Name = 'child has own Array';      Ok = ($perFrameText -match 'topArraySame=false') },
        @{ Name = 'child top is parent';      Ok = ($perFrameText -match 'childTopIsParent=true') },
        @{ Name = 'parent globals reachable'; Ok = ($perFrameText -match 'parentFn=PF-ok') },
        @{ Name = 'contentWindow reachable';  Ok = ($perFrameText -match 'childReachable=string') },
        @{ Name = 'node wrappers per realm';  Ok = ($perFrameText -match 'sameBody=false') },
        @{ Name = 'frames[0] is child window'; Ok = ($perFrameText -match 'framesIndexSame=true') },
        @{ Name = 'frames["fr"] is child window'; Ok = ($perFrameText -match 'framesNameSame=true') },
        @{ Name = 'window[0] is child window'; Ok = ($perFrameText -match 'winIndexSame=true') },
        @{ Name = 'window.length is frame count'; Ok = ($perFrameText -match 'framesLen=1') },
        @{ Name = 'child sees itself in parent.frames'; Ok = ($perFrameText -match 'childFramesByIndex=true') },
        @{ Name = 'child found by name/id';   Ok = ($perFrameText -match 'childFramesByName=true') },
        @{ Name = 'child found via contentWindow'; Ok = ($perFrameText -match 'childFramesByOwner=true') },
        @{ Name = 'child own frames empty';   Ok = ($perFrameText -match 'childOwnFrames=0:undefined') },
        @{ Name = 'frame DOMContentLoaded';   Ok = ($perFrameText -match 'childDcl=1') },
        @{ Name = 'parent DCL ran once';      Ok = ($perFrameText -match 'parentDcl=1') },
        @{ Name = 'child realm has timers';   Ok = ($perFrameText -match 'childTimer=function') },
        @{ Name = 'child realm has interval'; Ok = ($perFrameText -match 'childInterval=function') },
        @{ Name = 'child timer fires';        Ok = ($perFrameText -match 'childTimerWorks=1') },
        @{ Name = 'timer keeps child document'; Ok = ($perFrameText -match 'childTimerUrl=own') },
        @{ Name = 'timer sees child DOM';     Ok = ($perFrameText -match 'childTimerMarker=found') },
        @{ Name = 'timer document identity';  Ok = ($perFrameText -match 'childTimerDocSame=same') },
        @{ Name = 'per-realm constructors (B2-4)'; Ok = ($perFrameText -match 'childCtorDiff=true') },
        @{ Name = 'per-realm prototypes (B2-4)';   Ok = ($perFrameText -match 'childProtoDiff=true') }
    )
    foreach ($c in $checks) {
        if ($c.Ok) { Write-Host ("PASS  per-frame realm: {0}" -f $c.Name) }
        else {
            $failures++
            Write-Host ("FAIL  per-frame realm: {0}" -f $c.Name)
        }
    }
}

# ---- cross-document objects ----------------------------------------------
foreach ($key in @('crossPer', 'crossShared')) {
    if (-not $res.ContainsKey($key)) { continue }
    $r = $res[$key]
    $text = $r.Stdout
    $label = if ($key -eq 'crossShared') { 'shared realm' } else { 'per-frame realm' }
    $ok = $r.Code -eq 0 -and (Mismatches $r) -eq 0
    $checks = @(
        @{ Name = 'child location.search';   Ok = ($text -match 'childSearch=\?x=1') },
        @{ Name = 'pushState updates URL';   Ok = ($text -match 'childLocAfter=[^\|]*\?pushed=1') },
        @{ Name = 'document.location';       Ok = ($text -match 'childDocLocHref=[^\|]*\?x=1') },
        @{ Name = 'location identity';       Ok = ($text -match 'locIsWinLoc=true') },
        @{ Name = 'new Image() document';    Ok = ($text -match 'imgDoc=true') },
        @{ Name = 'parent.location';         Ok = ($text -match 'parentHref=[^\|]*cap_realm_cross\.html') },
        @{ Name = 'child title isolated';    Ok = ($text -match 'childTitle=cross child') },
        @{ Name = 'parent title isolated';   Ok = ($text -match 'parentTitle=cross parent') },
        @{ Name = 'child history length';    Ok = ($text -match 'histBefore=1' -and $text -match 'histAfter=2') },
        @{ Name = 'parent history untouched'; Ok = ($text -match 'parentHist=1') },
        @{ Name = 'contentDocument URL';     Ok = ($text -match 'contentDoc=true') }
    )
    if (-not $ok) {
        $failures++
        Write-Host ("FAIL  {0}: document-scoped objects (exit {1}, mismatch {2})" -f $label, $r.Code, (Mismatches $r))
    } else {
        Write-Host ("PASS  {0}: no document mismatch (AGB_REALM_CHECK=1)" -f $label)
        foreach ($c in $checks) {
            if ($c.Ok) { Write-Host ("PASS  {0}: {1}" -f $label, $c.Name) }
            else {
                $failures++
                Write-Host ("FAIL  {0}: {1}" -f $label, $c.Name)
            }
        }
    }
}

if ($res.ContainsKey('fetch')) {
    $r = $res.fetch
    if ($r.Code -ne 0 -or $r.Text -notmatch 'api=function') {
        $failures++
        Write-Host ("FAIL  frame realm has the host API (exit {0})" -f $r.Code)
    } else {
        Write-Host "PASS  frame realm has the host API (fetch reachable)"
    }
}

Remove-TestRun $run $failures
if ($failures -gt 0) {
    Write-Host "`nREALM CHECKS FAILED ($failures)"
    exit 1
}

Write-Host "`nALL REALM CHECKS PASSED"
exit 0
