/*
 * AgbCrash.cpp - crash reports with environment info and resolved symbols.
 *
 * See AgbCrash.h. Two rules drive the implementation:
 *
 *  1. The handler must never load a library or take the loader lock: dbghelp.dll
 *     is loaded and the module symbols are registered at install time, so a
 *     crash path only calls functions that are already in memory.
 *  2. The report has to be useful without a debugger. That means the modules
 *     (base/size/PDB), the OS/CPU/RAM, the AGB_* environment, the command line
 *     and every thread's stack, each frame resolved to `module!function+off
 *     (file:line)` when a PDB is sitting next to the module.
 */

#include "config.h"

#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "AgbCrash.h"

namespace {

const int kMaxModules = 96;

struct ModuleEntry {
    unsigned long long base;
    unsigned long long size;
    char path[MAX_PATH];
    int hasPdb;
};

struct CrashState {
    char tag[64];
    char info[512];
    AgbCrashSink sink;
    void* sinkUser;
    bool installed;
    unsigned long symbols;
    ModuleEntry modules[kMaxModules];
    int moduleCount;
};

CrashState g_crash;
volatile LONG g_fastWritten = 0;
volatile LONG g_fullWritten = 0;

/* dbghelp, resolved once at install time. */
struct DbgHelp {
    HMODULE lib;
    decltype(&SymInitializeW) initialize;
    decltype(&SymSetOptions) setOptions;
    decltype(&SymLoadModuleExW) loadModule;
    decltype(&SymFromAddrW) fromAddr;
    decltype(&SymGetLineFromAddrW64) lineFromAddr;
    decltype(&SymFunctionTableAccess64) functionTable;
    decltype(&SymGetModuleBase64) moduleBase;
    decltype(&SymCleanup) cleanup;
    decltype(&StackWalk64) stackWalk;
    bool ready;
};

DbgHelp g_dbg;

/* A fixed buffer: the crash path cannot allocate. */
struct Buffer {
    char* data;
    unsigned int cap;
    unsigned int len;
    bool truncated;
};

void bufAppend(Buffer* b, const char* text)
{
    unsigned int n;

    if (!text || !text[0])
        return;

    n = (unsigned int)strlen(text);
    if (b->len + n + 1 >= b->cap) {
        if (b->cap > b->len + 1) {
            n = b->cap - b->len - 2;
            memcpy(b->data + b->len, text, n);
            b->len += n;
            b->data[b->len] = 0;
        }
        b->truncated = true;
        return;
    }

    memcpy(b->data + b->len, text, n);
    b->len += n;
    b->data[b->len] = 0;
}

void bufPrintf(Buffer* b, const char* fmt, ...)
{
    char line[1024];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = 0;
    bufAppend(b, line);
}

const char* BaseName(const char* path)
{
    const char* slash = strrchr(path, '\\');
    const char* fwd = strrchr(path, '/');

    if (fwd && (!slash || fwd > slash))
        slash = fwd;

    return slash ? slash + 1 : path;
}

/* "C:\x\libagave.dll" -> "C:\x\libagave.pdb", 1 when that file exists. */
bool PdbNextTo(const char* modulePath, char* out, size_t cap)
{
    const char* dot = strrchr(modulePath, '.');

    if (!dot)
        return false;

    if ((size_t)(dot - modulePath) + 5 >= cap)
        return false;

    memcpy(out, modulePath, (size_t)(dot - modulePath));
    strcpy(out + (dot - modulePath), ".pdb");

    {
        DWORD attrs = GetFileAttributesA(out);

        return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    }
}

void CollectModules()
{
    HANDLE snap;
    MODULEENTRY32W me;

    g_crash.moduleCount = 0;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                    GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
        return;

    memset(&me, 0, sizeof(me));
    me.dwSize = sizeof(me);

    if (Module32FirstW(snap, &me)) {
        do {
            ModuleEntry* e;

            if (g_crash.moduleCount >= kMaxModules)
                break;

            e = &g_crash.modules[g_crash.moduleCount++];
            e->base = (unsigned long long)me.modBaseAddr;
            e->size = (unsigned long long)me.modBaseSize;
            WideCharToMultiByte(CP_UTF8, 0, me.szExePath, -1, e->path,
                                (int)sizeof(e->path), NULL, NULL);
            e->path[sizeof(e->path) - 1] = 0;
            e->hasPdb = 0;
        } while (Module32NextW(snap, &me));
    }

    CloseHandle(snap);
}

const ModuleEntry* ModuleForAddress(unsigned long long address)
{
    int i;

    for (i = 0; i < g_crash.moduleCount; ++i) {
        const ModuleEntry* e = &g_crash.modules[i];

        if (address >= e->base && address < e->base + e->size)
            return e;
    }

    return 0;
}

void OsVersionString(char* out, size_t cap)
{
    typedef LONG (WINAPI* RtlGetVersionFn)(OSVERSIONINFOEXW*);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    OSVERSIONINFOEXW os;
    const char* arch = "x86";
    SYSTEM_INFO si;

    memset(&os, 0, sizeof(os));
    os.dwOSVersionInfoSize = sizeof(os);

    if (ntdll) {
        RtlGetVersionFn fn = (RtlGetVersionFn)GetProcAddress(ntdll, "RtlGetVersion");

        if (!fn || fn(&os) != 0)
            memset(&os, 0, sizeof(os));
    }

    GetNativeSystemInfo(&si);
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
        arch = "x64";
    else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64)
        arch = "arm64";

    snprintf(out, cap, "Windows %lu.%lu.%lu %s", (unsigned long)os.dwMajorVersion,
             (unsigned long)os.dwMinorVersion, (unsigned long)os.dwBuildNumber, arch);
}

const char* ExceptionName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:            return "BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
    default:                              return "UNKNOWN";
    }
}

bool IsFatalCode(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return true;
    default:
        return false;
    }
}

/* ---- symbolization ------------------------------------------------------ */

void LoadDbgHelp()
{
    char searchPath[MAX_PATH * 3];

    if (g_dbg.ready)
        return;

    memset(&g_dbg, 0, sizeof(g_dbg));

    g_dbg.lib = LoadLibraryA("dbghelp.dll");
    if (!g_dbg.lib)
        return;

    g_dbg.initialize    = (decltype(&SymInitializeW))GetProcAddress(g_dbg.lib, "SymInitializeW");
    g_dbg.setOptions    = (decltype(&SymSetOptions))GetProcAddress(g_dbg.lib, "SymSetOptions");
    g_dbg.loadModule    = (decltype(&SymLoadModuleExW))GetProcAddress(g_dbg.lib, "SymLoadModuleExW");
    g_dbg.fromAddr      = (decltype(&SymFromAddrW))GetProcAddress(g_dbg.lib, "SymFromAddrW");
    g_dbg.lineFromAddr  = (decltype(&SymGetLineFromAddrW64))GetProcAddress(g_dbg.lib, "SymGetLineFromAddrW64");
    g_dbg.functionTable = (decltype(&SymFunctionTableAccess64))GetProcAddress(g_dbg.lib, "SymFunctionTableAccess64");
    g_dbg.moduleBase    = (decltype(&SymGetModuleBase64))GetProcAddress(g_dbg.lib, "SymGetModuleBase64");
    g_dbg.cleanup       = (decltype(&SymCleanup))GetProcAddress(g_dbg.lib, "SymCleanup");
    g_dbg.stackWalk     = (decltype(&StackWalk64))GetProcAddress(g_dbg.lib, "StackWalk64");

    if (!g_dbg.initialize || !g_dbg.fromAddr || !g_dbg.setOptions)
        return;

    {
        /* The modules' own directories plus an explicit search path: a PDB is
         * expected next to its binary (the build puts it there), and
         * AGB_SYMBOL_PATH can point at a symbol cache for older builds. */
        const char* extra = getenv("AGB_SYMBOL_PATH");
        char dir[MAX_PATH];

        searchPath[0] = 0;

        if (g_crash.moduleCount > 0) {
            snprintf(dir, sizeof(dir), "%s", g_crash.modules[0].path);
            {
                char* slash = strrchr(dir, '\\');

                if (slash)
                    slash[1] = 0;
            }
            lstrcatA(searchPath, dir);
        }

        if (extra && extra[0]) {
            lstrcatA(searchPath, ";");
            lstrcatA(searchPath, extra);
        }

        {
            char cwd[MAX_PATH];

            if (GetCurrentDirectoryA((DWORD)sizeof(cwd), cwd) > 0) {
                lstrcatA(searchPath, ";");
                lstrcatA(searchPath, cwd);
            }
        }
    }

    {
        wchar_t widePath[MAX_PATH * 3];

        widePath[0] = 0;
        if (searchPath[0])
            MultiByteToWideChar(CP_UTF8, 0, searchPath, -1, widePath, MAX_PATH * 3);

        if (!g_dbg.initialize(GetCurrentProcess(), widePath[0] ? widePath : NULL, FALSE))
            return;
    }

    g_dbg.setOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES |
                     SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);

    {
        int i;

        for (i = 0; i < g_crash.moduleCount; ++i) {
            ModuleEntry* e = &g_crash.modules[i];
            char pdb[MAX_PATH];
            wchar_t wide[MAX_PATH];

            e->hasPdb = PdbNextTo(e->path, pdb, sizeof(pdb)) ? 1 : 0;

            /* Only modules that carry a PDB are registered: loading symbols for
             * every system DLL would cost seconds at startup. */
            if (!e->hasPdb)
                continue;

            MultiByteToWideChar(CP_UTF8, 0, e->path, -1, wide, MAX_PATH);
            if (g_dbg.loadModule(GetCurrentProcess(), NULL, wide, NULL,
                                 (DWORD64)e->base, (DWORD)e->size, NULL, 0))
                g_crash.symbols++;
        }
    }

    g_dbg.ready = true;
}

/* `address` -> "module!function+0x1f (src/file.cpp:123)" or "module+0x...". */
void FormatFrame(Buffer* b, int index, unsigned long long address)
{
    const ModuleEntry* mod = ModuleForAddress(address);
    char line[768];
    int written = 0;

    line[0] = 0;

    if (mod) {
        written = snprintf(line, sizeof(line), "  #%02d %s+0x%llx", index,
                           BaseName(mod->path), address - mod->base);
    } else {
        written = snprintf(line, sizeof(line), "  #%02d 0x%llx", index, address);
    }

    if (g_dbg.ready) {
        char symbolBuf[sizeof(SYMBOL_INFOW) + 256 * sizeof(wchar_t)];
        SYMBOL_INFOW* symbol = (SYMBOL_INFOW*)symbolBuf;
        DWORD64 displacement = 0;

        memset(symbolBuf, 0, sizeof(symbolBuf));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
        symbol->MaxNameLen = 256;

        if (g_dbg.fromAddr(GetCurrentProcess(), (DWORD64)address, &displacement, symbol)) {
            char name[256];
            char file[MAX_PATH];
            DWORD lineNumber = 0;

            WideCharToMultiByte(CP_UTF8, 0, symbol->Name, -1, name, sizeof(name), NULL, NULL);
            name[sizeof(name) - 1] = 0;

            written = snprintf(line, sizeof(line), "  #%02d %s!%s+0x%llx", index,
                               mod ? BaseName(mod->path) : "?", name,
                               (unsigned long long)displacement);

            {
                IMAGEHLP_LINEW64 src;

                memset(&src, 0, sizeof(src));
                src.SizeOfStruct = sizeof(src);

                if (g_dbg.lineFromAddr && g_dbg.lineFromAddr(GetCurrentProcess(), (DWORD64)address,
                                                             &lineNumber, &src)) {
                    WideCharToMultiByte(CP_UTF8, 0, src.FileName, -1, file, sizeof(file), NULL, NULL);
                    file[sizeof(file) - 1] = 0;

                    if (written > 0 && written < (int)sizeof(line))
                        snprintf(line + written, sizeof(line) - (size_t)written, " (%s:%lu)",
                                 file, (unsigned long)lineNumber);
                }
            }
        } else if (mod && mod->hasPdb) {
            if (written > 0 && written < (int)sizeof(line))
                snprintf(line + written, sizeof(line) - (size_t)written, "  (not in PDB)");
        }
    }

    bufAppend(b, line);
    bufAppend(b, "\n");
}

void WalkThread(Buffer* b, HANDLE thread, DWORD tid, bool faulting,
                CONTEXT* context)
{
    bufPrintf(b, "\nthread %lu%s:\n", (unsigned long)tid, faulting ? " (faulting)" : "");

    if (g_dbg.ready && g_dbg.stackWalk && context) {
        STACKFRAME64 frame;
        CONTEXT ctx = *context;
        int index = 0;

        memset(&frame, 0, sizeof(frame));
#if defined(_M_X64) || defined(__x86_64__)
        frame.AddrPC.Offset = ctx.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
#else
        frame.AddrPC.Offset = ctx.Eip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Ebp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Esp;
        frame.AddrStack.Mode = AddrModeFlat;
#endif

        for (;;) {
            BOOL ok = g_dbg.stackWalk(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(),
                                      thread, &frame, &ctx, NULL,
                                      g_dbg.functionTable, g_dbg.moduleBase, NULL);

            if (!ok || !frame.AddrPC.Offset)
                break;

            FormatFrame(b, index++, frame.AddrPC.Offset);
            if (index >= 48)
                break;
        }

        if (index > 0)
            return;
    }

    /* Fallback: the faulting thread only (CaptureStackBackTrace on another
     * thread would dump ours). */
    if (faulting) {
        void* frames[64];
        unsigned short count = CaptureStackBackTrace(1, 48, frames, NULL);
        unsigned short i;

        for (i = 0; i < count; ++i)
            FormatFrame(b, i, (unsigned long long)frames[i]);
    }
}

void FormatHeader(Buffer* b, EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : NULL;
    SYSTEM_INFO si;
    MEMORYSTATUSEX mem;
    SYSTEMTIME local;
    char os[128];
    char cwd[MAX_PATH];

    GetLocalTime(&local);
    GetNativeSystemInfo(&si);
    memset(&mem, 0, sizeof(mem));
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);

    bufPrintf(b, "agave crash report\n");
    bufPrintf(b, "tag: %s\n", g_crash.tag[0] ? g_crash.tag : "?");
    bufPrintf(b, "time: %04u-%02u-%02u %02u:%02u:%02u.%03u\n",
              local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute,
              local.wSecond, local.wMilliseconds);
    bufPrintf(b, "pid: %lu  tid: %lu\n", (unsigned long)GetCurrentProcessId(),
              (unsigned long)GetCurrentThreadId());

    if (rec) {
        bufPrintf(b, "exception: 0x%08lx (%s) addr=0x%llx\n",
                  (unsigned long)rec->ExceptionCode, ExceptionName(rec->ExceptionCode),
                  (unsigned long long)rec->ExceptionAddress);

        if ((rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
             rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
            rec->NumberParameters >= 2) {
            const char* kind = rec->ExceptionInformation[0] == 0 ? "read" :
                               rec->ExceptionInformation[0] == 1 ? "write" :
                               rec->ExceptionInformation[0] == 8 ? "execute" : "?";

            bufPrintf(b, "fault: %s 0x%llx\n", kind,
                      (unsigned long long)rec->ExceptionInformation[1]);
        }
    } else {
        bufPrintf(b, "exception: none (report requested)\n");
    }

    if (g_crash.info[0])
        bufPrintf(b, "context: %s\n", g_crash.info);

    if (GetCurrentDirectoryA((DWORD)sizeof(cwd), cwd) > 0)
        bufPrintf(b, "cwd: %s\n", cwd);

    bufPrintf(b, "cmdline: %s\n", GetCommandLineA());

    OsVersionString(os, sizeof(os));
    bufPrintf(b, "os: %s  cpus=%lu  ram=%llu MB\n", os,
              (unsigned long)si.dwNumberOfProcessors,
              (unsigned long long)(mem.ullTotalPhys / (1024 * 1024)));

    bufPrintf(b, "symbols: %s (%lu modules with a PDB)\n",
              g_dbg.ready ? "dbghelp" : "unavailable", g_crash.symbols);
}

void FormatModules(Buffer* b)
{
    int i;

    bufAppend(b, "\nmodules:\n");

    for (i = 0; i < g_crash.moduleCount; ++i) {
        const ModuleEntry* e = &g_crash.modules[i];
        char version[64] = "";
        char pdb[MAX_PATH];
        DWORD handle = 0;

        (void)version;
        (void)handle;

        bufPrintf(b, "  %-24s base=0x%llx size=0x%llx pdb=%s\n",
                  BaseName(e->path), e->base, e->size,
                  PdbNextTo(e->path, pdb, sizeof(pdb)) ? BaseName(pdb) : "(none)");
    }
}

void FormatEnvironment(Buffer* b)
{
    wchar_t* env = GetEnvironmentStringsW();
    wchar_t* p;
    int shown = 0;

    if (!env)
        return;

    bufAppend(b, "\nenv:\n");

    for (p = env; *p; ) {
        size_t len = wcslen(p);
        char line[MAX_PATH * 2] = "";

        WideCharToMultiByte(CP_UTF8, 0, p, -1, line, sizeof(line), NULL, NULL);
        line[sizeof(line) - 1] = 0;

        if (!_strnicmp(line, "AGB_", 4) || !_strnicmp(line, "AGAVE", 5)) {
            bufPrintf(b, "  %s\n", line);
            if (++shown >= 40) {
                bufAppend(b, "  ... (more AGB_* vars omitted)\n");
                break;
            }
        }

        p += len + 1;
    }

    if (!shown)
        bufAppend(b, "  (no AGB_* variables set)\n");

    FreeEnvironmentStringsW(env);
}

unsigned int FormatReport(char* out, unsigned int cap, EXCEPTION_POINTERS* ep, int full)
{
    Buffer b;
    DWORD pid = GetCurrentProcessId();
    DWORD self = GetCurrentThreadId();

    if (!out || cap < 256)
        return 0;

    out[0] = 0;
    b.data = out;
    b.cap = cap;
    b.len = 0;
    b.truncated = false;

    FormatHeader(&b, ep);
    FormatModules(&b);
    FormatEnvironment(&b);

    if (ep && ep->ContextRecord) {
        WalkThread(&b, GetCurrentThread(), self, true, ep->ContextRecord);
    } else {
        WalkThread(&b, GetCurrentThread(), self, true, NULL);
    }

    if (full) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);

        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te;

            memset(&te, 0, sizeof(te));
            te.dwSize = sizeof(te);

            if (Thread32First(snap, &te)) {
                do {
                    HANDLE thread;
                    CONTEXT ctx;

                    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
                        continue;

                    if (b.truncated)
                        break;

                    thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                        THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                    if (!thread)
                        continue;

                    SuspendThread(thread);
                    memset(&ctx, 0, sizeof(ctx));
                    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;

                    if (GetThreadContext(thread, &ctx))
                        WalkThread(&b, thread, te.th32ThreadID, false, &ctx);

                    ResumeThread(thread);
                    CloseHandle(thread);
                } while (Thread32Next(snap, &te));
            }

            CloseHandle(snap);
        }
    }

    if (b.truncated)
        bufAppend(&b, "\n... report truncated (buffer full)\n");

    return b.len;
}

void Emit(const char* text, unsigned int len)
{
    if (g_crash.sink)
        g_crash.sink(text, len, g_crash.sinkUser);

    /* Also the last thing stderr gets; a host with no sink still sees it. */
    if (len) {
        fwrite(text, 1, len, stderr);
        fflush(stderr);
    }
}

void Handle(EXCEPTION_POINTERS* ep, int full)
{
    static char report[24 * 1024];
    unsigned int len = FormatReport(report, sizeof(report), ep, full);

    if (len)
        Emit(report, len);
}

LONG WINAPI CrashVeh(EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : NULL;

    if (!rec || !IsFatalCode(rec->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;

    /* First chance: the faulting thread only, so a crash inside the faulting
     * thread's own data cannot turn this into a second crash. */
    if (InterlockedExchange(&g_fastWritten, 1) == 0)
        Handle(ep, 0);

    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    /* The process is being terminated now: this is the last chance to walk
     * every thread (the faulting thread's stack is often not enough). */
    if (InterlockedExchange(&g_fullWritten, 1) == 0)
        Handle(ep, 1);

    return EXCEPTION_CONTINUE_SEARCH;
}

} /* namespace */

extern "C" {

AGB_CRASH_API void agbCrashInstall(const char* appTag, AgbCrashSink sink, void* user)
{
    if (g_crash.installed)
        return;

    g_crash.installed = true;
    g_crash.sink = sink;
    g_crash.sinkUser = user;
    g_crash.tag[0] = 0;
    g_crash.info[0] = 0;
    g_crash.symbols = 0;

    if (appTag) {
        strncpy(g_crash.tag, appTag, sizeof(g_crash.tag) - 1);
        g_crash.tag[sizeof(g_crash.tag) - 1] = 0;
    }

    CollectModules();
    LoadDbgHelp();

    AddVectoredExceptionHandler(1, CrashVeh);
    SetUnhandledExceptionFilter(CrashFilter);
}

AGB_CRASH_API void agbCrashSetInfo(const char* info)
{
    g_crash.info[0] = 0;

    if (info) {
        strncpy(g_crash.info, info, sizeof(g_crash.info) - 1);
        g_crash.info[sizeof(g_crash.info) - 1] = 0;
    }
}

AGB_CRASH_API unsigned int agbCrashFormat(char* out, unsigned int cap, EXCEPTION_POINTERS* ep, int full)
{
    return FormatReport(out, cap, ep, full);
}

AGB_CRASH_API unsigned int agbCrashSymbolCount(void)
{
    return g_crash.symbols;
}

} /* extern "C" */
