#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Browse plugins: the public plugin index (a JSON file on GitHub), fetched on a worker thread, and one-click
// install/update of an entry into <plugins>\<Name>\. Files are downloaded over HTTPS into a staging folder and checked
// against the SHA-256 in the index first, the move, unload and load run on the main thread (Tick).
namespace browse
{
    struct File { std::string path, url, sha256; bool config = false; };   // config: installed only when missing
    struct Entry
    {
        std::string name, displayName, author, description, url, version;
        std::string changes;           // optional one line: what changed in this version (shown in the update question)
        std::string minHost;           // optional MTPlugins version this build needs, older hosts see Incompatible
        uint32_t apps = 0;             // MODTOOLS_APP_* mask
        uint32_t apiVersion = 0;
        std::vector<File> files;
        // One of MTPlugins' own plugins not picked in Setup (see "Bundled plugins" in browse.cpp): no download,
        // Install runs the Setup kept beside the uninstaller, which adds `component`.
        bool bundled = false;
        std::string component;         // Setup's component name
        std::string note;              // optional line for the install question (what else Setup does for this plugin)
        // An update to an installed plugin of ours from the official feed (updates.json in the MTPlugins repo), installed
        // like an index entry. Listed only while it is newer than the installed copy.
        bool official = false;
    };
    // UpdateAvailable: files differ and the list's version is newer than the installed DLL's (or that is unknown).
    // Modified: files differ but the installed version is the same or newer (local build or edit), never offered as an update.
    enum class Status { NotInstalled, Installed, UpdateAvailable, Incompatible, Modified };

    void Refresh();                    // (re)load the index in the background
    bool Loading();
    bool Loaded();                     // an index is in hand (possibly empty)
    std::vector<Entry> Entries();      // main thread: the bundled plugins that are not installed, official updates, then the index
    Status StatusOf(const Entry& e);   // main thread, compares installed files with the index hashes
    const char* StatusName(Status s);

    std::vector<Entry> Updates();      // main thread: the installed, compatible entries whose status is UpdateAvailable
    std::string ChangesOf(const std::string& name, const std::string& version);   // the `changes` line the index or the feed has for it

    bool Install(const std::string& name);   // start download + verify, false (with Message) when refused or busy
    bool Installing();
    void InstallAll(const std::vector<std::string>& names);   // one after another, from Tick
    bool QueueBusy();                  // InstallAll still has plugins to do or one running
    std::vector<std::string> TakeQueueReport();   // one line per finished InstallAll item (the install message)
    std::string Message();             // last status or error line for the dialog
    unsigned Version();                // bumps when the index, the message or an install changes

    bool Uninstall(const std::string& name);   // main thread: unload, delete the plugin's files and forget it (finished in Tick)
    std::string UninstallNote(const std::string& name);   // the last uninstall result when it concerns this plugin ("" = any)
    std::string UninstallNote();

    void Tick();                       // main thread: finish a verified install or a deferred uninstall

    // Shared with update.cpp: an https:// URL (or, with localAllowed, a file path for tests), the same SHA-256, a worker
    // thread that pins the host DLL and never lets an exception out.
    bool Fetch(const std::string& src, bool localAllowed, size_t cap, std::string& body, std::string& err);
    std::string Sha256(const std::string& data);
    std::vector<int> ParseVersion(std::string v);                    // "v1.2.3" / "1.2" -> {1,2,3}, empty when not numbers and dots
    bool Newer(const std::vector<int>& a, const std::vector<int>& b);   // a > b, missing parts are 0
    bool StartWorker(void (*fn)(void*), void* arg);
}
