# check_window_visible.ps1 - does a process own any visible top-level window?
#
#   powershell -File tools/check_window_visible.ps1 -ProcessId 1234
#   powershell -File tools/check_window_visible.ps1 -ProcessId 1234 -Json
#
# Guards the headless promise of `agave_shell.exe --probe`: a probe must create
# no visible window (exit 0 = nothing visible), while AGB_PROBE_SHOW=1 must make
# one (exit 1, one VISIBLE line).
param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [switch]$Json
)

Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class WinProbe {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hwnd, StringBuilder buf, int max);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hwnd, StringBuilder buf, int max);

    public static List<string> VisibleWindowsOf(int pid, out int total) {
        var found = new List<string>();
        int count = 0;
        EnumWindows((hwnd, l) => {
            uint owner;
            GetWindowThreadProcessId(hwnd, out owner);
            if (owner != (uint)pid) return true;
            count++;
            var cls = new StringBuilder(256);
            var title = new StringBuilder(256);
            GetClassNameW(hwnd, cls, cls.Capacity);
            GetWindowTextW(hwnd, title, title.Capacity);
            bool visible = IsWindowVisible(hwnd);
            found.Add((visible ? "VISIBLE" : "hidden ") + " " + cls + " [" + title + "]");
            return true;
        }, IntPtr.Zero);
        total = count;
        return found;
    }
}
'@

$total = 0
$windows = [WinProbe]::VisibleWindowsOf($ProcessId, [ref]$total)
$visible = @($windows | Where-Object { $_ -like 'VISIBLE*' })

if ($Json) {
    [pscustomobject]@{
        processId = $ProcessId
        windowCount = $total
        visibleCount = $visible.Count
        windows = $windows
    } | ConvertTo-Json -Compress
} else {
    "process=$ProcessId windows=$total visible=$($visible.Count)"
    $windows | ForEach-Object { "  $_" }
}

if ($visible.Count -gt 0) { exit 1 } else { exit 0 }
