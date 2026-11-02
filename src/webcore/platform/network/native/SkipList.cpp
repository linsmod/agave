/*
 * SkipList.cpp - see SkipList.h.
 */

#include "config.h"

#include "SkipList.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#if PLATFORM(WIN32)
#include <windows.h>
#endif

namespace Net {

namespace {

std::vector<std::string> g_patterns;
std::string              g_path;
bool                     g_loaded = false;

std::string trim(const std::string& s)
{
    size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();
    size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

void addFile(const std::string& path)
{
    FILE* file = fopen(path.c_str(), "rb");

    if (!file)
        return;

    if (g_path.empty())
        g_path = path;

    {
        char line[1024];

        while (fgets(line, sizeof(line), file)) {
            std::string entry = trim(std::string(line));

            if (entry.empty() || entry[0] == '#')
                continue;

            g_patterns.push_back(entry);
        }
    }

    fclose(file);
}

void load()
{
    if (g_loaded)
        return;

    g_loaded = true;
    g_patterns.clear();
    g_path.clear();

    {
        const char* env = getenv("AGB_SKIPRES");
        if (env && env[0])
            addFile(env);
    }

    addFile("skipres.txt");

    /* next to the executable, for hosts started from another directory */
#if PLATFORM(WIN32)
    {
        char module[MAX_PATH];

        if (GetModuleFileNameA(NULL, module, sizeof(module))) {
            char* slash = strrchr(module, '\\');

            if (slash) {
                *(slash + 1) = 0;

                {
                    std::string besideExe(module);
                    besideExe += "skipres.txt";
                    addFile(besideExe);
                }
            }
        }
    }
#endif
}

} /* namespace */

void reloadSkipList()
{
    g_loaded = false;
    g_patterns.clear();
    g_path.clear();
}

const char* skipListPath()
{
    load();
    return g_path.c_str();
}

bool isSkippedResource(const std::string& url)
{
    load();

    if (g_patterns.empty() || url.empty())
        return false;

    for (size_t i = 0; i < g_patterns.size(); ++i) {
        if (url.find(g_patterns[i]) != std::string::npos)
            return true;
    }
    return false;
}

} /* namespace Net */
