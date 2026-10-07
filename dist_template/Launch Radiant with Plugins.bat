@echo off
rem Starts Radiant (BO3 mod tools) with the plugin host loaded.
rem Drop a .map file on this file to open it right away.
rem The plugins folder next to this file is installed into <game>\bin\plugins first (newer files only, inis kept).
cd /d "%~dp0"
if exist "%~dp0plugins\" "%~dp0RadiantPlugins_Launch.exe" --install-plugins "%~dp0plugins"
start "" "%~dp0RadiantPlugins_Launch.exe" %*
