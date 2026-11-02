/*
 * NetLog.cpp - see NetLog.h.
 */

#include "config.h"

#include "NetLog.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if PLATFORM(WIN32)
#include <windows.h>
#endif

namespace Net {

namespace {

int      g_state = -1;          /* -1 unknown, 0 disabled, 1 enabled */
FILE*    g_file = NULL;
long long g_t0 = 0;

long long nowTicks()
{
#if PLATFORM(WIN32)
    LARGE_INTEGER counter;

    if (QueryPerformanceCounter(&counter))
        return (long long)counter.QuadPart;

    return (long long)GetTickCount64();
#else
    return (long long)clock();
#endif
}

double ticksPerMs()
{
#if PLATFORM(WIN32)
    LARGE_INTEGER frequency;

    if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart)
        return (double)frequency.QuadPart / 1000.0;

    return 1.0;
#else
    return 1.0;
#endif
}

void init()
{
    const char* env;

    if (g_state >= 0)
        return;

    g_state = 0;
    g_t0 = nowTicks();

    env = getenv("AGB_NET_LOG");

    if (!env || !env[0])
        env = getenv("AGB_NET_DEBUG");

    if (!env || !env[0])
        return;

    g_state = 1;

    /* AGB_NET_LOG may name a file: AGB_NET_LOG=trace.log */
    if (strcmp(env, "1") != 0 && strchr(env, '.') != NULL) {
        g_file = fopen(env, "wb");

        if (g_file)
            fprintf(stderr, "[netlog] writing to %s\n", env);
    }
}

void emit(char* buffer)
{
    if (!g_state)
        return;

    fputs(buffer, stderr);

    if (g_file) {
        fputs(buffer, g_file);
        fflush(g_file);
    }
}

} /* namespace */

bool logEnabled()
{
    init();
    return g_state == 1;
}

void logReset()
{
    init();
    g_t0 = nowTicks();
}

double logElapsedMs()
{
    init();
    return (double)(nowTicks() - g_t0) / ticksPerMs();
}

void logLine(const char* format, ...)
{
    char message[1024];
    char line[1200];
    va_list args;

    init();

    if (!g_state)
        return;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    snprintf(line, sizeof(line), "[netlog] +%8.1fms  %s\n", logElapsedMs(), message);
    emit(line);
}

void logEvent(const char* category, const char* format, ...)
{
    char message[1024];
    char line[1200];
    va_list args;

    init();

    if (!g_state)
        return;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    snprintf(line, sizeof(line), "[netlog] +%8.1fms  %-9s %s\n", logElapsedMs(),
             category ? category : "event", message);
    emit(line);
}

} /* namespace Net */
