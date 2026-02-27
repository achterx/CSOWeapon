#include "logger.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>

static HANDLE g_hLog = INVALID_HANDLE_VALUE;

void Log(const char* fmt, ...)
{
    if (g_hLog == INVALID_HANDLE_VALUE)
        g_hLog = CreateFileA("csnz_weapons.log", GENERIC_WRITE,
                             FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    char buf[2048];
    va_list va; va_start(va, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    if (n > 0 && g_hLog != INVALID_HANDLE_VALUE)
    { DWORD w; WriteFile(g_hLog, buf, n, &w, nullptr); FlushFileBuffers(g_hLog); }
}
