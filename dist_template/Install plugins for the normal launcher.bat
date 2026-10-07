@echo off
rem One-time install: copies plugins\ into <game>\bin\plugins and adds a d3d9.dll that loads the host from THIS folder.
rem Run it again after moving the folder. Undo: RadiantPlugins_Launch.exe --uninstall-loader
cd /d "%~dp0"
if exist "%~dp0plugins\" "%~dp0RadiantPlugins_Launch.exe" --install-plugins "%~dp0plugins"
"%~dp0RadiantPlugins_Launch.exe" --install-loader
pause
