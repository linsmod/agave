/* agave_view_host - headless test host that drives agave through macross.
 *
 * It opens a URL in a view backed by an in-memory BGRA buffer, pumps Win32
 * messages plus macross_event_dispatch() (which fires the engine's timers and
 * therefore the network pump), prints the load result and dumps a BMP.
 *
 * This is the harness behind the perf/probe scripts: the only output channel a
 * page has is its title, so probes write their results into document.title and
 * read them from the "title :" line below.
 *
 * Usage:  agave_view_host.exe <url> [out.bmp] [--eval script] [--click x y]
 *                              [--type text] [--keys keys] [--repaint]
 *                              [--wheel n] [--wheelup n]
 *           --wheel n   n wheel-down events (scrolls the document down)
 *           --wheelup n n wheel-up events (scrolls the document up)
 * Env:    AGB_MAX_FRAMES  settle-loop frame budget (default 800)
 *         AGB_IDLE_FRAMES consecutive repaint-less frames that count as
 *                         "settled" once the load reached 100% (default 60);
 *                         0 disables the idle break, so the loop runs until
 *                         finished or AGB_MAX_FRAMES - use that when timing a
 *                         page that goes quiet between phases
 *         AGB_WAIT_MS     keep pumping until the load finishes or this many ms
 *                         elapse (needed for remote pages: they do not repaint
 *                         while requests are in flight)
 *         AGB_FRAME_LOG   print frames that took longer than 100 ms
 *         AGB_USERDATA    profile root (cookies/localStorage/caches); defaults
 *                         to %LOCALAPPDATA%\Agave\view-host so renders never
 *                         touch the GUI profile
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "macross.h"

namespace {

volatile int g_frames = 0;
volatile long long g_updateTicks = 0;
volatile long long g_dispatchTicks = 0;
volatile int g_finished = 0;
volatile int g_error = 0;
char         g_title[256] = { 0 };
char         g_location[512] = { 0 };
int          g_progress = -1;

void onInvalidate(MaCrossView*, const MC_RECT*) { g_frames++; }
void onTitle(MaCrossView*, const char* t)
{
    if (t) {
        strncpy(g_title, t, sizeof(g_title) - 1);
        g_title[sizeof(g_title) - 1] = '\0';
    }
}
void onLocation(MaCrossView*, const char* u)
{
    if (u) {
        strncpy(g_location, u, sizeof(g_location) - 1);
        g_location[sizeof(g_location) - 1] = '\0';
    }
}
void onProgress(MaCrossView*, unsigned int p, MC_BOOL finish)
{
    g_progress = (int)p;
    if (finish)
        g_finished = 1;
}
void onError(int err, const char*) { g_error = err; }

bool writeBmp(const char* path, const unsigned char* bgra, int w, int h)
{
    FILE* f = fopen(path, "wb");
    int rowSize = ((w * 3) + 3) & ~3;
    int dataSize = rowSize * h;
    unsigned char header[54] = { 0 };

    if (!f)
        return false;

    header[0] = 'B'; header[1] = 'M';
    *(int*)&header[2] = 54 + dataSize;
    *(int*)&header[10] = 54;
    *(int*)&header[14] = 40;
    *(int*)&header[18] = w;
    *(int*)&header[22] = h;
    *(short*)&header[26] = 1;
    *(short*)&header[28] = 24;
    *(int*)&header[34] = dataSize;

    fwrite(header, 1, 54, f);

    {
        unsigned char* row = (unsigned char*)malloc(rowSize);
        for (int y = h - 1; y >= 0; --y) {
            const unsigned char* src = bgra + (size_t)y * w * 4;
            memset(row, 0, rowSize);
            for (int x = 0; x < w; ++x) {
                row[x * 3 + 0] = src[x * 4 + 0];
                row[x * 3 + 1] = src[x * 4 + 1];
                row[x * 3 + 2] = src[x * 4 + 2];
            }
            fwrite(row, 1, rowSize, f);
        }
        free(row);
    }

    fclose(f);
    return true;
}

} /* namespace */

int main(int argc, char** argv)
{
    /* Headless renderer: give it its own profile root so a render pass never
     * reads or writes the cookies/cache a GUI session uses. $AGB_USERDATA wins;
     * otherwise %LOCALAPPDATA%\Agave\view-host. */
    {
        static char env[MAX_PATH + 32];

        if (!getenv("AGB_USERDATA")) {
            const char* base = getenv("LOCALAPPDATA");

            if (!base || !base[0])
                base = getenv("TEMP");

            if (base && base[0]) {
                _snprintf(env, sizeof(env), "AGB_USERDATA=%s\\Agave\\view-host", base);
                env[sizeof(env) - 1] = 0;
                putenv(env);
            }
        }
    }

    const char* url = (argc > 1) ? argv[1] : "https://example.com";
    const char* out = (argc > 2) ? argv[2] : "agave_view.bmp";
    const int W = 800, H = 600;
    MC_CALLBACK_INFO info;
    MaCrossView* view;
    unsigned char* buffer;
    unsigned long long colored = 0;

    memset(&info, 0, sizeof(info));
    info.cb_invalidate_rect  = onInvalidate;
    info.cb_set_title        = onTitle;
    info.cb_set_location     = onLocation;
    info.cb_loading_progress = onProgress;
    info.cb_error            = onError;

    macross_set_callback(&info);

    if (macross_initialize(PIXEL_FORMAT_BGRA32, W, H) != MC_STATUS_SUCCESS) {
        printf("macross_initialize failed\n");
        return 1;
    }

    buffer = (unsigned char*)calloc((size_t)W * H * 4, 1);
    if (!buffer) {
        printf("out of memory\n");
        return 1;
    }

    view = macross_view_create(buffer, W, H, W * 4, NULL);
    if (!view) {
        printf("macross_view_create failed\n");
        macross_shutdown();
        return 1;
    }

    printf("loading %s\n", url);
    if (macross_view_open_url(view, url) != MC_STATUS_SUCCESS) {
        printf("open_url failed\n");
    }

    /* optional interaction: --click X Y  --type TEXT  --enter */
    {
        int clickX = -1, clickY = -1;
        int click2X = -1, click2Y = -1;
        int wheelClicks = 0;
        int wheelUpClicks = 0;
        const char* typeText = 0;
        const char* evalScript = 0;
        const char* keysText = 0;
        bool forceRect = false;
        bool pressEnter = false;

        for (int i = 3; i < argc; ++i) {
            if (!strcmp(argv[i], "--click") && i + 2 < argc) {
                clickX = atoi(argv[i + 1]);
                clickY = atoi(argv[i + 2]);
                i += 2;
            } else if (!strcmp(argv[i], "--type") && i + 1 < argc) {
                typeText = argv[i + 1];
                ++i;
            } else if (!strcmp(argv[i], "--enter")) {
                pressEnter = true;
            } else if (!strcmp(argv[i], "--click2") && i + 2 < argc) {
                click2X = atoi(argv[i + 1]);
                click2Y = atoi(argv[i + 2]);
                i += 2;
            } else if (!strcmp(argv[i], "--repaint")) {
                forceRect = true;
            } else if (!strcmp(argv[i], "--wheel") && i + 1 < argc) {
                wheelClicks = atoi(argv[i + 1]);
                ++i;
            } else if (!strcmp(argv[i], "--wheelup") && i + 1 < argc) {
                wheelUpClicks = atoi(argv[i + 1]);
                ++i;
            } else if (!strcmp(argv[i], "--keys") && i + 1 < argc) {
                keysText = argv[i + 1];
                ++i;
            } else if (!strcmp(argv[i], "--eval") && i + 1 < argc) {
                evalScript = argv[i + 1];
                ++i;
            }
        }

        if (clickX >= 0 || typeText || keysText || evalScript || pressEnter || forceRect || wheelClicks > 0 || wheelUpClicks > 0) {
            /* let the page settle first */
            for (int i = 0; i < 120; ++i) {
                MSG msg;
                while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
                macross_event_dispatch();
                macross_view_update(view, NULL);
                Sleep(10);
            }

            printf("focus -> %d\n", (int)macross_view_set_focus(view));

            if (clickX >= 0) {
                MC_MOUSE_EVENT ev;
                memset(&ev, 0, sizeof(ev));
                ev.type   = EVT_MOUSE_DOWN;
                ev.button = MOUSE_BTN_LEFT;
                ev.point.x = clickX;
                ev.point.y = clickY;
                macross_mouse_event(view, &ev);
                ev.type = EVT_MOUSE_UP;
                macross_mouse_event(view, &ev);
                printf("clicked (%d,%d)\n", clickX, clickY);
            }

            if (typeText) {
                MC_STATUS st = macross_view_input_text(view, typeText);
                printf("typed '%s' -> %d\n", typeText, (int)st);
            }

            if (keysText) {
                /* type through the real key pipeline (keydown/keypress/keyup) */
                const char* c = keysText;
                for (; *c; ++c) {
                    MC_KEY_EVENT kev;
                    memset(&kev, 0, sizeof(kev));
                    kev.type = EVT_KEY_DOWN;
                    kev.key  = (MC_VIRTUAL_KEY)(unsigned char)toupper((unsigned char)*c);
                    macross_keyboard_event(view, &kev);
                    kev.type = EVT_KEY_UP;
                    macross_keyboard_event(view, &kev);
                    Sleep(15);
                }
                printf("keyed '%s'\n", keysText);
            }

            if (click2X >= 0) {
                MC_MOUSE_EVENT ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = EVT_MOUSE_DOWN;
                ev.button = MOUSE_BTN_LEFT;
                ev.point.x = click2X;
                ev.point.y = click2Y;
                macross_mouse_event(view, &ev);
                ev.type = EVT_MOUSE_UP;
                macross_mouse_event(view, &ev);
                printf("clicked2 (%d,%d)\n", click2X, click2Y);
            }

            if (wheelClicks > 0 || wheelUpClicks > 0) {
                /* Wheel events drive the real document scroll (ScrollView ->
                 * scrollBackingStore), the same path a scrollbar drag takes. */
                MC_MOUSE_EVENT ev;
                memset(&ev, 0, sizeof(ev));
                ev.type   = EVT_MOUSE_WHEEL;
                ev.button = wheelClicks > 0 ? MOUSE_WHEEL_DOWN : MOUSE_WHEEL_UP;
                ev.point.x = W / 2;
                ev.point.y = H / 2;
                int clicks = wheelClicks > 0 ? wheelClicks : wheelUpClicks;
                for (int i = 0; i < clicks; ++i) {
                    macross_mouse_event(view, &ev);
                    MSG msg;
                    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&msg);
                        DispatchMessage(&msg);
                    }
                    macross_event_dispatch();
                    macross_view_update(view, NULL);
                    Sleep(20);
                }
                printf("wheel x%d (button=%d)\n", clicks, (int)ev.button);
            }

            if (forceRect) {
                MC_RECT rc;
                rc.x = 0; rc.y = 0; rc.w = W; rc.h = H;
                macross_view_update(view, &rc);
                printf("forced full repaint\n");
            }

            if (evalScript) {
                printf("eval -> %d\n", (int)macross_view_eval_script(view, evalScript));
            }

            if (pressEnter) {
                MC_KEY_EVENT kev;
                memset(&kev, 0, sizeof(kev));
                kev.type = EVT_KEY_DOWN;
                kev.key  = KEY_ENTER;
                macross_keyboard_event(view, &kev);
                kev.type = EVT_KEY_UP;
                macross_keyboard_event(view, &kev);
                printf("pressed enter\n");
            }
        }
    }

    {
        /* Settle loop: stop as soon as the page has been idle for a while
         * (no invalidations) instead of always burning 800 frames. */
        int lastFrames = -1;
        int idle = 0;
        int frame = 0;

        int maxFrames = getenv("AGB_MAX_FRAMES") ? atoi(getenv("AGB_MAX_FRAMES")) : 800;
        if (maxFrames <= 0) maxFrames = 800;

        /* A remote page can be idle (no repaints) while requests are still in
         * flight, which used to make the host quit before the document even
         * finished loading. AGB_WAIT_MS keeps it pumping until the load finishes
         * or the deadline passes. */
        int waitMs = getenv("AGB_WAIT_MS") ? atoi(getenv("AGB_WAIT_MS")) : 0;
        DWORD settleStart = GetTickCount();

        /* How many consecutive frames without a repaint count as "settled".
         * 0 (or negative) disables the idle break: the loop then runs until the
         * load finishes or AGB_MAX_FRAMES is exhausted, which is what you want
         * when timing a page that stays quiet between phases. */
        int idleFrames = getenv("AGB_IDLE_FRAMES") ? atoi(getenv("AGB_IDLE_FRAMES")) : 60;
        bool idleEnabled = idleFrames > 0;

        if (getenv("AGB_FRAME_LOG"))
            printf("settle   : maxFrames=%d idleFrames=%d waitMs=%d\n",
                   maxFrames, idleEnabled ? idleFrames : 0, waitMs);

        for (int i = 0; i < maxFrames; ++i) {
            MSG msg;
            frame = i;

            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            LARGE_INTEGER t0, t1, freq;
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&t0);
            macross_event_dispatch();
            QueryPerformanceCounter(&t1);
            g_updateTicks += (t1.QuadPart - t0.QuadPart);
            if (getenv("AGB_FRAME_LOG")) {
                double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
                if (ms > 100.0)
                    printf("frame %d: %.0f ms (nodes=%d)\n", i, ms, 0);
            }
            macross_view_update(view, NULL);
            Sleep(10);

            if ((int)g_frames != lastFrames) {
                lastFrames = (int)g_frames;
                idle = 0;
            } else {
                ++idle;
            }

            /* Settle: stop when the load finished; otherwise only give up after
             * the page has been idle for AGB_IDLE_FRAMES frames AND the load
             * reached 100%. A page that is still fetching does not repaint, and
             * quitting on that idle truncated the load - remote pages then
             * "finished" at 74% with no tests run. */
            if (i > 40) {
                if (g_finished)
                    break;
                if (waitMs > 0) {
                    if ((long)(GetTickCount() - settleStart) > (long)waitMs)
                        break;
                } else if (idleEnabled && idle > idleFrames && g_progress >= 100) {
                    break;
                }
            }
        }

        if (getenv("AGB_FRAME_LOG"))
            printf("settle   : stopped at frame %d (idle=%d progress=%d finished=%d)\n",
                   frame, idle, g_progress, g_finished);
    }

    for (int i = 0; i < W * H; ++i) {
        unsigned char b = buffer[i * 4 + 0];
        unsigned char g = buffer[i * 4 + 1];
        unsigned char r = buffer[i * 4 + 2];
        if ((r | g | b) != 0)
            colored++;
    }

    printf("title    : %s\n", g_title[0] ? g_title : "(none)");
    printf("location : %s\n", g_location[0] ? g_location : "(none)");
    printf("progress : %d (finished=%d)\n", g_progress, g_finished);
    printf("frames   : %d\n", (int)g_frames);
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        printf("update   : %.1f ms total\n", (double)g_updateTicks * 1000.0 / (double)freq.QuadPart);
        printf("dispatch : %.1f ms total\n", (double)g_dispatchTicks * 1000.0 / (double)freq.QuadPart);
    }
    printf("error    : %d\n", (int)g_error);
    printf("pixels   : %llu / %d non-black\n", colored, W * H);

    if (writeBmp(out, buffer, W, H))
        printf("saved    : %s\n", out);

    macross_view_destroy(view);
    macross_shutdown();
    free(buffer);

    return (g_finished && !g_error) ? 0 : 2;
}
