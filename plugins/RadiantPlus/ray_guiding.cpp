#include "ray_guiding.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>

namespace br::guiding
{
namespace
{
bool Finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
double Dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec3 Normalize(Vec3 v)
{
    if (!Finite(v)) throw std::invalid_argument("Non-finite direction");
    const double scale = std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)});
    if (scale == 0) throw std::invalid_argument("Zero direction");
    v = {v.x/scale, v.y/scale, v.z/scale};
    const double length = std::sqrt(Dot(v,v));
    return {v.x/length,v.y/length,v.z/length};
}
double Unit(double value)
{
    if (!std::isfinite(value) || value < 0 || value > 1) throw std::invalid_argument("Sample outside unit square");
    return std::min(value,std::nextafter(1.0,0.0));
}
unsigned Quadrant(Point& p)
{
    unsigned x = p.u >= .5, y = p.v >= .5;
    p.u = 2*p.u-x; p.v = 2*p.v-y;
    return x+2*y;
}
std::uint64_t Mix(std::uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30))*0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27))*0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
double Random(std::uint64_t seed) { return double(Mix(seed) >> 11)*0x1.0p-53; }
double RadicalInverse(std::uint32_t bits)
{
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x00ff00ffU) << 8) | ((bits & 0xff00ff00U) >> 8);
    bits = ((bits & 0x0f0f0f0fU) << 4) | ((bits & 0xf0f0f0f0U) >> 4);
    bits = ((bits & 0x33333333U) << 2) | ((bits & 0xccccccccU) >> 2);
    bits = ((bits & 0x55555555U) << 1) | ((bits & 0xaaaaaaaaU) >> 1);
    return double(bits)*0x1.0p-32;
}
std::uint32_t Morton(Vec3 direction)
{
    auto p = ToSquare(direction);
    const auto x = std::uint32_t(p.u*65536), y = std::uint32_t(p.v*65536);
    std::uint32_t result = 0;
    for (unsigned i=0;i<16;++i) result |= ((x>>i)&1)<<(2*i) | ((y>>i)&1)<<(2*i+1);
    return result;
}
}

Vec3 ToDirection(Point p)
{
    const double phi = 2*Pi*Unit(p.u), z = 1-2*Unit(p.v);
    const double r = std::sqrt(std::max(0.0,1-z*z));
    return {r*std::cos(phi),r*std::sin(phi),z};
}
Point ToSquare(Vec3 direction)
{
    auto n = Normalize(direction);
    double u = std::atan2(n.y,n.x)/(2*Pi);
    if (u < 0) u += 1;
    return {Unit(u),Unit(std::clamp((1-n.z)*.5,0.0,1.0))};
}
double Luminance(Vec3 rgb) { return .2126*rgb.x + .7152*rgb.y + .0722*rgb.z; }

std::uint16_t ShiftHitMipQuarters(std::uint16_t flags, int quarters)
{
    if ((flags & 0x3000) == 0x3000 || (flags & 0x400)) return flags;
    const int detail = (flags >> 4) & 63;
    return std::uint16_t((flags & ~0x3f0) | (std::clamp(detail + std::clamp(quarters, -32, 32), 0, 32) << 4));
}
Point Sequence(std::uint32_t index, std::uint32_t count, std::uint64_t seed)
{
    if (!count || index >= count) throw std::invalid_argument("Invalid sequence index/count");
    const double u = (index+.5)/count+Random(seed), v = RadicalInverse(index)+Random(seed^0xd1b54a32d192ed03ULL);
    return {u-std::floor(u),v-std::floor(v)};
}
void CoherentOrder(std::vector<Sample>& samples)
{
    struct Key { std::uint32_t morton; Sample sample; };
    std::vector<Key> keys;
    keys.reserve(samples.size());
    for (auto s : samples) keys.push_back({Morton(s.direction),s});
    std::stable_sort(keys.begin(),keys.end(),[](const Key& a,const Key& b){return a.morton < b.morton;});
    for (std::size_t i=0;i<keys.size();++i) samples[i]=keys[i].sample;
}
std::vector<Cluster> ClusterReceivers(const std::vector<Receiver>& receivers)
{
    std::vector<Vec3> normals;
    for (const auto& r : receivers)
    {
        if (!Finite(r.position)) throw std::invalid_argument("Non-finite receiver position");
        normals.push_back(Normalize(r.normal));
    }
    std::vector<std::size_t> indices(receivers.size());
    std::iota(indices.begin(),indices.end(),0);
    std::vector<Cluster> result;
    auto split = [&](auto&& self, std::size_t begin, std::size_t end) -> void
    {
        if (begin == end) return;
        bool coherent = end-begin <= 64;
        if (coherent)
            for (auto i=begin;i<end && coherent;++i)
                for (auto j=begin;j<i;++j)
                    if (Dot(normals[indices[i]],normals[indices[j]]) < .5-1e-12) {coherent=false;break;}
        if (coherent) { result.push_back({{indices.begin()+begin,indices.begin()+end}}); return; }
        Vec3 low=receivers[indices[begin]].position, high=low;
        for (auto i=begin+1;i<end;++i)
        {
            auto p=receivers[indices[i]].position;
            low={std::min(low.x,p.x),std::min(low.y,p.y),std::min(low.z,p.z)};
            high={std::max(high.x,p.x),std::max(high.y,p.y),std::max(high.z,p.z)};
        }
        unsigned axis=0;
        if (high.y-low.y > high.x-low.x) axis=1;
        if (high.z-low.z > (axis ? high.y-low.y : high.x-low.x)) axis=2;
        auto coord=[&](std::size_t i){auto p=receivers[i].position;return axis==0?p.x:axis==1?p.y:p.z;};
        std::stable_sort(indices.begin()+begin,indices.begin()+end,[&](auto a,auto b){return coord(a)<coord(b);});
        auto middle=begin+(end-begin)/2;
        self(self,begin,middle); self(self,middle,end);
    };
    split(split,0,indices.size());
    return result;
}
void Training::Clear() { flux.fill(0); count.fill(0); }
bool Training::Add(Vec3 direction,double incomingLuminance,double pdf)
{
    if (!Finite(direction) || !std::isfinite(incomingLuminance) || incomingLuminance < 0 ||
        !std::isfinite(pdf) || pdf <= 0) return false;
    Point p;
    try { p=ToSquare(direction); } catch (const std::invalid_argument&) { return false; }
    return AddPoint(p, incomingLuminance/pdf);
}
bool Training::AddPoint(Point p,double contribution)
{
    if (!std::isfinite(contribution) || contribution < 0 || !std::isfinite(double(flux[0])+contribution) ||
        double(flux[0])+contribution>3.0e38 || count[0]==UINT32_MAX) return false;
    if (!(p.u >= 0 && p.u <= 1 && p.v >= 0 && p.v <= 1)) return false;
    p.u=std::min(p.u,std::nextafter(1.0,0.0)); p.v=std::min(p.v,std::nextafter(1.0,0.0));
    std::size_t node=0;
    for (unsigned depth=0;;++depth)
    {
        flux[node]+=float(contribution); ++count[node];
        if (depth==MaxDepth) break;
        node=4*node+1+Quadrant(p);
    }
    return true;
}
Guide::Guide() { nodes_[0].mass=1; }
void Guide::Build(const Training& t,Settings s)
{
    if (!std::isfinite(s.smoothing) || s.smoothing < 0 || s.smoothing > 1 ||
        !std::isfinite(s.uniformFraction) || s.uniformFraction < 1e-6 || s.uniformFraction > 1 ||
        !std::isfinite(s.splitFluxFraction) || s.splitFluxFraction < 0 || s.splitFluxFraction > 1 || !s.minSamples)
        throw std::invalid_argument("Invalid guide settings");
    nodes_.fill({});
    BuildNode(t,s,0,0,1-s.uniformFraction,1);
}
void Guide::BuildNode(const Training& t,const Settings& s,std::size_t i,unsigned depth,double mass,double area)
{
    auto& node=nodes_[i];
    node.split=depth<MaxDepth && t.count[i]>=s.minSamples && t.flux[i]>0 &&
        t.flux[i]>=s.splitFluxFraction*t.flux[0];
    if (!node.split) {node.mass=float(mass+s.uniformFraction*area);return;}
    // Normalize by the children's own sum: float accumulation makes it differ slightly from the parent's flux.
    const double childFlux=double(t.flux[4*i+1])+t.flux[4*i+2]+t.flux[4*i+3]+t.flux[4*i+4];
    for (unsigned q=0;q<4;++q)
    {
        const auto child=4*i+1+q;
        const double share=childFlux>0?double(t.flux[child])/childFlux:.25;
        const double fraction=(1-s.smoothing)*share+s.smoothing*.25;
        BuildNode(t,s,child,depth+1,mass*fraction,area*.25);
        node.mass+=nodes_[child].mass;
    }
}
Sample Guide::Draw(Point random,std::uint32_t id) const
{
    Point p{Unit(random.u),Unit(random.v)}, corner{};
    std::size_t i=0;
    double width=1;
    while (nodes_[i].split)
    {
        const auto c=4*i+1;
        const double left=double(nodes_[c].mass)+nodes_[c+2].mass;
        const double total=double(nodes_[c].mass)+nodes_[c+1].mass+nodes_[c+2].mass+nodes_[c+3].mass;
        const double px=left/total;
        const unsigned x=p.u>=px;
        p.u=Unit(x ? (p.u-px)/(1-px) : p.u/px);
        const double bottom=nodes_[c+x].mass;
        const double column=double(nodes_[c+x].mass)+nodes_[c+x+2].mass;
        const double py=bottom/column;
        const unsigned y=p.v>=py;
        p.v=Unit(y ? (p.v-py)/(1-py) : p.v/py);
        width*=.5; corner.u+=x*width;corner.v+=y*width;
        i=c+x+2*y;
    }
    return {ToDirection({corner.u+width*p.u,corner.v+width*p.v}),nodes_[i].mass/(width*width*4*Pi),id};
}
double Guide::Pdf(Vec3 direction) const
{
    Point p=ToSquare(direction);
    std::size_t i=0;
    double area=1;
    while (nodes_[i].split) {i=4*i+1+Quadrant(p);area*=.25;}
    return nodes_[i].mass/(area*4*Pi);
}
std::size_t Guide::Leaves() const
{
    std::size_t result=0;
    for (const auto& n:nodes_) if (!n.split && n.mass>0) ++result;
    return result;
}
bool Batch::Add(Vec3 v)
{
    if (!Finite(v) || count==UINT64_MAX) return false;
    const double lum=Luminance(v), nextCount=double(count+1), delta=lum-meanLuminance;
    Vec3 next{mean.x+(v.x-mean.x)/nextCount,mean.y+(v.y-mean.y)/nextCount,mean.z+(v.z-mean.z)/nextCount};
    const double nextLum=meanLuminance+delta/nextCount, nextM2=m2+delta*(lum-nextLum);
    if (!Finite(next) || !std::isfinite(nextM2) || !std::isfinite(nextLum)) return false;
    ++count; mean=next;meanLuminance=nextLum;m2=nextM2;return true;
}
double Batch::VarianceOfMean() const {return count>1?std::max(0.0,m2)/(double(count-1)*double(count)):0;}
Vec3 Combine(const std::vector<Batch>& batches,Combination mode)
{
    if (batches.empty()) return {};
    double scale=0;
    for (const auto& b:batches)
    {
        if (!b.count || b.count!=batches[0].count || !Finite(b.mean)) throw std::invalid_argument("Invalid batch combination");
        if (mode==Combination::InverseVariance && b.count<2) throw std::invalid_argument("Variance requires two samples");
        scale=std::max(scale,b.VarianceOfMean());
    }
    const double floor=std::max(1e-24,scale*1e-6);
    Vec3 result{};
    double total=0;
    for (const auto& b:batches)
    {
        const double weight=mode==Combination::Equal?1:floor/std::max(floor,b.VarianceOfMean());
        const double fraction=weight/(total+weight);
        result={result.x+(b.mean.x-result.x)*fraction,result.y+(b.mean.y-result.y)*fraction,result.z+(b.mean.z-result.z)*fraction};
        total+=weight;
    }
    return result;
}
double LuminanceCorrection(const std::vector<Vec3>& equal,const std::vector<Vec3>& weighted)
{
    if (equal.size()!=weighted.size()) throw std::invalid_argument("Correction buffer size mismatch");
    double a=0,b=0;
    for (std::size_t i=0;i<equal.size();++i)
    {
        const double x=Luminance(equal[i]),y=Luminance(weighted[i]);
        if (!std::isfinite(x) || !std::isfinite(y) || x<0 || y<0) throw std::invalid_argument("Invalid correction luminance");
        a+=(x-a)/double(i+1);b+=(y-b)/double(i+1);
    }
    if (b==0) return 1; // No scale can recover a signal from an all-black estimate.
    const double ratio=a/b;
    if (!std::isfinite(ratio)) throw std::overflow_error("Correction scale overflow");
    return ratio;
}
std::vector<GatherResult> Gather(const std::vector<Receiver>& receivers,const GatherSettings& settings,Trace trace,void* user)
{
    if (!trace || settings.iterations<1 || settings.iterations>16 || settings.raysPerIteration<2 || settings.raysPerIteration>4096)
        throw std::invalid_argument("Invalid gather configuration");
    const auto clusters=ClusterReceivers(receivers);
    auto guide=std::make_unique<Guide>();
    auto training=std::make_unique<Training>();
    guide->Build(*training,settings.guide); // Validate settings even on empty input.
    std::vector<GatherResult> results(receivers.size());
    std::vector<Sample> samples(settings.raysPerIteration);
    std::vector<Vec3> incoming(settings.raysPerIteration);
    std::vector<std::size_t> sampleOrder(settings.raysPerIteration);
    for (const auto& cluster:clusters)
    {
        training->Clear();guide->Build(*training,settings.guide);
        std::vector<std::vector<Batch>> batches(cluster.receivers.size());
        for (unsigned iteration=0;iteration<settings.iterations;++iteration)
        {
            training->Clear();
            for (std::size_t local=0;local<cluster.receivers.size();++local)
            {
                const auto index=cluster.receivers[local];
                const auto& receiver=receivers[index];
                const Vec3 normal=Normalize(receiver.normal);
                const auto seed=Mix(settings.seed^Mix(index)^Mix(std::uint64_t(iteration)+0x8ebc6af09c88c6e3ULL));
                for (unsigned i=0;i<settings.raysPerIteration;++i) samples[i]=guide->Draw(Sequence(i,settings.raysPerIteration,seed),i);
                if (settings.coherentPackets) CoherentOrder(samples);
                // Poison outputs to detect callbacks that return success without filling every ray.
                const double nan=std::numeric_limits<double>::quiet_NaN();
                std::fill(incoming.begin(),incoming.end(),Vec3{nan,nan,nan});
                if (!trace(user,receiver,samples.data(),incoming.data(),samples.size())) throw std::runtime_error("Gather cancelled by trace adapter");
                // Accumulate in original order so sorting cannot change floating-point reductions.
                for (std::size_t i=0;i<samples.size();++i) sampleOrder[samples[i].id]=i;
                Batch batch;
                for (auto i:sampleOrder)
                {
                    const auto light=incoming[i];const auto& sample=samples[i];
                    if (!Finite(light) || light.x<0 || light.y<0 || light.z<0) throw std::runtime_error("Invalid incoming radiance from trace adapter");
                    if (!training->Add(sample.direction,Luminance(light),sample.pdf)) throw std::runtime_error("Guide training overflow");
                    const double weight=std::max(0.0,Dot(normal,sample.direction))/sample.pdf;
                    if (!batch.Add({light.x*weight,light.y*weight,light.z*weight})) throw std::runtime_error("Gather accumulation overflow");
                }
                batches[local].push_back(batch);
            }
            guide->Build(*training,settings.guide);
        }
        for (std::size_t local=0;local<cluster.receivers.size();++local)
        {
            auto& result=results[cluster.receivers[local]];
            result.equalWeightIrradiance=Combine(batches[local]);
            result.irradiance=Combine(batches[local],settings.combination);
            result.samples=std::uint64_t(settings.iterations)*settings.raysPerIteration;
        }
    }
    if (settings.combination==Combination::InverseVariance)
    {
        std::vector<Vec3> equal,weighted;
        for (const auto& result:results) {equal.push_back(result.equalWeightIrradiance);weighted.push_back(result.irradiance);}
        const double scale=LuminanceCorrection(equal,weighted);
        for (auto& result:results)
        {
            auto& v=result.irradiance;v={v.x*scale,v.y*scale,v.z*scale};
            if (!Finite(v)) throw std::overflow_error("Corrected irradiance overflow");
        }
    }
    return results;
}
}
