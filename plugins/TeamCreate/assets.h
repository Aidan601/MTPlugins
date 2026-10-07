#pragma once
#include <string>
namespace net { struct Message; }
namespace assets
{
    void Start(bool host);
    void Stop();
    // Main thread, session over: ask once whether to keep streamed assets (saved to teamcreate_assets), then Stop.
    void EndSession(const char* why);
    void SetEndAnswer(int answer);    // tests: -1 ask (default), 0 discard, 1 keep without a box
    bool HasStreamed();               // temporary streamed assets are held (close offers a save first)
    void Tick();
    void OnMessage(const net::Message& message);
    void OnSaved(const std::wstring& path);
    std::string Status();
    std::string HudLine();            // camera view: live transfer line, else the last event for 10 s, else empty
    void SetBackupRetention(unsigned days, unsigned keep);   // [backup] asset_days / asset_keep
    void SetSendConverted(bool on);   // [assets] send_converted
}
