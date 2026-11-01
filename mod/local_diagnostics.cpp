#include "local_diagnostics.h"

#include <mod/amlmod.h>
#include <aml-psdk/game_sa/base/Timer.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace LocalDiagnostics
{
    namespace
    {
        FILE* g_file = nullptr;
        int g_users = 0;
        char g_path[512] = {};
    }

    void Open()
    {
        ++g_users;
        if (g_file || !aml) return;

        const char* dataPath = aml->GetAndroidDataPath();
        if (!dataPath || !dataPath[0]) return;
        const size_t pathLength = std::strlen(dataPath);
        std::snprintf(g_path, sizeof(g_path), "%s%sRubbishRat.log", dataPath,
            pathLength > 0 && (dataPath[pathLength - 1] == '/' ||
                dataPath[pathLength - 1] == '\\') ? "" : "/");
        g_file = std::fopen(g_path, "a");
    }

    void Close()
    {
        if (g_users > 0) --g_users;
        if (g_users == 0 && g_file)
        {
            std::fclose(g_file);
            g_file = nullptr;
        }
    }

    void Write(const char* format, ...)
    {
        if (!g_file || !format) return;
        std::fprintf(g_file, "[%u ms] ", CTimer::GetTimeMS());
        va_list args;
        va_start(args, format);
        std::vfprintf(g_file, format, args);
        va_end(args);
        std::fputc('\n', g_file);
        std::fflush(g_file);
    }
}
