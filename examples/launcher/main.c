/*
 * agave_launcher.exe - taskbar-pinnable launcher for the agave browser shell.
 *
 * Windows cannot pin a .bat (or a shortcut to one) to the taskbar, and
 * double-clicking agave_shell.exe goes through Explorer -> ShellExecute, which
 * may hit SmartScreen for this unsigned build. This tiny GUI process finds
 * agave_shell.exe next to itself, starts it with --backend process (one
 * agave_render_host.exe per tab, falling back to local when the renderer is
 * missing) and forwards every argument it received.
 *
 * CreateProcess is used directly, so SmartScreen's shell check never sees the
 * shell. Pin agave.exe itself; the launcher exits as soon as the shell starts.
 *
 * Set AGAVE_LAUNCHER_WAIT=1 to wait for the shell and return its exit code
 * (used by the probe smoke test in tools/run_shell_launcher_tests.ps1).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <wchar.h>

static void dir_of(const wchar_t *path, wchar_t *out, size_t count)
{
    wcsncpy(out, path, count);
    out[count - 1] = 0;
    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash)
        slash[1] = 0;
    else
        out[0] = 0;
}

static const wchar_t *args_after_program(const wchar_t *cmdline)
{
    const wchar_t *p = cmdline;
    if (*p == L'"') {
        p++;
        while (*p && *p != L'"')
            p++;
        if (*p)
            p++;
    } else {
        while (*p && *p != L' ' && *p != L'\t')
            p++;
    }
    while (*p == L' ' || *p == L'\t')
        p++;
    return p;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR cmdline, int show)
{
    (void)instance;
    (void)previous;
    (void)cmdline;
    (void)show;

    wchar_t self[MAX_PATH];
    if (!GetModuleFileNameW(NULL, self, MAX_PATH)) {
        MessageBoxW(NULL, L"Cannot resolve the launcher path.", L"agave", MB_ICONERROR);
        return 1;
    }

    wchar_t dir[MAX_PATH];
    dir_of(self, dir, MAX_PATH);

    wchar_t shell[MAX_PATH];
    wchar_t renderer[MAX_PATH];
    _snwprintf(shell, MAX_PATH, L"%sagave_shell.exe", dir);
    shell[MAX_PATH - 1] = 0;
    _snwprintf(renderer, MAX_PATH, L"%sagave_render_host.exe", dir);
    renderer[MAX_PATH - 1] = 0;

    if (GetFileAttributesW(shell) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL,
                    L"agave_shell.exe was not found next to this launcher.\n"
                    L"Build it first:  pwsh -File tools\\build_project.ps1",
                    L"agave", MB_ICONERROR);
        return 1;
    }

    const wchar_t *backend =
        (GetFileAttributesW(renderer) != INVALID_FILE_ATTRIBUTES) ? L"process" : L"local";
    const wchar_t *rest = args_after_program(GetCommandLineW());

    size_t need = wcslen(shell) + wcslen(backend) + wcslen(rest) + 64;
    wchar_t *command = (wchar_t *)calloc(need, sizeof(wchar_t));
    if (!command)
        return 1;
    _snwprintf(command, need, L"\"%s\" --backend %s %s", shell, backend, rest);
    command[need - 1] = 0;

    wchar_t cwd[MAX_PATH];
    dir_of(shell, cwd, MAX_PATH); /* the shell lives in the build dir; run there */

    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));

    if (!CreateProcessW(shell, command, NULL, NULL, FALSE, 0, NULL, cwd, &startup, &process)) {
        wchar_t message[512];
        _snwprintf(message, 512, L"Could not start:\n%s\n\nError %lu", command, GetLastError());
        message[511] = 0;
        MessageBoxW(NULL, message, L"agave", MB_ICONERROR);
        free(command);
        return 1;
    }

    DWORD exit_code = 0;
    if (GetEnvironmentVariableW(L"AGAVE_LAUNCHER_WAIT", NULL, 0) > 0) {
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &exit_code);
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    free(command);
    return (int)exit_code;
}
