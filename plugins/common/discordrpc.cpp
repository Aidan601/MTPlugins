// Discord Rich Presence over Discord's local IPC pipe, no SDK.
#include "modtools_plugin.hpp"
#include "discordrpc.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace discordrpc
{
namespace
{
    const char kDefaultAppId[] = "1557144059963899994";            // the Discord application all three tools share, [discord] app_id overrides it
    const wchar_t kSharedName[] = L"Local\\ModToolsDiscordRpc";
    const DWORD kStepMs = 500;                  // worker period
    const uint64_t kStaleMs = 5000;             // an owner or presenter without a beat for this long is gone
    const uint64_t kSendWindowMs = 20000;       // Discord allows 5 activity updates per 20 s
    const int kSendsPerWindow = 5;
    const uint64_t kRetryMs = 15000;            // Discord not running
    const uint64_t kNoAnswerRetryMs = 3000;     // the pipe opened but the handshake got no answer
    const uint64_t kRejectedRetryMs = 600000;   // Discord refused the application id
    const DWORD kMaxFrame = 64 * 1024;
    enum : uint32_t { OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4 };

    // One per session, mapped by every tool. The owner (the tool focused last) writes its activity here,
    // the presenter (the one tool connected to Discord) sends it. Text fields under `lock`.
    struct Shared
    {
        volatile LONG lock;
        volatile LONG owner;        // pid whose activity is shown, 0 = none
        volatile LONG presenter;    // pid connected to Discord, 0 = none
        volatile LONG gen;          // bumped by the owner on every change
        volatile LONG textOwner;    // pid whose activity the text fields hold
        volatile LONG64 ownerBeat, presenterBeat;   // GetTickCount64 of their last step
        long long start;            // the owner's process start (unix seconds)
        char details[256], state[256], art[32], label[64];
    };

    // main thread state
    std::string g_appId;
    uint64_t g_nextDue = 0;
    HANDLE g_thread = nullptr;

    // shared with the worker
    std::string g_art, g_label;                 // set before the worker starts
    std::atomic<int> g_mode{ Tool };
    std::atomic<unsigned> g_gen{ 0 };           // bumped when g_activity or the mode changes
    SRWLOCK g_lock = SRWLOCK_INIT;              // g_activity, g_status
    Activity g_activity;
    std::string g_status = "not started";
    HANDLE g_stop = nullptr;                    // manual reset, set by Shutdown

    // worker only
    HANDLE g_map = nullptr;
    Shared* g_shared = nullptr;
    HANDLE g_pipe = INVALID_HANDLE_VALUE;
    HANDLE g_ioEvent = nullptr;
    bool g_ready = false;                       // handshake answered with READY
    unsigned g_publishedGen = ~0u;              // our g_gen last written to Shared
    LONG g_sentGen = -1;                        // Shared::gen last sent to Discord (presenter)
    LONG g_sentOwner = -1;
    uint64_t g_sends[kSendsPerWindow] = {};     // times of the last sends, oldest first
    uint64_t g_nextTry = 0;
    uint64_t g_noOwnerSince = 0;                // presenter: when the owner went away, a new one usually follows at once
    unsigned g_nonce = 0;
    long long g_startUnix = 0;                  // this process's start, the elapsed time Discord shows

    LONG Me() { return LONG(GetCurrentProcessId()); }

    void SetStatus(const std::string& s)
    {
        AcquireSRWLockExclusive(&g_lock);
        const bool changed = g_status != s;
        if (changed) g_status = s;
        ReleaseSRWLockExclusive(&g_lock);
        if (changed) rp::Log("discord: %s", s.c_str());
    }

    // bounded: a tool that died holding it cannot block the others
    void Lock() { for (int i = 0; i < 2000 && InterlockedCompareExchange(&g_shared->lock, 1, 0) != 0; ++i) Sleep(0); g_shared->lock = 1; }
    void Unlock() { InterlockedExchange(&g_shared->lock, 0); }

    void Copy(char* dst, size_t cap, const std::string& s)
    {
        size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
        while (n && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;   // not inside a UTF-8 sequence
        memcpy(dst, s.data(), n);
        dst[n] = 0;
    }

    void Disconnect()
    {
        if (g_pipe != INVALID_HANDLE_VALUE) { CloseHandle(g_pipe); g_pipe = INVALID_HANDLE_VALUE; }
        g_ready = false;
        g_sentGen = -1;
        g_sentOwner = -1;
    }

    // keeps this tool's roles alive while a pipe operation waits on Discord
    void Beat()
    {
        if (!g_shared) return;
        const LONG64 now = LONG64(GetTickCount64());
        if (g_shared->owner == Me()) InterlockedExchange64(&g_shared->ownerBeat, now);
        if (g_shared->presenter == Me()) InterlockedExchange64(&g_shared->presenterBeat, now);
    }

    // overlapped so a stuck Discord or Shutdown never blocks the worker
    bool Io(bool write, void* buf, DWORD size, DWORD timeoutMs)
    {
        char* p = static_cast<char*>(buf);
        DWORD done = 0;
        while (done < size)
        {
            OVERLAPPED ov = {};
            ov.hEvent = g_ioEvent;
            ResetEvent(g_ioEvent);
            const BOOL ok = write ? WriteFile(g_pipe, p + done, size - done, nullptr, &ov) : ReadFile(g_pipe, p + done, size - done, nullptr, &ov);
            DWORD got = 0;
            if (!ok)
            {
                if (GetLastError() != ERROR_IO_PENDING) return false;
                const HANDLE waits[2] = { g_ioEvent, g_stop };
                DWORD r = WAIT_TIMEOUT;
                for (DWORD waited = 0; r == WAIT_TIMEOUT && waited < timeoutMs; waited += 250)
                {
                    r = WaitForMultipleObjects(2, waits, FALSE, 250);
                    Beat();
                }
                if (r != WAIT_OBJECT_0)
                {
                    CancelIoEx(g_pipe, &ov);
                    GetOverlappedResult(g_pipe, &ov, &got, TRUE);
                    return false;
                }
            }
            if (!GetOverlappedResult(g_pipe, &ov, &got, FALSE) || !got) return false;
            done += got;
        }
        return true;
    }

    // frame = uint32 opcode, uint32 length (little endian), JSON
    bool SendFrame(uint32_t op, const std::string& json)
    {
        std::string frame(8, '\0');
        const uint32_t len = uint32_t(json.size());
        memcpy(&frame[0], &op, 4);
        memcpy(&frame[4], &len, 4);
        frame += json;
        return Io(true, &frame[0], DWORD(frame.size()), 2000);
    }

    bool ReadFrame(uint32_t& op, std::string& json, DWORD timeoutMs)
    {
        uint32_t head[2] = {};
        if (!Io(false, head, 8, timeoutMs) || head[1] > kMaxFrame) return false;
        op = head[0];
        json.assign(head[1], '\0');
        return !head[1] || Io(false, &json[0], head[1], 2000);
    }

    std::string Escape(const std::string& s)
    {
        std::string out;
        for (unsigned char c : s)
        {
            if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
            else if (c < 0x20) { char u[8]; snprintf(u, sizeof u, "\\u%04x", c); out += u; }
            else out += char(c);
        }
        return out;
    }

    // Discord takes 2 to 128 characters per line
    std::string Line(std::string s)
    {
        if (s.size() > 120)
        {
            size_t n = 120;
            while (n && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
            s.resize(n);
            s += "...";
        }
        if (s.size() == 1) s += ' ';
        return s;
    }

    bool Connect()
    {
        const uint64_t now = GetTickCount64();
        for (int i = 0; i < 10 && g_pipe == INVALID_HANDLE_VALUE; ++i)
        {
            wchar_t name[48];
            swprintf_s(name, L"\\\\.\\pipe\\discord-ipc-%d", i);
            g_pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        }
        if (g_pipe == INVALID_HANDLE_VALUE) { g_nextTry = now + kRetryMs; SetStatus("Discord is not running"); return false; }
        uint32_t op = 0;
        std::string reply;
        if (!SendFrame(OP_HANDSHAKE, "{\"v\":1,\"client_id\":\"" + Escape(g_appId) + "\"}") || !ReadFrame(op, reply, 10000))
        {
            Disconnect();
            g_nextTry = now + kNoAnswerRetryMs;
            SetStatus("Discord did not answer the handshake");
            return false;
        }
        if (op != OP_FRAME || reply.find("READY") == std::string::npos)
        {
            Disconnect();
            g_nextTry = now + kRejectedRetryMs;
            SetStatus("Discord refused application id " + g_appId + ": " + reply.substr(0, 160));
            return false;
        }
        g_ready = true;
        SetStatus("connected");
        return true;
    }

    // answers pings and drops command replies, false = the pipe is gone
    bool Pump()
    {
        for (;;)
        {
            DWORD avail = 0;
            if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr)) return false;
            if (avail < 8) return true;
            uint32_t op = 0;
            std::string json;
            if (!ReadFrame(op, json, 2000) || op == OP_CLOSE) return false;
            if (op == OP_PING && !SendFrame(OP_PONG, json)) return false;
            if (op == OP_FRAME && json.find("\"evt\":\"ERROR\"") != std::string::npos) rp::Log("discord: error reply: %s", json.substr(0, 200).c_str());
        }
    }

    bool CanSend(uint64_t now) { return now - g_sends[0] >= kSendWindowMs || !g_sends[0]; }
    void NoteSend(uint64_t now)
    {
        memmove(g_sends, g_sends + 1, sizeof(g_sends) - sizeof(g_sends[0]));
        g_sends[kSendsPerWindow - 1] = now;
    }

    // the owner's activity from Shared, or a clear when there is no owner
    bool SendShared()
    {
        Lock();
        const LONG owner = g_shared->owner;
        const std::string details = g_shared->details, state = g_shared->state, art = g_shared->art, label = g_shared->label;
        const long long start = g_shared->start;
        Unlock();
        char head[96], tail[64];
        snprintf(head, sizeof head, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu", GetCurrentProcessId());
        snprintf(tail, sizeof tail, "},\"nonce\":\"%lu-%u\"}", GetCurrentProcessId(), ++g_nonce);
        std::string json = head;
        if (owner && !details.empty())
        {
            json += ",\"activity\":{\"details\":\"" + Escape(Line(details)) + "\",";
            if (!state.empty()) json += "\"state\":\"" + Escape(Line(state)) + "\",";
            json += "\"timestamps\":{\"start\":" + std::to_string(start) + "},";
            json += "\"assets\":{\"large_image\":\"" + Escape(art) + "\",\"large_text\":\"" + Escape(label) + "\"}}";
        }
        json += tail;
        return SendFrame(OP_FRAME, json);
    }

    bool HasVisibleWindow()
    {
        struct Find { DWORD pid; bool found; } f = { GetCurrentProcessId(), false };
        EnumWindows([](HWND w, LPARAM p) -> BOOL
        {
            Find* f = reinterpret_cast<Find*>(p);
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid != f->pid || !IsWindowVisible(w)) return TRUE;
            f->found = true;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&f));
        return f.found;
    }

    // the focused tool owns the presence, the last owner keeps it while nothing of ours is focused, a tool with a window
    // takes over from an owner that went away
    bool Owns(uint64_t now)
    {
        DWORD fgPid = 0;
        const HWND fg = GetForegroundWindow();
        if (fg) GetWindowThreadProcessId(fg, &fgPid);
        const LONG owner = g_shared->owner;
        if (LONG(fgPid) == Me()) InterlockedExchange(&g_shared->owner, Me());
        else if (owner != Me() && (!owner || now - uint64_t(g_shared->ownerBeat) > kStaleMs) && HasVisibleWindow())
            InterlockedCompareExchange(&g_shared->owner, Me(), owner);
        if (g_shared->owner != Me()) return false;
        InterlockedExchange64(&g_shared->ownerBeat, LONG64(now));
        return true;
    }

    // writes only while this tool is still the owner, so a tool that just lost the focus cannot overwrite the new owner's text
    void Publish()
    {
        const unsigned gen = g_gen.load();
        if (gen == g_publishedGen && g_shared->textOwner == Me()) return;
        Activity a;
        if (g_mode.load() == Detail)
        {
            AcquireSRWLockShared(&g_lock);
            a = g_activity;
            ReleaseSRWLockShared(&g_lock);
        }
        if (a.details.empty()) a = Activity{ g_label, "" };
        Lock();
        if (g_shared->owner != Me()) { Unlock(); return; }
        Copy(g_shared->details, sizeof g_shared->details, a.details);
        Copy(g_shared->state, sizeof g_shared->state, a.state);
        Copy(g_shared->art, sizeof g_shared->art, g_art);
        Copy(g_shared->label, sizeof g_shared->label, g_label);
        g_shared->start = g_startUnix;
        g_shared->textOwner = Me();
        InterlockedIncrement(&g_shared->gen);
        Unlock();
        g_publishedGen = gen;
    }

    // gives up the owner and presenter roles, the other tools take them over
    void Release()
    {
        Disconnect();
        if (!g_shared) return;
        if (InterlockedCompareExchange(&g_shared->owner, 0, Me()) == Me()) InterlockedIncrement(&g_shared->gen);
        InterlockedCompareExchange(&g_shared->presenter, 0, Me());
    }

    void Step()
    {
        if (g_mode.load() == Off) { Release(); SetStatus("off"); return; }
        const uint64_t now = GetTickCount64();

        // owner: put this tool's activity up
        const bool owns = Owns(now);
        if (owns) Publish();

        // presenter: the one connection to Discord, kept across focus changes
        const LONG presenter = g_shared->presenter;
        if (presenter != Me())
        {
            Disconnect();
            if (presenter && now - uint64_t(g_shared->presenterBeat) <= kStaleMs) { SetStatus(owns ? "shown through another tool's connection" : "another tool is shown"); return; }
            if (now < g_nextTry) return;
            if (InterlockedCompareExchange(&g_shared->presenter, Me(), presenter) != presenter) return;
            InterlockedExchange64(&g_shared->presenterBeat, LONG64(now));
        }
        InterlockedExchange64(&g_shared->presenterBeat, LONG64(now));
        if (!g_ready)
        {
            if (now < g_nextTry || !Connect()) { InterlockedCompareExchange(&g_shared->presenter, 0, Me()); return; }
        }
        if (!Pump()) { Disconnect(); InterlockedCompareExchange(&g_shared->presenter, 0, Me()); SetStatus("Discord closed the connection"); return; }
        const LONG gen = g_shared->gen, owner = g_shared->owner;
        if ((gen == g_sentGen && owner == g_sentOwner) || !CanSend(now)) return;
        if (!owner)
        {
            if (!g_noOwnerSince) g_noOwnerSince = now;
            if (now - g_noOwnerSince < 1500) return;
        }
        else g_noOwnerSince = 0;
        if (!SendShared()) { Disconnect(); InterlockedCompareExchange(&g_shared->presenter, 0, Me()); SetStatus("Discord closed the connection"); return; }
        NoteSend(now);
        g_sentGen = gen;
        g_sentOwner = owner;
        SetStatus(owner == Me() ? "connected, showing this tool" : owner ? "connected, showing another tool" : "connected, nothing to show");
    }

    DWORD WINAPI Worker(void*)
    {
        try
        {
            g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), kSharedName);
            g_shared = g_map ? static_cast<Shared*>(MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared))) : nullptr;
            g_ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!g_shared) SetStatus("shared memory failed");
            while (g_shared && g_ioEvent && WaitForSingleObject(g_stop, kStepMs) == WAIT_TIMEOUT) Step();
            Release();
        }
        catch (...) { Disconnect(); }
        g_publishedGen = ~0u;
        if (g_shared) { UnmapViewOfFile(g_shared); g_shared = nullptr; }
        if (g_map) { CloseHandle(g_map); g_map = nullptr; }
        if (g_ioEvent) { CloseHandle(g_ioEvent); g_ioEvent = nullptr; }
        return 0;
    }

    void Start()
    {
        if (g_thread || g_appId.empty()) return;
        g_nextTry = 0;
        ResetEvent(g_stop);
        g_thread = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
        if (!g_thread) SetStatus("worker thread failed");
    }

    void Stop()
    {
        if (!g_thread) return;
        SetEvent(g_stop);
        WaitForSingleObject(g_thread, INFINITE);   // every wait in the worker also ends on g_stop
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

const char* const kModeLabels[3] = { "Off", "Show the tool", "Show the tool and what is open" };

namespace
{
    const char* const kModeKeys[3] = { "off", "tool", "detail" };
    int ModeFromKey(const std::string& key)
    {
        for (int i = 0; i < 3; ++i) if (key == kModeKeys[i]) return i;
        return -1;
    }
}

void Setup(const char* art, const char* label)
{
    g_art = art;
    g_label = label;
    g_appId = rp::ConfigGet("discord", "app_id", "");
    if (g_appId.empty()) g_appId = kDefaultAppId;
    const int mode = ModeFromKey(rp::ConfigGet("discord", "mode", "tool"));
    g_mode = mode < 0 ? Tool : mode;
    FILETIME created, a, b, c;
    ULARGE_INTEGER t = {};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &a, &b, &c)) GetSystemTimeAsFileTime(&created);
    t.LowPart = created.dwLowDateTime;
    t.HighPart = created.dwHighDateTime;
    g_startUnix = static_cast<long long>((t.QuadPart - 116444736000000000ULL) / 10000000ULL);
    if (!g_stop) g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_appId.empty()) { SetStatus("no application id ([discord] app_id)"); return; }
    if (g_mode.load() != Off) Start();
    else SetStatus("off");
}

bool Due()
{
    if (!g_thread || g_mode.load() != Detail) return false;
    const uint64_t now = GetTickCount64();
    if (now < g_nextDue) return false;
    g_nextDue = now + 1000;
    return true;
}

void Update(const Activity& activity)
{
    AcquireSRWLockExclusive(&g_lock);
    const bool changed = activity.details != g_activity.details || activity.state != g_activity.state;
    if (changed) g_activity = activity;
    ReleaseSRWLockExclusive(&g_lock);
    if (changed) g_gen.fetch_add(1);
}

Mode GetMode() { return Mode(g_mode.load()); }

void SetMode(Mode mode)
{
    if (mode < Off || mode > Detail) return;
    g_mode = mode;
    g_gen.fetch_add(1);   // republish in the new mode
    g_nextDue = 0;
    rp::host->ConfigSet(rp::self, "discord", "mode", kModeKeys[mode]);
    if (mode != Off) Start();   // the worker gives up its roles by itself when turned off
}

void Shutdown()
{
    Stop();
    if (g_stop) { CloseHandle(g_stop); g_stop = nullptr; }
}

bool Command(const std::string& c, std::string& reply)
{
    if (c != "discord" && c.rfind("discord ", 0) != 0) return false;
    const std::string arg = c.size() > 8 ? c.substr(8) : "";
    if (ModeFromKey(arg) >= 0) SetMode(Mode(ModeFromKey(arg)));
    else if (arg.rfind("appid ", 0) == 0)
    {
        Stop();
        g_appId = arg.substr(6);
        rp::host->ConfigSet(rp::self, "discord", "app_id", g_appId.c_str());
        if (g_mode.load() != Off) Start();
    }
    AcquireSRWLockShared(&g_lock);
    const std::string status = g_status;
    const Activity a = g_activity;
    ReleaseSRWLockShared(&g_lock);
    reply = "discord: " + std::string(kModeKeys[g_mode.load()]) + ", " + status + ", application id " + (g_appId.empty() ? "none" : g_appId)
          + ", detail activity \"" + a.details + "\" / \"" + a.state + "\" (discord off|tool|detail|appid <id>)";
    return true;
}
}
