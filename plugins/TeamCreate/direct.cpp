// Direct link (direct.h). Host: one server thread opens a TCP port on the router (UPnP) or, without one, meets each client
// halfway from the listener's public endpoint (STUN, TCP hole punching). A link proves the session token, then asks for blocks (zonesync.cpp answers).
// Threads: the server thread is joined by HostStop(true), link threads are detached and counted, the server waits for them.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <bcrypt.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <set>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include "direct.h"
#include "net.h"
#include "compat.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "bcrypt.lib")

namespace
{
    constexpr int kMaxLinks = 8;
    constexpr int kLeaseSeconds = 3600;
    constexpr ULONGLONG kRenewMs = 20 * 60 * 1000;
    constexpr size_t kMaxRequest = 4096;
    constexpr int kPunchMs = 10000;                 // both sides dial each other this long
    const char* kStunServers = "stun.nextcloud.com:443,stun.voipgate.com:3478,stun.freeswitch.org:3478";   // STUN over TCP

    std::mutex g_m;
    std::thread g_thread;
    std::atomic<bool> g_stop{ false }, g_done{ true }, g_unmapping{ false };
    std::atomic<int> g_links{ 0 };
    std::set<SOCKET> g_linkSocks;          // g_m
    SOCKET g_listen = INVALID_SOCKET;      // g_m
    bool g_ready = false;                  // g_m
    bool g_punch = false;                  // g_m: no open port, links are punched
    uint16_t g_port = 0;                   // g_m: the listener's port
    std::vector<uint32_t> g_asks;          // g_m: peers waiting for an offer
    std::vector<std::pair<std::string, uint16_t>> g_punches;   // g_m: endpoints to dial
    direct::Offer g_offer;                 // g_m
    std::string g_status = "off";          // g_m
    direct::Serve g_serve;                 // g_m
    direct::OfferFn g_send;                // g_m

    // Radiant exits without OnUnload: a joinable thread at static destruction would terminate the process.
    struct ExitThreadGuard { ~ExitThreadGuard() { if (g_thread.joinable()) g_thread.detach(); } } g_exitThreadGuard;

    bool Stopping() { return g_stop && !g_unmapping; }   // the unmap request at the end still reads its answer
    void SetStatus(const std::string& s) { { std::lock_guard<std::mutex> lock(g_m); g_status = s; } LOG("direct: %s", s.c_str()); }

    bool Parse4(const std::string& ip, unsigned b[4])
    {
        in_addr a{};
        if (inet_pton(AF_INET, ip.c_str(), &a) != 1) return false;
        const uint32_t v = ntohl(a.s_addr);
        b[0] = v >> 24; b[1] = (v >> 16) & 255; b[2] = (v >> 8) & 255; b[3] = v & 255;
        return true;
    }

    bool SendAll(SOCKET s, const char* p, size_t n)
    {
        while (n) { int k = send(s, p, int(std::min<size_t>(n, 1 << 20)), 0); if (k <= 0) return false; p += k; n -= size_t(k); }
        return true;
    }
    bool RecvAll(SOCKET s, char* p, size_t n)
    {
        while (n) { int k = recv(s, p, int(std::min<size_t>(n, 1 << 20)), 0); if (k <= 0) return false; p += k; n -= size_t(k); }
        return true;
    }
    void SetTimeouts(SOCKET s, DWORD recvMs, DWORD sendMs)
    {
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recvMs), sizeof recvMs);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendMs), sizeof sendMs);
    }
    // A TCP connect that gives up after timeoutMs (a filtered port would block about 20 s). localPort 0 = any,
    // else the socket shares that port with the listener (SO_REUSEADDR), which hole punching needs.
    SOCKET ConnectFrom(uint16_t localPort, const sockaddr_in& addr, int timeoutMs)
    {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return s;
        if (localPort)
        {
            BOOL yes = TRUE; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
            sockaddr_in me{}; me.sin_family = AF_INET; me.sin_port = htons(localPort);
            if (bind(s, reinterpret_cast<sockaddr*>(&me), sizeof me) != 0) { closesocket(s); return INVALID_SOCKET; }
        }
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
        bool ok = connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0;
        if (!ok && WSAGetLastError() == WSAEWOULDBLOCK)
        {
            fd_set w; FD_ZERO(&w); FD_SET(s, &w);
            fd_set e; FD_ZERO(&e); FD_SET(s, &e);
            timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
            ok = select(0, nullptr, &w, &e, &tv) > 0 && FD_ISSET(s, &w);
        }
        if (!ok) { closesocket(s); return INVALID_SOCKET; }
        nb = 0; ioctlsocket(s, FIONBIO, &nb);
        BOOL nd = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof nd);
        return s;
    }
    SOCKET ConnectTimeout(const std::string& ip, uint16_t port, int timeoutMs, uint16_t localPort = 0)
    {
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) return INVALID_SOCKET;
        return ConnectFrom(localPort, addr, timeoutMs);
    }

    // ---- hole punching ----
    // The public endpoint of a local TCP port as one STUN server sees it (RFC 5389 binding request over TCP).
    bool Stun(uint16_t localPort, const std::string& server, std::string& ip, uint16_t& port)
    {
        const size_t colon = server.rfind(':');
        if (colon == std::string::npos) return false;
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(server.substr(0, colon).c_str(), server.substr(colon + 1).c_str(), &hints, &res) != 0 || !res) return false;
        sockaddr_in to = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
        freeaddrinfo(res);
        SOCKET s = ConnectFrom(localPort, to, 2500);
        if (s == INVALID_SOCKET) return false;
        SetTimeouts(s, 2500, 2500);
        uint8_t req[20] = { 0, 1, 0, 0, 0x21, 0x12, 0xA4, 0x42 };
        BCryptGenRandom(nullptr, req + 8, 12, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        uint8_t head[20]; std::vector<uint8_t> body;
        bool ok = SendAll(s, reinterpret_cast<const char*>(req), sizeof req) && RecvAll(s, reinterpret_cast<char*>(head), sizeof head);
        const size_t len = ok ? size_t(head[2]) << 8 | head[3] : 0;
        if (ok && len && len <= 1024) { body.resize(len); ok = RecvAll(s, reinterpret_cast<char*>(body.data()), len); }
        closesocket(s);
        if (!ok || memcmp(head + 8, req + 8, 12) != 0) return false;
        bool found = false;
        for (size_t p = 0; p + 4 <= body.size(); )
        {
            const uint16_t type = uint16_t(body[p] << 8 | body[p + 1]); const size_t al = size_t(body[p + 2]) << 8 | body[p + 3];
            if (p + 4 + al > body.size()) break;
            const uint8_t* v = body.data() + p + 4;
            if ((type == 0x0020 || (type == 0x0001 && !found)) && al >= 8 && v[1] == 1)
            {
                uint8_t a[4]; memcpy(a, v + 4, 4); port = uint16_t(v[2] << 8 | v[3]);
                if (type == 0x0020) { port ^= 0x2112; a[0] ^= 0x21; a[1] ^= 0x12; a[2] ^= 0xA4; a[3] ^= 0x42; }
                char text[32]; snprintf(text, sizeof text, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
                ip = text; found = true;
            }
            p += 4 + ((al + 3) & ~size_t(3));
        }
        return found;
    }
    // The public endpoint of a local port, checked against a second server: a NAT that gives every destination another port cannot be punched.
    bool PublicEndpoint(uint16_t localPort, std::string& ip, uint16_t& port, std::string& why, const std::atomic<bool>& cancel)
    {
        std::string list = rp::ConfigGet("direct", "stun", "");
        if (list.find(':') == std::string::npos) list = kStunServers;
        int answers = 0;
        for (size_t p = 0; p <= list.size() && answers < 2 && !cancel; )
        {
            size_t e = list.find(',', p); if (e == std::string::npos) e = list.size();
            std::string server = list.substr(p, e - p); p = e + 1;
            while (!server.empty() && server.front() == ' ') server.erase(server.begin());
            std::string ip2; uint16_t port2 = 0;
            if (server.empty() || !Stun(localPort, server, ip2, port2)) continue;
            if (answers && (ip2 != ip || port2 != port)) { why = "this network gives every connection another public port (symmetric NAT)"; return false; }
            ip = ip2; port = port2; ++answers;
        }
        if (!answers) { why = "no STUN server answered"; return false; }
        if (!direct::IsRoutable(ip)) { why = "no public address"; return false; }
        return true;
    }
    // Dials the other side from our port again and again while it does the same: the outgoing SYNs open both NATs, the two meet.
    SOCKET PunchConnect(uint16_t localPort, const std::string& ip, uint16_t port, int totalMs, const std::atomic<bool>& cancel)
    {
        const ULONGLONG end = GetTickCount64() + ULONGLONG(totalMs);
        while (GetTickCount64() < end && !cancel)
        {
            SOCKET s = ConnectTimeout(ip, port, 700, localPort);
            if (s != INVALID_SOCKET) return s;
            Sleep(120);
        }
        return INVALID_SOCKET;
    }

    // ---- UPnP ----
    struct Url { std::string host; uint16_t port = 80; std::string path = "/"; };
    bool ParseUrl(const std::string& u, Url& out)
    {
        if (_strnicmp(u.c_str(), "http://", 7) != 0) return false;
        const size_t slash = u.find('/', 7);
        std::string hp = u.substr(7, slash == std::string::npos ? std::string::npos : slash - 7);
        out.path = slash == std::string::npos ? "/" : u.substr(slash);
        const size_t colon = hp.find(':');
        out.host = hp.substr(0, colon);
        out.port = colon == std::string::npos ? 80 : uint16_t(atoi(hp.c_str() + colon + 1));
        return !out.host.empty() && out.port != 0;
    }
    std::string LowerCopy(std::string s) { for (auto& c : s) c = char(tolower(uint8_t(c))); return s; }
    std::string Header(const std::string& head, const char* name)   // value of one header, name in lower case
    {
        const std::string low = LowerCopy(head), key = std::string("\n") + name + ":";
        size_t p = low.find(key);
        if (p == std::string::npos) return "";
        p += key.size();
        size_t e = head.find('\r', p); if (e == std::string::npos) e = head.size();
        std::string v = head.substr(p, e - p);
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        while (!v.empty() && (v.back() == ' ' || v.back() == '\n')) v.pop_back();
        return v;
    }
    // One HTTP/1.1 request to a LAN device, the whole answer read until the device closes.
    bool Http(const Url& u, const std::string& method, const std::string& headers, const std::string& body, int& status, std::string& out, std::string* localIp)
    {
        SOCKET s = ConnectTimeout(u.host, u.port, 2000);
        if (s == INVALID_SOCKET) return false;
        SetTimeouts(s, 3000, 3000);
        if (localIp)
        {
            sockaddr_in me{}; int len = sizeof me; char ip[64] = {};
            if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0 && inet_ntop(AF_INET, &me.sin_addr, ip, sizeof ip)) *localIp = ip;
        }
        std::string req = method + " " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port) + "\r\nConnection: close\r\nUser-Agent: MTPlugins/1.0 UPnP/1.1\r\n" + headers;
        if (method == "POST") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "\r\n" + body;
        std::string resp;
        bool ok = SendAll(s, req.data(), req.size());
        char buf[8192];
        while (ok && resp.size() < (512u << 10) && !Stopping())
        {
            int n = recv(s, buf, sizeof buf, 0);
            if (n <= 0) break;
            resp.append(buf, size_t(n));
            const size_t he = resp.find("\r\n\r\n");
            if (he == std::string::npos) continue;
            const std::string cl = Header(resp.substr(0, he + 2), "content-length");
            if (!cl.empty() && resp.size() >= he + 4 + size_t(atoll(cl.c_str()))) break;
        }
        closesocket(s);
        const size_t he = resp.find("\r\n\r\n");
        if (!ok || he == std::string::npos || resp.size() < 12) return false;
        status = atoi(resp.c_str() + 9);
        const std::string head = resp.substr(0, he + 2);
        out = resp.substr(he + 4);
        if (LowerCopy(Header(head, "transfer-encoding")).find("chunked") != std::string::npos)
        {
            std::string plain; size_t p = 0;
            while (p < out.size())
            {
                const size_t e = out.find("\r\n", p);
                if (e == std::string::npos) break;
                const size_t n = size_t(strtoul(out.c_str() + p, nullptr, 16));
                if (!n || e + 2 + n > out.size()) break;
                plain.append(out, e + 2, n); p = e + 2 + n + 2;
            }
            out.swap(plain);
        }
        return true;
    }
    std::string Tag(const std::string& xml, const char* name, size_t from = 0, size_t until = std::string::npos)
    {
        const std::string open = std::string("<") + name + ">", close = std::string("</") + name + ">";
        const size_t a = xml.find(open, from);
        if (a == std::string::npos || a >= until) return "";
        const size_t b = xml.find(close, a);
        if (b == std::string::npos) return "";
        std::string v = xml.substr(a + open.size(), b - a - open.size());
        while (!v.empty() && isspace(uint8_t(v.front()))) v.erase(v.begin());
        while (!v.empty() && isspace(uint8_t(v.back()))) v.pop_back();
        return v;
    }

    struct Gateway { Url control; std::string service, localIp; };

    // IPv4 addresses of this PC on private LANs (a router that speaks UPnP sits on one of them).
    std::vector<std::string> LanAddresses()
    {
        std::vector<std::string> out;
        ULONG size = 32 << 10;
        std::vector<uint8_t> buf(size);
        auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
        if (rc == ERROR_BUFFER_OVERFLOW)
        {
            buf.resize(size); list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
            rc = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
        }
        if (rc != NO_ERROR) return out;
        for (auto* a = list; a; a = a->Next)
        {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
            {
                char ip[64] = {};
                auto* sa = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
                if (!sa || sa->sin_family != AF_INET || !inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip)) continue;
                if (direct::IsLan(ip) && strncmp(ip, "127.", 4) != 0) out.push_back(ip);
            }
        }
        return out;
    }

    // SSDP search on every LAN adapter, then the device description of each answer until one has a WAN connection service.
    bool Discover(Gateway& gw)
    {
        static const char* kTypes[] = { "urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANPPPConnection:1" };
        std::vector<std::string> locations;
        for (const std::string& lan : LanAddresses())
        {
            SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == INVALID_SOCKET) continue;
            sockaddr_in me{}; me.sin_family = AF_INET; inet_pton(AF_INET, lan.c_str(), &me.sin_addr);
            if (bind(s, reinterpret_cast<sockaddr*>(&me), sizeof me) != 0) { closesocket(s); continue; }
            setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&me.sin_addr), sizeof me.sin_addr);
            sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(1900); inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
            for (const char* st : kTypes)
            {
                const std::string q = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: ") + st + "\r\n\r\n";
                sendto(s, q.data(), int(q.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
            }
            const ULONGLONG end = GetTickCount64() + 1800;
            while (GetTickCount64() < end && !g_stop)
            {
                fd_set r; FD_ZERO(&r); FD_SET(s, &r);
                timeval tv{ 0, 200 * 1000 };
                if (select(0, &r, nullptr, nullptr, &tv) <= 0) { if (!locations.empty()) break; continue; }
                char buf[2048]; int n = recv(s, buf, sizeof buf - 1, 0);
                if (n <= 0) continue;
                const std::string loc = Header("\r\n" + std::string(buf, size_t(n)), "location");
                if (!loc.empty() && std::find(locations.begin(), locations.end(), loc) == locations.end()) locations.push_back(loc);
            }
            closesocket(s);
            if (!locations.empty()) break;
        }
        for (const std::string& loc : locations)
        {
            Url base; int status = 0; std::string xml, localIp;
            if (g_stop || !ParseUrl(loc, base) || !Http(base, "GET", "", "", status, xml, &localIp) || status != 200) continue;
            for (const char* st : kTypes)
            {
                const size_t at = xml.find(std::string("<serviceType>") + st + "</serviceType>");
                if (at == std::string::npos) continue;
                const std::string ctl = Tag(xml, "controlURL", at, xml.find("</service>", at));
                if (ctl.empty()) continue;
                Url c = base;
                const std::string urlBase = Tag(xml, "URLBase");
                if (!urlBase.empty()) ParseUrl(urlBase, c);
                if (_strnicmp(ctl.c_str(), "http://", 7) == 0) { if (!ParseUrl(ctl, c)) continue; }
                else c.path = ctl[0] == '/' ? ctl : "/" + ctl;
                gw.control = c; gw.service = st; gw.localIp = localIp;
                return true;
            }
        }
        return false;
    }
    // One SOAP action on the gateway. 0 = done, a UPnP error code, or -1 when the router did not answer.
    int Soap(const Gateway& gw, const char* action, const std::string& args, std::string& out)
    {
        const std::string body = std::string("<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:") +
            action + " xmlns:u=\"" + gw.service + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>\r\n";
        const std::string headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + gw.service + "#" + action + "\"\r\n";
        int status = 0;
        if (!Http(gw.control, "POST", headers, body, status, out, nullptr)) return -1;
        if (status == 200) return 0;
        const std::string code = Tag(out, "errorCode");
        return code.empty() ? -1 : atoi(code.c_str());
    }
    int AddMapping(const Gateway& gw, uint16_t external, uint16_t internal, int lease)
    {
        std::string out;
        const std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(external) + "</NewExternalPort><NewProtocol>TCP</NewProtocol><NewInternalPort>" + std::to_string(internal) +
            "</NewInternalPort><NewInternalClient>" + gw.localIp + "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>MTPlugins Team Create</NewPortMappingDescription><NewLeaseDuration>" +
            std::to_string(lease) + "</NewLeaseDuration>";
        return Soap(gw, "AddPortMapping", args, out);
    }
    void DeleteMapping(const Gateway& gw, uint16_t external)
    {
        std::string out;
        Soap(gw, "DeletePortMapping", "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(external) + "</NewExternalPort><NewProtocol>TCP</NewProtocol>", out);
    }
    // Maps a port to our listener: the same number first, 725 = the router knows only permanent mappings.
    bool Map(const Gateway& gw, uint16_t internal, uint16_t& external, int& lease, std::string& why)
    {
        lease = kLeaseSeconds;
        for (int attempt = 0; attempt < 4 && !g_stop; ++attempt)
        {
            external = uint16_t(internal + attempt * 7);
            int rc = AddMapping(gw, external, internal, lease);
            if (rc == 725) { lease = 0; rc = AddMapping(gw, external, internal, lease); }
            if (rc == 0) return true;
            if (rc != 718) { why = rc < 0 ? "the router did not answer the port request" : "the router refused the port (UPnP error " + std::to_string(rc) + ")"; return false; }
        }
        why = "the router has no free port for the link";
        return false;
    }

    // ---- links ----
    std::string Token()
    {
        uint8_t raw[16] = {};
        BCryptGenRandom(nullptr, raw, sizeof raw, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        return std::string(reinterpret_cast<const char*>(raw), sizeof raw);
    }
    void LinkMain(SOCKET s)
    {
        try
        {
            SetTimeouts(s, 15000, 60000);
            uint16_t type = 0; std::vector<uint8_t> payload;
            std::string token; direct::Serve serve;
            { std::lock_guard<std::mutex> lock(g_m); token = g_offer.token; serve = g_serve; }
            bool ok = direct::ReadFrame(s, type, payload, 256) && type == net::MSG_DIRECT_HELLO;
            if (ok) { net::Reader r(payload); const std::string got = r.blob(); ok = r.ok && got.size() == token.size() && !token.empty() && memcmp(got.data(), token.data(), token.size()) == 0; }
            if (ok) { net::Writer w; w.u8(1); ok = direct::SendFrame(s, net::MSG_DIRECT_HELLO, w.buf); }
            if (ok) LOG("direct: a peer opened its link");
            SetTimeouts(s, 120000, 60000);
            uint64_t served = 0;
            while (ok && !g_stop && serve)
            {
                uint16_t replyType = 0; std::vector<uint8_t> reply;
                if (!direct::ReadFrame(s, type, payload, kMaxRequest) || !serve(type, payload, replyType, reply) || !direct::SendFrame(s, replyType, reply)) break;
                served += reply.size();
            }
            if (ok) LOG("direct: a peer's link closed (%.1f MB sent)", served / 1048576.0);
        }
        catch (...) {}
        { std::lock_guard<std::mutex> lock(g_m); g_linkSocks.erase(s); }
        closesocket(s);
        --g_links;
    }
    // A client asked to be met halfway: dial its public endpoint from the listener's port, then serve the link.
    void PunchMain(uint16_t localPort, std::string ip, uint16_t port)
    {
        SOCKET s = INVALID_SOCKET;
        try { s = PunchConnect(localPort, ip, port, kPunchMs, g_stop); } catch (...) {}
        if (s == INVALID_SOCKET) { --g_links; return; }   // the client reached the listener instead, or no path
        { std::lock_guard<std::mutex> lock(g_m); g_linkSocks.insert(s); }
        LinkMain(s);
    }

    void ServerMain(SOCKET listenSock, uint16_t port, std::string testOffer, bool testPunch)
    {
        Gateway gw; bool mapped = false; uint16_t external = 0; int lease = 0;
        try
        {
            std::string ip, why;
            bool punch = false;
            if (!testOffer.empty()) { ip = testOffer; external = port; punch = testPunch; }
            else
            {
                SetStatus("looking for a router that opens ports (UPnP)");
                if (!Discover(gw)) why = "no router here answers UPnP";
                else if ((mapped = Map(gw, port, external, lease, why)))
                {
                    std::string out;
                    if (Soap(gw, "GetExternalIPAddress", "", out) == 0) ip = Tag(out, "NewExternalIPAddress");
                    if (!direct::IsRoutable(ip)) { ip.clear(); why = "the router has no public address (it sits behind another router or a carrier NAT)"; }
                }
                if (ip.empty() && mapped) { DeleteMapping(gw, external); mapped = false; }
                // no open port: the listener's public endpoint by STUN, each client is met halfway
                if (ip.empty() && !g_stop && rp::ConfigGetBool("direct", "punch", true))
                {
                    std::string why2;
                    if (PublicEndpoint(port, ip, external, why2, g_stop)) punch = true;
                    else { ip.clear(); why += ", and hole punching cannot work: " + why2; }
                }
            }
            if (ip.empty()) { if (!g_stop) SetStatus("not available: " + why + "; builds travel over the session link"); }
            else
            {
                {
                    std::lock_guard<std::mutex> lock(g_m);
                    g_offer.ip = ip; g_offer.port = external; g_offer.mode = punch ? 1 : 0; g_punch = punch; g_ready = true;
                }
                SetStatus(punch ? "ready: no open port, links are made by hole punching (public port " + std::to_string(external) + ")" :
                          "ready: port " + std::to_string(external) + (testOffer.empty() ? (lease ? " opened on the router for an hour at a time" : " opened on the router") : " (test offer)"));
                ULONGLONG renewAt = GetTickCount64() + kRenewMs;
                while (!g_stop)
                {
                    // offers that waited for the port, punch requests
                    std::vector<uint32_t> asks; std::vector<std::pair<std::string, uint16_t>> punches;
                    direct::OfferFn send; direct::Offer offer;
                    { std::lock_guard<std::mutex> lock(g_m); asks.swap(g_asks); punches.swap(g_punches); send = g_send; offer = g_offer; }
                    if (!asks.empty() && punch && testOffer.empty())
                    {
                        // a fresh look per request: the mapping of an idle port can move
                        std::string ip2, why2; uint16_t port2 = 0;
                        if (PublicEndpoint(port, ip2, port2, why2, g_stop)) { offer.ip = ip2; offer.port = port2; std::lock_guard<std::mutex> lock(g_m); g_offer.ip = ip2; g_offer.port = port2; }
                    }
                    for (uint32_t peer : asks) if (send) send(peer, offer);
                    for (const auto& p : punches)
                    {
                        if (g_links >= kMaxLinks) break;
                        ++g_links;
                        std::thread(PunchMain, port, p.first, p.second).detach();
                    }
                    fd_set r; FD_ZERO(&r); FD_SET(listenSock, &r);
                    timeval tv{ 0, 200 * 1000 };
                    if (select(0, &r, nullptr, nullptr, &tv) > 0)
                    {
                        SOCKET s = accept(listenSock, nullptr, nullptr);
                        if (s == INVALID_SOCKET) continue;
                        if (g_links >= kMaxLinks) { closesocket(s); continue; }
                        BOOL nd = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof nd);
                        { std::lock_guard<std::mutex> lock(g_m); g_linkSocks.insert(s); }
                        ++g_links;
                        std::thread(LinkMain, s).detach();
                    }
                    if (mapped && lease && GetTickCount64() > renewAt) { AddMapping(gw, external, port, lease); renewAt = GetTickCount64() + kRenewMs; }
                }
            }
        }
        catch (...) {}
        {
            std::lock_guard<std::mutex> lock(g_m);
            g_ready = false; g_asks.clear(); g_punches.clear();
            for (SOCKET s : g_linkSocks) shutdown(s, SD_BOTH);
            if (g_listen == listenSock) g_listen = INVALID_SOCKET;
        }
        closesocket(listenSock);
        for (int i = 0; i < 240 && g_links > 0; ++i) Sleep(50);
        if (mapped) { g_unmapping = true; DeleteMapping(gw, external); g_unmapping = false; }
        g_done = true;
    }
}

namespace direct
{
    bool IsLan(const std::string& ip)
    {
        unsigned b[4];
        if (!Parse4(ip, b)) return false;
        return b[0] == 127 || b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) || (b[0] == 192 && b[1] == 168);
    }
    bool IsRoutable(const std::string& ip)
    {
        unsigned b[4];
        if (!Parse4(ip, b) || IsLan(ip)) return false;
        return b[0] != 0 && b[0] < 224 && !(b[0] == 169 && b[1] == 254) && !(b[0] == 100 && b[1] >= 64 && b[1] <= 127);
    }

    void HostEnsure(Serve serve, OfferFn send)
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_thread.joinable())
        {
            if (!g_done || !g_stop) return;      // running, or tried already in this session
            g_thread.join();
        }
        std::string bindAddr;
        for (wchar_t c : Cfg().bindAddr) bindAddr += char(c);
        const std::string testOffer = rp::ConfigGet("direct", "test_offer", "");
        if (testOffer.empty() && strncmp(bindAddr.c_str(), "127.", 4) == 0) { g_status = "off: the session listens on this PC only"; return; }
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        // shared with the sockets that dial out from this port (hole punching)
        BOOL yes = TRUE; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
        sockaddr_in addr{}; addr.sin_family = AF_INET;
        if (inet_pton(AF_INET, bindAddr.c_str(), &addr.sin_addr) != 1) addr.sin_addr.s_addr = htonl(INADDR_ANY);
        int len = sizeof addr;
        if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(s, 8) != 0 || getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        {
            if (s != INVALID_SOCKET) closesocket(s);
            g_status = "not available: no listening port on this PC";
            return;
        }
        g_listen = s; g_port = ntohs(addr.sin_port); g_serve = std::move(serve); g_send = std::move(send);
        g_offer = Offer{}; g_offer.token = Token();
        g_ready = false; g_punch = false; g_stop = false; g_done = false;
        g_asks.clear(); g_punches.clear();
        g_thread = std::thread(ServerMain, s, g_port, testOffer, rp::ConfigGetBool("direct", "test_punch", false));
    }
    void HostAsk(uint32_t peer)
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (!g_thread.joinable() || g_done) return;
        if (std::find(g_asks.begin(), g_asks.end(), peer) == g_asks.end()) g_asks.push_back(peer);
    }
    void HostPunch(const std::string& ip, uint16_t port)
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (!g_ready || !g_punch || !port || g_punches.size() >= size_t(kMaxLinks)) return;
        if (!IsRoutable(ip) && rp::ConfigGet("direct", "test_offer", "").empty()) return;
        g_punches.push_back({ ip, port });
    }
    void HostStop(bool wait)
    {
        std::unique_lock<std::mutex> lock(g_m);
        if (!g_thread.joinable()) { g_status = "off"; return; }
        g_stop = true;
        if (!wait && !g_done) return;            // the server thread unmaps and ends by itself
        lock.unlock();
        g_thread.join();
        lock.lock();
        g_status = "off";
    }
    std::string Status() { std::lock_guard<std::mutex> lock(g_m); return g_status + (g_links > 0 ? ", " + std::to_string(g_links.load()) + " link(s) open" : ""); }

    static Sock Hello(SOCKET s, const Offer& o)
    {
        SetTimeouts(s, 5000, 60000);
        net::Writer w; w.blob(o.token);
        uint16_t type = 0; std::vector<uint8_t> payload;
        if (!SendFrame(Sock(s), net::MSG_DIRECT_HELLO, w.buf) || !ReadFrame(Sock(s), type, payload, 64) || type != net::MSG_DIRECT_HELLO || payload.empty() || payload[0] != 1)
        { closesocket(s); return kNoSock; }
        SetTimeouts(s, 0, 60000);
        return Sock(s);
    }
    Sock Dial(const Offer& o, int timeoutMs, const std::atomic<bool>& cancel, const std::function<void(const std::string&, uint16_t)>& announce, std::string& why)
    {
        if (o.mode == 0)
        {
            SOCKET s = ConnectTimeout(o.ip, o.port, timeoutMs);
            if (s == INVALID_SOCKET) { why = "the host's port did not answer"; return kNoSock; }
            const Sock k = Hello(s, o);
            if (k == kNoSock) why = "the host refused the link";
            return k;
        }
        // hole punching: a port of our own, its public endpoint to the host, then both sides dial
        SOCKET hold = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOL yes = TRUE; setsockopt(hold, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
        sockaddr_in me{}; me.sin_family = AF_INET; int len = sizeof me;
        if (hold == INVALID_SOCKET || bind(hold, reinterpret_cast<sockaddr*>(&me), sizeof me) != 0 || getsockname(hold, reinterpret_cast<sockaddr*>(&me), &len) != 0)
        { if (hold != INVALID_SOCKET) closesocket(hold); why = "no local port"; return kNoSock; }
        const uint16_t local = ntohs(me.sin_port);
        std::string ip = rp::ConfigGet("direct", "test_offer", ""); uint16_t port = local;
        if (ip.empty() && !PublicEndpoint(local, ip, port, why, cancel)) { closesocket(hold); return kNoSock; }
        announce(ip, port);
        SOCKET s = PunchConnect(local, o.ip, o.port, kPunchMs, cancel);
        closesocket(hold);
        if (s == INVALID_SOCKET) { why = "the two networks did not let a connection through"; return kNoSock; }
        const Sock k = Hello(s, o);
        if (k == kNoSock) why = "the host refused the link";
        return k;
    }
    void Close(Sock s) { if (s != kNoSock) { shutdown(SOCKET(s), SD_BOTH); closesocket(SOCKET(s)); } }
    bool SendFrame(Sock s, uint16_t type, const std::vector<uint8_t>& payload)
    {
        char head[10]; const uint32_t len = uint32_t(payload.size());
        for (int i = 0; i < 4; ++i) head[i] = char(len >> (8 * i));
        head[4] = char(type); head[5] = char(type >> 8); memset(head + 6, 0, 4);
        return SendAll(SOCKET(s), head, sizeof head) && (payload.empty() || SendAll(SOCKET(s), reinterpret_cast<const char*>(payload.data()), payload.size()));
    }
    bool ReadFrame(Sock s, uint16_t& type, std::vector<uint8_t>& payload, size_t maxLen)
    {
        uint8_t head[10];
        if (!RecvAll(SOCKET(s), reinterpret_cast<char*>(head), sizeof head)) return false;
        const uint32_t len = head[0] | (head[1] << 8) | (head[2] << 16) | (uint32_t(head[3]) << 24);
        if (len > maxLen) return false;
        type = uint16_t(head[4] | (head[5] << 8));
        payload.resize(len);
        return len == 0 || RecvAll(SOCKET(s), reinterpret_cast<char*>(payload.data()), len);
    }
}
