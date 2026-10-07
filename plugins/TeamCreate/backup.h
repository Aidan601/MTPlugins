#pragma once
#include <string>

// Safety copies of the map files Team Create rewrites on disk (the root map on reload, prefab files changed by peers). Made before every rewrite unless the file
// still is what this session last copied or wrote (NoteWritten), kept as <root>\radiant\teamcreate_backup\<map>_<timestamp>.map (newest [backup] keep per map, only our own names are deleted).
// Snapshot copies the open map at session start. A failed copy asks the user (Yes = write anyway, No = leave the file).
namespace backup
{
    enum class Gate { Go, Wait, Skip };                 // write now / question open, retry later / do not write

    void         NewSession();                         // StartHost/StartClient: next rewrite copies again
    void         NoteWritten(const std::wstring& path); // after Team Create wrote the file: no copy while it stays as is
    void         Snapshot(const std::wstring& path);   // main thread, session start: copy once, a failure warns in chat
    bool         BeforeWrite(const std::wstring& path); // main thread: copy, on failure ask in a modal box, false = do not write
    Gate         BeforeWriteLater(const std::wstring& path); // main thread, from a tick: same, the question is posted (no modal loop)
    std::wstring Dir();                                // backup folder for the open map (empty when no root is known)
    std::string  Status();                             // "backup" command
}
