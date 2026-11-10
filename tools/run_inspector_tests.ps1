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
#   4. vConsole (286 KiB) injected, mounted, then its panel opened by a click.
#
# The four probes run at the same time; the two cases that need mouse events
# are driven from this process while they are alive (each has its own log and
# pid). The clipboard is the per-case mock file, so these cases cannot race
# with each other or with another suite. Expected wall time: ~4 s.
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

if (-not (Test-Path $exe)) { throw "missing $exe - run tools/build_project.ps1" }
foreach ($f in @($probeScript, $ctxScript, $clickScript, $sendEvents)) {
    if (-not (Test-Path $f)) { throw "missing $f" }
}

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking
$run = New-TestRun -Suite 'inspector'

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

function ExitLine([string]$text) {
    if (-not $text) { return '' }
    $m = [regex]::Match($text, '\[exit\] (.*)')
    if ($m.Success) { return $m.Groups[1].Value.Trim() }
    return ''
}

# One probe with --inject inspector (or vconsole), own clipboard file.
function Start-PluginProbe {
    param([string]$Name, [string]$Inject, [string]$EvalFile, [int]$Settle)

    $raw = @('--probe', $page, '--inject', $Inject,
             '--wait', '300', '--settle', "$Settle", '--timeout', '40000')
    if ($EvalFile) { $raw += @('--eval-file', $EvalFile) }
    return Start-TestProbe -Run $run -Name $Name -RawArgs $raw -ClipboardValue ''
}

$jobs = [ordered]@{}
$jobs.menu = Start-PluginProbe -Name 'menu' -Inject 'inspector' -EvalFile $probeScript -Settle 3000

# vConsole needs a JS driver of its own (it is not the inspector).
$vcJs = Join-Path $run.Dir 'vconsole_eval.js'
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
$jobs.vconsole = Start-PluginProbe -Name 'vconsole' -Inject 'vconsole' -EvalFile $vcJs -Settle 2500

# Drive one mouse case while its probe is alive: wait for the inspector's
# "hover to inspect" line in that probe's own stderr, give --eval a moment,
# then post the real window messages. Each message is retried until the
# utility thread logs that it received it - the control is busy parsing the
# page under a loaded batch and a single posted message is not enough.
function Send-Verified {
    param($Job, [switch]$Right, [switch]$Left, [int]$X, [int]$Y, [string]$Pattern)

    for ($try = 0; $try -lt 4; $try++) {
        if ($Job.Proc.HasExited) { return $false }
        if ($Right) { & $sendEvents -ProcessId $Job.Proc.Id -RightClick -X $X -Y $Y | Out-Null }
        else { & $sendEvents -ProcessId $Job.Proc.Id -LeftClick -X $X -Y $Y | Out-Null }
        for ($i = 0; $i -lt 15; $i++) {
            Start-Sleep -Milliseconds 100
            $log = Get-Content $Job.Err -Raw -ErrorAction SilentlyContinue
            if ($log -and $log -match $Pattern) { return $true }
            if ($Job.Proc.HasExited) { return $false }
        }
    }
    return $false
}

function Invoke-InspectorAction {
    param([string]$Name, [string]$EvalFile, [switch]$WithClick)

    $job = Start-PluginProbe -Name $Name -Inject 'inspector' -EvalFile $EvalFile -Settle 15000
    $deadline = (Get-Date).AddSeconds(25)
    $ready = $false
    while ((Get-Date) -lt $deadline) {
        $log = Get-Content $job.Err -Raw -ErrorAction SilentlyContinue
        if ($log -and $log -match 'hover to inspect') { $ready = $true; break }
        if ($job.Proc.HasExited) { break }
        Start-Sleep -Milliseconds 100
    }
    if ($ready -and -not $job.Proc.HasExited) {
        Start-Sleep -Milliseconds 1200
        if (Send-Verified -Job $job -Right -X 120 -Y 140 -Pattern 'mouse down right at 120,140') {
            if ($WithClick) {
                Start-Sleep -Milliseconds 900
                Send-Verified -Job $job -Left -X 226 -Y 171 -Pattern 'mouse down left at 226,171' | Out-Null
            }
        }
    }
    return Complete-TestProbe -Job $job
}

$res = @{}
$res.menu = Complete-TestProbe -Job $jobs.menu
$res.vconsole = Complete-TestProbe -Job $jobs.vconsole
$res.rightclick = Invoke-InspectorAction -Name 'rightclick' -EvalFile $ctxScript
$res.click = Invoke-InspectorAction -Name 'click' -EvalFile $clickScript -WithClick

"== 1. in-process: menu items, hit-test, element info, clipboard"
$r = $res.menu
$line = ExitLine $r.Stdout
Check 'probe exit 0' ($r.Code -eq 0) "exit=$($r.Code) out=$($r.Stdout)"
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
$clip = Get-TestClipboard $r.Job
Check 'clipboard holds the copied text' ($clip -eq 'target') "clipboard=$clip"

"== 2. real right-click: engine dispatches contextmenu, native menu suppressed"
$r = $res.rightclick
$line = ExitLine $r.Stdout
Check 'probe exit 0' ($r.Code -eq 0) "exit=$($r.Code) out=$($r.Stdout)"
Check 'inspector menu opened on real right-click' ($line -match 'ctx labels=Copy element info') $line
Check 'utility event reached the control' ($r.Stderr -match 'mouse down right at 120,140') 'no [ui] right-down'
Check 'native context menu callback suppressed' (-not ($r.Stderr -match 'context menu at')) 'shell on_context_menu ran'

"== 3. real left-click on the first row copies element info"
$r = $res.click
$line = ExitLine $r.Stdout
$clip = Get-TestClipboard $r.Job
Check 'probe exit 0' ($r.Code -eq 0) "exit=$($r.Code) out=$($r.Stdout)"
Check 'row click closed the menu' ($line -match 'row clicked') $line
Check 'clipboard holds element info JSON' ($clip -match '"kind":"agave-element"') "clipboard=$clip"
Check 'element info keeps the page url' ($clip -match '"url":"file:') "clipboard=$clip"
Check 'native context menu callback suppressed' (-not ($r.Stderr -match 'context menu at')) 'shell on_context_menu ran'

"== 4. vConsole (286 KiB): DOM mutation followed by host focus"
# The shell evals the bundle, then focuses the view. vConsole leaves layout
# dirty, so Frame::setIsActive used to measure the selection before layout and
# trip RenderBlock::selectionGapRects' ASSERT(!needsLayout()) (R170).
$r = $res.vconsole
Check 'vconsole probe exit 0' ($r.Code -eq 0) "exit=$($r.Code) out=$($r.Text)"
Check 'vconsole opens its panel and lays it out' ($r.Text -match 'vc=true root=true panel=true open=true layout=true') $r.Text

Remove-TestRun $run $script:failed

""
if ($script:failed -eq 0) {
    "inspector: $($script:checks)/$($script:checks) checks passed"
    exit 0
}
"inspector: $($script:checks - $script:failed)/$($script:checks) checks passed, $script:failed FAILED"
exit 1
