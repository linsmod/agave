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

$failures = 0

function Invoke-Probe {
    param([string]$PageEval)

    $stdout = Join-Path $tmp 'out.txt'
    $stderr = Join-Path $tmp 'err.txt'
    $old = $env:AGB_PER_FRAME_REALM
    $oldCheck = $env:AGB_REALM_CHECK
    if ($null -eq $PageEval) { Remove-Item Env:AGB_PER_FRAME_REALM -ErrorAction SilentlyContinue }
    else { $env:AGB_PER_FRAME_REALM = $PageEval }
    # Every realm test also runs the mismatch guard (R132).
    $env:AGB_REALM_CHECK = '1'

    try {
        $p = Start-Process -FilePath $exe -NoNewWindow -PassThru -Wait -ArgumentList @(
            '--probe', $parentPage, '--eval-file', $checkJs,
            '--wait', '1500', '--settle', '500', '--timeout', '60000'
        ) -RedirectStandardOutput $stdout -RedirectStandardError $stderr

        [pscustomobject]@{
            Code     = $p.ExitCode
            Stdout   = (Get-Content $stdout -Raw) -as [string]
            Mismatch = if (Test-Path $stderr) { (Select-String -Path $stderr -Pattern 'realm-mismatch').Count } else { 0 }
        }
    } finally {
        if ($null -eq $old) { Remove-Item Env:AGB_PER_FRAME_REALM -ErrorAction SilentlyContinue }
        else { $env:AGB_PER_FRAME_REALM = $old }
        if ($null -eq $oldCheck) { Remove-Item Env:AGB_REALM_CHECK -ErrorAction SilentlyContinue }
        else { $env:AGB_REALM_CHECK = $oldCheck }
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
          $sharedText -notmatch 'parentFn=PF-ok' -or
          $sharedText -notmatch 'framesIndexSame=true' -or
          $sharedText -notmatch 'framesNameSame=true' -or
          $sharedText -notmatch 'winIndexSame=true' -or
          $sharedText -notmatch 'framesLen=1' -or
          $sharedText -notmatch 'childTimerUrl=own' -or
          $sharedText -notmatch 'childTimerMarker=found' -or
          $shared.Mismatch -gt 0) {
    $failures++
    Write-Host "FAIL  shared realm baseline: cross-frame access broken"
    if ($shared.Mismatch -gt 0) { Write-Host ("      realm-mismatch lines: {0}" -f $shared.Mismatch) }
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
    if ($perFrame.Mismatch -gt 0) {
        $failures++
        Write-Host ("FAIL  per-frame realm: realm-mismatch guard ({0} lines)" -f $perFrame.Mismatch)
    } else {
        Write-Host "PASS  per-frame realm: no document mismatch on the write path"
    }
    $checks = @(
        @{ Name = 'child var does not leak';  Want = 'childVarLeak=undefined';   Ok = ($perFrameText -match 'childVarLeak=undefined') },
        @{ Name = 'child has own Array';      Want = 'topArraySame=false';       Ok = ($perFrameText -match 'topArraySame=false') },
        @{ Name = 'child top is parent';      Want = 'childTopIsParent=true';    Ok = ($perFrameText -match 'childTopIsParent=true') },
        @{ Name = 'parent globals reachable'; Want = 'parentFn=PF-ok';           Ok = ($perFrameText -match 'parentFn=PF-ok') },
        @{ Name = 'contentWindow reachable';  Want = 'childReachable=string';    Ok = ($perFrameText -match 'childReachable=string') },
        @{ Name = 'node wrappers per realm';  Want = 'sameBody=false';           Ok = ($perFrameText -match 'sameBody=false') },
        @{ Name = 'frames[0] is child window'; Want = 'framesIndexSame=true';     Ok = ($perFrameText -match 'framesIndexSame=true') },
        @{ Name = 'frames["fr"] is child window'; Want = 'framesNameSame=true';   Ok = ($perFrameText -match 'framesNameSame=true') },
        @{ Name = 'window[0] is child window'; Want = 'winIndexSame=true';        Ok = ($perFrameText -match 'winIndexSame=true') },
        @{ Name = 'window.length is frame count'; Want = 'framesLen=1';           Ok = ($perFrameText -match 'framesLen=1') },
        @{ Name = 'child sees itself in parent.frames'; Want = 'childFramesByIndex=true'; Ok = ($perFrameText -match 'childFramesByIndex=true') },
        @{ Name = 'child found by name/id';   Want = 'childFramesByName=true';    Ok = ($perFrameText -match 'childFramesByName=true') },
        @{ Name = 'child found via contentWindow'; Want = 'childFramesByOwner=true'; Ok = ($perFrameText -match 'childFramesByOwner=true') },
        @{ Name = 'child own frames empty';   Want = 'childOwnFrames=0:undefined'; Ok = ($perFrameText -match 'childOwnFrames=0:undefined') },
        @{ Name = 'frame DOMContentLoaded';   Want = 'childDcl=1';               Ok = ($perFrameText -match 'childDcl=1') },
        @{ Name = 'parent DCL ran once';      Want = 'parentDcl=1';              Ok = ($perFrameText -match 'parentDcl=1') },
        @{ Name = 'child realm has timers';   Want = 'childTimer=function';      Ok = ($perFrameText -match 'childTimer=function') },
        @{ Name = 'child realm has interval'; Want = 'childInterval=function';   Ok = ($perFrameText -match 'childInterval=function') },
        @{ Name = 'child timer fires';        Want = 'childTimerWorks=1';        Ok = ($perFrameText -match 'childTimerWorks=1') },
        @{ Name = 'timer keeps child document'; Want = 'childTimerUrl=own';      Ok = ($perFrameText -match 'childTimerUrl=own') },
        @{ Name = 'timer sees child DOM';     Want = 'childTimerMarker=found';   Ok = ($perFrameText -match 'childTimerMarker=found') },
        @{ Name = 'timer document identity';  Want = 'childTimerDocSame=same';   Ok = ($perFrameText -match 'childTimerDocSame=same') },
        @{ Name = 'per-realm constructors (B2-4)'; Want = 'childCtorDiff=true'; Ok = ($perFrameText -match 'childCtorDiff=true') },
        @{ Name = 'per-realm prototypes (B2-4)';   Want = 'childProtoDiff=true'; Ok = ($perFrameText -match 'childProtoDiff=true') }
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

# 4. Document-scoped window objects (R131): location/history must answer for
#    the document they belong to, in both per-frame and shared-realm modes.
$crossPage = Join-Path $root 'tools/pages/cap_realm_cross.html'

if (Test-Path $crossPage) {
    $crossJs = Join-Path $tmp 'cross_check.js'
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

    foreach ($mode in @('1', '0')) {
        $stdout = Join-Path $tmp "cross_$mode.txt"
        $stderr = Join-Path $tmp "cross_$mode.err"
        $old = $env:AGB_PER_FRAME_REALM
        $oldCheck = $env:AGB_REALM_CHECK
        if ($mode -eq '0') { $env:AGB_PER_FRAME_REALM = '0' }
        else { Remove-Item Env:AGB_PER_FRAME_REALM -ErrorAction SilentlyContinue }
        $env:AGB_REALM_CHECK = '1'

        try {
            $p = Start-Process -FilePath $exe -NoNewWindow -PassThru -Wait -ArgumentList @(
                '--probe', $crossPage, '--eval-file', $crossJs,
                '--wait', '1500', '--settle', '500', '--timeout', '60000'
            ) -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        } finally {
            if ($null -eq $old) { Remove-Item Env:AGB_PER_FRAME_REALM -ErrorAction SilentlyContinue }
            else { $env:AGB_PER_FRAME_REALM = $old }
            if ($null -eq $oldCheck) { Remove-Item Env:AGB_REALM_CHECK -ErrorAction SilentlyContinue }
            else { $env:AGB_REALM_CHECK = $oldCheck }
        }

        $text = (Get-Content $stdout -Raw) -as [string]
        $mismatch = if (Test-Path $stderr) { (Select-String -Path $stderr -Pattern 'realm-mismatch').Count } else { 0 }
        $label = if ($mode -eq '0') { 'shared realm' } else { 'per-frame realm' }
        $ok = $p.ExitCode -eq 0 -and $mismatch -eq 0
        $checks = @(
            @{ Name = 'child location.search';   Want = 'childSearch=?x=1';            Ok = ($text -match 'childSearch=\?x=1') },
            @{ Name = 'pushState updates URL';   Want = 'childLocAfter=...pushed=1';   Ok = ($text -match 'childLocAfter=[^\|]*\?pushed=1') },
            @{ Name = 'document.location';       Want = 'childDocLocHref=...?x=1';     Ok = ($text -match 'childDocLocHref=[^\|]*\?x=1') },
            @{ Name = 'location identity';       Want = 'locIsWinLoc=true';            Ok = ($text -match 'locIsWinLoc=true') },
            @{ Name = 'new Image() document';    Want = 'imgDoc=true';                Ok = ($text -match 'imgDoc=true') },
            @{ Name = 'parent.location';         Want = 'parentHref=...cap_realm_cross.html'; Ok = ($text -match 'parentHref=[^\|]*cap_realm_cross\.html') },
            @{ Name = 'child title isolated';    Want = 'childTitle=cross child';      Ok = ($text -match 'childTitle=cross child') },
            @{ Name = 'parent title isolated';   Want = 'parentTitle=cross parent';    Ok = ($text -match 'parentTitle=cross parent') },
            @{ Name = 'child history length';    Want = 'histBefore=1 histAfter=2';   Ok = ($text -match 'histBefore=1' -and $text -match 'histAfter=2') },
            @{ Name = 'parent history untouched'; Want = 'parentHist=1';              Ok = ($text -match 'parentHist=1') },
            @{ Name = 'contentDocument URL';     Want = 'contentDoc=true';            Ok = ($text -match 'contentDoc=true') }
        )

        if (-not $ok) {
            $failures++
            Write-Host ("FAIL  {0}: document-scoped objects (exit {1}, mismatch {2})" -f $label, $p.ExitCode, $mismatch)
            Write-Host (($text -split "`n" | Select-Object -Last 2) -join "`n")
        } else {
            Write-Host ("PASS  {0}: no document mismatch (AGB_REALM_CHECK=1)" -f $label)
            foreach ($c in $checks) {
                if ($c.Ok) {
                    Write-Host ("PASS  {0}: {1}" -f $label, $c.Name)
                } else {
                    $failures++
                    Write-Host ("FAIL  {0}: {1} (wanted) {2}" -f $label, $c.Name, $c.Want)
                    Write-Host (($text -split "`n" | Select-Object -Last 2) -join "`n")
                }
            }
        }
    }
}

# 5. A frame realm must have the host API. A child page that calls fetch while
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
