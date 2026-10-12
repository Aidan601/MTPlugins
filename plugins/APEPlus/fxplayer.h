#pragma once
// A small .efx player for APE's preview (APE has no fx runtime): sprite, line and tail elements with their graphs, atlas
// and runners, drawn over the preview at Present.
#include <string>
#include <vector>

namespace ba::fxplayer
{
struct Xform { float o[3]; float a[9]; };   // row vectors: world = p * a + o

// material -> its colorMap's source image (relative to the BO3 root), its materialType and its atlas grid, false when unknown
using Resolver = bool (*)(const std::string& material, std::string& image, std::string& materialType, int& cols, int& rows);
void SetResolver(Resolver r);
// Hooks IDXGISwapChain::Present. OnLoad, after the APE address checks.
bool Install();
// Unhooks nothing (the host removes detours), releases the GPU objects. Plugin OnUnload.
void Shutdown();
// Plays <root>\share\raw\<path> (an .efx) on anchor `anchor`. APE main thread.
void Play(const std::string& path, int anchor);
void StopAll();
// The anchor's transform for this frame (tag_flash of the gun). APE main thread.
void SetAnchor(int anchor, const Xform& x);
// The preview camera of this frame (camera origin, three axes, tan of the half height and width) and its window.
void SetCamera(const float origin[3], const float forward[3], const float right[3], const float up[3], float tanY, float tanX, void* hwnd);
// Advances the particles and builds the sprite list for the next Present. APE main thread, once per preview frame.
void Update();
bool Active();
// The model elements alive this frame (shell ejects): xmodel and world transform. Drawn by the caller. Main thread.
struct ModelPart { std::string model; Xform x; };
const std::vector<ModelPart>& Models();
// Every xmodel an effect (and its runners) can spawn.
std::vector<std::string> ModelNames(const std::string& path);
// Test harness: particle time runs at this rate (1 = real time).
void SetTimeScale(double s);
std::string Dump();
std::string DepthReport();
void ProbeDepth(int anchor);
std::string Status();
}
