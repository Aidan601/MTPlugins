#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include "log.h"

namespace
{
    FILE*      g_file = nullptr;
    std::mutex g_mutex;

    void WriteLine(const char* prefix, const char* line)
    {
        SYSTEMTIME st; GetLocalTime(&st);
        char stamp[64];
        _snprintf_s(stamp, _TRUNCATE, "[%02d:%02d:%02d.%03d t%05lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());

        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file) { fputs(stamp, g_file); if (prefix) { fputs(prefix, g_file); fputc(' ', g_file); } fputs(line, g_file); fputc('\n', g_file); fflush(g_file); }
        OutputDebugStringA(prefix ? prefix : "[RadiantPlugins]"); OutputDebugStringA(" "); OutputDebugStringA(line); OutputDebugStringA("\n");
    }

    void FormatW(const char* prefix, const wchar_t* fmt, va_list ap)
    {
        wchar_t wbuf[2048];
        _vsnwprintf_s(wbuf, _TRUNCATE, fmt, ap);
        char buf[4096];
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, sizeof(buf), nullptr, nullptr);
        WriteLine(prefix, buf);
    }
}

namespace rtclog
{
    void Init(const std::wstring& logPath)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // _SH_DENYNO: the log stays readable while open
        if (!g_file) g_file = _wfsopen(logPath.c_str(), L"a", _SH_DENYNO);
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file) { fclose(g_file); g_file = nullptr; }
    }

    void Printf(const char* fmt, ...)
    {
        va_list ap; va_start(ap, fmt);
        VPrintfPrefixed(nullptr, fmt, ap);
        va_end(ap);
    }

    void PrintfW(const wchar_t* fmt, ...)
    {
        va_list ap; va_start(ap, fmt);
        FormatW(nullptr, fmt, ap);
        va_end(ap);
    }

    void VPrintfPrefixed(const char* prefix, const char* fmt, va_list ap)
    {
        char buf[2048];
        _vsnprintf_s(buf, _TRUNCATE, fmt, ap);
        WriteLine(prefix, buf);
    }

    void VPrintfPrefixedW(const char* prefix, const wchar_t* fmt, va_list ap) { FormatW(prefix, fmt, ap); }
}
