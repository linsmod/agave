# tools/check_shell_tabsearch.ps1
#
# Manual host check for the "Search tabs" popup (NOT part of the offline suite:
# it starts the real GUI shell hidden, so it needs a desktop session).
#
#   * the tab strip's v-button opens the popup (layered + topmost);
#   * Esc closes it and the shell keeps working.
#
# Windows are matched by pid: a shell the developer already has open is never
# driven by this script.
#
# Usage:
#   pwsh -File tools/check_shell_tabsearch.ps1 [-BuildDir build-zig-native]

param([string]$BuildDir = 'build-zig-native')

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "$BuildDir\agave_shell.exe"
$profileDir = Join-Path $root "$BuildDir\test-runs\tabsearch-profile"

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }

Remove-Item -Recurse -Force $profileDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $profileDir | Out-Null
$outFile = Join-Path $profileDir 'out.txt'
$errFile = Join-Path $profileDir 'err.txt'

$code = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class TabSearchCheck {
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int idx);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public struct RECT { public int left, top, right, bottom; }

  /* Scoped to the process we started (a developer's shell has the same class). */
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
}
'@
Add-Type -TypeDefinition $code
[void][TabSearchCheck]::SetProcessDPIAware()

$failed = 0
function Check([string]$name, [bool]$ok) {
    if (-not $ok) { $script:failed++ }
    Write-Host ("{0}  {1}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name)
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
    $hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 60 -and $hwnd -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 100
        $hwnd = [TabSearchCheck]::FindByClass('AgaveShellWindow', [uint32]$proc.Id)
    }
    if ($hwnd -eq [IntPtr]::Zero) {
        if ($proc.HasExited) {
            throw ("agave_shell exited (code=$($proc.ExitCode)) without a window - " +
                   "a shell already running on the same profile? see $errFile")
        }
        throw 'AgaveShellWindow not found (of our own pid)'
    }
    Start-Sleep -Milliseconds 800

    $dpi = [TabSearchCheck]::GetDpiForWindow($hwnd)
    if ($dpi -eq 0) { $dpi = 96 }
    $scale = $dpi / 96.0

    Check 'popup-absent' ([TabSearchCheck]::FindByClass('AgaveShellTabSearch', [uint32]$proc.Id) -eq [IntPtr]::Zero)

    # The tab-search entry sits at the left end of the strip: pad 8, button 32
    # wide/tall, so its centre is (24, 24) @96.
    $bx = [int][math]::Round(24 * $scale)
    $by = [int][math]::Round(24 * $scale)

    [void][TabSearchCheck]::PostMessage($hwnd, 0x0200, [IntPtr]::Zero, (Lp $bx $by))
    [void][TabSearchCheck]::PostMessage($hwnd, 0x0201, [IntPtr]1, (Lp $bx $by))
    [void][TabSearchCheck]::PostMessage($hwnd, 0x0202, [IntPtr]::Zero, (Lp $bx $by))

    $popup = [IntPtr]::Zero
    for ($i = 0; $i -lt 30 -and $popup -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 100
        $popup = [TabSearchCheck]::FindByClass('AgaveShellTabSearch', [uint32]$proc.Id)
    }
    Check 'popup-open' ($popup -ne [IntPtr]::Zero)

    if ($popup -ne [IntPtr]::Zero) {
        $r = New-Object TabSearchCheck+RECT
        [void][TabSearchCheck]::GetWindowRect($popup, [ref]$r)
        $ex = [TabSearchCheck]::GetWindowLongPtr($popup, -20).ToInt64()
        $pw = $r.right - $r.left
        $ph = $r.bottom - $r.top

        Write-Host ("      popup rect=({0},{1}) {2}x{3}" -f $r.left, $r.top, $pw, $ph)
        Check 'popup-visible' ([TabSearchCheck]::IsWindowVisible($popup))
        Check 'popup-layered' (($ex -band 0x80000) -ne 0)
        Check 'popup-topmost' (($ex -band 0x8) -ne 0)
        Check 'popup-size' ($pw -gt [int](300 * $scale) -and $ph -gt [int](60 * $scale))

        # Esc closes it - keys reach the shell's loop (the popup has the mouse,
        # the shell has the keyboard), which forwards them to the popup. The
        # popup is *hidden*, not destroyed, so the check is on visibility.
        [void][TabSearchCheck]::PostMessage($hwnd, 0x0100, [IntPtr]0x1B, [IntPtr]::Zero)
        $gone = $false
        for ($i = 0; $i -lt 30 -and -not $gone; $i++) {
            Start-Sleep -Milliseconds 100
            $gone = -not [TabSearchCheck]::IsWindowVisible($popup)
        }
        Check 'esc-closes-popup' $gone
    }

    # The shell must still answer afterwards.
    [void][TabSearchCheck]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) # WM_CLOSE
    $closed = $false
    for ($i = 0; $i -lt 40 -and -not $closed; $i++) {
        Start-Sleep -Milliseconds 100
        $closed = $proc.HasExited
    }
    Check 'shell-exits-clean' $closed

    if ($failed -ne 0) { exit 1 }
    Write-Host 'TABSEARCH=PASS'
    exit 0
} finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    if ($null -eq $savedHidden) { Remove-Item Env:AGB_SHELL_HIDDEN -ErrorAction SilentlyContinue }
    else { $env:AGB_SHELL_HIDDEN = $savedHidden }
    Remove-Item -Recurse -Force $profileDir -ErrorAction SilentlyContinue
}
