@echo off
rem Makes a new plugin project from the HelloPlugin example. Usage: NewPlugin.bat [MyTool] (asks for the name without one)
rem Creates MyPlugins\MyTool\ (vcxproj, cpp, CMakeLists.txt), see GETTING_STARTED.md.
setlocal
set "NAME=%~1"
set "ASKED="
if "%NAME%"=="" (
    set ASKED=1
    set /p "NAME=Name of the new plugin (letters, digits and _ only, for example MyTool): "
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\NewPlugin.ps1" -Name "%NAME%"
set RESULT=%ERRORLEVEL%
if defined ASKED pause
exit /b %RESULT%
