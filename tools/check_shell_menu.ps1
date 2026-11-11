# tools/check_shell_menu.ps1
#
# Manual host check for the self-drawn menus (NOT part of the offline suite:
# it starts the real GUI shell hidden, so it needs a desktop session).
#
# `--ui-shot --ui-menu` proves the menu model/layout/paint in a DIB. The parts
# that only exist with a real HWND are here:
#   * the app menu opens as a WS_EX_LAYERED | WS_EX_TOOLWINDOW popup right
#     under the 3-dot button, and a click on the first row chooses it;
#   * the page context menu opens from the control's real WM_RBUTTONDOWN/UP
#     callback and Esc dismisses it without leaving the modal loop behind.
#
# The shell window is created with AGB_SHELL_HIDDEN=1: never shown, never
# focused, no taskbar entry. Window coordinates are physical pixels, so
# PowerShell opts into DPI awareness first.
#
# Usage:
#   pwsh -File tools/check_shell_menu.ps1
#   pwsh -File tools/check_shell_menu.ps1 -BuildDir build-zig-native

param([string]$BuildDir = 'build-zig-native')

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "$BuildDir\agave_shell.exe"
$profileDir = Join-Path $root "$BuildDir\test-runs\menu-host-profile"

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }

Remove-Item -Recurse -Force $profileDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $profileDir | Out-Null
$outFile = Join-Path $profileDir 'out.txt'
$errFile = Join-Path $profileDir 'err.txt'

$code = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class MenuCheck {
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int idx);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
  public struct RECT { public int left, top, right, bottom; }

  public static IntPtr FindByClass(string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr lp) {
      var sb = new StringBuilder(256);
      GetClassNameW(h, sb, 256);
      if (sb.ToString() == cls) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
'@
Add-Type -TypeDefinition $code
[void][MenuCheck]::SetProcessDPIAware()

$failed = 0
function Check([string]$name, [bool]$ok) {
    if (-not $ok) { $script:failed++ }
    Write-Host ("{0}  {1}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name)
}
function Wait-Class([string]$cls, [int]$tries = 40) {
    $found = [IntPtr]::Zero
    for ($i = 0; $i -lt $tries -and $found -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 100
        $found = [MenuCheck]::FindByClass($cls)
    }
    return $found
}
function Lp([int]$x, [int]$y) {
    return [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
}

$savedHidden = $env:AGB_SHELL_HIDDEN
$env:AGB_SHELL_HIDDEN = '1'
$proc = Start-Process -FilePath $exe -PassThru -WorkingDirectory $root `
    -ArgumentList @('--userdata', $profileDir, 'about:blank') `
    -RedirectStandardOutput $outFile -RedirectStandardError $errFile

try {
    $hwnd = Wait-Class 'AgaveShellWindow'
    if ($hwnd -eq [IntPtr]::Zero) { throw 'AgaveShellWindow not found' }
    Start-Sleep -Milliseconds 700

    $cr = New-Object MenuCheck+RECT
    [void][MenuCheck]::GetClientRect($hwnd, [ref]$cr)
    $dpi = [MenuCheck]::GetDpiForWindow($hwnd)
    if ($dpi -eq 0) { $dpi = 96 }
    $scale = $dpi / 96.0

    # uiChromeLayout @96: pad 6, button 32, tabstrip 40, toolbar 44.
    $btn = [int][math]::Round(32 * $scale)
    $menuX = $cr.right - [int][math]::Round(6 * $scale) - [int]($btn / 2)
    $menuY = [int][math]::Round(40 * $scale) +
             [int]([math]::Round(44 * $scale) - $btn) / 2 + [int]($btn / 2)

    [void][MenuCheck]::PostMessage($hwnd, 0x0200, [IntPtr]::Zero, (Lp $menuX $menuY))
    [void][MenuCheck]::PostMessage($hwnd, 0x0201, [IntPtr]1, (Lp $menuX $menuY))
    [void][MenuCheck]::PostMessage($hwnd, 0x0202, [IntPtr]::Zero, (Lp $menuX $menuY))

    $popup = Wait-Class 'AgaveShellMenuPopup'
    Check 'app-menu-popup' ($popup -ne [IntPtr]::Zero)
    if ($popup -eq [IntPtr]::Zero) { throw 'app menu popup not found' }

    $pr = New-Object MenuCheck+RECT
    [void][MenuCheck]::GetWindowRect($popup, [ref]$pr)
    $ex = [MenuCheck]::GetWindowLongPtr($popup, -20).ToInt64()
    $pw = $pr.right - $pr.left
    $ph = $pr.bottom - $pr.top

    Check 'menu-layered' (($ex -band 0x80000) -ne 0)
    Check 'menu-toolwindow' (($ex -band 0x80) -ne 0)
    Check 'menu-size' ($pw -gt [int](150 * $scale) -and $ph -gt [int](100 * $scale))
    Check 'menu-on-screen' ($pr.left -ge -2 -and $pr.top -ge -2 -and
                            $pr.right -le [int](6000) -and $pr.bottom -le [int](6000))

    # First row ("Add bookmark", id 1): click the middle of its 30 px row.
    $firstRowY = [int][math]::Round(16 * $scale) + [int][math]::Round(8 * $scale) +
                 [int]([math]::Round(30 * $scale) / 2)
    $firstRowX = [int][math]::Round(16 * $scale) + [int]([math]::Round(120 * $scale) / 2)
    [void][MenuCheck]::PostMessage($popup, 0x0202, [IntPtr]::Zero, (Lp $firstRowX $firstRowY))

    $gone = $false
    for ($i = 0; $i -lt 30 -and -not $gone; $i++) {
        Start-Sleep -Milliseconds 100
        $gone = ([MenuCheck]::FindByClass('AgaveShellMenuPopup') -eq [IntPtr]::Zero)
    }
    Check 'app-menu-click-closes' $gone
    Start-Sleep -Milliseconds 250

    $log = if (Test-Path $errFile) { Get-Content $errFile -Raw } else { '' }
    Check 'app-menu-chose-item' ($log -match 'app menu choice=1')
    Check 'bookmark-added' (Test-Path (Join-Path $profileDir 'bookmarks.txt')) 

    # Page context menu: real right click on the control's HWND.
    $view = [MenuCheck]::FindWindowExW($hwnd, [IntPtr]::Zero, 'AgaveWebViewControl', $null)
    Check 'view-control-found' ($view -ne [IntPtr]::Zero)
    if ($view -eq [IntPtr]::Zero) { throw 'web view control not found' }
    Start-Sleep -Milliseconds 200

    [void][MenuCheck]::PostMessage($view, 0x0200, [IntPtr]::Zero, (Lp 120 200))
    [void][MenuCheck]::PostMessage($view, 0x0204, [IntPtr]2, (Lp 120 200))   # WM_RBUTTONDOWN
    [void][MenuCheck]::PostMessage($view, 0x0205, [IntPtr]::Zero, (Lp 120 200)) # WM_RBUTTONUP

    $popup2 = Wait-Class 'AgaveShellMenuPopup'
    Check 'context-menu-popup' ($popup2 -ne [IntPtr]::Zero)
    if ($popup2 -eq [IntPtr]::Zero) { throw 'context menu popup not found' }

    [void][MenuCheck]::PostMessage($popup2, 0x0100, [IntPtr]0x1B, [IntPtr]::Zero) # VK_ESCAPE
    $gone2 = $false
    for ($i = 0; $i -lt 30 -and -not $gone2; $i++) {
        Start-Sleep -Milliseconds 100
        $gone2 = ([MenuCheck]::FindByClass('AgaveShellMenuPopup') -eq [IntPtr]::Zero)
    }
    Check 'context-menu-esc-dismiss' $gone2
    Start-Sleep -Milliseconds 250

    $log = if (Test-Path $errFile) { Get-Content $errFile -Raw } else { '' }
    Check 'context-menu-logged' ($log -match 'context menu at 120,200 .* choice=0')

    # The shell must still answer after both modal loops.
    [void][MenuCheck]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) # WM_CLOSE
    $closed = $false
    for ($i = 0; $i -lt 40 -and -not $closed; $i++) {
        Start-Sleep -Milliseconds 100
        $closed = $proc.HasExited
    }
    Check 'shell-exits-clean' $closed

    if ($failed -ne 0) { exit 1 }
    Write-Host 'MENU-HOST=PASS'
    exit 0
} finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    if ($null -eq $savedHidden) { Remove-Item Env:AGB_SHELL_HIDDEN -ErrorAction SilentlyContinue }
    else { $env:AGB_SHELL_HIDDEN = $savedHidden }
    Remove-Item -Recurse -Force $profileDir -ErrorAction SilentlyContinue
}
