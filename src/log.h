#pragma once
#include <string>
#include <cstdarg>

// Thread-safe logger: <dll dir>\RadiantPlugins.log and OutputDebugString.
namespace rtclog
{
    void Init(const std::wstring& logPath);
    void Shutdown();
    void Printf(const char* fmt, ...);
    void PrintfW(const wchar_t* fmt, ...);
    void VPrintfPrefixed(const char* prefix, const char* fmt, va_list ap);
    void VPrintfPrefixedW(const char* prefix, const wchar_t* fmt, va_list ap);
}

#define LOG(...)  ::rtclog::Printf(__VA_ARGS__)
#define LOGW(...) ::rtclog::PrintfW(__VA_ARGS__)
