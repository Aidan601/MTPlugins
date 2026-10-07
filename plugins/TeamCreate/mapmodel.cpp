#include "mapmodel.h"
#include <cstring>

namespace
{
    std::string Trim(const std::string& s)
    {
        size_t a = s.find_first_not_of(" \t\r");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r");
        return s.substr(a, b - a + 1);
    }

    bool StartsWith(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

    // Split into lines without '\n' or a trailing '\r'
    std::vector<std::string> Lines(const std::string& text)
    {
        std::vector<std::string> out;
        size_t pos = 0;
        while (pos <= text.size())
        {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) { if (pos < text.size()) out.push_back(text.substr(pos)); break; }
            std::string l = text.substr(pos, nl - pos);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            out.push_back(l);
            pos = nl + 1;
        }
        return out;
    }

    std::string ClassnameOf(const std::string& header)
    {
        size_t p = header.find("\"classname\"");
        if (p == std::string::npos) return "";
        size_t q1 = header.find('"', p + 11); if (q1 == std::string::npos) return "";
        size_t q2 = header.find('"', q1 + 1); if (q2 == std::string::npos) return "";
        return header.substr(q1 + 1, q2 - q1 - 1);
    }

    // Parses one entity at lines[i] == "{", advances i past the closing "}"
    bool ParseEntityLines(const std::vector<std::string>& lines, size_t& i, mapmodel::Entity& e, std::string* error)
    {
        while (i < lines.size() && Trim(lines[i]).empty()) ++i;
        if (i >= lines.size() || Trim(lines[i]) != "{") { if (error) *error = "entity: expected '{' at line " + std::to_string(i + 1); return false; }
        e = mapmodel::Entity();
        e.header = "{\n";
        ++i;
        // header lines until a brush or the closing brace
        while (i < lines.size())
        {
            std::string t = Trim(lines[i]);
            if (StartsWith(t, "// brush") || t == "{") break;
            if (t == "}") { ++i; e.classname = ClassnameOf(e.header); e.guid = mapmodel::ExtractGuid(e.header); return true; }
            e.header += lines[i]; e.header += '\n';
            ++i;
        }
        // brushes
        while (i < lines.size())
        {
            std::string t = Trim(lines[i]);
            if (StartsWith(t, "// brush")) { ++i; continue; }
            if (t == "}") { ++i; break; }
            if (t != "{") { if (error) *error = "entity: unexpected '" + t + "' at line " + std::to_string(i + 1); return false; }
            mapmodel::Brush b;
            int depth = 0;
            do
            {
                std::string tt = Trim(lines[i]);
                if (tt == "{") ++depth; else if (tt == "}") --depth;
                b.text += lines[i]; b.text += '\n';
                ++i;
            } while (i < lines.size() && depth > 0);
            if (depth != 0) { if (error) *error = "brush: unbalanced braces"; return false; }
            b.guid = mapmodel::ExtractGuid(b.text);
            e.brushes.push_back(std::move(b));
        }
        e.classname = ClassnameOf(e.header);
        e.guid = mapmodel::ExtractGuid(e.header);
        return true;
    }
}

namespace mapmodel
{
    Entity* Model::FindEntity(const std::string& guid)
    {
        for (auto& e : entities) if (e.guid == guid) return &e;
        return nullptr;
    }
    const Entity* Model::FindEntity(const std::string& guid) const
    {
        for (auto& e : entities) if (e.guid == guid) return &e;
        return nullptr;
    }
    Brush* Model::FindBrush(const std::string& guid, Entity** owner)
    {
        for (auto& e : entities)
            for (auto& b : e.brushes)
                if (b.guid == guid) { if (owner) *owner = &e; return &b; }
        return nullptr;
    }
    bool Model::RemoveBrush(const std::string& guid)
    {
        for (auto& e : entities)
            for (size_t k = 0; k < e.brushes.size(); ++k)
                if (e.brushes[k].guid == guid) { e.brushes.erase(e.brushes.begin() + k); return true; }
        return false;
    }
    bool Model::RemoveEntity(const std::string& guid)
    {
        for (size_t k = 0; k < entities.size(); ++k)
            if (entities[k].guid == guid) { entities.erase(entities.begin() + k); return true; }
        return false;
    }

    std::string ExtractGuid(const std::string& text)
    {
        size_t p = text.find("guid \"");
        if (p == std::string::npos) return "";
        size_t q = text.find('"', p + 6);
        if (q == std::string::npos) return "";
        return text.substr(p + 6, q - p - 6);
    }

    std::string ClassnameOfHeader(const std::string& header) { return ClassnameOf(header); }

    std::string ReplaceGuid(const std::string& text, const std::string& newGuid)
    {
        size_t p = text.find("guid \"");
        if (p == std::string::npos) return text;
        size_t q = text.find('"', p + 6);
        if (q == std::string::npos) return text;
        return text.substr(0, p + 6) + newGuid + text.substr(q);
    }

    bool ParseMap(const std::string& text, Model& out, std::string* error)
    {
        out = Model();
        std::vector<std::string> lines = Lines(text);
        size_t i = 0;
        while (i < lines.size() && !StartsWith(Trim(lines[i]), "// entity")) { out.fileHeader += lines[i]; out.fileHeader += '\n'; ++i; }
        while (i < lines.size())
        {
            std::string t = Trim(lines[i]);
            if (t.empty() || StartsWith(t, "// entity")) { ++i; continue; }
            Entity e;
            if (!ParseEntityLines(lines, i, e, error)) return false;
            out.entities.push_back(std::move(e));
        }
        return true;
    }

    bool ParseEntity(const std::string& text, Entity& out, std::string* error)
    {
        std::vector<std::string> lines = Lines(text);
        size_t i = 0;
        while (i < lines.size() && StartsWith(Trim(lines[i]), "// entity")) ++i;
        return ParseEntityLines(lines, i, out, error);
    }

    std::string SerializeEntity(const Entity& e, int* brushCounter)
    {
        std::string s = e.header;
        int local = 0; int& n = brushCounter ? *brushCounter : local;
        for (const auto& b : e.brushes) { s += "// brush " + std::to_string(n++) + "\n"; s += b.text; }
        s += "}\n";
        return s;
    }

    std::string Serialize(const Model& m)
    {
        std::string s = m.fileHeader;
        int ent = 0;
        for (const auto& e : m.entities)
        {
            s += "// entity " + std::to_string(ent++) + "\n";
            s += SerializeEntity(e);
        }
        return s;
    }
}
