#pragma once
// CPU ray-guiding primitives, independently implemented from Silvennoinen and Sloan (2019).
// No engine pointers or global mutable state. One Training/Guide pair belongs to one worker.
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace br::guiding
{
constexpr double Pi = 3.14159265358979323846;
constexpr unsigned MaxDepth = 6;
constexpr std::size_t NodeCount = 5461; // Complete 4-ary tree, depths 0 through 6.
struct Vec3 { double x = 0, y = 0, z = 0; };
struct Point { double u = 0, v = 0; };
struct Sample { Vec3 direction; double pdf = 0; std::uint32_t id = 0; };
struct Receiver { Vec3 position, normal; std::uint32_t id = 0; };
struct Cluster { std::vector<std::size_t> receivers; };

Vec3 ToDirection(Point p);
Point ToSquare(Vec3 direction);
double Luminance(Vec3 rgb);
// Radiant's resolved-hit detail field is bits 4..9, quarter-mip units in [0,32].
std::uint16_t ShiftHitMipQuarters(std::uint16_t flags, int quarters);
Point Sequence(std::uint32_t index, std::uint32_t count, std::uint64_t seed);
// Reorders whole samples, including identity and density. Does not change the estimator.
void CoherentOrder(std::vector<Sample>& samples);
// Input normals are normalized internally, invalid or zero normals are rejected.
std::vector<Cluster> ClusterReceivers(const std::vector<Receiver>& receivers);

// Float storage keeps a cluster's training and guide at about 43 KB each (a bake can hold thousands of clusters).
struct Training
{
    std::array<float, NodeCount> flux{};
    std::array<std::uint32_t, NodeCount> count{};
    void Clear();
    // pdf is per solid angle. Correct for the generating distribution when learning radiance.
    // Rejected samples leave the training state unchanged.
    bool Add(Vec3 direction, double incomingLuminance, double pdf);
    // Same as Add for a point already mapped to the unit square, contribution = luminance / pdf. Never throws.
    bool AddPoint(Point p, double contribution);
};

struct Settings
{
    double smoothing = 0.25;
    double uniformFraction = 0.01; // At least 1e-6, including after filtering.
    double splitFluxFraction = 0.01;
    std::uint32_t minSamples = 4;
};

class Guide
{
public:
    Guide();
    void Build(const Training& training, Settings settings = {});
    Sample Draw(Point random, std::uint32_t id = 0) const;
    double Pdf(Vec3 direction) const;
    std::size_t Leaves() const;
private:
    struct Node { float mass = 0; bool split = false; };
    std::array<Node, NodeCount> nodes_{};
    void BuildNode(const Training&, const Settings&, std::size_t, unsigned, double, double);
};

// Samples added here must already be divided by their actual PDF (and include any cosine/MIS).
// Welford's online statistics avoid cancellation in low-variance batches.
struct Batch
{
    std::uint64_t count = 0;
    Vec3 mean;
    double meanLuminance = 0, m2 = 0;
    bool Add(Vec3 weightedContribution);
    double VarianceOfMean() const;
};
enum class Combination { Equal, InverseVariance };
// InverseVariance is experimental and biased when the same samples estimate mean and variance.
// Batches must have equal, nonzero sample counts. Zero variance is regularized.
Vec3 Combine(const std::vector<Batch>& batches, Combination mode = Combination::Equal);
// Global correction preserves average luminance only. It does not remove local estimator bias.
double LuminanceCorrection(const std::vector<Vec3>& equal, const std::vector<Vec3>& weighted);

struct GatherSettings
{
    Settings guide;
    unsigned iterations = 4;
    unsigned raysPerIteration = 128;
    std::uint64_t seed = 1;
    bool coherentPackets = true;
    Combination combination = Combination::Equal;
};
struct GatherResult
{
    Vec3 irradiance;
    Vec3 equalWeightIrradiance;
    std::uint64_t samples = 0;
};
// Trace returns *unweighted incoming radiance*, in the order requested. False cancels.
// This is an adapter contract for surface final gather, not Radiant's internal ABI.
using Trace = bool(*)(void* user,const Receiver&,const Sample*,Vec3*,std::size_t);
// Processes clusters serially with bounded guide scratch. Throws on invalid data or cancellation.
// InverseVariance applies the global luminance correction only after all receivers complete.
std::vector<GatherResult> Gather(const std::vector<Receiver>& receivers,const GatherSettings&,Trace,void* user);
}
