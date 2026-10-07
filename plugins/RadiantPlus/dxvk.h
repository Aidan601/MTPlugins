#pragma once
// The DXVK (Vulkan) renderer switch (radiantplus.cpp "DXVK toggle"), shown in Preferences > Camera by themes.cpp.

namespace br::dxvk
{
// The setting: DXVK's dxgi.dll + d3d11.dll sit in <root>\bin (this Radiant may still run the other renderer).
bool Setting();
// OK/Apply in Preferences: turn it on or off once Preferences has closed, then offer the restart.
void Request(bool on);
}
