<#
.SYNOPSIS
  Screenshot what agave_shell actually paints for a URL.

.DESCRIPTION
  Starts agave_shell with the URL and captures the window with PrintWindow.
  The window is never shown and never activated: screenshots must not disturb
  the user or steal focus. Prefer the headless probe for everything that only
  needs numbers; use this only when the question is "does it LOOK right".

.EXAMPLE
  pwsh -File tools/screenshot.ps1 -Url https://chat.deepseek.com/sign_in `
       -Out debug/qr.png -Wait 16
#>
param(
    [Parameter(Mandatory = $true)][string]$Url,
    [string]$Out = 'debug/shot.png',
    [int]$Wait = 14,
    [int]$Width = 1100,
    [int]$Height = 800
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build-zig-native\agave_shell.exe'
if (-not (Test-Path $exe)) { throw "missing $exe - run tools/build_project.ps1" }
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public struct RECT { public int Left, Top, Right, Bottom; }
public static class ShotWin {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder b, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowDC(IntPtr h);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
    public static IntPtr FindShell(uint pid) {
        // A process can own more than one window with this class (hidden
        // helpers, transient popups); the browser window is the biggest one.
        IntPtr found = IntPtr.Zero;
        int bestArea = 0;
        EnumWindows((h, l) => {
            uint owner; GetWindowThreadProcessId(h, out owner);
            if (owner != pid) return true;
            var sb = new StringBuilder(256); GetClassNameW(h, sb, sb.Capacity);
            if (sb.ToString() != "AgaveShellWindow") return true;
            RECT r; if (!GetWindowRect(h, out r)) return true;
            int area = (r.Right - r.Left) * (r.Bottom - r.Top);
            if (area > bestArea) { bestArea = area; found = h; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@

$proc = Start-Process -FilePath $exe -ArgumentList $Url -PassThru
try {
    Start-Sleep -Seconds $Wait
    $hwnd = [ShotWin]::FindShell([uint32]$proc.Id)
    if ($hwnd -eq [IntPtr]::Zero) { throw "no AgaveShellWindow for pid $($proc.Id)" }

    # Never show or focus the window: the user's desktop must not be disturbed.
    # A hidden window keeps whatever size it was created with, so set the size
    # without activating it and render it with PrintWindow instead of a screen
    # grab (a grab would also capture whatever happens to be on top).
    [ShotWin]::SetWindowPos($hwnd, [IntPtr]::Zero, 0, 0, $Width, $Height,
        0x0002 -bor 0x0004 -bor 0x0010) | Out-Null   # NOMOVE|NOZORDER|NOACTIVATE
    Start-Sleep -Milliseconds 500

    $rect = New-Object RECT
    [ShotWin]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
    $w = $rect.Right - $rect.Left
    $h = $rect.Bottom - $rect.Top

    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    # PW_RENDERFULLCONTENT (2): works while the window is occluded, unlike a
    # screen grab, which captures whatever is on top (task view, other apps).
    $ok = [ShotWin]::PrintWindow($hwnd, $hdc, 2)
    $g.ReleaseHdc($hdc)
    if (-not $ok) {
        $g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bmp.Size)
    }
    $bmp.Save($Out)
    $g.Dispose()
    $bmp.Dispose()
    "shot=$($w)x$($h) -> $Out"
} finally {
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
}
