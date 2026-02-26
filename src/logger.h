#pragma once
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdarg>

inline void Log(const char* fmt, ...) {
    static FILE* f = nullptr;
    if (!f) {
        f = fopen("frostbite_fix.log", "a");
        if (!f) return;
        setvbuf(f, nullptr, _IONBF, 0);
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
}
