#include <windows.h>
#include "assetfiles.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace assetfiles
{
    namespace
    {
        void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
        struct Token { std::string value; size_t begin, end; bool quoted; };
        std::vector<Token> Lex(const std::string& text)
        {
            std::vector<Token> out;
            for (size_t i = 0; i < text.size();)
            {
                if (isspace(static_cast<unsigned char>(text[i]))) { ++i; continue; }
                if (text.compare(i, 2, "//") == 0) { i = text.find('\n', i); if (i == std::string::npos) break; continue; }
                size_t begin = i;
                if (text[i++] == '"')
                {
                    size_t end = text.find('"', i);
                    Require(end != std::string::npos, "unterminated GDT string");
                    out.push_back({text.substr(i, end - i), begin, end + 1, true}); i = end + 1;
                }
                else out.push_back({text.substr(begin, 1), begin, i, false});
            }
            return out;
        }
        std::string Quote(const std::string& value)
        {
            Require(value.find('"') == std::string::npos && value.find('\0') == std::string::npos, "invalid GDT value");
            return "\"" + value + "\"";
        }
        // A copy of one of the user's files that a streamed asset is about to replace, read back for verification. Only such files are
        // copied, never the session's own temporary ones. The session folder appears with its first copy.
        void BackupFile(const fs::path& root, const fs::path& backup, const std::string& relative)
        {
            fs::path source = Under(root, relative);
            if (!fs::exists(source)) return;
            std::string bytes = Read(source);
            fs::path dest = Under(backup, Hash(bytes) + "/" + relative);
            if (!fs::exists(dest)) Write(dest, bytes);
            Require(Read(dest) == bytes, "asset backup verification failed");
            fs::path note = backup / "original_root.txt";
            if (!fs::exists(note)) Write(note, fs::absolute(root).lexically_normal().u8string() + "\r\n");
        }
        // Removes converted preview files (root-relative, under share/assetconvert) whose bytes still hash as installed,
        // a changed or missing one is left alone. Returns how many went.
        size_t RemoveConverted(const fs::path& root, const std::vector<File>& files)
        {
            size_t removed = 0;
            for (const auto& f : files)
            {
                if (!SafeRelative(f.path) || Lower(f.path).rfind("share/assetconvert/", 0) != 0) continue;
                std::error_code ec; fs::path p = Under(root, f.path);
                if (!fs::is_regular_file(fs::symlink_status(p, ec))) continue;
                try { if (Hash(Read(p)) != f.hash) continue; } catch (...) { continue; }
                if (fs::remove(p, ec)) ++removed;
            }
            return removed;
        }
        // Puts a local GDT's bytes from before the session back, when the file still is what the session last wrote
        // (nobody else edited it meanwhile). False = not done, the caller merges the hidden entries back one by one.
        bool RestorePristine(const fs::path& root, const fs::path& backup, const std::string& path, const std::string& pristine, const std::string& written)
        {
            try
            {
                fs::path target = Under(root, path), copy = Under(backup, pristine + "/" + path);
                if (!fs::exists(target) || !fs::exists(copy) || Hash(Read(target)) != written) return false;
                std::string bytes = Read(copy); if (Hash(bytes) != pristine) return false;
                Write(target, bytes); return true;
            }
            catch (...) { return false; }
        }
        // "<yyyymmdd>-<hhmmss>-<pid>-<tick>", the name assets::Start gives a session's backup folder: the only names PruneBackups removes.
        bool SessionName(const std::wstring& n)
        {
            if (n.size() < 19 || n[8] != L'-' || n[15] != L'-') return false;
            size_t dashes = 0;
            for (size_t i = 0; i < n.size(); ++i)
            {
                if (n[i] == L'-') { ++dashes; if (i + 1 == n.size() || n[i + 1] == L'-') return false; }
                else if (n[i] < L'0' || n[i] > L'9') return false;
            }
            return dashes == 3;
        }
        // Deletes a folder tree without following links: a link or junction inside is removed itself, its target left alone.
        void RemoveTree(const fs::path& dir, uint64_t& bytes)
        {
            WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir / L"*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                    fs::path p = dir / fd.cFileName; bool folder = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { if (folder) RemoveDirectoryW(p.c_str()); else DeleteFileW(p.c_str()); }
                    else if (folder) RemoveTree(p, bytes);
                    else
                    {
                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                        if (DeleteFileW(p.c_str())) bytes += (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            RemoveDirectoryW(dir.c_str());
        }
        // An interrupted session nobody recovered yet: its root's recovery.txt still names this backup folder.
        bool Unrecovered(const fs::path& session)
        {
            try
            {
                fs::path note = session / "original_root.txt"; if (!fs::exists(note)) return false;
                std::string root = Read(note, 65536); while (!root.empty() && (root.back() == '\r' || root.back() == '\n')) root.pop_back();
                fs::path record = fs::u8path(root) / "__streamed_assets" / "recovery.txt"; if (!fs::exists(record)) return false;
                return Lower(Read(record, 16 * 1024 * 1024)).find(Lower(session.filename().u8string())) != std::string::npos;
            }
            catch (...) { return true; }   // cannot tell: leave it
        }
        // `dest`: where the file lives now, root-relative ("__streamed_assets/<path>" while streamed,
        // "teamcreate_assets/<path>" once saved)
        std::string Reference(const Asset& a, const std::string& key, const std::string& path, const std::string& dest)
        {
            std::string original = Lower(a.block.values.at(key));
            for (size_t p; (p = original.find("//")) != std::string::npos;) original.erase(p, 1);
            // The converter resolves a relative reference against the property's base directory (model_export for xmodel filenames, the root for
            // images). Keep the original's base: "..\\_custom\\x.xmodel_bin" becomes "..\\__streamed_assets\\_custom\\x.xmodel_bin",
            // an absolute original gets the type's default base.
            auto bases = ReferenceBases(a.block.type); std::string base = bases.empty() ? std::string() : bases.front();
            for (const auto& b : bases) if (LexicalJoin(b, original) == path) { base = b; break; }
            std::string prefix; for (char c : base) if (c == '/') prefix += "../";
            std::string relative = prefix + dest, escaped;
            // BO3 GDT source paths use escaped Windows separators, including in its signature cache.
            for (char c : relative) { if (c == '/') escaped += "\\\\"; else escaped += c; }
            return escaped;
        }
        std::string StreamReference(const Asset& a, const std::string& key, const std::string& path) { return Reference(a, key, path, "__streamed_assets/" + path); }
        // Empty folders under `dir` (never `dir` itself), deepest first, links are not followed.
        void PruneEmptyFolders(const fs::path& dir)
        {
            std::error_code ec; if (!fs::is_directory(dir, ec)) return;
            std::vector<fs::path> folders;
            for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                if (fs::is_directory(it->symlink_status(ec))) folders.push_back(it->path());   // a link or junction is not a plain folder
            std::sort(folders.begin(), folders.end(), [](const fs::path& a, const fs::path& b) { return a.native().size() > b.native().size(); });
            for (const auto& f : folders) if (fs::is_empty(f, ec)) fs::remove(f, ec);   // remove() only takes an empty folder
        }
    }
    std::string SavedPath(const std::string& path) { return path.rfind(SavedFolder + "/", 0) == 0 ? path : SavedFolder + "/" + path; }
    std::string Lower(std::string s)
    {
        for (char& c : s) { c = char(tolower(static_cast<unsigned char>(c))); if (c == '\\') c = '/'; }
        return s;
    }
    std::string LexicalJoin(const std::string& base, const std::string& relative)
    {
        if (relative.empty() || relative[0] == '/') return {};
        std::vector<std::string> parts; std::istringstream in(base + relative); std::string part;
        while (std::getline(in, part, '/'))
        {
            if (part.empty() || part == ".") continue;
            if (part == "..") { if (parts.empty()) return {}; parts.pop_back(); continue; }
            parts.push_back(part);
        }
        std::string out; for (const auto& p : parts) out += (out.empty() ? "" : "/") + p;
        return out;
    }
    std::vector<std::string> ReferenceBases(const std::string& type)
    {
        if (type == "xmodel") return {"model_export/", ""};
        if (type == "xanim") return {"xanim_export/", ""};
        return {"", "texture_assets/", "source_data/"};
    }
    std::string Hash(const std::string& bytes)
    {
        uint64_t h = 14695981039346656037ull;
        for (unsigned char c : bytes) { h ^= c; h *= 1099511628211ull; }
        std::ostringstream s; s << std::hex << std::setfill('0') << std::setw(16) << h; return s.str();
    }
    std::string Read(const fs::path& path, size_t limit)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        Require(bool(f), "cannot read asset file");
        auto size = f.tellg(); Require(size >= 0 && uint64_t(size) <= limit, "asset file exceeds size limit");
        std::string data(size_t(size), '\0'); f.seekg(0);
        if (!data.empty()) f.read(&data[0], std::streamsize(data.size()));
        Require(bool(f), "short asset read"); return data;
    }
    void Write(const fs::path& path, const std::string& bytes)
    {
        fs::create_directories(path.parent_path());
        fs::path temp = path; temp += L".rtc_asset_tmp";
        DWORD attr = GetFileAttributesW(temp.c_str());
        Require(attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_REPARSE_POINT), "asset temporary file is a reparse point");
        { std::ofstream f(temp, std::ios::binary | std::ios::trunc); Require(bool(f), "cannot write asset file");
          f.write(bytes.data(), std::streamsize(bytes.size())); f.flush(); Require(bool(f), "asset write failed"); }
        // gdtdb /update, Radiant or APE may have the destination open for a moment: retry for up to 5 s before giving up.
        bool moved = false;
        for (int attempt = 0; attempt < 100 && !moved; ++attempt)
        {
            moved = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
            DWORD error = moved ? 0 : GetLastError();
            if (!moved && error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED && error != ERROR_LOCK_VIOLATION) break;
            if (!moved) Sleep(50);
        }
        Require(moved, "cannot commit asset file");
    }
    bool SafeRelative(const std::string& path)
    {
        if (path.empty() || path.size() > 220 || path[0] == '/' || path.find_first_of("\\:\"<>|?*\r\n\t") != std::string::npos) return false;
        std::istringstream in(path); std::string part;
        while (std::getline(in, part, '/'))
        {
            if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
            for (unsigned char c : part) if (c < 32 || c == 127) return false;
            std::string stem = Lower(part.substr(0, part.find('.')));
            if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
                (stem.size() == 4 && (stem.substr(0, 3) == "com" || stem.substr(0, 3) == "lpt") && stem[3] >= '0' && stem[3] <= '9')) return false;
        }
        return path.back() != '/';
    }
    fs::path Under(const fs::path& root, const std::string& relative)
    {
        Require(SafeRelative(relative), "unsafe asset path");
        fs::path path = fs::absolute(root).lexically_normal();
        // Reject junctions at every ancestor as well as the destination.
        fs::path check;
        for (const auto& part : path) { check /= part; DWORD a = GetFileAttributesW(check.c_str()); Require(a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_REPARSE_POINT), "asset root crosses a reparse point"); }
        for (const auto& part : fs::u8path(relative))
        {
            path /= part; DWORD a = GetFileAttributesW(path.c_str());
            Require(a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_REPARSE_POINT), "asset path crosses a reparse point");
        }
        return path;
    }
    std::vector<Block> Parse(const std::string& text)
    {
        auto t = Lex(text); std::vector<Block> out;
        Require(t.size() >= 2 && t.front().value == "{" && t.back().value == "}", "invalid GDT envelope");
        for (size_t i = 1; i + 1 < t.size();)
        {
            Require(i + 5 < t.size() && t[i].quoted && (t[i + 1].value == "(" || t[i + 1].value == "["), "invalid GDT asset header");
            Block b; b.begin = t[i].begin; b.name = Lower(t[i].value);
            bool parent = t[i + 1].value == "[";
            Require(t[i + 2].quoted && t[i + 3].value == (parent ? "]" : ")") && t[i + 4].value == "{", "invalid GDT asset type");
            if (parent) b.parent = Lower(t[i + 2].value);
            else { b.type = Lower(t[i + 2].value); if (b.type.size() > 4 && b.type.substr(b.type.size() - 4) == ".gdf") b.type.resize(b.type.size() - 4); }
            i += 5;
            while (i < t.size() && t[i].value != "}")
            {
                Require(i + 1 < t.size() && t[i].quoted && t[i + 1].quoted, "invalid GDT property");
                b.values[t[i].value] = t[i + 1].value; i += 2;
            }
            Require(i + 1 < t.size(), "unterminated GDT asset"); b.end = t[i++].end; out.push_back(std::move(b));
        }
        return out;
    }
    std::string Emit(const Block& b)
    {
        std::string out = "\t" + Quote(b.name) + (b.parent.empty() ? " ( " + Quote(b.type + ".gdf") + " )" : " [ " + Quote(b.parent) + " ]") + "\r\n\t{\r\n";
        for (const auto& p : b.values) out += "\t\t" + Quote(p.first) + " " + Quote(p.second) + "\r\n";
        return out + "\t}\r\n";
    }
    std::string Merge(const std::string& text, const Block& block, bool remove)
    {
        auto blocks = Parse(text); std::string result = text;
        size_t matches = 0;
        for (const auto& b : blocks) if (b.name == block.name && (b.type == block.type || b.type.empty())) ++matches;
        Require(matches <= 1, "ambiguous or duplicate asset entries in destination GDT; retained unchanged");
        bool found = false;
        for (auto i = blocks.rbegin(); i != blocks.rend(); ++i)
            if (i->name == block.name && (i->type == block.type || i->type.empty()))
            { result.erase(i->begin, i->end - i->begin); found = true; }
        if (!remove) result.insert(result.find_last_of('}'), "\r\n" + Emit(block));
        (void)found; return result;
    }
    // Asset names may start with a digit (ported packs: "0a7141f5_portal_button_blue"), plain numbers are not names.
    static bool Named(const std::string& token)
    {
        for (unsigned char c : token) if (isalpha(c) || c == '_') return true;
        return false;
    }
    std::set<std::string> Tokens(const std::string& text)
    {
        std::set<std::string> out; std::string token;
        for (unsigned char c : text)
        {
            if (isalnum(c) || c == '_' || c == '-' || c == '/' || c == '.' || c == ':') { if (token.size() < 256) token += char(tolower(c)); }
            else { if (!token.empty() && token.size() < 220 && Named(token)) out.insert(token); token.clear(); }
        }
        if (!token.empty() && token.size() < 220 && Named(token)) out.insert(token); return out;
    }
    std::string ModelText(const std::string& bytes)
    {
        if (bytes.compare(0, 5, "*LZ4*") != 0) return bytes;
        Require(bytes.size() >= 9, "short model container");
        uint32_t size = 0; memcpy(&size, bytes.data() + 5, 4); Require(size <= MaxFile, "model decompression limit");
        std::string out; out.reserve(size); size_t p = 9;
        auto length = [&](size_t n) { if (n == 15) { unsigned char c; do { Require(p < bytes.size(), "short LZ4 length"); c = bytes[p++]; n += c; Require(n <= size, "LZ4 length limit"); } while (c == 255); } return n; };
        while (p < bytes.size())
        {
            unsigned char token = bytes[p++]; size_t literals = length(token >> 4);
            Require(literals <= bytes.size() - p && literals <= size - out.size(), "invalid LZ4 literals");
            out.append(bytes, p, literals); p += literals; if (p == bytes.size()) break;
            Require(p + 2 <= bytes.size(), "short LZ4 offset");
            size_t offset = uint8_t(bytes[p]) | (size_t(uint8_t(bytes[p + 1])) << 8); p += 2;
            size_t count = length(token & 15) + 4;
            Require(offset && offset <= out.size() && count <= size - out.size(), "invalid LZ4 match");
            while (count--) out += out[out.size() - offset];
        }
        Require(out.size() == size, "model decompression size mismatch"); return out;
    }
    Pruned PruneBackups(const fs::path& all, const fs::path& current, unsigned days, unsigned keep)
    {
        Pruned out; if (!days) return out;
        std::vector<std::wstring> names; std::error_code ec;
        for (auto it = fs::directory_iterator(all, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (fs::is_directory(it->symlink_status(ec)) && SessionName(it->path().filename().native())) names.push_back(it->path().filename().native());
        std::sort(names.begin(), names.end());                              // the stamp in the name sorts by time
        SYSTEMTIME now, then; GetLocalTime(&now); FILETIME ft; if (!SystemTimeToFileTime(&now, &ft)) return out;
        ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime; u.QuadPart -= uint64_t(days) * 24 * 3600 * 10000000ull;
        ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart; if (!FileTimeToSystemTime(&ft, &then)) return out;
        wchar_t cutoff[32]; swprintf_s(cutoff, L"%04u%02u%02u-%02u%02u%02u", then.wYear, then.wMonth, then.wDay, then.wHour, then.wMinute, then.wSecond);
        const size_t old = names.size() > keep ? names.size() - keep : 0;   // the newest `keep` stay whatever their age
        for (size_t i = 0; i < names.size(); ++i)
        {
            fs::path dir = all / names[i];
            if (i >= old || names[i].compare(0, 15, cutoff) >= 0 || names[i] == current.filename().native() || Unrecovered(dir)) { ++out.kept; continue; }
            RemoveTree(dir, out.bytes);
            if (fs::exists(dir, ec)) ++out.kept; else ++out.sessions;
        }
        return out;
    }
    void RegisterConverter(const fs::path& root, const fs::path& backup, std::set<std::string> missing)
    {
        const std::string key = "bin/converter_gdt_dirs_0.txt";
        std::string text = Read(Under(root, key), 1024 * 1024), line;
        std::istringstream lines(text);
        while (std::getline(lines, line))
        {
            line.erase(0, line.find_first_not_of(" \t")); size_t end = line.find_last_not_of(" \t\r"); if (end != std::string::npos) line.resize(end + 1);
            missing.erase(Lower(line));
        }
        if (missing.empty()) return;
        BackupFile(root, backup, key);
        if (!text.empty() && text.back() != '\n') text += "\r\n";
        for (const auto& m : missing) text += m + "\r\n";
        Write(Under(root, key), text);
    }
    Store::Store(const fs::path& root, const fs::path& outsideBackup) : root_(fs::absolute(root).lexically_normal()), backup_(fs::absolute(outsideBackup).lexically_normal())
    {
        auto r = Lower(root_.u8string()), b = Lower(backup_.u8string());
        Require(b != r && b.rfind(r + "/", 0) != 0, "asset backups must be outside BO3");
        fs::path lockPath = Under(root_, "__streamed_assets/.session.lock"); fs::create_directories(lockPath.parent_path());
        lock_ = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(lock_ != INVALID_HANDLE_VALUE, "another editor owns the asset stream folder");
        try
        {
            Recover();
            // the temporary folder, and where saved streamed assets live once a session saved some
            std::set<std::string> folders = {"__streamed_assets"}; std::error_code ec;
            if (fs::exists(Under(root_, SavedGdt), ec)) folders.insert(SavedFolder);
            RegisterConverter(root_, backup_, folders);
        }
        catch (...) { CloseHandle(lock_); lock_ = nullptr; throw; }
    }
    Store::~Store() { if (lock_ && lock_ != INVALID_HANDLE_VALUE) CloseHandle(lock_); }
    void Store::Replace(const std::string& path, const std::string& bytes)
    {
        auto dest = Under(root_, path);
        auto owned = owned_.find(path);
        if (owned != owned_.end() && fs::exists(dest)) Require(Hash(Read(dest)) == owned->second, "streamed file was edited outside Team Create; retained");
        if (fs::exists(dest) && Read(dest) == bytes) return;
        Write(dest, bytes);   // no copy here: the callers back up what belongs to the user (BackupFile), never the session's own files
    }
    void Store::Journal()
    {
        std::string text = "Team Create asset recovery. Backups: " + backup_.u8string() + "\r\n";
        for (const auto& h : hidden_) text += "Temporarily removed " + h.first + " from " + h.second.path + "\r\n";
        for (const auto& o : owned_) text += "Owned " + o.first + " " + o.second + "\r\n";
        // a local GDT only this session's hides touched: its hash before the session (the copy in the backups) and as last written
        for (const auto& l : local_) if (l.second.clean) text += "Pristine " + l.second.pristine + " " + l.second.written + " " + l.first + "\r\n";
        // converted preview files installed for still temporary assets: removed with them by Finish or Recover
        for (const auto& c : converted_) for (const auto& f : c.second) text += "Converted " + c.first + " " + f.hash + " " + f.path + "\r\n";
        Write(Under(root_, "__streamed_assets/recovery.txt"), text);
        // The hidden local definitions themselves, GDT syntax under a non-.gdt name so gdtdb ignores them.
        fs::path entries = Under(root_, "__streamed_assets/hidden_entries.txt");
        if (hidden_.empty()) { std::error_code ec; fs::remove(entries, ec); }
        else { std::string e = "{\r\n"; for (const auto& h : hidden_) e += Emit(h.second.block); Write(entries, e + "}\r\n"); }
    }
    void Store::Recover()
    {
        // An interrupted session (crash, killed editor, failed cleanup) leaves recovery.txt: hidden local definitions and the files the
        // session wrote. Undo it here, never refuse to start.
        const std::string gdtKey = "__streamed_assets/streamed_assets.gdt";
        fs::path record = Under(root_, "__streamed_assets/recovery.txt"), entries = Under(root_, "__streamed_assets/hidden_entries.txt"), gdt = Under(root_, gdtKey);
        if (!fs::exists(record) && !fs::exists(gdt)) return;
        fs::path oldBackup; std::vector<std::pair<std::string, std::string>> hidden; std::map<std::string, std::string> owned;
        struct Earlier { std::string pristine, written, path; }; std::vector<Earlier> earlier;
        std::vector<File> converted;
        if (fs::exists(record))
        {
            std::istringstream lines(Read(record, 16 * 1024 * 1024)); std::string line;
            const std::string kBackups = "Team Create asset recovery. Backups: ", kHidden = "Temporarily removed ", kOwned = "Owned ", kPristine = "Pristine ", kConverted = "Converted ";
            while (std::getline(lines, line))
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
                if (line.rfind(kBackups, 0) == 0) oldBackup = fs::u8path(line.substr(kBackups.size()));
                else if (line.rfind(kHidden, 0) == 0) { auto from = line.find(" from ", kHidden.size()); if (from != std::string::npos) hidden.push_back({line.substr(kHidden.size(), from - kHidden.size()), line.substr(from + 6)}); }
                else if (line.rfind(kOwned, 0) == 0) { auto space = line.rfind(' '); if (space != std::string::npos && space > kOwned.size()) owned[line.substr(kOwned.size(), space - kOwned.size())] = line.substr(space + 1); }
                else if (line.rfind(kPristine, 0) == 0 && line.size() > kPristine.size() + 34 && line[kPristine.size() + 16] == ' ' && line[kPristine.size() + 33] == ' ')
                    earlier.push_back({line.substr(kPristine.size(), 16), line.substr(kPristine.size() + 17, 16), line.substr(kPristine.size() + 34)});
                else if (line.rfind(kConverted, 0) == 0)
                {
                    // "Converted <id> <hash> <path>": the id carries no spaces, the hash is 16 characters
                    auto space = line.find(' ', kConverted.size());
                    if (space != std::string::npos && line.size() > space + 18 && line[space + 17] == ' ')
                        converted.push_back({line.substr(space + 18), line.substr(space + 1, 16), 0});
                }
            }
        }
        size_t restored = 0, removed = 0, backedUp = 0; std::vector<std::string> notes;
        // 1. Streamed definitions go first, so a restored local entry never coexists with its streamed twin.
        if (fs::exists(gdt)) Write(gdt, "{\r\n}\r\n");
        // 2. Hidden local entries. A GDT nobody else edited gets its exact bytes from before that session back, else each
        //    entry is merged back in, from hidden_entries.txt or from the newest external backup of that GDT still holding it.
        std::set<std::string> exact;
        for (const auto& e : earlier) if (!oldBackup.empty() && SafeRelative(e.path) && RestorePristine(root_, oldBackup, e.path, e.pristine, e.written)) exact.insert(e.path);
        std::vector<Block> saved; if (fs::exists(entries)) { try { saved = Parse(Read(entries)); } catch (...) { notes.push_back("hidden_entries.txt unreadable"); } }
        for (const auto& h : hidden)
        {
            auto colon = h.first.find(':'); if (colon == std::string::npos || !SafeRelative(h.second)) continue;
            if (exact.count(h.second)) { ++restored; continue; }
            std::string type = h.first.substr(0, colon), name = h.first.substr(colon + 1);
            auto matches = [&](const Block& b) { return b.name == name && (b.type == type || b.type.empty()); };
            Block block; bool found = false;
            for (const auto& b : saved) if (matches(b)) { block = b; found = true; break; }
            if (!found && !oldBackup.empty() && fs::exists(oldBackup))
            {
                std::error_code ec; fs::file_time_type newest{};
                for (const auto& dir : fs::directory_iterator(oldBackup, ec))
                {
                    fs::path copy = dir.path() / fs::u8path(h.second); if (!fs::exists(copy)) continue;
                    auto stamp = fs::last_write_time(copy, ec); if (found && stamp <= newest) continue;
                    try { for (const auto& b : Parse(Read(copy))) if (matches(b)) { block = b; found = true; newest = stamp; break; } } catch (...) {}
                }
            }
            if (!found) { notes.push_back("no copy of " + h.first + " found for " + h.second); continue; }
            fs::path target = Under(root_, h.second); std::string current = fs::exists(target) ? Read(target) : "{\r\n}\r\n";
            bool present = false;
            try { for (const auto& b : Parse(current)) if (matches(b)) present = true; }
            catch (...) { notes.push_back(h.second + " unreadable, " + h.first + " not restored"); continue; }
            if (present) continue;
            Replace(h.second, Merge(current, block)); ++restored;
        }
        // 3. Files the previous session wrote: removed, one edited since is copied to the backups first.
        owned[gdtKey] = "";
        for (const auto& o : owned)
        {
            if (!SafeRelative(o.first) || o.first.rfind("__streamed_assets/", 0) != 0) continue;
            fs::path p = Under(root_, o.first); if (!fs::exists(p)) continue;
            if (o.first != gdtKey && Hash(Read(p)) != o.second) { BackupFile(root_, backup_, o.first); ++backedUp; }
            fs::remove(p); ++removed;
        }
        // 4. Converted preview files the previous session installed for assets it never saved: Radiant's own cache,
        //    removed only while the bytes are still the ones installed (Radiant regenerates a missing file).
        size_t previews = RemoveConverted(root_, converted);
        std::error_code ec; fs::remove(record, ec); fs::remove(entries, ec);
        PruneEmptyFolders(Under(root_, "__streamed_assets"));
        recovered_ = "recovered the previous asset session: " + std::to_string(restored) + " local definition(s) restored, " + std::to_string(removed) + " streamed file(s) removed";
        if (backedUp) recovered_ += " (" + std::to_string(backedUp) + " edited since, copied to the backups first)";
        if (previews) recovered_ += ", " + std::to_string(previews) + " converted preview file(s) removed";
        for (const auto& n : notes) recovered_ += "; " + n;
    }
    void Store::Publish()
    {
        std::string text = "{\r\n";
        for (const auto& a : staged_)
        {
            Block b = a.second.block;
            for (const auto& p : a.second.paths) b.values[p.first] = StreamReference(a.second, p.first, p.second);
            text += Emit(b);
        }
        text += "}\r\n"; Replace("__streamed_assets/streamed_assets.gdt", text);
        owned_["__streamed_assets/streamed_assets.gdt"] = Hash(text); Journal();
    }
    void Store::Stage(const Asset& asset, const std::vector<std::string>& bytes, const std::string& localGdt)
    {
        Require(bytes.size() == asset.files.size(), "incomplete asset transfer");
        std::map<std::string, uint32_t> sizes;
        for (const auto& a : staged_) if (a.first != asset.Id()) for (const auto& f : a.second.files) sizes[f.path] = f.size;
        for (const auto& f : asset.files) sizes[f.path] = f.size;
        uint64_t total = 0; for (const auto& s : sizes) total += s.second;
        Require(total <= 4ull * 1024 * 1024 * 1024, "temporary asset session exceeds 4 GB");
        for (size_t i = 0; i < bytes.size(); ++i)
        {
            Require(bytes[i].size() == asset.files[i].size && Hash(bytes[i]) == asset.files[i].hash, "asset checksum mismatch");
            const auto& f = asset.files[i];
            for (const auto& other : staged_) if (other.first != asset.Id())
                for (const auto& shared : other.second.files) if (shared.path == f.path)
                    Require(shared.hash == f.hash, "two streamed assets disagree on a shared source file");
        }
        // Backups and recovery instructions precede any change to a real GDT.
        if (!localGdt.empty() && !hidden_.count(asset.Id()))
        {
            auto path = Under(root_, localGdt); std::string text = Read(path);
            size_t matches = 0;
            for (const auto& b : Parse(text)) if (b.name == asset.block.name && (b.type == asset.block.type || b.type.empty())) ++matches;
            Require(matches <= 1, "ambiguous local derived asset entries; retained unchanged");
            for (const auto& b : Parse(text)) if (b.name == asset.block.name && (b.type == asset.block.type || b.type.empty()))
            {
                // One copy per GDT per session: its bytes before the first streamed asset replaced an entry in it,
                // which is what Finish and Recover put back when nothing else changed the file meanwhile.
                std::string without = Merge(text, asset.block, true);
                auto l = local_.find(localGdt);
                if (l == local_.end()) { BackupFile(root_, backup_, localGdt); l = local_.emplace(localGdt, Local{Hash(text), std::string(), true}).first; }
                else if (Hash(text) != l->second.written) l->second.clean = false;   // edited by someone else since: its entries are merged back one by one
                l->second.written = Hash(without);
                hidden_[asset.Id()] = { localGdt, b }; Journal();
                Replace(localGdt, without);
                { std::error_code ec; hiddenSeen_[localGdt] = fs::last_write_time(path, ec); }   // HiddenReturned looks again when this moves
                break;
            }
        }
        for (size_t i = 0; i < bytes.size(); ++i)
        {
            Require(bytes[i].size() == asset.files[i].size && Hash(bytes[i]) == asset.files[i].hash, "asset checksum mismatch");
            std::string path = "__streamed_assets/" + asset.files[i].path;
            if (!owned_.count(path)) BackupFile(root_, backup_, path);    // a stray file this session did not write: kept before it is replaced
            Replace(path, bytes[i]); owned_[path] = asset.files[i].hash;
        }
        staged_[asset.Id()] = asset; Publish();
    }
    void Store::Save(const std::set<std::string>& ids)
    {
        // Keep a rollback for this promotion, a failure must not leave half a saved asset.
        std::map<std::string, std::string> edits; bool redefines = false;
        for (const auto& id : ids)
        {
            auto it = staged_.find(id); if (it == staged_.end()) continue; const Asset& a = it->second;
            for (const auto& f : a.files)
            {
                std::string bytes = Read(Under(root_, "__streamed_assets/" + f.path));
                Require(Hash(bytes) == f.hash, "staged source changed before save"); edits[SavedPath(f.path)] = std::move(bytes);
            }
            // every saved definition goes into the one saved GDT, its file references pointed at the saved folder
            auto g = edits.find(SavedGdt);
            std::string text = g != edits.end() ? g->second : fs::exists(Under(root_, SavedGdt)) ? Read(Under(root_, SavedGdt)) : "{\r\n}\r\n";
            Block saved = a.block;
            for (const auto& p : a.paths) saved.values[p.first] = Reference(a, p.first, p.second, SavedPath(p.second));
            for (const auto& b : Parse(text)) if (b.name == saved.name && (b.type == saved.type || b.type.empty()) && (b.values != saved.values || b.parent != saved.parent)) redefines = true;
            edits[SavedGdt] = Merge(text, saved);
        }
        if (edits.empty()) return;
        // The local definitions these saves replace for good, kept next to the saved assets where nothing prunes them
        // (the external backup folder is pruned after [backup] asset_days): a dated header line, then the block as it was.
        {
            std::string replaced;
            for (const auto& id : ids)
            {
                auto h = hidden_.find(id); if (h == hidden_.end() || !staged_.count(id)) continue;
                SYSTEMTIME t; GetLocalTime(&t); char when[32]; sprintf_s(when, "%04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
                replaced += "// " + std::string(when) + ": your definition of " + id + " from " + h->second.path + ", replaced by the streamed one saved to " + SavedGdt +
                            " (copy also in " + backup_.u8string() + ")\r\n{\r\n" + Emit(h->second.block) + "}\r\n";
            }
            if (!replaced.empty())
            {
                std::error_code ec; fs::path p = Under(root_, ReplacedRecord);
                std::string text = fs::exists(p, ec) ? Read(p) : "// Team Create: local asset definitions that saved streamed assets replaced. Paste a block back into a GDT to use it again.\r\n";
                edits[ReplacedRecord] = text + replaced;
            }
        }
        RegisterConverter(root_, backup_, {SavedFolder});
        std::map<std::string, std::pair<bool, std::string>> rollback;
        for (const auto& e : edits)
        {
            auto p = Under(root_, e.first); bool exists = fs::exists(p);
            rollback[e.first] = {exists, exists ? Read(p) : std::string()};
            // a copy only of what a saved asset replaces: a saved file with other bytes, the saved GDT when an entry in it is redefined
            if (exists && e.first != ReplacedRecord && (e.first == SavedGdt ? redefines : rollback[e.first].second != e.second)) BackupFile(root_, backup_, e.first);
        }
        std::map<std::string, Asset> promoting;
        for (const auto& id : ids)
        {
            auto i = staged_.find(id); if (i != staged_.end()) { promoting.emplace(*i); staged_.erase(i); }
        }
        try
        {
            // Remove temporary definitions FIRST. gdtdb watches these folders and rejects even
            // a transient duplicate (same name/type in streamed_assets.gdt and a destination GDT).
            Publish();
            for (const auto& e : edits) if (fs::u8path(e.first).extension() != ".gdt") Replace(e.first, e.second);
            for (const auto& e : edits) if (fs::u8path(e.first).extension() == ".gdt") Replace(e.first, e.second);
        }
        catch (...)
        {
            for (const auto& r : rollback)
            {
                auto p = Under(root_, r.first);
                if (r.second.first) { if (!fs::exists(p) || Read(p) != r.second.second) Write(p, r.second.second); }
                else fs::remove(p);
            }
            staged_.insert(promoting.begin(), promoting.end()); Publish();
            throw;
        }
        // a saved asset's local twin stays out of its GDT for good, so that GDT can no longer go back to its earlier bytes as a whole
        for (const auto& id : ids)
        {
            converted_.erase(id);   // a saved asset keeps its converted previews (Radiant's cache, the bytes it would write itself)
            auto h = hidden_.find(id); if (h == hidden_.end()) continue;
            auto l = local_.find(h->second.path); if (l != local_.end()) l->second.clean = false;
            hidden_.erase(h);
        }
        // Source files shared with remaining temporary assets stay until those assets are saved/discarded.
        std::set<std::string> needed;
        for (const auto& a : staged_) for (const auto& f : a.second.files) needed.insert("__streamed_assets/" + f.path);
        for (auto i = owned_.begin(); i != owned_.end();)
        {
            if (i->first != "__streamed_assets/streamed_assets.gdt" && !needed.count(i->first))
            {
                auto p = Under(root_, i->first); Require(!fs::exists(p) || Hash(Read(p)) == i->second, "streamed file changed outside Team Create; retained");
                fs::remove(p); i = owned_.erase(i);
            }
            else ++i;
        }
        Journal();
        PruneEmptyFolders(Under(root_, "__streamed_assets"));
    }
    std::string Store::Finish()
    {
        std::string notes;
        // As with promotion, remove the streamed definitions before restoring local definitions.
        // Keep the source bytes and recovery record until all restores have succeeded.
        if (owned_.count("__streamed_assets/streamed_assets.gdt"))
        {
            const std::string empty = "{\r\n}\r\n";
            Replace("__streamed_assets/streamed_assets.gdt", empty);
            owned_["__streamed_assets/streamed_assets.gdt"] = Hash(empty); Journal();
        }
        // Nothing saved = nothing changed: a GDT only this session's hides touched gets its exact bytes from before the session back,
        // any other one gets each hidden entry merged back in below.
        while (!local_.empty())
        {
            const std::string path = local_.begin()->first; const Local state = local_.begin()->second; local_.erase(local_.begin());
            if (!state.clean || !RestorePristine(root_, backup_, path, state.pristine, state.written)) continue;
            for (auto it = hidden_.begin(); it != hidden_.end();) { if (it->second.path == path) it = hidden_.erase(it); else ++it; }
            Journal();
        }
        for (auto it = hidden_.begin(); it != hidden_.end();)
        {
            auto p = Under(root_, it->second.path); std::string text = Read(p); bool duplicate = false;
            for (const auto& b : Parse(text)) if (b.name == it->second.block.name && (b.type == it->second.block.type || b.type.empty())) duplicate = true;
            // The entry is back in the file already (APE's Save All of an asset held open across the hide, or the user pasted it): the file holds
            // what they want, so it is not merged a second time (as Recover does).
            if (duplicate) notes += (notes.empty() ? "" : "; ") + it->first + " was already back in " + it->second.path + " (edited during the session), left as it is";
            else Replace(it->second.path, Merge(text, it->second.block));
            it = hidden_.erase(it); Journal();
        }
        for (auto i = owned_.begin(); i != owned_.end();)
        {
            auto p = Under(root_, i->first); Require(!fs::exists(p) || Hash(Read(p)) == i->second, "streamed file changed outside Team Create; retained");
            fs::remove(p); i = owned_.erase(i); Journal();
        }
        // Converted previews of the assets discarded here: Radiant's cache would otherwise keep them for ever.
        std::vector<File> previews; for (const auto& c : converted_) if (staged_.count(c.first)) previews.insert(previews.end(), c.second.begin(), c.second.end());
        size_t removed = RemoveConverted(root_, previews); converted_.clear();
        if (removed) notes += (notes.empty() ? "" : "; ") + std::to_string(removed) + " converted preview file(s) of discarded assets removed from Radiant's cache";
        staged_.clear(); std::error_code ec;
        fs::remove(Under(root_, "__streamed_assets/recovery.txt"), ec); fs::remove(Under(root_, "__streamed_assets/hidden_entries.txt"), ec);
        PruneEmptyFolders(Under(root_, "__streamed_assets"));
        return notes;
    }
    void Store::NoteConverted(const std::string& id, const std::vector<File>& files)
    {
        if (files.empty() || !staged_.count(id)) return;   // an identical local asset's previews are the user's own
        auto& list = converted_[id];
        for (const auto& f : files) { bool known = false; for (const auto& k : list) if (k.path == f.path) known = true; if (!known) list.push_back(f); }
        Journal();
    }
    std::vector<std::string> Store::HiddenReturned()
    {
        std::vector<std::string> back; std::set<std::string> paths;
        for (const auto& h : hidden_) paths.insert(h.second.path);
        for (const auto& path : paths)
        {
            std::error_code ec; auto stamp = fs::last_write_time(Under(root_, path), ec); if (ec) continue;
            auto seen = hiddenSeen_.find(path);
            if (seen == hiddenSeen_.end()) { hiddenSeen_[path] = stamp; continue; }   // first look: as this session wrote it
            if (seen->second == stamp) continue;
            seen->second = stamp;
            std::vector<Block> blocks; try { blocks = Parse(Read(Under(root_, path))); } catch (...) { continue; }
            for (const auto& h : hidden_) if (h.second.path == path)
                for (const auto& b : blocks) if (b.name == h.second.block.name && (b.type == h.second.block.type || b.type.empty())) { back.push_back(h.first); break; }
        }
        return back;
    }
}
