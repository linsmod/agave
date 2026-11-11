# tools/check_shell_frame.ps1
#
# Manual frame/NC-click check for the desktop shell. NOT part of the offline
# suite: it starts the real GUI shell with AGB_SHELL_HIDDEN=1 (the window is
# never shown, never takes focus) and drives the real HWND with Win32 messages.
#
# Why a separate check: --ui-shot renders the chrome into a DIB, but it cannot
# exercise WM_NCCALCSIZE / WM_NCHITTEST / WM_NCLBUTTONDOWN. Those are exactly
# where the "native caption above tabs" and "classic pressed button over our
# glyph" regressions live.
#
#   * client origin y == window top (no native caption);
#   * hit-test: min=HTMINBUTTON(8), max=HTMAXBUTTON(9), close=HTCLOSE(20),
#     tab=HTCLIENT(1), empty tab-strip=HTCAPTION(2);
#   * WM_NCLBUTTONDOWN + WM_LBUTTONUP performs maximize/restore, minimize,
#     close. The shell intercepts WM_NCLBUTTONDOWN (Chromium CUSTOM_DRAWN
#     path), so DefWindowProc must never paint the classic pressed state.
#
# Usage:
#   pwsh -File tools/check_shell_frame.ps1
#   pwsh -File tools/check_shell_frame.ps1 -BuildDir build-zig-native

param([string]$BuildDir = 'build-zig-native')

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "$BuildDir\agave_shell.exe"
$profileDir = Join-Path $root "$BuildDir\test-runs\frame-check-profile"

if (-not (Test-Path $exe)) { throw "agave_shell.exe not found: $exe" }

Remove-Item -Recurse -Force $profileDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $profileDir | Out-Null

$code = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class W {
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Ansi)] public static extern int GetClassNameA(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  public struct RECT { public int left, top, right, bottom; }
  public struct POINT { public int x, y; }

  public static IntPtr FindByClass(string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr lp) {
      var sb = new StringBuilder(256);
      GetClassNameA(h, sb, 256);
      if (sb.ToString() == cls) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
'@
Add-Type -TypeDefinition $code

# The shell is Per-Monitor V2. Without this, PowerShell is DPI-virtualized and
# GetClientRect/ClientToScreen return logical coords; the shell then maps our
# screen points to physical client pixels (120,140 -> 150,175 on a 125% box).
[void][W]::SetProcessDPIAware()

$savedHidden = $env:AGB_SHELL_HIDDEN
$env:AGB_SHELL_HIDDEN = '1'
$proc = Start-Process -FilePath $exe -PassThru -WorkingDirectory $root `
    -ArgumentList @('--userdata', $profileDir, 'about:blank') `
    -RedirectStandardOutput (Join-Path $profileDir 'out.txt') `
    -RedirectStandardError (Join-Path $profileDir 'err.txt')

$failed = 0
try {
    $hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 50 -and $hwnd -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 100
        $hwnd = [W]::FindByClass('AgaveShellWindow')
    }
    if ($hwnd -eq [IntPtr]::Zero) { throw 'AgaveShellWindow not found' }
    Start-Sleep -Milliseconds 500

    # The custom caption must also paint the DWM frame border (Windows 11):
    # without DWMWA_BORDER_COLOR the rounded frame keeps a light 1 px rim
    # around the dark chrome. Win10 has no such attribute; only Win11 can be
    # required to accept it.
    $frameLog = if (Test-Path (Join-Path $profileDir 'err.txt')) {
        Get-Content (Join-Path $profileDir 'err.txt') -Raw
    } else { '' }
    $isWin11 = [Environment]::OSVersion.Version.Build -ge 22000
    $frameLine = ($frameLog -split "`n" | Where-Object { $_ -match 'window frame' } |
        Select-Object -Last 1)
    if ($frameLine) { Write-Host ("      " + $frameLine.Trim()) }

    $wr = New-Object W+RECT
    $cr = New-Object W+RECT
    [void][W]::GetWindowRect($hwnd, [ref]$wr)
    [void][W]::GetClientRect($hwnd, [ref]$cr)

    $origin = New-Object W+POINT
    [void][W]::ClientToScreen($hwnd, [ref]$origin)
    $topOffset = $origin.y - $wr.top

    # The chrome metrics are 96-DPI based; the real window may be 120/144.
    $dpi = [W]::GetDpiForWindow($hwnd)
    if ($dpi -eq 0) { $dpi = 96 }
    $scale = $dpi / 96.0
    $btn = [int][math]::Round(46 * $scale)   # ui_theme window_btn_w
    $cy = [int][math]::Round(20 * $scale)    # tab-strip vertical center
    $closeX = $cr.right - [int]($btn / 2)
    $maxX = $cr.right - $btn - [int]($btn / 2)
    $minX = $cr.right - 2 * $btn - [int]($btn / 2)
    $tabX = [int][math]::Round(120 * $scale)
    $captionX = [int]($cr.right / 2)

    function Hit([int]$x, [int]$y) {
        $pt = New-Object W+POINT
        $pt.x = $x
        $pt.y = $y
        [void][W]::ClientToScreen($hwnd, [ref]$pt)
        $lp = [IntPtr](($pt.y -shl 16) -bor ($pt.x -band 0xFFFF))
        return [int][W]::SendMessage($hwnd, 0x0084, [IntPtr]::Zero, $lp)  # WM_NCHITTEST
    }

    function NcClick([int]$x, [int]$y, [int]$hitCode) {
        $pt = New-Object W+POINT
        $pt.x = $x
        $pt.y = $y
        [void][W]::ClientToScreen($hwnd, [ref]$pt)
        $screenLp = [IntPtr](($pt.y -shl 16) -bor ($pt.x -band 0xFFFF))
        $clientLp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
        [void][W]::SendMessage($hwnd, 0x00A1, [IntPtr]$hitCode, $screenLp)  # WM_NCLBUTTONDOWN
        [void][W]::SendMessage($hwnd, 0x0202, [IntPtr]::Zero, $clientLp)     # WM_LBUTTONUP
        Start-Sleep -Milliseconds 250
    }

    $checks = [ordered]@{}
    $checks['client-origin-top'] = ($topOffset -eq 0)
    $checks['client-size'] = ($cr.right -gt [int](600 * $scale) -and
                              $cr.bottom -gt [int](500 * $scale))
    $checks['frame-border'] = (-not $isWin11) -or
        ($frameLog -match 'window frame dark=[01] border=#[0-9a-fA-F]{6} applied=1')
    $checks['hit-close'] = ((Hit $closeX $cy) -eq 20)
    $checks['hit-max'] = ((Hit $maxX $cy) -eq 9)
    $checks['hit-min'] = ((Hit $minX $cy) -eq 8)
    $checks['hit-tab'] = ((Hit $tabX $cy) -eq 1)
    $checks['hit-caption'] = ((Hit $captionX $cy) -eq 2)

    NcClick $maxX $cy 9
    $checks['click-max'] = [W]::IsZoomed($hwnd)

    [void][W]::GetClientRect($hwnd, [ref]$cr)
    $closeX = $cr.right - [int]($btn / 2)
    $maxX = $cr.right - $btn - [int]($btn / 2)
    $minX = $cr.right - 2 * $btn - [int]($btn / 2)
    NcClick $maxX $cy 9
    $checks['click-restore'] = -not [W]::IsZoomed($hwnd)

    [void][W]::GetClientRect($hwnd, [ref]$cr)
    $closeX = $cr.right - [int]($btn / 2)
    $maxX = $cr.right - $btn - [int]($btn / 2)
    $minX = $cr.right - 2 * $btn - [int]($btn / 2)
    NcClick $minX $cy 8
    $checks['click-min'] = [W]::IsIconic($hwnd)

    # The close button is tested on a fresh hidden instance: a minimized
    # window has no usable client rect for hit-testing the chrome.
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Wait-Process -Id $proc.Id -Timeout 5 -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300

    $proc = Start-Process -FilePath $exe -PassThru -WorkingDirectory $root `
        -ArgumentList @('--userdata', $profileDir, 'about:blank') `
        -RedirectStandardOutput (Join-Path $profileDir 'close-out.txt') `
        -RedirectStandardError (Join-Path $profileDir 'close-err.txt')

    $hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 50 -and $hwnd -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 100
        $hwnd = [W]::FindByClass('AgaveShellWindow')
    }
    if ($hwnd -eq [IntPtr]::Zero) { throw 'AgaveShellWindow not found (close case)' }
    Start-Sleep -Milliseconds 500
    [void][W]::GetClientRect($hwnd, [ref]$cr)

    foreach ($key in $checks.Keys) {
        $ok = $checks[$key]
        if (-not $ok) { $failed++ }
        Write-Host ("{0}  {1}" -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $key)
    }

    Write-Host ("window={0}x{1} client={2}x{3} topOffset={4}" -f `
        ($wr.right - $wr.left), ($wr.bottom - $wr.top), $cr.right, $cr.bottom, $topOffset)

    # Close last: the click must actually end the process.
    [void][W]::GetClientRect($hwnd, [ref]$cr)
    $closeX = $cr.right - [int]($btn / 2)
    NcClick $closeX $cy 20
    Start-Sleep -Milliseconds 500
    $closed = $proc.HasExited
    if (-not $closed) { $failed++ }
    Write-Host ("{0}  click-close" -f ($(if ($closed) { 'PASS' } else { 'FAIL' })))

    if ($failed -ne 0) { exit 1 }
    Write-Host 'FRAME=PASS'
    exit 0
} finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    if ($null -eq $savedHidden) { Remove-Item Env:AGB_SHELL_HIDDEN -ErrorAction SilentlyContinue }
    else { $env:AGB_SHELL_HIDDEN = $savedHidden }
    Remove-Item -Recurse -Force $profileDir -ErrorAction SilentlyContinue
}
