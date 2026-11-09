/*
 * InternalPages.cpp - the browser's own pages as a loader-level scheme provider.
 *
 * A page is a file next to the executable: agave://downloads -> downloads.html.
 * Serving it from the loader (instead of a special case in WebView::loadUrl)
 * is what makes reload work: a reload re-requests the current URL through
 * MainResourceLoader, which asks this provider for the bytes (R162).
 */

#include "config.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "InternalPages.h"

#include "KURL.h"
#include "PageScheme.h"
#include "SharedBuffer.h"
#include "PlatformString.h"

/* Reads one of the browser's own pages into memory. The name is restricted to
 * [a-z0-9_-] so a crafted URL cannot walk out of the directory. */
PassRefPtr<WebCore::SharedBuffer> agaveInternalPage(const WebCore::KURL& url,
                                                    WebCore::String& mimeType)
{
    using namespace WebCore;

    String protocol = url.protocol();

    if (!equalIgnoringCase(protocol, String("agave")))
        return 0;

    {
        /* KURL keeps the path without its leading slashes: agave://downloads ->
         * "downloads". Query/fragment never reach the file name. */
        /* The page name is whatever follows the scheme: agave://downloads has it
         * in the *host* (a URL with no path), agave://downloads/x in the path.
         * Take the raw string after "agave://" and stop at the first delimiter. */
        CString utf8 = url.url().utf8();
        const char* name = utf8.data();
        const char* sep = strstr(name, "://");

        name = sep ? sep + 3 : name;

        while (name[0] == '/')
            ++name;
        char page[64];
        char file[1024];
        size_t n = 0;
        char* buffer;
        FILE* fp;
        long size;
        size_t read;

        while (name[n] && name[n] != '/' && name[n] != '?' && name[n] != '#') {
            char c = name[n];

            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
                return 0;

            if (n + 1 >= sizeof(page))
                return 0;

            page[n] = c;
            ++n;
        }

        page[n] = 0;

        if (!n)
            return 0;

        {
            DWORD len = GetModuleFileNameA(NULL, file, (DWORD)sizeof(file));

            if (!len || len >= sizeof(file))
                return 0;

            {
                char* slash = strrchr(file, '\\');

                if (!slash)
                    return 0;

                slash[1] = 0;
            }
        }

        strncat(file, page, sizeof(file) - strlen(file) - 1);
        strncat(file, ".html", sizeof(file) - strlen(file) - 1);

        fp = fopen(file, "rb");

        if (!fp) {
            fprintf(stderr, "[agave] internal page %s not found (looked for %s)\n",
                    utf8.data(), file);
            return 0;
        }

        fseek(fp, 0, SEEK_END);
        size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        if (size <= 0) {
            fclose(fp);
            return 0;
        }

        buffer = (char*)malloc((size_t)size);

        if (!buffer) {
            fclose(fp);
            return 0;
        }

        read = fread(buffer, 1, (size_t)size, fp);
        fclose(fp);

        if (!read) {
            free(buffer);
            return 0;
        }

        mimeType = String("text/html");

        {
            /* WebCore::Shared starts its count at 0, so a plain RefPtr takes
             * ownership (adoptRef would leave the count at 0 and the buffer
             * would die with the first release - the crash this path had). */
            RefPtr<SharedBuffer> data = new SharedBuffer(buffer, (int)read);

            free(buffer);
            return data.release();
        }
    }
}

void agaveRegisterInternalPages(void)
{
    WebCore::registerPageScheme("agave", agaveInternalPage);
}
