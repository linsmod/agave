# run_inspector_tests.ps1 - regression for the injected element inspector.
#
# Everything here goes through the real paths:
#   1. inspector.js injected by the shell (same code the menu runs) and driven
#      in-process: menu items, row hit-testing, element-info JSON, clipboard.
#   2. a real WM_RBUTTONDOWN/UP posted to the hidden probe control: the engine
#      must dispatch DOM contextmenu, the inspector must open its menu AND
#      cancel the event, so the shell's native menu callback never runs.
#   3. a real WM_LBUTTONDOWN/UP on the first menu row: the click has to land on
#      the row and copy the element info JSON to the clipboard.
#
#   pwsh -File tools/run_inspector_tests.ps1
#
# The window is never shown (headless probe), so this does not disturb anyone.

param(
    [string]$Page = 'tools\pages\inspector_demo.html'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build-zig-native\agave_shell.exe'
$page = if ([System.IO.Path]::IsPathRooted($Page)) { $Page } else { Join-Path $root $Page }
$sendEvents = Join-Path $PSScriptRoot 'send_ui_events.ps1'
$probeScript = Join-Path $root 'tools\pages\check_inspector_menu.js'
$ctxScript = Join-Path $root 'tools\pages\check_inspector_ctx.js'
$clickScript = Join-Path $root 'tools\pages\check_inspector_click.js'
$tmp = Join-Path $env:TEMP 'agave_inspector_tests'

if (-not (Test-Path $exe)) { throw "missing $exe - run tools/build_project.ps1" }
foreach ($f in @($probeScript, $ctxScript, $clickScript, $sendEvents)) {
    if (-not (Test-Path $f)) { throw "missing $f" }
}
if (-not (Test-Path $tmp)) { New-Item -ItemType Directory -Path $tmp | Out-Null }

$script:failed = 0
$script:checks = 0

function Check([string]$name, [bool]$ok, [string]$detail) {
    $script:checks++
    if ($ok) {
        "PASS $name"
    } else {
        $script:failed++
        "FAIL $name  ($detail)"
    }
}

# Runs the probe with --inject inspector. $Action (optional) runs once the
# inject line appeared, so a click can be posted while the page is alive.
function Run-Probe {
    param(
        [string]$Name,
        [string]$EvalFile,
        [scriptblock]$Action,
        [int]$Settle = 15000
    )

    $out = Join-Path $tmp "$Name.out"
    $err = Join-Path $tmp "$Name.err"
    Remove-Item $out, $err -ErrorAction SilentlyContinue

    $proc = Start-Process -FilePath $exe -PassThru -WindowStyle Hidden -ArgumentList @(
        '--probe', $page, '--inject', 'inspector',
        '--eval-file', $EvalFile,
        '--wait', '300', '--settle', "$Settle", '--timeout', '40000'
    ) -RedirectStandardOutput $out -RedirectStandardError $err

    $deadline = (Get-Date).AddSeconds(25)
    $ready = $false
    while ((Get-Date) -lt $deadline) {
        $log = Get-Content $err -Raw -ErrorAction SilentlyContinue
        if ($log -and $log -match 'hover to inspect') { $ready = $true; break }
        if ($proc.HasExited) { break }
        Start-Sleep -Milliseconds 100
    }

    if ($ready -and $Action -and -not $proc.HasExited) {
        Start-Sleep -Milliseconds 1200      # let --eval run
        & $Action $proc.Id | Out-Null
    }

    $proc.WaitForExit(30000) | Out-Null
    Start-Sleep -Milliseconds 150

    return @{
        Exit = $proc.ExitCode
        Out = (Get-Content $out -Raw -ErrorAction SilentlyContinue)
        Err = (Get-Content $err -Raw -ErrorAction SilentlyContinue)
    }
}

function ExitLine([string]$text) {
    if (-not $text) { return '' }
    $m = [regex]::Match($text, '\[exit\] (.*)')
    if ($m.Success) { return $m.Groups[1].Value.Trim() }
    return ''
}

"== 1. in-process: menu items, hit-test, element info, clipboard"
Set-Clipboard -Value ''
$r = Run-Probe -Name 'menu' -EvalFile $probeScript -Settle 3000
$line = ExitLine $r.Out
Check 'probe exit 0' ($r.Exit -eq 0) "exit=$($r.Exit) out=$($r.Out)"
Check 'hover finds #target' ($line -match 'hover=div#target') $line
Check 'menu has element info first' ($line -match 'labels=Copy element info\|Copy selector\|Copy bounds\|Copy text\|Copy HTML\|Select parent\|Exit inspector') $line
Check 'menu rows are hit-testable' ($line -match 'row0=div:Copy element info') $line
Check 'row belongs to the overlay' ($line -match 'row0inside=1') $line
Check 'menu closes after selecting' ($line -match 'menuClosedOnSelect=yes') $line
Check 'element info JSON kind' ($line -match 'infoKind=1') $line
Check 'element info selector' ($line -match 'infoSelector=1') $line
Check 'element info viewport rect' ($line -match 'infoRect=1') $line
Check 'element info document rect' ($line -match 'infoDocRect=1') $line
Check 'element info inViewport' ($line -match 'infoViewport=1') $line
Check 'select parent walks up' ($line -match 'parent=body') $line
Check 'contextmenu preventDefault' ($line -match 'prevented=1') $line
Check 'menu opened by contextmenu' ($line -match 'menuOpen=1') $line
Check 'copy text action' ($line -match 'copyText=1') $line
Check 'exit destroys inspector' (($line -match 'exited=1') -and ($line -match 'gone=1')) $line
Check 'clipboard holds the copied text' ((Get-Clipboard -Raw) -eq 'target') "clipboard=$(Get-Clipboard -Raw)"

"== 2. real right-click: engine dispatches contextmenu, native menu suppressed"
$r = Run-Probe -Name 'rightclick' -EvalFile $ctxScript -Settle 15000 -Action {
    param($procId)
    Start-Sleep -Milliseconds 300
    & $sendEvents -ProcessId $procId -RightClick -X 120 -Y 140 | Out-Null
}
$line = ExitLine $r.Out
Check 'probe exit 0' ($r.Exit -eq 0) "exit=$($r.Exit) out=$($r.Out)"
Check 'inspector menu opened on real right-click' ($line -match 'ctx labels=Copy element info') $line
Check 'utility event reached the control' ($r.Err -match 'mouse down right at 120,140') 'no [ui] right-down'
Check 'native context menu callback suppressed' (-not ($r.Err -match 'context menu at')) 'shell on_context_menu ran'

"== 3. real left-click on the first row copies element info"
Set-Clipboard -Value ''
$r = Run-Probe -Name 'click' -EvalFile $clickScript -Settle 15000 -Action {
    param($procId)
    Start-Sleep -Milliseconds 300
    & $sendEvents -ProcessId $procId -RightClick -X 120 -Y 140 | Out-Null
    Start-Sleep -Milliseconds 900
    & $sendEvents -ProcessId $procId -LeftClick -X 226 -Y 171 | Out-Null
}
$line = ExitLine $r.Out
$clip = Get-Clipboard -Raw
Check 'probe exit 0' ($r.Exit -eq 0) "exit=$($r.Exit) out=$($r.Out)"
Check 'row click closed the menu' ($line -match 'row clicked') $line
Check 'clipboard holds element info JSON' ($clip -match '"kind":"agave-element"') "clipboard=$clip"
Check 'element info keeps the page url' ($clip -match '"url":"file:') "clipboard=$clip"
Check 'native context menu callback suppressed' (-not ($r.Err -match 'context menu at')) 'shell on_context_menu ran'

"== 4. vConsole (286 KiB): DOM mutation followed by host focus"
# The shell evals the bundle, then focuses the view. vConsole leaves layout
# dirty, so Frame::setIsActive used to measure the selection before layout and
# trip RenderBlock::selectionGapRects' ASSERT(!needsLayout()) (R170).
$vcOut = Join-Path $tmp 'vconsole.out'
$vcErr = Join-Path $tmp 'vconsole.err'
$vcJs = Join-Path $tmp 'vconsole_eval.js'
Set-Content -Path $vcJs -Encoding ascii -Value @'
var root = document.getElementById("__vconsole");
var sw = document.querySelector(".vc-switch");
if (sw) sw.click();
setTimeout(function () {
  var f = document.querySelector(".vc-scroller-footer");
  var v = document.querySelector(".vc-scroller-viewport");
  var fr = f ? f.getBoundingClientRect() : null;
  var vr = v ? v.getBoundingClientRect() : null;
  var layout = !!(fr && vr && fr.height > 0 && Math.abs(fr.bottom - vr.bottom) <= 1);
  probeExit("vc=" + !!window.__agaveVConsole + " root=" + !!root +
    " panel=" + !!sw + " open=" + (root && String(root.className).indexOf("vc-toggle") >= 0) +
    " layout=" + layout);
}, 700);
'@
Remove-Item $vcOut, $vcErr -ErrorAction SilentlyContinue
$vc = Start-Process -FilePath $exe -WindowStyle Hidden -PassThru -ArgumentList @(
    '--probe', $page, '--inject', 'vconsole',
    '--eval-file', $vcJs,
    '--wait', '300', '--settle', '2500', '--timeout', '40000'
) -RedirectStandardOutput $vcOut -RedirectStandardError $vcErr
$vc.WaitForExit(40000) | Out-Null
$vcText = (Get-Content $vcOut -Raw -ErrorAction SilentlyContinue) +
          (Get-Content $vcErr -Raw -ErrorAction SilentlyContinue)
Check 'vconsole probe exit 0' ($vc.ExitCode -eq 0) "exit=$($vc.ExitCode) out=$vcText"
Check 'vconsole opens its panel and lays it out' ($vcText -match 'vc=true root=true panel=true open=true layout=true') $vcText

""
if ($script:failed -eq 0) {
    "inspector: $($script:checks)/$($script:checks) checks passed"
    exit 0
}
"inspector: $($script:checks - $script:failed)/$($script:checks) checks passed, $script:failed FAILED"
exit 1
