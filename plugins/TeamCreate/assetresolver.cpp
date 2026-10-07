#include <windows.h>
#include "assetresolver.h"
#include <algorithm>
#include <functional>
#include <sstream>
#include <stdexcept>

namespace assetfiles
{
    std::set<std::string> ModelMaterials(const std::string& bytes)
    {
            std::set<std::string> out; std::string data = ModelText(bytes);
            if (bytes.compare(0, 5, "*LZ4*") == 0)
            {
                // XMODEL_BIN MATERIAL record: four-byte aligned u16 0xA700, u16 index, then three strings (material name, shading model, texture).
                // Each string is NUL-terminated and padded with NULs to the next four-byte boundary (length 3 mod 4: one NUL, any other length: two to four).
                for (size_t p = 0; p + 8 < data.size(); p += 4)
                {
                    if (uint8_t(data[p]) != 0 || uint8_t(data[p + 1]) != 0xa7 || uint8_t(data[p + 3]) > 3) continue;
                    size_t end = data.find('\0', p + 4); if (end == std::string::npos || end - p - 4 > 127) continue;
                    std::string name = Lower(data.substr(p + 4, end - p - 4));
                    if (!SafeRelative(name)) continue;
                    size_t shaderBegin = (end + 1 + 3) & ~size_t(3); if (shaderBegin >= data.size()) continue;
                    size_t shader = data.find('\0', shaderBegin); if (shader == std::string::npos || shader == shaderBegin || shader - shaderBegin > 64) continue;
                    bool printable = true; for (size_t j = shaderBegin; j < shader; ++j) if (uint8_t(data[j]) < 32 || uint8_t(data[j]) > 126) printable = false;
                    if (printable) out.insert("material:" + name);
                }
            }
            else
            {
                std::istringstream lines(data); std::string line;
                while (std::getline(lines, line))
                {
                    std::istringstream fields(line); std::string kind; fields >> kind;
                    if (kind != "MATERIAL") continue;
                    size_t begin = line.find('"'), end = begin == std::string::npos ? begin : line.find('"', begin + 1);
                    if (end != std::string::npos) { auto name = Lower(line.substr(begin + 1, end - begin - 1)); if (SafeRelative(name)) out.insert("material:" + name); }
                }
            }
            return out;
    }
    namespace
    {
        std::string Normalize(std::string s)
        {
            s = Lower(s);
            for (size_t p; (p = s.find("//")) != std::string::npos;) s.erase(p, 1);
            return s;
        }
        bool SourcePath(const std::string& path)
        {
            if (!SafeRelative(path)) return false;
            std::string ext = Lower(fs::u8path(path).extension().u8string());
            // Source data only. Neither executables nor engine configuration can be shared.
            static const std::set<std::string> extensions = {".xmodel_bin", ".xmodel_export", ".xanim_bin", ".xanim_export", ".tga", ".tif", ".tiff", ".dds", ".png", ".jpg", ".jpeg", ".exr", ".hdr", ".wav", ".flac", ".fbx", ".obj", ".smd", ".xmodel_json"};
            return extensions.count(ext) != 0;
        }
    }
    struct Resolver::Impl
    {
        fs::path root;
        HMODULE library = nullptr; void* db = nullptr;
        int (*open)(const char*, void**, int, const char*) = nullptr;
        int (*close)(void*) = nullptr;
        int (*prepare)(void*, const char*, int, void**, const char**) = nullptr;
        int (*bind)(void*, int, const char*, int, void(*)(void*)) = nullptr;
        int (*step)(void*) = nullptr;
        const unsigned char* (*column)(void*, int) = nullptr;
        int (*finalize)(void*) = nullptr;
        struct CachedGdt { fs::file_time_type stamp; std::vector<Block> blocks; };
        std::map<std::string, CachedGdt> gdts;
        struct CachedFile { fs::file_time_type stamp; uintmax_t size; File file; std::set<std::string> materials; };
        std::map<std::string, CachedFile> files;
        std::map<std::string, uint64_t> missing;
        explicit Impl(const fs::path& r) : root(r)
        {
            library = LoadLibraryW((root / L"bin/sqlite64r.dll").c_str());
            if (!library) throw std::runtime_error("cannot load BO3 sqlite64r.dll");
#define SQL_FN(member, name) member = reinterpret_cast<decltype(member)>(GetProcAddress(library, name)); if (!member) throw std::runtime_error("missing sqlite export")
            try
            {
                SQL_FN(open, "sqlite3_open_v2"); SQL_FN(close, "sqlite3_close"); SQL_FN(prepare, "sqlite3_prepare_v2");
                SQL_FN(bind, "sqlite3_bind_text"); SQL_FN(step, "sqlite3_step"); SQL_FN(column, "sqlite3_column_text"); SQL_FN(finalize, "sqlite3_finalize");
                if (open((root / L"gdtdb/gdt.db").u8string().c_str(), &db, 1 | 0x10000, nullptr) != 0) throw std::runtime_error("cannot open gdt.db read-only");
            }
            catch (...) { if (db && close) close(db); FreeLibrary(library); throw; }
#undef SQL_FN
        }
        ~Impl() { if (db) close(db); if (library) FreeLibrary(library); }
        std::string Gdt(const std::string& name, const std::string& type)
        {
            std::string id = type + ":" + name;
            auto miss = missing.find(id); if (miss != missing.end() && GetTickCount64() - miss->second < 5000) return {};
            const char* sql = "SELECT t.name FROM _entity e JOIN _gdf g ON e.FK_gdf=g.PK_id JOIN _gdt t ON e.FK_gdt=t.PK_id WHERE e.name=?1 AND g.name=?2";
            void* stmt = nullptr;
            if (prepare(db, sql, -1, &stmt, nullptr) != 0) throw std::runtime_error("gdt.db query failed");
            bind(stmt, 1, name.c_str(), -1, nullptr); bind(stmt, 2, type.c_str(), -1, nullptr);
            std::string result;
            while (step(stmt) == 100)
            {
                const unsigned char* c = column(stmt, 0); if (!c) continue;
                std::string p = Normalize(reinterpret_cast<const char*>(c));
                std::string base = Normalize(root.u8string()) + "/";
                if (p.rfind(base, 0) == 0) p.erase(0, base.size());
                if (SafeRelative(p) && p.rfind("__streamed_assets/", 0) != 0) { result = p; break; }
            }
            finalize(stmt); if (result.empty()) missing[id] = GetTickCount64(); else missing.erase(id); return result;
        }
        const std::vector<Block>& Blocks(const std::string& path)
        {
            auto p = Under(root, path); auto stamp = fs::last_write_time(p);
            auto it = gdts.find(path);
            if (it == gdts.end() || it->second.stamp != stamp)
                gdts[path] = {stamp, Parse(Read(p, 64 * 1024 * 1024))};
            return gdts.at(path).blocks;
        }
        bool Definition(const std::string& id, Block& b, std::string& gdt, std::set<std::string>& parents)
        {
            size_t colon = id.find(':'); if (colon == std::string::npos || parents.size() >= 32 || !parents.insert(id).second) return false;
            std::string type = id.substr(0, colon), name = id.substr(colon + 1); gdt = Gdt(name, type); if (gdt.empty()) return false;
            for (const auto& candidate : Blocks(gdt)) if (candidate.name == name && (candidate.type == type || candidate.type.empty()))
            {
                b = candidate;
                if (!b.parent.empty())
                {
                    Block parent; std::string unused;
                    if (!Definition(type + ":" + b.parent, parent, unused, parents)) throw std::runtime_error("missing/cyclic derived asset parent: " + id);
                    auto own = b.values; b.values = parent.values;
                    for (const auto& kv : own) b.values[kv.first] = kv.second;
                    b.parent.clear();
                }
                b.type = type; return true;
            }
            return false;
        }
        // Root-relative path of a file-valued property, empty when the value is not a file or the file is not
        // on this PC (then `missing` names it: the definition is streamed without it, mirroring this PC's state).
        std::string FilePath(const std::string& value, const std::string& type, std::string& missing)
        {
            std::string path = Normalize(value);
            std::string base = Normalize(root.u8string()) + "/";
            if (path.rfind(base, 0) == 0) path.erase(0, base.size());
            // Exporters often retain a developer's drive prefix, resolve only the asset-root suffix locally.
            if (path.find(':') != std::string::npos)
                for (const char* dir : {"model_export/", "texture_assets/", "source_data/", "xanim_export/", "art_assets/"})
                { auto p = path.find(std::string("/") + dir); if (p != std::string::npos) { path.erase(0, p + 1); break; } }
            // A relative reference may climb out of its base ("..\\_custom\\x\\door.xmodel_bin" next to model_export): resolve it against each base
            // the converter would use, root first so a root-relative image path wins over texture_assets.
            std::vector<std::string> bases = {""};
            for (const auto& b : ReferenceBases(type)) if (!b.empty()) bases.push_back(b);
            for (const char* b : {"texture_assets/", "source_data/"}) if (std::find(bases.begin(), bases.end(), b) == bases.end()) bases.push_back(b);
            bool fileLike = false;
            for (const auto& b : bases)
            {
                std::string p = LexicalJoin(b, path); if (p.empty() || !SourcePath(p)) continue;
                fileLike = true; if (fs::is_regular_file(Under(root, p))) return p;
            }
            if (!fileLike)
            {
                // Not resolvable under any base: only report it when it names a source file at all.
                std::string ext = Lower(fs::u8path(path).extension().u8string());
                static const std::set<std::string> sourceExt = {".xmodel_bin", ".xmodel_export", ".xanim_bin", ".xanim_export", ".tga", ".tif", ".tiff", ".dds", ".png", ".jpg", ".jpeg", ".exr", ".hdr", ".wav", ".flac", ".fbx", ".obj", ".smd", ".xmodel_json"};
                if (!sourceExt.count(ext)) return {};
            }
            missing = value; return {};
        }
    };
    Resolver::Resolver(const fs::path& root) : impl_(new Impl(root)) {}
    Resolver::~Resolver() = default;
    bool Resolver::Exists(const std::string& id)
    {
        Block b; std::string gdt; std::set<std::string> parents;
        return impl_->Definition(id, b, gdt, parents);
    }
    bool Resolver::Find(const std::string& id, Asset& result)
    {
        Asset a; std::set<std::string> parents;
        if (!impl_->Definition(id, a.block, a.gdt, parents)) return false;
        std::set<std::string> dependencies, paths, materials, undefined;
        std::map<std::string, std::string> overrides;   // xmodel skinOverride: "from to" pairs
        auto add = [&](const std::string& type, const std::string& value)
        {
            for (const auto& token : Tokens(value)) if (!impl_->Gdt(token, type).empty()) dependencies.insert(type + ":" + token);
        };
        auto words = [](std::string values)
        {
            for (const char* esc : {"\\r", "\\n"}) for (size_t p; (p = values.find(esc)) != std::string::npos;) values.replace(p, 2, " ");
            std::vector<std::string> out; std::istringstream in(Lower(values)); std::string w;
            while (in >> w) out.push_back(w);
            return out;
        };
        for (const auto& kv : a.block.values)
        {
            std::string key = Lower(kv.first), lost, path = impl_->FilePath(kv.second, a.block.type, lost);
            if (!path.empty()) { a.paths[kv.first] = path; paths.insert(path); }
            if (!lost.empty()) a.missingFiles.push_back(lost);
            if (a.block.type == "material" && key.find("map") != std::string::npos) add("image", kv.second);
            if (a.block.type == "image" && key.rfind("compositeimage", 0) == 0) add("image", kv.second);
            if (a.block.type == "xmodel")
            {
                // The model's materials: the GDT "materials" list and the names inside the export, with "skinOverride" ("from to" pairs) applied,
                // so an overridden material is not a dependency (a stock skybox override would otherwise pull a 201 MB image).
                if (key == "skinoverride") { auto w = words(kv.second); for (size_t i = 0; i + 1 < w.size(); i += 2) overrides[w[i]] = w[i + 1]; }
                else if (key == "materials" || key.find("material") != std::string::npos) { for (const auto& w : words(kv.second)) materials.insert(w); }
                if (key == "hitboxmodel" || (key.rfind("submodel", 0) == 0 && key.find("name") != std::string::npos)) add("xmodel", kv.second);
            }
        }
        for (const auto& path : paths)
        {
            auto p = Under(impl_->root, path); auto stamp = fs::last_write_time(p); auto size = fs::file_size(p);
            auto cached = impl_->files.find(path);
            if (cached == impl_->files.end() || cached->second.stamp != stamp || cached->second.size != size)
            {
                std::string bytes = Read(p);
                Impl::CachedFile c{stamp, size, {path, Hash(bytes), uint32_t(bytes.size())}, {}};
                if (path.find(".xmodel_bin") != std::string::npos || path.find(".xmodel_export") != std::string::npos)
                    c.materials = ModelMaterials(bytes);
                impl_->files[path] = std::move(c);
            }
            const auto& c = impl_->files.at(path); a.files.push_back(c.file);
            for (const auto& m : c.materials) materials.insert(m.substr(m.find(':') + 1));
        }
        for (const auto& m : materials)
        {
            auto o = overrides.find(m); const std::string& effective = o == overrides.end() ? m : o->second;
            if (!SafeRelative(effective)) continue;
            if (!impl_->Gdt(effective, "material").empty()) dependencies.insert("material:" + effective); else undefined.insert("material:" + effective);
        }
        dependencies.erase(id); a.dependencies.assign(dependencies.begin(), dependencies.end());
        a.undefined.assign(undefined.begin(), undefined.end()); result = std::move(a); return true;
    }
    std::set<std::string> Resolver::References(const std::string& mapText)
    {
        std::set<std::string> out;
        for (const auto& t : Tokens(mapText))
            for (const char* type : {"xmodel", "material"})
                if (!impl_->Gdt(t, type).empty()) out.insert(std::string(type) + ":" + t);
        return out;
    }
    std::vector<std::string> Resolver::Bytes(const Asset& asset)
    {
        std::vector<std::string> result;
        for (const auto& f : asset.files)
        {
            auto bytes = Read(Under(impl_->root, f.path));
            if (Hash(bytes) != f.hash) throw std::runtime_error("source changed during asset transfer; retrying");
            result.push_back(std::move(bytes));
        }
        return result;
    }
}
