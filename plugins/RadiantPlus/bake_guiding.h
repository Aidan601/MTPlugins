#pragma once
// Light bake improvements shared by RadiantPlus and BuildPlusLightRT.dll (the launcher's Light step):
// adaptive override, cone correction (cone_correct.h), experimental ray guiding (off by default).
#include <cstdint>
#include <string>

namespace br::bakeguide
{
// What the module needs from its host process. Every pointer must be set except running (null = never baking).
struct Env
{
    void* (*address)(std::uintptr_t ida);                              // Radiant_modtools.exe address (IDA, base 0x140000000)
    bool (*detour)(void* target, void* detour, void** original);        // install an enabled detour
    void (*log)(const char* line);
    std::string (*get)(const char* key, const char* fallback);          // the module's config section
    void (*set)(const char* key, const char* value);
    std::uint8_t* (*camWnd)();                                          // CCamWnd* (the lighting manager) or null
    bool (*running)();                                                  // a bake is in progress
};

void Setup(const Env& env);
void Shutdown();
// Handles "bake guide ...", "bake cone ..." and "bake adaptive ..." commands, false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
// One-line state of the adaptive override and cone correction.
std::string Summary();
// Writes the reference-bake switches (free_space_skip, post_smooth, default 1 = Radiant's own) into Radiant's render
// options. Radiant resets them at startup, so a headless export calls this before it snapshots them.
void ApplyRenderOptions();
}
