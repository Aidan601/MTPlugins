// Quiet Steam start: replaces <game>\modtools_launcher.bat with one that skips setx when the TA_ variables match
// and does not keep its cmd window open.
#pragma once
#include <string>

namespace steamstart
{
    void Setup(const std::wstring& bin);   // writes ours on launcher start
    std::string Status();
}
