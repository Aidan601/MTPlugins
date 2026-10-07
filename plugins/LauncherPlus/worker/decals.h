// cod2map -onlyents zeroes the four uvBaseAndScale floats (record offset 28..43) of every LUMP_VOLUME_DECALS record
// (72, 60-byte records), the full compile keeps them. The worker keeps the old lump and writes it back after the run
// when the new one differs only in those fields. Any other difference means a full compile is needed.
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

namespace decals
{
    constexpr uint32_t kLumpVolumeDecals = 72;
    constexpr size_t kRecord = 60, kUvBegin = 28, kUvEnd = 44;

    inline bool ReadAll(const std::wstring& path, std::string& out)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size = {};
        if (!GetFileSizeEx(h, &size) || size.QuadPart > (1LL << 31)) { CloseHandle(h); return false; }
        out.resize((size_t)size.QuadPart);
        size_t off = 0;
        while (off < out.size())
        {
            DWORD got = 0;
            const DWORD want = (DWORD)(out.size() - off > (1u << 22) ? (1u << 22) : out.size() - off);
            if (!ReadFile(h, out.data() + off, want, &got, nullptr) || !got) { CloseHandle(h); return false; }
            off += got;
        }
        CloseHandle(h);
        return true;
    }

    // Byte range of one lump in a d3dbsp: "IBSP", version, count, count x (id, length), then the lump data in table order.
    // The alignment is whichever of 4, 1, 16, 8, 32 makes the table span the file exactly (as bsp_oracle.py).
    inline bool Locate(const std::string& bsp, uint32_t id, size_t& offset, size_t& length)
    {
        if (bsp.size() < 12 || memcmp(bsp.data(), "IBSP", 4)) return false;
        uint32_t count = 0;
        memcpy(&count, bsp.data() + 8, 4);
        const size_t tableEnd = 12 + 8ull * count;
        if (count > 4096 || tableEnd > bsp.size()) return false;
        for (size_t align : { 4, 1, 16, 8, 32 })
        {
            size_t pos = tableEnd, foundOffset = 0, foundLength = 0;
            bool found = false;
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t kind = 0, len = 0;
                memcpy(&kind, bsp.data() + 12 + 8ull * i, 4);
                memcpy(&len, bsp.data() + 16 + 8ull * i, 4);
                pos = (pos + align - 1) / align * align;
                if (kind == id) { found = true; foundOffset = pos; foundLength = len; }
                pos += len;
            }
            if ((pos + align - 1) / align * align >= bsp.size() && bsp.size() >= pos)
            {
                if (!found) return false;
                offset = foundOffset; length = foundLength;
                return offset + length <= bsp.size();
            }
        }
        return false;
    }

    struct Keeper
    {
        std::string lump;     // the lump before the -onlyents run, empty when the BSP had none
        bool captured = false;

        void Capture(const std::wstring& bsp)
        {
            std::string data;
            size_t off = 0, len = 0;
            captured = ReadAll(bsp, data) && Locate(data, kLumpVolumeDecals, off, len);
            lump = captured ? data.substr(off, len) : std::string();
        }

        // 1 restored, 0 nothing to do (identical or no decals), -1 the lump changed in other ways (do not use it)
        int Restore(const std::wstring& bsp, size_t& records)
        {
            records = 0;
            if (!captured || lump.empty()) return 0;
            std::string data;
            size_t off = 0, len = 0;
            if (!ReadAll(bsp, data) || !Locate(data, kLumpVolumeDecals, off, len)) return -1;
            if (len != lump.size() || len % kRecord) return -1;
            if (!memcmp(data.data() + off, lump.data(), len)) return 0;
            for (size_t r = 0; r < len; r += kRecord)
            {
                const char* fresh = data.data() + off + r;
                const char* old = lump.data() + r;
                if (memcmp(fresh, old, kUvBegin) || memcmp(fresh + kUvEnd, old + kUvEnd, kRecord - kUvEnd)) return -1;
                if (memcmp(fresh + kUvBegin, old + kUvBegin, kUvEnd - kUvBegin))
                {
                    for (size_t i = kUvBegin; i < kUvEnd; ++i) if (fresh[i]) return -1;   // zeroed, nothing else
                    ++records;
                }
            }
            HANDLE h = CreateFileW(bsp.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) return -1;
            LARGE_INTEGER pos = {};
            pos.QuadPart = (LONGLONG)off;
            DWORD written = 0;
            const bool ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) && WriteFile(h, lump.data(), (DWORD)len, &written, nullptr) && written == len;
            CloseHandle(h);
            return ok ? 1 : -1;
        }
    };
}
