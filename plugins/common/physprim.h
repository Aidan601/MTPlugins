// Physics primitives: Radiant's Brush > Primitive box and cylinder (saved as parameters, which cod2map refuses in a level map) turned back into ordinary brushes (3-point face lines).
//   physics_box { ax ay az bx by bz cx cy cz ox oy oz ha hb hc } = three axes, centre, half size per axis. physics_cylinder { ux uy uz ox oy oz height radius } = axis, centre, full height, radius. matName <material> may follow.
//   The cylinder is what Radiant rebuilds on load: 16 sides touching the circle at k * 22.5 degrees from the axis's "left" (roll 0), plus two caps. Face lines: outward normal = cross(p0 - p1, p2 - p1). Header only.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace physprim
{
    struct Vec { double x = 0, y = 0, z = 0; };
    inline Vec Add(Vec a, Vec b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline Vec Sub(Vec a, Vec b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline Vec Mul(Vec a, double s) { return { a.x * s, a.y * s, a.z * s }; }
    inline double Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Vec Cross(Vec a, Vec b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    inline bool Normalize(Vec& v)
    {
        const double l = std::sqrt(Dot(v, v));
        if (!(l > 1e-9) || !std::isfinite(l)) return false;
        v = Mul(v, 1.0 / l);
        return true;
    }

    // one face: three points, outward normal = cross(p[0] - p[1], p[2] - p[1])
    struct Face { Vec p[3]; };

    // the face through point q with outward normal n (unit)
    inline Face FaceFromPlane(Vec n, Vec q)
    {
        Vec t1 = std::fabs(n.z) < 0.9 ? Cross({ 0, 0, 1 }, n) : Cross({ 1, 0, 0 }, n);
        Normalize(t1);
        const Vec t2 = Cross(n, t1);              // t1 x t2 = n
        const double s = 64.0;
        Face f;
        f.p[1] = q;
        f.p[0] = Add(q, Mul(t1, s));
        f.p[2] = Add(q, Mul(t2, s));
        return f;
    }

    inline bool Finite(const double* v, size_t n) { for (size_t i = 0; i < n; ++i) if (!std::isfinite(v[i])) return false; return true; }

    // v: the 15 numbers of a physics_box block
    inline bool BoxFaces(const double* v, std::vector<Face>& out)
    {
        out.clear();
        if (!Finite(v, 15)) return false;
        const Vec c = { v[9], v[10], v[11] };
        for (int i = 0; i < 3; ++i)
        {
            Vec a = { v[i * 3], v[i * 3 + 1], v[i * 3 + 2] };
            const double h = std::fabs(v[12 + i]);
            if (!Normalize(a) || !(h > 1e-4)) return false;
            out.push_back(FaceFromPlane(a, Add(c, Mul(a, h))));
            out.push_back(FaceFromPlane(Mul(a, -1), Sub(c, Mul(a, h))));
        }
        return true;
    }

    constexpr int kCylinderSides = 16;   // Radiant's loader builds every physics cylinder with 16 sides

    // v: the 8 numbers of a physics_cylinder block
    inline bool CylinderFaces(const double* v, std::vector<Face>& out)
    {
        out.clear();
        if (!Finite(v, 8)) return false;
        Vec f = { v[0], v[1], v[2] };
        const Vec c = { v[3], v[4], v[5] };
        const double half = std::fabs(v[6]) * 0.5, r = std::fabs(v[7]);
        if (!Normalize(f) || !(half > 1e-4) || !(r > 1e-4)) return false;
        out.push_back(FaceFromPlane(f, Add(c, Mul(f, half))));
        out.push_back(FaceFromPlane(Mul(f, -1), Sub(c, Mul(f, half))));
        const double yaw = (std::fabs(f.x) < 1e-9 && std::fabs(f.y) < 1e-9) ? 0.0 : std::atan2(f.y, f.x);
        Vec left = { -std::sin(yaw), std::cos(yaw), 0 };
        Vec up = Cross(f, left);
        if (!Normalize(up)) return false;
        left = Cross(up, f);
        for (int k = 0; k < kCylinderSides; ++k)
        {
            const double a = k * (6.283185307179586 / kCylinderSides);
            const Vec n = Add(Mul(left, std::cos(a)), Mul(up, std::sin(a)));
            out.push_back(FaceFromPlane(n, Add(c, Mul(n, r))));
        }
        return true;
    }

    // a face line in the .map brush format, the texture is Radiant's default projection, 128 units per repeat
    inline std::string FaceLine(const Face& f, const std::string& material, const std::string& indent)
    {
        char buf[512];
        snprintf(buf, sizeof buf, "%s( %.9g %.9g %.9g ) ( %.9g %.9g %.9g ) ( %.9g %.9g %.9g ) %s 128 128 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n",
                 indent.c_str(), f.p[0].x, f.p[0].y, f.p[0].z, f.p[1].x, f.p[1].y, f.p[1].z, f.p[2].x, f.p[2].y, f.p[2].z,
                 material.empty() ? "$default" : material.c_str());
        return buf;
    }

    inline bool Contains(const std::string& text) { return text.find("physics_box") != std::string::npos || text.find("physics_cylinder") != std::string::npos; }

    struct Result { size_t boxes = 0, cylinders = 0, failed = 0; };

    // Every physics_box / physics_cylinder block in .map or writer text replaced by its face lines (the brush's own
    // "{", guid line and "}" stay). A block that does not parse is left as it was and counted in failed.
    inline std::string Convert(const std::string& text, Result* result = nullptr)
    {
        Result r;
        if (!Contains(text)) { if (result) *result = r; return text; }
        std::vector<std::string> lines;
        for (size_t pos = 0; pos <= text.size();)
        {
            size_t eol = text.find('\n', pos);
            if (eol == std::string::npos) { if (pos < text.size()) lines.push_back(text.substr(pos)); break; }
            lines.push_back(text.substr(pos, eol + 1 - pos));
            pos = eol + 1;
        }
        auto trimmed = [](const std::string& l) {
            size_t b = l.find_first_not_of(" \t\r\n"), e = l.find_last_not_of(" \t\r\n");
            return b == std::string::npos ? std::string() : l.substr(b, e - b + 1);
        };
        std::string out;
        out.reserve(text.size() + 4096);
        for (size_t i = 0; i < lines.size(); ++i)
        {
            const std::string t = trimmed(lines[i]);
            const bool box = t == "physics_box", cyl = t == "physics_cylinder";
            if (!box && !cyl) { out += lines[i]; continue; }
            // "{", the numbers (any number of lines), "}", then an optional "matName <material>"
            size_t j = i + 1;
            while (j < lines.size() && trimmed(lines[j]).empty()) ++j;
            std::vector<double> nums;
            bool ok = j < lines.size() && trimmed(lines[j]) == "{";
            if (ok)
            {
                for (++j; j < lines.size(); ++j)
                {
                    const std::string n = trimmed(lines[j]);
                    if (n == "}") break;
                    const char* s = n.c_str();
                    for (char* e = nullptr;; s = e)
                    {
                        while (*s == ' ' || *s == '\t') ++s;
                        if (!*s) break;
                        const double d = strtod(s, &e);
                        if (e == s) { ok = false; break; }
                        nums.push_back(d);
                    }
                    if (!ok) break;
                }
                ok = ok && j < lines.size();
            }
            std::string material;
            size_t last = j;
            if (ok)
            {
                size_t k = j + 1;
                while (k < lines.size() && trimmed(lines[k]).empty()) ++k;
                if (k < lines.size())
                {
                    const std::string m = trimmed(lines[k]);
                    if (m.rfind("matName", 0) == 0 && m.size() > 7 && (m[7] == ' ' || m[7] == '\t')) { material = trimmed(m.substr(7)); last = k; }
                }
            }
            std::vector<Face> faces;
            ok = ok && (box ? nums.size() == 15 && BoxFaces(nums.data(), faces) : nums.size() == 8 && CylinderFaces(nums.data(), faces));
            if (!ok) { ++r.failed; out += lines[i]; continue; }
            const std::string& l = lines[i];
            const std::string indent = l.substr(0, l.find_first_not_of(" \t"));
            for (const Face& f : faces) out += FaceLine(f, material, indent);
            if (box) ++r.boxes; else ++r.cylinders;
            i = last;
        }
        if (result) *result = r;
        return out;
    }
}
