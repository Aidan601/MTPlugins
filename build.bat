@echo off
rem Build the host (RadiantPlugins.dll), the launcher and every plugin.   Usage: build.bat [Release|Debug]
rem Output: build\<Config>\RadiantPlugins.dll, RadiantPlugins_Launch.exe, plugins\<Name>\<Name>.dll (+ .ini)
setlocal
set CFG=%1
if "%CFG%"=="" set CFG=Release
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
msbuild RadiantPlugins.sln /p:Configuration=%CFG% /p:Platform=x64 /m /v:m /nologo
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
if not exist "build\%CFG%\RadiantPlugins.ini" copy /y RadiantPlugins.ini "build\%CFG%\RadiantPlugins.ini" >nul
copy /y installer\images\MTPlugins-logo.png "build\%CFG%\MTPlugins-logo.png" >nul
rem Keep the PDBs of each Release build in build\symbols\<stamp>\ (newest 30) so old crash dumps can be read
if not "%CFG%"=="Release" goto :done
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set SYMDIR=build\symbols\%%i
mkdir "%SYMDIR%" 2>nul
copy /y "build\%CFG%\RadiantPlugins.pdb" "%SYMDIR%\" >nul
copy /y "build\%CFG%\d3d9.pdb" "%SYMDIR%\" >nul
for /d %%p in (build\%CFG%\plugins\*) do copy /y "%%p\*.pdb" "%SYMDIR%\" >nul
for /f "skip=30" %%d in ('dir /b /ad /o-n build\symbols') do rmdir /s /q "build\symbols\%%d"
:done
echo BUILD OK: build\%CFG%\RadiantPlugins.dll  RadiantPlugins_Launch.exe  plugins\*
