/*
 *  This file is part of the KDE libraries
 *  Copyright (C) 2005 Apple Computer, Inc.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"
#include "FastMalloc.h"
#include <stdlib.h>
#if PLATFORM(WIN32)
#include "windows.h"
#define strdup _strdup 
#endif
#include <string.h>
#include <stdio.h>

#if PLATFORM(WIN32)
#include <unordered_set>

/*
 * Optional guard-page allocator (debug tool, off by default).
 *
 * AGB_GUARD_MALLOC=<spec> routes WTF allocations through page-aligned
 * VirtualAlloc regions with an inaccessible page after the payload, and
 * *unmaps* the region on free. That turns the two bugs a normal heap hides
 * into an immediate access violation at the guilty instruction:
 *
 *   - write/read after free  -> the address is no longer mapped
 *   - write past the end     -> the guard page
 *
 * The faulting stack is then the writer, not a later allocation that happens
 * to notice a corrupted free block (R62's debug-heap message).
 *
 * This is the ASan/PageHeap substitute for this toolchain: zig ships no
 * compiler-rt for windows-gnu, and Application Verifier / gflags need
 * elevation.
 *
 * Spec: "all"       every WTF allocation (slow, exhaustive)
 *       "512-2048"  only sizes in [512, 2048] (targeted, cheap)
 *       "0" / unset off
 *
 * Only WTF::fastMalloc/fastFree/fastRealloc go through it, so the scope is
 * WebCore containers (Vector, ...): where the R62 corruption lives.
 */
struct GuardHeader {
    unsigned long long magic;
    void*              mapBase;
    size_t             mapSize;
    size_t             userSize;
};

static const unsigned long long kGuardMagic = 0x4147424744524454ull; /* "AGBGDRDT" */

static bool             g_guardInit;
static bool             g_guardOn;
static size_t           g_guardLo;
static size_t           g_guardHi;
static CRITICAL_SECTION g_guardLock;
static std::unordered_set<void*>* g_guardSet;

static void guardInit()
{
    if (g_guardInit)
        return;

    g_guardInit = true;
    InitializeCriticalSection(&g_guardLock);
    g_guardSet = new std::unordered_set<void*>();

    const char* spec = getenv("AGB_GUARD_MALLOC");

    if (!spec || !spec[0] || !strcmp(spec, "0") || !strcmp(spec, "off"))
        return;

    if (!strcmp(spec, "all") || !strcmp(spec, "1")) {
        g_guardLo = 0;
        g_guardHi = (size_t)-1;
    } else {
        unsigned long long lo = 0, hi = 0;

        if (sscanf(spec, "%llu-%llu", &lo, &hi) != 2 || hi < lo)
            return;

        g_guardLo = (size_t)lo;
        g_guardHi = (size_t)hi;
    }

    g_guardOn = true;
    fprintf(stderr, "[guard] WTF fastMalloc guard pages on, sizes %llu..%llu\n",
            (unsigned long long)g_guardLo, (unsigned long long)g_guardHi);
}

static bool guardWanted(size_t n)
{
    return g_guardOn && n >= g_guardLo && n <= g_guardHi;
}

static GuardHeader* guardHeader(void* p)
{
    return (GuardHeader*)((char*)p - ((sizeof(GuardHeader) + 15u) & ~(size_t)15u));
}

static void* guardAlloc(size_t n)
{
    SYSTEM_INFO si;

    GetSystemInfo(&si);

    const size_t page = si.dwPageSize ? si.dwPageSize : 4096;
    const size_t payload = (n + 15u) & ~(size_t)15u;
    const size_t header = (sizeof(GuardHeader) + 15u) & ~(size_t)15u;
    const size_t dataBytes = ((payload + header + page - 1) / page) * page;
    const size_t mapSize = dataBytes + page; /* trailing guard page */
    char* base = (char*)VirtualAlloc(NULL, mapSize, MEM_RESERVE, PAGE_NOACCESS);

    if (!base)
        return NULL;

    if (!VirtualAlloc(base, dataBytes, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(base, 0, MEM_RELEASE);
        return NULL;
    }

    char* user = base + dataBytes - payload;
    GuardHeader* h = guardHeader(user);

    h->magic = kGuardMagic;
    h->mapBase = base;
    h->mapSize = mapSize;
    h->userSize = n;

    EnterCriticalSection(&g_guardLock);
    g_guardSet->insert(user);
    LeaveCriticalSection(&g_guardLock);

    return user;
}

static bool guardOwns(void* p)
{
    bool owned = false;

    EnterCriticalSection(&g_guardLock);
    owned = g_guardSet->find(p) != g_guardSet->end();
    if (owned)
        g_guardSet->erase(p);
    LeaveCriticalSection(&g_guardLock);

    return owned;
}

static void guardRelease(void* p)
{
    GuardHeader* h = guardHeader(p);

    if (h->magic != kGuardMagic) {
        /* The registry said it is ours, so the header was overwritten:
         * fail loudly instead of unmapping a wild pointer. */
        fprintf(stderr, "[guard] bad header for %p (magic %llx)\n", p, h->magic);
        return;
    }

    h->magic = 0;
    /* Unmap completely: any later access (read or write) faults at the
     * instruction that does it. */
    VirtualFree(h->mapBase, 0, MEM_RELEASE);
}
#endif /* PLATFORM(WIN32) */

using namespace std;

namespace WTF {
    
void *fastMalloc(size_t n) 
{
#if PLATFORM(WIN32)
    if (!g_guardInit)
        guardInit();
    if (guardWanted(n)) {
        void* p = guardAlloc(n);
        if (p)
            return p;
    }
#endif
    return malloc(n);
}

char *fastStrdup(const char *str)
{
    return strdup(str);
}

void *fastCalloc(size_t n_elements, size_t element_size)
{
#if PLATFORM(WIN32)
    if (g_guardOn) {
        size_t n = n_elements * element_size;
        void* p = fastMalloc(n);
        if (p)
            memset(p, 0, n);
        return p;
    }
#endif
    return calloc(n_elements, element_size);
}

void fastFree(void* p)
{
    if (!p)
        return;

#if PLATFORM(WIN32)
    if (!g_guardInit)
        guardInit();
    if (g_guardOn && guardOwns(p)) {
        guardRelease(p);
        return;
    }
#endif
    free(p);
}

void *fastRealloc(void* p, size_t n)
{
    if (!p)
        return fastMalloc(n);

#if PLATFORM(WIN32)
    if (!g_guardInit)
        guardInit();
    if (g_guardOn && guardOwns(p)) {
        GuardHeader* h = guardHeader(p);
        size_t old = h->userSize;
        void* q = fastMalloc(n);

        if (!q)
            return NULL;
        memcpy(q, p, old < n ? old : n);
        guardRelease(p);
        return q;
    }
#endif
    return realloc(p, n);
}

} // namespace WTF
