// Quiet Steam start: see steamstart.h.
#include "steamstart.h"
#include "modtools_plugin.hpp"
#include <windows.h>

namespace steamstart
{
namespace
{
    std::wstring g_bat;          // <game>\modtools_launcher.bat
    std::string g_state = "not checked";

    const char kMarker[] = "rem Written by Launcher+";

    // Treyarch's file byte for byte (CRLF on disk)
    const char kStock[] = R"BAT(@echo off

SET FOLDER=%~dp0

IF %FOLDER:~-1% NEQ \ (SET FOLDER="%FOLDER%\")
SET FOLDER=%FOLDER:"=%

SET TA_GAME_PATH=%FOLDER%
SET TA_LOCAL_ASSET_CACHE=%FOLDER%share\assetconvert\
SET TA_TOOLS_PATH=%FOLDER%

setx TA_GAME_PATH "%TA_GAME_PATH%\"
setx TA_LOCAL_ASSET_CACHE "%TA_LOCAL_ASSET_CACHE%\"
setx TA_TOOLS_PATH "%TA_TOOLS_PATH%\"

"%TA_TOOLS_PATH%"\bin\modlauncher.exe
)BAT";

    // @ is replaced by the colon line. Same variables and values as the stock file, setx only on a mismatch.
    const char kQuiet[] = R"BAT(@echo off
rem Written by Launcher+: sets the TA_ variables only when they differ, starts the launcher and closes this window.
rem The colon line ends a cmd that is still running the previous file of this name.
goto main
@
exit
:main
set "F=%~dp0"
set "TA_GAME_PATH=%F%"
set "TA_LOCAL_ASSET_CACHE=%F%share\assetconvert\"
set "TA_TOOLS_PATH=%F%"
set N=0
for /f "tokens=1,2,*" %%a in ('reg query HKCU\Environment 2^>nul') do (
if /i "%%a"=="TA_GAME_PATH" if /i "%%c"=="%F%" set /a N+=1
if /i "%%a"=="TA_LOCAL_ASSET_CACHE" if /i "%%c"=="%F%share\assetconvert\" set /a N+=1
if /i "%%a"=="TA_TOOLS_PATH" if /i "%%c"=="%F%" set /a N+=1
)
if %N%==3 goto run
setx TA_GAME_PATH "%F%\" >nul
setx TA_LOCAL_ASSET_CACHE "%F%share\assetconvert\\" >nul
setx TA_TOOLS_PATH "%F%\" >nul
:run
start "" "%F%bin\modlauncher.exe"
exit
)BAT";

    std::string Crlf(const char* text)
    {
        std::string out;
        for (const char* c = text; *c; ++c)
        {
            if (*c == '\n') out += '\r';
            out += *c;
        }
        return out;
    }

    std::string QuietText()
    {
        std::string out = Crlf(kQuiet);
        const size_t at = out.find('@', 2);
        if (at != std::string::npos) out.replace(at, 1, std::string(2048, ':'));
        return out;
    }

    std::string NoCr(const std::string& s)
    {
        std::string out;
        for (char c : s) if (c != '\r') out += c;
        return out;
    }

    bool ReadAll(const std::wstring& path, std::string& out)
    {
        const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size = {};
        bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1 << 20);
        if (ok)
        {
            out.resize(static_cast<size_t>(size.QuadPart));
            DWORD got = 0;
            ok = out.empty() || (::ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
        }
        CloseHandle(h);
        return ok;
    }

    // temp file plus rename, so cmd never reads a half written file
    bool WriteAtomic(const std::wstring& path, const std::string& text)
    {
        const std::wstring tmp = path + L".launcherplus.tmp";
        const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD put = 0;
        const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &put, nullptr) && put == text.size();
        CloseHandle(h);
        if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        DeleteFileW(tmp.c_str());
        return false;
    }

    enum class Kind { Missing, Stock, Ours, Other };

    Kind Classify(std::string& text)
    {
        if (!ReadAll(g_bat, text)) return Kind::Missing;
        if (NoCr(text) == kStock) return Kind::Stock;
        if (text.find(kMarker) != std::string::npos) return Kind::Ours;
        return Kind::Other;
    }

    // Writes ours over the stock file or an older version of ours. A file edited by the user is left alone.
    void Apply()
    {
        std::string text;
        const Kind kind = Classify(text);
        if (kind == Kind::Missing) { g_state = "modtools_launcher.bat not found"; return; }
        if (kind == Kind::Other) { g_state = "modtools_launcher.bat was changed by someone else, left alone"; rp::Log("Quiet Steam start: %s", g_state.c_str()); return; }
        const std::string want = QuietText();
        if (text == want) { g_state = "quiet file in place"; return; }
        if (WriteAtomic(g_bat, want))
        {
            g_state = "quiet file written";
            rp::Log("Quiet Steam start: %s (%ls)", g_state.c_str(), g_bat.c_str());
        }
        else
        {
            g_state = "could not write modtools_launcher.bat (error " + std::to_string(GetLastError()) + ")";
            rp::Log("Quiet Steam start: %s", g_state.c_str());
        }
    }
}

    void Setup(const std::wstring& bin)
    {
        std::wstring game = bin;
        const size_t slash = game.find_last_of(L'\\');
        if (slash != std::wstring::npos) game.resize(slash);
        g_bat = game + L"\\modtools_launcher.bat";
        Apply();
    }

    std::string Status() { return "Quiet Steam start: " + g_state; }
}
