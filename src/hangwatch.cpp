#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "hangwatch.h"
#include "mainthread.h"
#include "config.h"
#include "log.h"

namespace
{
    const UINT   kPingMsg     = WM_APP + 0x48;  // to our message-only window on the main thread
    const WPARAM kPingMagic   = 0x48574154;     // 'HWAT'
    const size_t kCopyBytes   = 256 * 1024;     // main-thread stack copied per sample
    const int    kFrames      = 32;
    const int    kMaxSamples  = 400;
    const int    kFastSamples = 50;             // every 100 ms, then one per second

    std::atomic<HWND>     g_pingWnd{ nullptr };
    std::atomic<uint64_t> g_pongSeq{ 0 }, g_pongTick{ 0 };
    std::atomic<uintptr_t> g_stackLo{ 0 }, g_stackHi{ 0 };
    HANDLE   g_main = nullptr;
    DWORD    g_mainTid = 0;
    unsigned g_thresholdMs = 1000;
    BYTE*    g_copy = nullptr;
    uintptr_t g_exeBase = 0;

    std::mutex g_namesMutex;
    std::vector<std::pair<uintptr_t, const char*>> g_names;

    struct Sample { int n; uintptr_t f[kFrames]; };
    std::vector<Sample> g_samples;              // reserved once, filled during a freeze

    // ---- Lock-free PE lookups (the main thread may hold the loader lock while frozen) ----

    const IMAGE_NT_HEADERS* NtHeaders(uintptr_t base)
    {
        __try
        {
            auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
            auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    uintptr_t ImageBaseOf(uintptr_t addr)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) || mbi.Type != MEM_IMAGE) return 0;
        return reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    }

    // .pdata binary search, in place of RtlLookupFunctionEntry and its table lock
    PRUNTIME_FUNCTION FindFunction(uintptr_t addr, uintptr_t* base)
    {
        uintptr_t b = ImageBaseOf(addr);
        const IMAGE_NT_HEADERS* nt = b ? NtHeaders(b) : nullptr;
        if (!nt) return nullptr;
        __try
        {
            const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
            if (!d.VirtualAddress || d.Size < sizeof(RUNTIME_FUNCTION)) return nullptr;
            auto table = reinterpret_cast<PRUNTIME_FUNCTION>(b + d.VirtualAddress);
            const DWORD rva = DWORD(addr - b);
            size_t lo = 0, hi = d.Size / sizeof(RUNTIME_FUNCTION);
            while (lo < hi)
            {
                size_t mid = (lo + hi) / 2;
                if (rva < table[mid].BeginAddress) hi = mid;
                else if (rva >= table[mid].EndAddress) lo = mid + 1;
                else { *base = b; return &table[mid]; }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return nullptr;
    }

    typedef std::vector<std::pair<DWORD, const char*>> ExportList;
    void ReadExports(uintptr_t base, const IMAGE_DATA_DIRECTORY* d, ExportList* out)
    {
        __try
        {
            auto ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + d->VirtualAddress);
            auto funcs = reinterpret_cast<const DWORD*>(base + ed->AddressOfFunctions);
            auto names = reinterpret_cast<const DWORD*>(base + ed->AddressOfNames);
            auto ords = reinterpret_cast<const WORD*>(base + ed->AddressOfNameOrdinals);
            for (DWORD i = 0; i < ed->NumberOfNames; ++i)
            {
                DWORD rva = funcs[ords[i]];
                if (rva >= d->VirtualAddress && rva < d->VirtualAddress + d->Size) continue;   // forwarder
                out->emplace_back(rva, reinterpret_cast<const char*>(base + names[i]));
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { out->clear(); }
    }

    // ---- Sampling ----

    bool CopyStack(void* dst, uintptr_t src, size_t len)
    {
        __try { memcpy(dst, reinterpret_cast<const void*>(src), len); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool UnwindStep(CONTEXT* ctx, uintptr_t base, PRUNTIME_FUNCTION fn)
    {
        __try
        {
            void* handler = nullptr; DWORD64 est = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx->Rip, fn, ctx, &handler, &est, nullptr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Suspended only for GetThreadContext and the copy, the unwind runs on the copy after the resume
    bool Capture(Sample& s)
    {
        s.n = 0;
        const uintptr_t stackLo = g_stackLo.load(), stackHi = g_stackHi.load();
        if (SuspendThread(g_main) == DWORD(-1)) return false;
        CONTEXT ctx; memset(&ctx, 0, sizeof ctx); ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        bool ok = GetThreadContext(g_main, &ctx) != 0;
        const uintptr_t rsp = ok ? uintptr_t(ctx.Rsp) : 0;
        size_t len = 0;
        if (ok && stackHi && rsp >= stackLo && rsp < stackHi)
        {
            len = std::min<size_t>(stackHi - rsp, kCopyBytes) & ~size_t(7);
            if (!CopyStack(g_copy, rsp, len)) len = 0;
        }
        ResumeThread(g_main);
        if (!ok) return false;
        s.f[s.n++] = uintptr_t(ctx.Rip);
        if (!len) return true;

        // point every value inside the copied range at the copy, saved frame pointers included
        const uintptr_t copyLo = reinterpret_cast<uintptr_t>(g_copy), copyHi = copyLo + len;
        auto reloc = [&](DWORD64& v) { if (v >= rsp && v < rsp + len) v = v - rsp + copyLo; };
        for (size_t i = 0; i < len / 8; ++i) reloc(reinterpret_cast<DWORD64*>(g_copy)[i]);
        DWORD64* regs[] = { &ctx.Rsp, &ctx.Rbp, &ctx.Rbx, &ctx.Rsi, &ctx.Rdi, &ctx.R12, &ctx.R13, &ctx.R14, &ctx.R15, &ctx.Rax, &ctx.Rcx, &ctx.Rdx, &ctx.R8, &ctx.R9, &ctx.R10, &ctx.R11 };
        for (DWORD64* r : regs) reloc(*r);

        while (s.n < kFrames)
        {
            if (ctx.Rsp < copyLo || ctx.Rsp + 8 > copyHi) break;
            uintptr_t base = 0;
            PRUNTIME_FUNCTION fn = FindFunction(uintptr_t(ctx.Rip), &base);
            if (!fn) { ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp); ctx.Rsp += 8; }   // leaf: return address at rsp
            else if (!UnwindStep(&ctx, base, fn)) break;
            if (!ctx.Rip) break;
            s.f[s.n++] = uintptr_t(ctx.Rip);
        }
        return true;
    }

    // ---- Names (after the resume) ----

    struct ModuleInfo { std::string name; ExportList exports; bool exe = false; uintptr_t prefBase = 0; };
    std::map<uintptr_t, ModuleInfo> g_modules;  // watchdog thread only

    const ModuleInfo& Module(uintptr_t base)
    {
        auto it = g_modules.find(base);
        if (it != g_modules.end()) return it->second;
        ModuleInfo mi;
        wchar_t path[MAX_PATH * 2] = {};
        if (GetMappedFileNameW(GetCurrentProcess(), reinterpret_cast<void*>(base), path, MAX_PATH * 2))
        {
            const wchar_t* n = wcsrchr(path, L'\\'); n = n ? n + 1 : path;
            char buf[MAX_PATH]; WideCharToMultiByte(CP_UTF8, 0, n, -1, buf, sizeof buf, nullptr, nullptr);
            mi.name = buf;
        }
        else mi.name = "?";
        mi.exe = base == g_exeBase;
        if (const IMAGE_NT_HEADERS* nt = NtHeaders(base))
        {
            mi.prefBase = uintptr_t(nt->OptionalHeader.ImageBase);
            const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (d.VirtualAddress && d.Size)
            {
                ReadExports(base, &d, &mi.exports);
                std::sort(mi.exports.begin(), mi.exports.end());
            }
        }
        return g_modules.emplace(base, std::move(mi)).first->second;
    }

    std::string FrameText(uintptr_t addr)
    {
        char buf[256];
        uintptr_t base = ImageBaseOf(addr);
        if (!base) { snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)addr); return buf; }
        const ModuleInfo& m = Module(base);
        uintptr_t fnBase = 0;
        PRUNTIME_FUNCTION fn = FindFunction(addr, &fnBase);
        const DWORD rva = DWORD(addr - base);
        if (fn)
        {
            const uintptr_t start = base + fn->BeginAddress;
            {
                std::lock_guard<std::mutex> lock(g_namesMutex);
                for (const auto& n : g_names)
                    if (n.first == start) { snprintf(buf, sizeof buf, "%s!%s+0x%X", m.name.c_str(), n.second, rva - fn->BeginAddress); return buf; }
            }
            auto e = std::lower_bound(m.exports.begin(), m.exports.end(), std::make_pair(DWORD(fn->BeginAddress), (const char*)nullptr));
            if (e != m.exports.end() && e->first == fn->BeginAddress)
            {
                snprintf(buf, sizeof buf, "%s!%.80s+0x%X", m.name.c_str(), e->second, rva - fn->BeginAddress);
                return buf;
            }
        }
        // the exe at its IDA address, other modules by offset
        if (m.exe && m.prefBase) snprintf(buf, sizeof buf, "%s!0x%llX", m.name.c_str(), (unsigned long long)(m.prefBase + rva));
        else snprintf(buf, sizeof buf, "%s+0x%X", m.name.c_str(), rva);
        return buf;
    }

    std::string StackText(const Sample& s)
    {
        std::string out;
        for (int i = 0; i < s.n; ++i) { if (i) out += " < "; out += FrameText(s.f[i]); }
        return out.empty() ? "(no stack)" : out;
    }

    std::string ClockText(uint64_t tick)
    {
        // local wall time of a GetTickCount64 value
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
        u.QuadPart -= (GetTickCount64() - tick) * 10000ULL;
        ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
        FILETIME lft; SYSTEMTIME st{};
        FileTimeToLocalFileTime(&ft, &lft); FileTimeToSystemTime(&lft, &st);
        char buf[32]; snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        return buf;
    }

    // stacks grouped by their innermost 12 frames, the most common first
    void LogGroups(int maxGroups)
    {
        std::map<std::vector<uintptr_t>, std::pair<int, size_t>> groups;   // key -> count, first sample
        for (size_t i = 0; i < g_samples.size(); ++i)
        {
            const Sample& s = g_samples[i];
            std::vector<uintptr_t> key(s.f, s.f + std::min(s.n, 12));
            auto& g = groups[key];
            if (!g.first) g.second = i;
            ++g.first;
        }
        std::vector<std::pair<int, size_t>> order;
        for (const auto& g : groups) order.push_back(g.second);
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        // frames of the top stack in at least half of the samples, innermost first, with their share
        if (g_samples.size() >= 3 && !order.empty())
        {
            std::map<uintptr_t, int> incl;
            for (const Sample& s : g_samples)
                for (int i = 0; i < s.n; ++i)
                    if (std::find(s.f, s.f + i, s.f[i]) == s.f + i) ++incl[s.f[i]];
            const Sample& top = g_samples[order[0].second];
            std::string shared; int n = 0;
            char pct[16];
            for (int i = 0; i < top.n && n < 8; ++i)
            {
                const int c = incl[top.f[i]];
                if (c * 2 < int(g_samples.size()) || (i && top.f[i] == top.f[i - 1])) continue;
                snprintf(pct, sizeof pct, "%d%% ", int(100 * c / g_samples.size()));
                if (n++) shared += " < ";
                shared += pct + FrameText(top.f[i]);
            }
            if (n) LOG("freeze:   in half or more of the samples: %s", shared.c_str());
        }
        for (size_t i = 0; i < order.size() && int(i) < maxGroups; ++i)
            LOG("freeze:   %d/%zu  %s", order[i].first, g_samples.size(), StackText(g_samples[order[i].second]).c_str());
        if (int(order.size()) > maxGroups) LOG("freeze:   (%zu more stack shapes)", order.size() - maxGroups);
    }

    // ---- Watchdog ----

    struct WatchState
    {
        uint64_t pingSeq = 0, pingTick = 0, lastLoop = GetTickCount64();
        bool frozen = false;
        uint64_t nextSample = 0, nextNote = 0;
        int taken = 0;
    };

    // A freeze is a ping left unanswered for the threshold. It started at the ping's post, at most 250 ms after the real start.
    void WatchStep(WatchState& w)
    {
        const uint64_t now = GetTickCount64();
        const bool gap = now - w.lastLoop > 3000;   // this thread did not run (sleep, hibernate)
        w.lastLoop = now;
        HWND wnd = g_pingWnd.load();
        if (!wnd) return;
        const bool answered = g_pongSeq.load() == w.pingSeq;

        if (!w.frozen)
        {
            if (answered)
            {
                if (now - w.pingTick >= 250 && PostMessageW(wnd, kPingMsg, kPingMagic, LPARAM(w.pingSeq + 1))) { ++w.pingSeq; w.pingTick = now; }
                return;
            }
            if (gap) { w.pingTick = now; return; }
            if (now - w.pingTick < g_thresholdMs) return;
            w.frozen = true; w.taken = 0;
            g_samples.clear();
            w.nextSample = now; w.nextNote = w.pingTick + 5000;
        }
        if (answered)
        {
            const uint64_t end = g_pongTick.load();
            LOG("freeze: the main thread took no messages for %.1f s (%s to %s), %d stack sample(s)",
                double(end - w.pingTick) / 1000.0, ClockText(w.pingTick).c_str(), ClockText(end).c_str(), w.taken);
            LogGroups(3);
            w.frozen = false;
            g_modules.clear();             // a plugin may unload, its export names with it
            return;
        }
        if (now >= w.nextSample && w.taken < kMaxSamples)
        {
            Sample s;
            if (Capture(s)) { g_samples.push_back(s); ++w.taken; }
            w.nextSample = now + (w.taken < kFastSamples ? 100 : 1000);
        }
        if (now >= w.nextNote)
        {
            LOG("freeze: still no messages after %.0f s (since %s), %d stack sample(s) so far", double(now - w.pingTick) / 1000.0, ClockText(w.pingTick).c_str(), w.taken);
            LogGroups(1);
            w.nextNote = now + (now - w.pingTick < 30000 ? 25000 : 60000);
        }
    }

    LRESULT CALLBACK PingProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == kPingMsg && wp == kPingMagic) { g_pongTick.store(GetTickCount64()); g_pongSeq.store(uint64_t(lp)); return 0; }
        return DefWindowProcW(h, msg, wp, lp);
    }

    DWORD WINAPI WatchThread(LPVOID)
    {
        WatchState w;
        for (;;)
        {
            Sleep(50);
            try { WatchStep(w); }
            catch (...) { w.frozen = false; g_samples.clear(); }   // an exception must never leave this thread
        }
    }
}

namespace hangwatch
{
    void NameFunction(const void* start, const char* name)
    {
        std::lock_guard<std::mutex> lock(g_namesMutex);
        g_names.emplace_back(reinterpret_cast<uintptr_t>(start), name);
    }

    void Start()
    {
        if (g_main) return;
        g_thresholdMs = Cfg().freezeMs;
        if (!g_thresholdMs) { LOG("freeze: watch off ([diag] freeze_ms=0)"); return; }
        if (g_thresholdMs < 200) g_thresholdMs = 200;
        g_mainTid = mt::MainThreadId();
        if (!g_mainTid) { LOG("freeze: watch off (no main thread)"); return; }
        g_main = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_mainTid);
        g_copy = static_cast<BYTE*>(VirtualAlloc(nullptr, kCopyBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!g_main || !g_copy) { LOG("freeze: watch off (OpenThread or buffer failed, error %lu)", GetLastError()); return; }
        g_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        g_samples.reserve(kMaxSamples);
        mt::Post([] {
            ULONG_PTR lo = 0, hi = 0; GetCurrentThreadStackLimits(&lo, &hi); g_stackLo.store(lo); g_stackHi.store(hi);
            WNDCLASSW wc{}; wc.lpfnWndProc = PingProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"RadiantPlugins_FreezeWatch";
            RegisterClassW(&wc);
            HWND h = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
            if (!h) LOG("freeze: watch off (window failed, error %lu)", GetLastError());
            g_pingWnd.store(h);
        });
        // the thread runs until the process exits: keep the host mapped under it
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&WatchThread), &self);
        HANDLE t = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
        if (!t) { LOG("freeze: watch off (CreateThread failed, error %lu)", GetLastError()); return; }
        SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(t);
        LOG("freeze: watch on (main thread %lu, logs stalls of %u ms and longer)", g_mainTid, g_thresholdMs);
    }
}
