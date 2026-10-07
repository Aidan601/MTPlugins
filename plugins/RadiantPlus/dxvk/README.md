# DXVK for Radiant (optional, off by default)

`RadiantPlus dxvk 1` (command channel) or Settings > Preferences > Camera, "Use DXVK (Vulkan) renderer", copies DXVK's `dxgi.dll` and `d3d11.dll` from this
folder into `<root>\bin`, next to the editors; Windows resolves those names from the exe's folder before System32, so at
the next start Radiant's Direct3D 11 renderer runs on Vulkan through DXVK (https://github.com/doitsujin/dxvk, zlib
license) instead of the native driver. `dxvk 0` removes them again (renamed to `*.dxvk-off` while a running editor
still maps them, deleted at the next status call). `RadiantPlus dxvk` reports whether DXVK is active in the running
editor and whether the files are in bin. Restart Radiant to apply.

Why bin and not a loader preload: Radiant loads dxgi/d3d11 by name from its exe TLS initialiser, which Windows runs before
any DLL's DllMain, so no in-process hook is early enough (tried 2026-09-24 in `bin\d3d9.dll`, reverted). Side effect: APE (its
exe imports d3d11 statically) uses DXVK too while the files are there. The launcher and its tools have no 3D renderer.

Contents: `dxgi.dll`, `d3d11.dll` (the release tarball's `x64` folder; never its `d3d9.dll`: `bin\d3d9.dll` is the plugin
loader), `version.txt` (shown by the status), `LICENSE.txt` (zlib, ships with the DLLs). The installer ships all four. The DLLs are not in git:
`fetch_dxvk.ps1` downloads the pinned release and checks its hash (`installer\build_installer.bat` runs it when they are missing); the dev
copy in `build\Release\plugins\RadiantPlus\dxvk\` is DXVK 3.1.1 (2026-09-15, sha256 of dxvk-3.1.1.tar.gz
40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608). DXVK writes `radiant_modtools_dxgi.log` /
`_d3d11.log` next to the exe (bin).
