# tools/check_shell_findbar.ps1
#
# Manual host check for the find bar overlay (NOT part of the offline suite: it
# starts the real GUI shell hidden, so it needs a desktop session).
#
# `--ui-shot` proves the card's layout and pixels in a DIB. What only exists with
# real windows is checked here:
#   * the bar is its own child window (AgaveShellFindBar) sitting over the view,
#     with the native EDIT inside it and above it in z-order;
#   * the page does NOT reflow: the view keeps the full content rect while the
#     bar is open (the old implementation pushed the page down by find_h);
#   * Esc closes the bar from the shell's message loop.
#
# The shell window is created with AGB_SHELL_HIDDEN=1 and AGB_SHELL_FIND=1, so it
# is never shown, never focused, and the bar is open from the first layout.
#
# Usage:
#   pwsh -File tools/check_shell_findbar.ps1
#   pwsh -File tools/check_shell_findbar.ps1 -BuildDir build-zig-native

param([string]$BuildDir = 'build-zig-native')

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "$BuildDir\agave_shell.exe"
$profileDir = Join-Path $root "$BuildDir\test-runs\findbar-profile"

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }

Remove-Item -Recurse -Force $profileDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $profileDir | Out-Null
$outFile = Join-Path $profileDir 'out.txt'
$errFile = Join-Path $profileDir 'err.txt'

$code = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class FindCheck {
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int idx);

  /* The shell is started hidden, so IsWindowVisible() on any child is always
   * false (it folds in the ancestors). The child's own WS_VISIBLE bit is what
   * this check needs. */
  public static bool OwnStyleVisible(IntPtr h) {
    return ((long)GetWindowLongPtr(h, -16 /* GWL_STYLE */) & 0x10000000L) != 0;
  }
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public struct RECT { public int left, top, right, bottom; }
  public struct POINT { public int x, y; }

  /* Scoped to the process we started: a developer's own shell has the same
   * class name and must never be driven by this script. */
  public static IntPtr FindByClass(string cls, uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr lp) {
      var sb = new StringBuilder(256);
      GetClassNameW(h, sb, 256);
      if (sb.ToString() != cls) { return true; }
      uint owner;
      GetWindowThreadProcessId(h, out owner);
      if (owner != pid) { return true; }
      found = h;
      return false;
    }, IntPtr.Zero);
    return found;
  }

  public static IntPtr FindChildById(IntPtr parent, int id) {
    IntPtr found = IntPtr.Zero;
    EnumChildWindows(parent, delegate(IntPtr h, IntPtr lp) {
      if (GetDlgCtrlID(h) != id) { return true; }
      found = h;
      return false;
    }, IntPtr.Zero);
    return found;
  }

  /* EnumChildWindows, not FindWindowEx: the overlay of a hidden shell is itself
   * hidden, and FindWindowEx-style lookups skip those. */
  public static IntPtr FindChildByClass(IntPtr parent, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumChildWindows(parent, delegate(IntPtr h, IntPtr lp) {
      var sb = new StringBuilder(256);
      GetClassNameW(h, sb, 256);
      if (sb.ToString() != cls) { return true; }
      found = h;
      return false;
    }, IntPtr.Zero);
    return found;
  }

  /* Position of a child in the shell's z-order (0 = topmost). */
  public static int ZOrder(IntPtr parent, IntPtr child) {
    IntPtr h = GetWindow(parent, 5 /* GW_CHILD */);
    int index = 0;
    while (h != IntPtr.Zero) {
      if (h == child) { return index; }
      h = GetWindow(h, 2 /* GW_HWNDNEXT */);
      ++index;
    }
    return -1;
  }
}
'@
Add-Type -TypeDefinition $code
[void][FindCheck]::SetProcessDPIAware()

$failed = 0
function Check([string]$name, [bool]$ok) {
    if (-not $ok) { $script:failed++ }
    Write-Host ("{0}  {1}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name)
}

$savedHidden = $env:AGB_SHELL_HIDDEN
$savedFind = $env:AGB_SHELL_FIND
$env:AGB_SHELL_HIDDEN = '1'
$env:AGB_SHELL_FIND = '1'
$proc = Start-Process -FilePath $exe -PassThru -WorkingDirectory $root `
    -ArgumentList @('--userdata', $profileDir, 'about:blank') `
    -RedirectStandardOutput $outFile -RedirectStandardError $errFile

try {
    $hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 60 -and $hwnd -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 100
        $hwnd = [FindCheck]::FindByClass('AgaveShellWindow', [uint32]$proc.Id)
    }
    if ($hwnd -eq [IntPtr]::Zero) {
        if ($proc.HasExited) {
            throw ("agave_shell exited (code=$($proc.ExitCode)) without a window - " +
                   "a shell already running on the same profile? see $errFile")
        }
        throw 'AgaveShellWindow not found (of our own pid)'
    }
    Start-Sleep -Milliseconds 800

    $dpi = [FindCheck]::GetDpiForWindow($hwnd)
    if ($dpi -eq 0) { $dpi = 96 }
    $scale = $dpi / 96.0

    $cr = New-Object FindCheck+RECT
    [void][FindCheck]::GetClientRect($hwnd, [ref]$cr)
    $origin = New-Object FindCheck+POINT
    [void][FindCheck]::ClientToScreen($hwnd, [ref]$origin)

    # Absolute screen rect -> shell client coordinates.
    function ClientRect([IntPtr]$h) {
        $r = New-Object FindCheck+RECT
        [void][FindCheck]::GetWindowRect($h, [ref]$r)
        return [pscustomobject]@{
            x = $r.left - $origin.x
            y = $r.top - $origin.y
            w = $r.right - $r.left
            h = $r.bottom - $r.top
        }
    }

    $bar = [FindCheck]::FindChildByClass($hwnd, 'AgaveShellFindBar')
    Check 'findbar-window' ($bar -ne [IntPtr]::Zero)
    Check 'findbar-visible' ($bar -ne [IntPtr]::Zero -and [FindCheck]::OwnStyleVisible($bar))
    if ($bar -eq [IntPtr]::Zero) { throw 'find bar overlay not found' }

    # ID_FIND_EDIT == 1010 (ui_chrome.h).
    $edit = [FindCheck]::FindChildById($hwnd, 1010)
    Check 'findbar-edit' ($edit -ne [IntPtr]::Zero)

    $view = [FindCheck]::FindChildByClass($hwnd, 'AgaveWebViewControl')
    Check 'findbar-view' ($view -ne [IntPtr]::Zero)
    if ($view -eq [IntPtr]::Zero) { throw 'web view control not found' }

    $b = ClientRect $bar
    $e = if ($edit -ne [IntPtr]::Zero) { ClientRect $edit } else { $null }
    $v = ClientRect $view

    # Tab strip 40 + toolbar 44 above the page, status bar 24 below it.
    $contentTop = [int][math]::Round(84 * $scale)
    $statusH = [int][math]::Round(24 * $scale)

    Write-Host ("      client={0}x{1} bar=({2},{3} {4}x{5}) edit=({6},{7} {8}x{9}) view=({10},{11} {12}x{13})" -f `
        $cr.right, $cr.bottom, $b.x, $b.y, $b.w, $b.h,
        $e.x, $e.y, $e.w, $e.h, $v.x, $v.y, $v.w, $v.h)

    Check 'bar-inside-content' ($b.x -ge 0 -and $b.y -ge $contentTop -and
                                $b.x + $b.w -le $cr.right -and
                                $b.y + $b.h -le $cr.bottom - $statusH)
    Check 'bar-top-right' ($b.x + $b.w -ge $cr.right - [int][math]::Round(12 * $scale))
    Check 'bar-size' ($b.w -ge [int](200 * $scale) -and $b.h -ge [int](30 * $scale))

    if ($e) {
        Check 'edit-inside-bar' ($e.x -ge $b.x -and $e.y -ge $b.y -and
                                 $e.x + $e.w -le $b.x + $b.w -and
                                 $e.y + $e.h -le $b.y + $b.h)
    }

    # The page must not reflow: the view still spans the whole content area.
    Check 'page-not-reflowed' ($v.y -eq $contentTop -and
                               $v.y + $v.h -eq $cr.bottom - $statusH)

    $zEdit = if ($edit -ne [IntPtr]::Zero) { [FindCheck]::ZOrder($hwnd, $edit) } else { -1 }
    $zBar = [FindCheck]::ZOrder($hwnd, $bar)
    $zView = [FindCheck]::ZOrder($hwnd, $view)
    Write-Host ("      z-order edit={0} bar={1} view={2}" -f $zEdit, $zBar, $zView)
    Check 'z-bar-above-view' ($zBar -ge 0 -and $zView -ge 0 -and $zBar -lt $zView)
    if ($edit -ne [IntPtr]::Zero) {
        Check 'z-edit-above-bar' ($zEdit -ge 0 -and $zBar -ge 0 -and $zEdit -lt $zBar)
    }

    # Esc closes the overlay from the shell's message loop (U11).
    [void][FindCheck]::PostMessage($hwnd, 0x0100, [IntPtr]0x1B, [IntPtr]::Zero) # VK_ESCAPE
    $hidden = $false
    for ($i = 0; $i -lt 30 -and -not $hidden; $i++) {
        Start-Sleep -Milliseconds 100
        $hidden = -not [FindCheck]::OwnStyleVisible($bar)
    }
    Check 'esc-hides-bar' $hidden

    $v2 = ClientRect $view
    Check 'page-still-full' ($v2.y -eq $v.y -and $v2.h -eq $v.h)

    $log = if (Test-Path $errFile) { Get-Content $errFile -Raw } else { '' }
    Check 'esc-logged' ($log -match 'esc closes find bar')

    [void][FindCheck]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) # WM_CLOSE
    $closed = $false
    for ($i = 0; $i -lt 40 -and -not $closed; $i++) {
        Start-Sleep -Milliseconds 100
        $closed = $proc.HasExited
    }
    Check 'shell-exits-clean' $closed

    if ($failed -ne 0) { exit 1 }
    Write-Host 'FINDBAR=PASS'
    exit 0
} finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    if ($null -eq $savedHidden) { Remove-Item Env:AGB_SHELL_HIDDEN -ErrorAction SilentlyContinue }
    else { $env:AGB_SHELL_HIDDEN = $savedHidden }
    if ($null -eq $savedFind) { Remove-Item Env:AGB_SHELL_FIND -ErrorAction SilentlyContinue }
    else { $env:AGB_SHELL_FIND = $savedFind }
    Remove-Item -Recurse -Force $profileDir -ErrorAction SilentlyContinue
}
