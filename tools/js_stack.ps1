<#
.SYNOPSIS
  Ask a running agave process to write its live JS stack to a file - no debugger,
  no restart, the target keeps running.

.DESCRIPTION
  For a window whose engine thread is stuck ("JS 线程卡住"). Two obvious routes
  are dead ends:

    * the engine prints its stack to stderr, which is unreadable from outside;
    * `cdb -p <pid> -c ".call antjsb!antb_dump_last_js_stack_to_file(..)"` faults
      on the first `_tls_index` load (ant uses __declspec(thread); a .call runs
      with a TLS array the dll never initialised for that thread), and because a
      failed cdb command table skips `qd`, cdb then terminates the debuggee
      (docs/dev-debugging.md).

  So run the export on a *real* thread inside the target instead:
  CreateRemoteThread gives that thread a normal TEB/TLS, and its single argument
  (the path) lands in rcx - exactly the signature of
  `int antb_dump_last_js_stack_to_file(const char* path)`.

  The export address is computed from the local build: the target must load the
  same antjsb.dll as build-zig-native (it does when launched from there).

.EXAMPLE
  pwsh -File tools/js_stack.ps1 -Pid 12345
  # writes debug\jsstack_<stamp>.txt and prints it

.EXAMPLE
  Get-Process agave_shell | ForEach-Object { pwsh -File tools/js_stack.ps1 -Pid $_.Id }
#>
param(
    # Not `$Pid`: that is a read-only automatic variable in PowerShell.
    [Parameter(Mandatory = $true, Position = 0)][Alias('Pid')][int]$ProcessId,
    [string]$Out = '',
    [string]$Dll = '',
    [string]$Export = 'antb_dump_last_js_stack_to_file',
    [int]$TimeoutSec = 20
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if (-not $Dll) { $Dll = Join-Path $root 'build-zig-native\antjsb.dll' }
if (-not (Test-Path $Dll)) { throw "missing $Dll (build it with tools\build_antjsb.ps1)" }
$Dll = (Resolve-Path $Dll).Path

if (-not $Out) {
    $debug = Join-Path $root 'debug'
    if (-not (Test-Path $debug)) { New-Item -ItemType Directory -Path $debug | Out-Null }
    $Out = Join-Path $debug ('jsstack_{0}.txt' -f (Get-Date -Format 'yyyyMMdd_HHmmss'))
}
$Out = [IO.Path]::GetFullPath($Out)
if ($Out.Length -ge 400) { throw "output path too long: $Out" }

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class JsStackNative
{
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr LoadLibraryW(string path);

    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Ansi)]
    public static extern IntPtr GetProcAddress(IntPtr module, string name);

    [DllImport("kernel32", SetLastError = true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);

    [DllImport("kernel32", SetLastError = true)]
    public static extern IntPtr VirtualAllocEx(IntPtr proc, IntPtr addr, UIntPtr size, uint type, uint protect);

    [DllImport("kernel32", SetLastError = true)]
    public static extern bool WriteProcessMemory(IntPtr proc, IntPtr addr, byte[] buf, UIntPtr size, out UIntPtr written);

    [DllImport("kernel32", SetLastError = true)]
    public static extern IntPtr CreateRemoteThread(IntPtr proc, IntPtr attr, UIntPtr stackSize,
                                                   IntPtr start, IntPtr arg, uint flags, out int tid);

    [DllImport("kernel32", SetLastError = true)]
    public static extern bool VirtualFreeEx(IntPtr proc, IntPtr addr, UIntPtr size, uint type);

    [DllImport("kernel32", SetLastError = true)]
    public static extern uint WaitForSingleObject(IntPtr handle, uint ms);

    [DllImport("kernel32", SetLastError = true)]
    public static extern bool GetExitCodeThread(IntPtr handle, out uint code);

    [DllImport("kernel32", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32", SetLastError = true)]
    public static extern IntPtr CreateToolhelp32Snapshot(uint flags, int pid);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct MODULEENTRY32W
    {
        public uint dwSize;
        public uint th32ModuleID;
        public uint th32ProcessID;
        public uint GlblcntUsage;
        public uint ProccntUsage;
        public IntPtr modBaseAddr;
        public uint modBaseSize;
        public IntPtr hModule;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string szModule;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string szExePath;
    }

    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool Module32FirstW(IntPtr snap, ref MODULEENTRY32W entry);

    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool Module32NextW(IntPtr snap, ref MODULEENTRY32W entry);
}
'@ -ErrorAction SilentlyContinue

$PROCESS_ACCESS   = 0x0002 -bor 0x0008 -bor 0x0010 -bor 0x0020 -bor 0x0400  # thread|vmop|vmread|vmwrite|query
$MEM_COMMIT       = 0x1000
$MEM_RESERVE      = 0x2000
$MEM_RELEASE      = 0x8000
$PAGE_READWRITE   = 0x04
$TH32CS_SNAPMODULE = 0x00000008 -bor 0x00000010

# ---------------------------------------------------------------- addresses --
$hLocal = [JsStackNative]::LoadLibraryW($Dll)
if ($hLocal -eq [IntPtr]::Zero) { throw "LoadLibraryW failed for $Dll (Win32 $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))" }
$pLocal = [JsStackNative]::GetProcAddress($hLocal, $Export)
if ($pLocal -eq [IntPtr]::Zero) { throw "export $Export not found in $Dll - rebuild antjsb (tools\build_antjsb.ps1)" }
$rva = [int64]$pLocal - [int64]$hLocal

$snap = [JsStackNative]::CreateToolhelp32Snapshot([uint32]$TH32CS_SNAPMODULE, $ProcessId)
if ($snap -eq [IntPtr]::new(-1)) { throw "cannot snapshot modules of pid ${Pid}: is it alive? Win32 $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
$base = [IntPtr]::Zero
try {
    $me = New-Object JsStackNative+MODULEENTRY32W
    $me.dwSize = [uint32][Runtime.InteropServices.Marshal]::SizeOf([type]'JsStackNative+MODULEENTRY32W')
    $ok = [JsStackNative]::Module32FirstW($snap, [ref]$me)
    while ($ok) {
        if ($me.szModule -ieq 'antjsb.dll') { $base = $me.modBaseAddr; break }
        $ok = [JsStackNative]::Module32NextW($snap, [ref]$me)
    }
} finally { [void][JsStackNative]::CloseHandle($snap) }

if ($base -eq [IntPtr]::Zero) { throw "pid $ProcessId has no antjsb.dll loaded (is it an agave process?)" }
$remoteFn = [IntPtr]::new([int64]$base + $rva)

Write-Host "== js stack"
Write-Host ("  pid      : {0}" -f $ProcessId)
Write-Host ("  antjsb   : {0} base=0x{1:x}" -f $Dll, [int64]$base)
Write-Host ("  function : {0}+0x{1:x} -> 0x{2:x}" -f $Export, $rva, [int64]$remoteFn)
Write-Host ("  output   : {0}" -f $Out)

Remove-Item $Out -Force -ErrorAction SilentlyContinue

# ------------------------------------------------------------------- inject --
$proc = [JsStackNative]::OpenProcess([uint32]$PROCESS_ACCESS, $false, $ProcessId)
if ($proc -eq [IntPtr]::Zero) { throw "OpenProcess($ProcessId) failed: Win32 $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
try {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Out + "`0")
    $mem = [JsStackNative]::VirtualAllocEx($proc, [IntPtr]::Zero, [UIntPtr][uint64]$bytes.Length,
                                           [uint32]($MEM_COMMIT -bor $MEM_RESERVE), [uint32]$PAGE_READWRITE)
    if ($mem -eq [IntPtr]::Zero) { throw "VirtualAllocEx failed: Win32 $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
    try {
        $written = [UIntPtr]::Zero
        if (-not [JsStackNative]::WriteProcessMemory($proc, $mem, $bytes, [UIntPtr][uint64]$bytes.Length, [ref]$written)) {
            throw "WriteProcessMemory failed: Win32 $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
        }

        $tid = 0
        $thread = [JsStackNative]::CreateRemoteThread($proc, [IntPtr]::Zero, [UIntPtr]::Zero,
                                                      $remoteFn, $mem, 0, [ref]$tid)
        if ($thread -eq [IntPtr]::Zero) { throw "CreateRemoteThread failed: Win32 $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
        try {
            $wait = [JsStackNative]::WaitForSingleObject($thread, [uint32]($TimeoutSec * 1000))
            $code = [uint32]259
            [void][JsStackNative]::GetExitCodeThread($thread, [ref]$code)
            if ($wait -ne 0) { Write-Host ("  note: dump thread still running after {0}s (exit code {1})" -f $TimeoutSec, $code) -ForegroundColor Yellow }
            elseif ($code -ne 1) { Write-Host ("  note: dump returned {0} (0 = no engine / fopen failed)" -f $code) -ForegroundColor Yellow }
        } finally { [void][JsStackNative]::CloseHandle($thread) }
    } finally { [void][JsStackNative]::VirtualFreeEx($proc, $mem, [UIntPtr]::Zero, [uint32]$MEM_RELEASE) }
} finally { [void][JsStackNative]::CloseHandle($proc) }

# -------------------------------------------------------------------- report --
if (-not (Test-Path $Out)) {
    # Only the dump-to-file exports produce the file. -Export also lets callers
    # run other void exports (e.g. antb_interrupt_all_engines) through the same
    # remote-thread channel; those must not look like a failure.
    if ($Export -eq 'antb_dump_last_js_stack_to_file' -or $Export -eq 'antb_dump_js_stack_to_file') {
        throw "pid $ProcessId did not write $Out (engine gone, or the VM walk faulted)"
    }
    Write-Host "(no file expected: '$Export' is not a dump-to-file export)" -ForegroundColor DarkGray
    return
}
$lines = @(Get-Content $Out)
Write-Host ("== {0} line(s)" -f $lines.Count) -ForegroundColor Cyan
$lines | ForEach-Object { Write-Host "  $_" }
Write-Host ('file: {0}' -f $Out)
