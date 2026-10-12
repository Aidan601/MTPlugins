#pragma once
// Reads a source xanim export (.xanim_bin): bone names and per-frame model-space transforms.
#include <string>
#include <vector>

namespace ba::xanimbin
{
struct Pose { float o[3] = {0, 0, 0}; float axis[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; bool set = false; };   // origin, X Y Z axes
struct Export
{
    std::vector<std::string> bones;              // lower case
    std::vector<std::vector<Pose>> frames;       // frames[frame][bone]
};
bool Load(const std::string& path, Export& out, std::string& err);
}
