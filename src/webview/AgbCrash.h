#ifndef AGB_CRASH_H
#define AGB_CRASH_H

/* agb_crash - crash reports with environment info and (when a PDB sits next to
 * the module) resolved symbols.
 *
 * The product this exists for is the process backend: when a renderer dies the
 * control puts a "Page crashed" page in front of the user, and that page has to
 * say something useful about *why*. A plain exit code does not.
 *
 * Usage (both the shell and agave_render_host call it once at startup):
 *
 *   static void onReport(const char* text, unsigned int len, void* user);
 *   agbCrashInstall("render-host", onReport, NULL);
 *   agbCrashSetInfo("ipc=Local\\agave_view_...");
 *
 * The report is delivered through the sink, so the renderer can copy it into
 * the shared section (which outlives the dying process) and the shell can write
 * a file. Symbolization is set up at install time (dbghelp.dll is loaded then),
 * so the handler itself never has to load a library. */

#ifdef _WIN32
#  ifdef _EXPORT_            /* building libagave itself */
#    define AGB_CRASH_API __declspec(dllexport)
#  elif defined(AGW_USE_DLL) /* a host that links the DLL */
#    define AGB_CRASH_API __declspec(dllimport)
#  else
#    define AGB_CRASH_API
#  endif
#else
#  define AGB_CRASH_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Called from the crashing thread with the finished report (NUL-terminated,
 * `len` bytes without the NUL). Must not block, allocate from the CRT heap or
 * throw: the process is already dying. */
typedef void (*AgbCrashSink)(const char* text, unsigned int len, void* user);

/* Installs the first-chance (VEH) and unhandled-exception handlers. `appTag`
 * labels the report ("render-host", "shell", ...). A NULL sink keeps the report
 * on stderr only. */
AGB_CRASH_API void agbCrashInstall(const char* appTag, AgbCrashSink sink, void* user);

/* Extra context lines printed in the report (e.g. the IPC name of a renderer).
 * Copied, so the caller's buffer may go away. */
AGB_CRASH_API void agbCrashSetInfo(const char* info);

/* Builds the report into `out` (bounded, NUL-terminated) and returns its
 * length. `full` = 0 formats only the faulting thread, `full` = 1 walks every
 * thread of the process (slower and uses more of the buffer). Exposed for
 * diagnostics that want to print a stack without crashing. */
AGB_CRASH_API unsigned int agbCrashFormat(char* out, unsigned int cap, EXCEPTION_POINTERS* ep,
                            int full);

/* Number of symbols that resolved (or 0 when no PDB was found). Filled after an
 * install, for hosts that want to warn "this build has no symbols". */
AGB_CRASH_API unsigned int agbCrashSymbolCount(void);

#ifdef __cplusplus
}
#endif

#endif /* AGB_CRASH_H */
