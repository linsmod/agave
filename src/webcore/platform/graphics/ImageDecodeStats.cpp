/*
 * ImageDecodeStats.cpp: frame-local accounting for image decoding.
 *
 * Copyright (C) 2026 agave project
 *
 * See ImageDecodeStats.h for the contract. Everything here is touched from the
 * engine thread only (paint -> RenderImage -> BitmapImage::draw -> ImageSource),
 * which is the same reason no lock is needed.
 */

#include "config.h"
#include "ImageDecodeStats.h"

#if defined(_WIN32) || defined(WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

namespace WebCore {

static double s_decodeMs;
static double s_ticksPerSecond;
static bool s_inFrame;

double imageDecodeStatsNow()
{
#if defined(_WIN32) || defined(WIN32)
    LARGE_INTEGER counter;

    if (s_ticksPerSecond == 0.0) {
        LARGE_INTEGER frequency;

        QueryPerformanceFrequency(&frequency);
        s_ticksPerSecond = (double)frequency.QuadPart;
    }
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / s_ticksPerSecond;
#else
    /* No QPC off Windows; the portable clock is good enough to attribute a
     * multi-hundred-millisecond decode, not to budget 5ms slices. */
    return (double)clock() / (double)CLOCKS_PER_SEC;
#endif
}

void imageDecodeStatsAddMs(double ms)
{
    s_decodeMs += ms;
}

double imageDecodeStatsPeekMs()
{
    return s_decodeMs;
}

void imageDecodeStatsBeginFrame()
{
    s_decodeMs = 0.0;
    s_inFrame = true;
}

void imageDecodeStatsEndFrame()
{
    s_inFrame = false;
}

bool imageDecodeStatsInFrame()
{
    return s_inFrame;
}

}
