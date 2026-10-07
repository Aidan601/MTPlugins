#pragma once
// "Draft" in front of Low in the launcher's Light quality box.
#include <string>

namespace lightquality
{
void Setup(const std::wstring& ini, bool (*lightRouted)());   // OnLoad
void Tick();                                                   // adds or removes the entry, records the pick in the ini
void Shutdown();                                               // OnUnload: the box gets its three stock entries back
std::string Status();
}
