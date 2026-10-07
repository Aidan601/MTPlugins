@echo off
rem Builds build\installer\MTPlugins-Setup-<version>.exe. Usage: installer\build_installer.bat [version] (= MTPLUGINS_VERSION in src\version.h)
rem Runs build.bat (Release), fetches DXVK when missing, compiles installer\MTPlugins.iss.
rem Signing: set RP_SIGN_THUMBPRINT or RP_SIGN_PFX + RP_SIGN_PFX_PASSWORD, unsigned otherwise.
setlocal
cd /d "%~dp0.."
set VER=%1
rem The host compares GitHub releases with its own MTPLUGINS_VERSION, so the two must agree
set HOSTVER=
for /f "tokens=3" %%v in ('findstr /c:"#define MTPLUGINS_VERSION" src\version.h') do set HOSTVER=%%~v
if "%VER%"=="" set VER=%HOSTVER%
if not "%VER%"=="%HOSTVER%" (echo INSTALLER FAILED: version %VER% but src\version.h says %HOSTVER%; bump MTPLUGINS_VERSION there first & exit /b 1)
call build.bat Release || exit /b 1
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
msbuild plugins\RadiantPlus\RadiantPlus.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir=%CD%\ /v:m /nologo || (echo INSTALLER FAILED: RadiantPlus build & exit /b 1)
rem DXVK is not in git: fetch the pinned release (hash checked) when missing
if not exist plugins\RadiantPlus\dxvk\d3d11.dll powershell -NoProfile -ExecutionPolicy Bypass -File plugins\RadiantPlus\dxvk\fetch_dxvk.ps1 || (echo INSTALLER FAILED: DXVK fetch & exit /b 1)
if not exist plugins\RadiantPlus\dxvk\dxgi.dll powershell -NoProfile -ExecutionPolicy Bypass -File plugins\RadiantPlus\dxvk\fetch_dxvk.ps1 || (echo INSTALLER FAILED: DXVK fetch & exit /b 1)

set ISCC=
for %%p in ("%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" "%ProgramFiles%\Inno Setup 6\ISCC.exe") do if not defined ISCC if exist %%p set ISCC=%%~p
if not defined ISCC (echo INSTALLER FAILED: Inno Setup 6 not found ^(https://jrsoftware.org/isdl.php^) & exit /b 1)

set SIGNARGS=
if defined RP_SIGN_THUMBPRINT set SIGNARGS=/sha1 %RP_SIGN_THUMBPRINT%
if defined RP_SIGN_PFX set SIGNARGS=/f "%RP_SIGN_PFX%" /p "%RP_SIGN_PFX_PASSWORD%"
if not defined SIGNARGS goto :compile
set SIGNTOOL=
for /f "delims=" %%f in ('dir /b /s /o-n "C:\Program Files (x86)\Windows Kits\10\bin\10.*\x64\signtool.exe" 2^>nul') do if not defined SIGNTOOL set SIGNTOOL=%%f
if not defined SIGNTOOL (echo INSTALLER FAILED: signtool.exe not found in the Windows SDK & exit /b 1)
set SIGNCMD="%SIGNTOOL%" sign %SIGNARGS% /fd SHA256 /tr http://timestamp.digicert.com /td SHA256
for %%f in (build\Release\d3d9.dll build\Release\RadiantPlugins.dll) do %SIGNCMD% "%%f" >nul || (echo INSTALLER FAILED: signing %%f & exit /b 1)
for %%d in (TeamCreate RadiantPlus APEPlus LauncherPlus PlayTest) do (
    for %%f in (build\Release\plugins\%%d\*.dll build\Release\plugins\%%d\*.exe) do %SIGNCMD% "%%f" >nul || (echo INSTALLER FAILED: signing %%f & exit /b 1)
)
"%ISCC%" /Q /DAppVersion=%VER% /DSign "/Smtsign=%SIGNCMD% $f" installer\MTPlugins.iss || (echo INSTALLER FAILED & exit /b 1)
goto :done

:compile
echo NOTE: unsigned installer (set RP_SIGN_THUMBPRINT or RP_SIGN_PFX to sign)
"%ISCC%" /Q /DAppVersion=%VER% installer\MTPlugins.iss || (echo INSTALLER FAILED & exit /b 1)
:done
echo INSTALLER OK: build\installer\MTPlugins-Setup-%VER%.exe
