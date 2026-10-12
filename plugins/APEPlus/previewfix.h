#pragma once
// Preview fixes for xanims: useBones 0 (viewmodel) anims without the double bind offset, the AssetViewer Preview
// Options attach that honours the child tag, tolerates swapped tags and plays the anim on the attached model.
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ba::previewfix
{
// Reads [xanimpreview] from APEPlus.ini and detours the converter, the local pose, the attach and the placement. OnLoad.
void Install();
// Puts APE's child tag check back. Plugin OnUnload.
void Shutdown();
// Bracket APE's preview frame function: an attached model's anim is hidden from the frame label loop in between.
void BeforeFrame(void* widget);
void AfterFrame();
// Places a preview entry at xform12 (origin, three axis rows) every frame, null stops it. APE main thread.
void SetFree(void* entry, const float* xform12);
void ClearFree();
// A tag of the entry's model in world space: its bind transform on the model's current placement (origin, three axis rows).
bool TagWorld(void* entry, const char* tag, float* out12);
// Test harness: the entry model's bone names, roots marked, * where `anim` has a part of that name.
std::string BoneList(void* entry, void* animOrNull);
// Test harness: the first bones of the entry model with their parent field.
std::string BoneParents(void* entry);
// While anim `top` plays, the bones it does not move take anim `base` (a pt_ torso anim over a pb_ body anim). Main thread.
bool SetLayer(const std::string& top, const std::string& base);
void ClearLayers();
// ADS as the game's viewmodel tree blends it: per bone, the playing anim at weight 1 (its root bones left out, as the converter
// drops them) and the ADS Up and Down anims at their weights and times (0 to 1). Tracks, when given, replace an ADS anim's
// own (the export's bones local to their nearest exported ancestor, as the linker builds them). Main thread.
struct AdsKey { float q[4]; float v[3]; };
using AdsTracks = std::unordered_map<std::string, std::vector<AdsKey>>;   // lower-case bone name -> keys
bool SetAds(const std::string& up, float upWeight, float upTime, const AdsTracks* upTracks,
            const std::string& down, float downWeight, float downTime, const AdsTracks* downTracks);
void ClearAds();
// The entry model's bones with their parent's name (lower case, "" above the root). Main thread.
bool Hierarchy(void* entry, std::vector<std::pair<std::string, std::string>>& out);
// Handles "xanimpreview [status|on|off|frame <n>]", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
