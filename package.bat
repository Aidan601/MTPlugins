@echo off
rem Builds the tester package: dist\RadiantPlugins-<yyyymmdd-hhmm>.zip and nothing else (dist\ is emptied first).
rem Contents: launcher, host, loader, ini, README, .bat files, SDK\, plugins\<Name>\ and the SETUP note.
setlocal
cd /d "%~dp0"
call build.bat || exit /b 1
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmm"') do set STAMP=%%i
set NAME=RadiantPlugins-%STAMP%
set OUT=dist\%NAME%
rem only the newest package is kept: empty dist\ (files and folders), after the build succeeded
if not exist dist mkdir dist
del /f /q dist\* 2>nul
for /d %%d in (dist\*) do rmdir /s /q "%%d"
mkdir "%OUT%\plugins\TeamCreate" "%OUT%\plugins\RadiantPlus" "%OUT%\plugins\APEPlus" "%OUT%\plugins\LauncherPlus"
copy /y build\Release\RadiantPlugins_Launch.exe "%OUT%\" >nul
copy /y build\Release\RadiantPlugins.dll "%OUT%\" >nul
copy /y installer\images\MTPlugins-logo.png "%OUT%\" >nul
copy /y dist_template\RadiantPlugins.ini "%OUT%\" >nul
copy /y dist_template\README.txt "%OUT%\" >nul
rem the public release carries the GPL-3.0 text
if exist LICENSE copy /y LICENSE "%OUT%\LICENSE.txt" >nul
copy /y dist_template\SETUP-past-antivirus.txt "%OUT%\" >nul
copy /y "dist_template\Launch Radiant with Plugins.bat" "%OUT%\" >nul
copy /y "dist_template\Launch APE with Plugins.bat" "%OUT%\" >nul
copy /y "dist_template\Install plugins for the normal launcher.bat" "%OUT%\" >nul
copy /y build\Release\d3d9.dll "%OUT%\" >nul
copy /y loader\RadiantLoader.ini "%OUT%\" >nul
copy /y build\Release\plugins\TeamCreate\TeamCreate.dll "%OUT%\plugins\TeamCreate\" >nul
copy /y plugins\TeamCreate\TeamCreate.ini "%OUT%\plugins\TeamCreate\" >nul
copy /y plugins\TeamCreate\README.md "%OUT%\plugins\TeamCreate\" >nul
copy /y build\Release\plugins\RadiantPlus\RadiantPlus.dll "%OUT%\plugins\RadiantPlus\" >nul
copy /y plugins\RadiantPlus\RadiantPlus.ini "%OUT%\plugins\RadiantPlus\" >nul
copy /y plugins\RadiantPlus\README.md "%OUT%\plugins\RadiantPlus\" >nul
copy /y build\Release\plugins\APEPlus\APEPlus.dll "%OUT%\plugins\APEPlus\" >nul
copy /y plugins\APEPlus\APEPlus.ini "%OUT%\plugins\APEPlus\" >nul
copy /y plugins\APEPlus\README.md "%OUT%\plugins\APEPlus\" >nul
xcopy /y /i /e plugins\APEPlus\themes "%OUT%\plugins\APEPlus\themes\" >nul
xcopy /y /i plugins\APEPlus\deffiles "%OUT%\plugins\APEPlus\deffiles\" >nul
xcopy /y /i /e plugins\RadiantPlus\themes "%OUT%\plugins\RadiantPlus\themes\" >nul
copy /y build\Release\plugins\LauncherPlus\LauncherPlus.dll "%OUT%\plugins\LauncherPlus\" >nul
copy /y build\Release\plugins\LauncherPlus\BuildPlusBridge.dll "%OUT%\plugins\LauncherPlus\" >nul
copy /y build\Release\plugins\LauncherPlus\BuildPlusWorker.exe "%OUT%\plugins\LauncherPlus\" >nul
copy /y build\Release\plugins\LauncherPlus\BuildPlusCompiler.dll "%OUT%\plugins\LauncherPlus\" >nul
copy /y build\Release\plugins\LauncherPlus\BuildPlusLinker.exe "%OUT%\plugins\LauncherPlus\" >nul
copy /y build\Release\plugins\LauncherPlus\BuildPlusLinkerRT.dll "%OUT%\plugins\LauncherPlus\" >nul
copy /y build\Release\plugins\LauncherPlus\BuildPlusLight.exe "%OUT%\plugins\LauncherPlus\" >nul
copy /y build\Release\plugins\LauncherPlus\BuildPlusLightRT.dll "%OUT%\plugins\LauncherPlus\" >nul
copy /y plugins\LauncherPlus\LauncherPlus.ini "%OUT%\plugins\LauncherPlus\" >nul
copy /y plugins\LauncherPlus\README.md "%OUT%\plugins\LauncherPlus\" >nul
copy /y plugins\LauncherPlus\defender_exclusions.ps1 "%OUT%\plugins\LauncherPlus\" >nul
rem Code signing: set RP_SIGN_THUMBPRINT (certificate in the user's store) or RP_SIGN_PFX + RP_SIGN_PFX_PASSWORD
rem and every exe and dll is signed (SHA-256, timestamped). Unsigned otherwise, this step then only prints a note.
if defined RP_SIGN_THUMBPRINT goto :sign
if defined RP_SIGN_PFX goto :sign
echo NOTE: unsigned package (set RP_SIGN_THUMBPRINT or RP_SIGN_PFX to sign the binaries)
goto :signed
:sign
set SIGNTOOL=
for /f "delims=" %%f in ('dir /b /s /o-n "C:\Program Files (x86)\Windows Kits\10\bin\10.*\x64\signtool.exe" 2^>nul') do if not defined SIGNTOOL set SIGNTOOL=%%f
if not defined SIGNTOOL (echo PACKAGE FAILED: signtool.exe not found in the Windows SDK & exit /b 1)
set SIGNCERT=/sha1 %RP_SIGN_THUMBPRINT%
if defined RP_SIGN_PFX set SIGNCERT=/f "%RP_SIGN_PFX%" /p "%RP_SIGN_PFX_PASSWORD%"
for /r "%OUT%" %%f in (*.exe *.dll) do (
    "%SIGNTOOL%" sign %SIGNCERT% /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 "%%f" >nul || (echo PACKAGE FAILED: signing %%f & exit /b 1)
)
echo signed every .exe and .dll in the package
:signed
rem the plugin SDK, without build output or user projects
robocopy sdk "%OUT%\SDK" /E /XD bin obj out .vs MyPlugins /XF *.user *.aps >nul
powershell -NoProfile -Command "Compress-Archive -Path '%OUT%\*' -DestinationPath 'dist\%NAME%.zip' -Force"
if not exist "dist\%NAME%.zip" (echo PACKAGE FAILED: zip not written & exit /b 1)
powershell -NoProfile -Command "(Get-ChildItem -Recurse '%OUT%' -File).FullName.Substring((Resolve-Path '%OUT%').Path.Length + 1)"
rmdir /s /q "%OUT%"
echo PACKAGE OK: dist\%NAME%.zip
