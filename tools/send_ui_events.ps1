# send_ui_events.ps1 - post synthetic mouse/key messages to the webview control
# so the [ui] logging in agave_shell can be exercised without a human at the mouse.
#
#   powershell -File tools/send_ui_events.ps1 -ProcessId <gui agave_shell pid>
#
# Prints the control handle it found and exits 1 when the control is missing
# (the probe runs headless: there is no control, so use a GUI run).
param(
    [Parameter(Mandatory = $true)][int]$ProcessId
)

Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class UiSend {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hwnd, StringBuilder buf, int max);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);

    public static IntPtr FindControl(int pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((top, l) => {
            uint owner;
            GetWindowThreadProcessId(top, out owner);
            if (owner != (uint)pid) return true;
            EnumChildWindows(top, (child, l2) => {
                var cls = new StringBuilder(256);
                GetClassNameW(child, cls, cls.Capacity);
                if (cls.ToString() == "AgaveWebViewControl") { found = child; return false; }
                return true;
            }, IntPtr.Zero);
            return found == IntPtr.Zero;
        }, IntPtr.Zero);
        return found;
    }
}
'@

$hwnd = [UiSend]::FindControl($ProcessId)
if ($hwnd -eq [IntPtr]::Zero) { "control window not found for pid $ProcessId"; exit 1 }
"control=$hwnd"

function Post([uint32]$msg, [int]$w, [int]$l) {
    [UiSend]::PostMessageW($hwnd, $msg, [IntPtr]$w, [IntPtr]$l) | Out-Null
    Start-Sleep -Milliseconds 120
}

# WM_MOUSEMOVE=0x200, WM_LBUTTONDOWN=0x201, WM_LBUTTONUP=0x202, WM_MOUSEWHEEL=0x20A,
# WM_KEYDOWN=0x100, WM_KEYUP=0x101, WM_CHAR=0x102
Post 0x200 0 ((40 -shl 16) -bor 30)        # move (only logged with AGB_UI_DEBUG)
Post 0x201 0 ((40 -shl 16) -bor 30)        # left down at 30,40
Post 0x202 0 ((40 -shl 16) -bor 30)        # left up
Post 0x20A (120 -shl 16) ((40 -shl 16) -bor 30)   # wheel up
Post 0x100 0x41 0                          # 'A' key down
Post 0x101 0x41 0                          # 'A' key up
Post 0x102 0x42 0                          # 'B' char
"posted 7 messages"
exit 0
