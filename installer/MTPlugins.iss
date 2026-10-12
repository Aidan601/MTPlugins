; MTPlugins installer (Inno Setup 6), built by installer\build_installer.bat from build\Release.
; Installs d3d9.dll + RadiantLoader.ini and plugins\ (host, one folder per plugin, uninstall\) into <game>\bin.
; Settings files (*.ini) are installed only when missing, so an update keeps the user's settings.

#ifndef AppVersion
  #define AppVersion "1.1.0"
#endif
#define Bin "..\build\Release"
#if FileExists(AddBackslash(SourcePath) + "..\LICENSE")
  #define LicenseText "..\LICENSE"
#else
  #define LicenseText "..\tools\publish\LICENSE.public"
#endif

[Setup]
AppId={{6C2D8F4E-3B1A-4E7C-9A52-8D0F1B7E6A31}
AppName=MTPlugins
AppVersion={#AppVersion}
AppVerName=MTPlugins {#AppVersion}
AppPublisher=Figglebottom
AppPublisherURL=https://github.com/Aidan601/MTPlugins
AppSupportURL=https://github.com/Aidan601/MTPlugins/issues
AppUpdatesURL=https://github.com/Aidan601/MTPlugins/releases
AppCopyright=Copyright (C) 2026 Figglebottom
VersionInfoVersion={#AppVersion}
VersionInfoCompany=Figglebottom
VersionInfoDescription=MTPlugins setup
VersionInfoProductName=MTPlugins
VersionInfoProductVersion={#AppVersion}
LicenseFile={#LicenseText}
DefaultDirName={code:DefaultGameDir}
AppendDefaultDirName=no
DirExistsWarning=no
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
DisableWelcomePage=no
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallFilesDir={app}\bin\plugins\uninstall
UninstallDisplayName=MTPlugins (Black Ops III mod tools plugins)
UninstallDisplayIcon={uninstallexe}
CloseApplications=yes
RestartApplications=no
OutputDir=..\build\installer
OutputBaseFilename=MTPlugins-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Wizard images and icon from assets\logo*.png (tools/make_branding.py), one file per display scale
SetupIconFile=images\MTPlugins.ico
WizardImageFile=images\wizard-164x314.bmp,images\wizard-192x386.bmp,images\wizard-246x459.bmp,images\wizard-273x556.bmp,images\wizard-328x628.bmp,images\wizard-355x700.bmp,images\wizard-410x797.bmp
WizardSmallImageFile=images\wizard-small-55x55.bmp,images\wizard-small-64x68.bmp,images\wizard-small-83x80.bmp,images\wizard-small-92x97.bmp,images\wizard-small-110x106.bmp,images\wizard-small-119x123.bmp,images\wizard-small-138x140.bmp
SetupLogging=yes
#ifdef Sign
SignTool=mtsign $f
SignedUninstaller=yes
#endif

[Messages]
SelectDirDesc=Where is Call of Duty: Black Ops III installed?
SelectDirLabel3=Setup adds the plugins into your Black Ops III root folder. No files will be replaced.
WizardSelectComponents=Select Plugins
SelectComponentsDesc=Which plugins should be installed?
SelectComponentsLabel2=Select which plugins you would like to have installed. Click Next when you are ready to continue.
SelectDirBrowseLabel=Select the Black Ops III folder, then click Next.
FinishedLabel=MTPlugins is installed. Start Radiant or APE the way you always do (Mod Tools Launcher, Steam or a shortcut); press Ctrl+Shift+P in the editor for the Plugins window.

[Types]
Name: "recommended"; Description: "Recommended plugins"
Name: "full"; Description: "All plugins"
Name: "custom"; Description: "Choose plugins"; Flags: iscustom

[Components]
Name: "core"; Description: "Plugin host and loader (required)"; Types: recommended full custom; Flags: fixed
Name: "teamcreate"; Description: "Team Create: Allows for multiple users to edit a map in Radiant at the same time, and to play it together with Play Test"; Types: full
Name: "radiantplus"; Description: "Radiant+: QOL fixes and improvements to Radiant"; Types: recommended full
Name: "apeplus"; Description: "APE+: QOL fixes and improvements to APE"; Types: recommended full
Name: "buildplus"; Description: "Launcher+: QOL fixes and improvements to the Launcher"; Types: recommended full
Name: "playtest"; Description: "Play Test: Play test your maps within Radiant, see edits live while playing, and join a Team Create host's game"; Types: full

[Tasks]
; Windows Defender, both undone by the uninstaller, administrator installs only.
; Play Test: Defender removes BO3LiveBridge.dll otherwise. BuildPlus: the process exclusions of defender_exclusions.ps1.
Name: "defenderplaytest"; Description: "Keep Windows Defender from deleting Play Test's files (recommended)"; GroupDescription: "Windows Defender:"; Components: playtest; Check: IsAdmin
Name: "defenderbuild"; Description: "Faster map builds: Windows Defender skips scanning files the compiler and linker open"; GroupDescription: "Windows Defender:"; Components: buildplus; Check: IsAdmin

[Files]
; loader and host
Source: "{#Bin}\d3d9.dll"; DestDir: "{app}\bin"; Components: core; Flags: ignoreversion; Check: FullRun
Source: "..\loader\RadiantLoader.ini"; DestDir: "{app}\bin"; Components: core; Flags: onlyifdoesntexist; Check: FullRun
Source: "{#Bin}\RadiantPlugins.dll"; DestDir: "{app}\bin\plugins"; Components: core; Flags: ignoreversion; Check: FullRun
Source: "images\MTPlugins-logo.png"; DestDir: "{app}\bin\plugins"; Components: core; Flags: ignoreversion; Check: FullRun
Source: "..\dist_template\RadiantPlugins.ini"; DestDir: "{app}\bin\plugins"; Components: core; Flags: onlyifdoesntexist; Check: FullRun
Source: "{#LicenseText}"; DestDir: "{app}\bin\plugins"; DestName: "MTPlugins-LICENSE.txt"; Components: core; Flags: ignoreversion; Check: FullRun
; Browse plugins: a copy of this Setup and bundled.ini (not when the copy itself runs)
Source: "{srcexe}"; DestDir: "{app}\bin\plugins\uninstall"; DestName: "MTPlugins-Setup.exe"; Components: core; Flags: external ignoreversion; Check: KeepSetup
Source: "bundled.ini"; DestDir: "{app}\bin\plugins\uninstall"; Components: core; Flags: ignoreversion; Check: FullRun
; plugins
; shared themes (each plugin carries a copy it restores them from), add mode copies only the missing ones
Source: "..\themes\*"; DestDir: "{app}\bin\themes"; Components: radiantplus apeplus buildplus; Flags: ignoreversion recursesubdirs; Check: FullRun
Source: "..\themes\*"; DestDir: "{app}\bin\themes"; Components: radiantplus apeplus buildplus; Flags: onlyifdoesntexist recursesubdirs; Check: AddsThemes
Source: "{#Bin}\plugins\TeamCreate\TeamCreate.dll"; DestDir: "{app}\bin\plugins\TeamCreate"; Components: teamcreate; Flags: ignoreversion; Check: Wanted('TeamCreate')
Source: "..\plugins\TeamCreate\TeamCreate.ini"; DestDir: "{app}\bin\plugins\TeamCreate"; Components: teamcreate; Flags: onlyifdoesntexist; Check: Wanted('TeamCreate')
Source: "..\plugins\TeamCreate\README.md"; DestDir: "{app}\bin\plugins\TeamCreate"; Components: teamcreate; Flags: ignoreversion; Check: Wanted('TeamCreate')
Source: "{#Bin}\plugins\RadiantPlus\RadiantPlus.dll"; DestDir: "{app}\bin\plugins\RadiantPlus"; Components: radiantplus; Flags: ignoreversion; Check: Wanted('RadiantPlus')
Source: "..\plugins\RadiantPlus\RadiantPlus.ini"; DestDir: "{app}\bin\plugins\RadiantPlus"; Components: radiantplus; Flags: onlyifdoesntexist; Check: Wanted('RadiantPlus')
Source: "..\plugins\RadiantPlus\README.md"; DestDir: "{app}\bin\plugins\RadiantPlus"; Components: radiantplus; Flags: ignoreversion; Check: Wanted('RadiantPlus')
Source: "..\themes\*"; DestDir: "{app}\bin\plugins\RadiantPlus\themes"; Components: radiantplus; Flags: ignoreversion recursesubdirs; Check: Wanted('RadiantPlus')
; DXVK 3.1.1 (doitsujin/dxvk, zlib) for the renderer switch, off until the user turns it on (fetched by build_installer.bat)
; APE Plus reads Radiant Plus's copy, so it gets its own only without Radiant Plus
Source: "..\plugins\RadiantPlus\dxvk\dxgi.dll"; DestDir: "{app}\bin\plugins\RadiantPlus\dxvk"; Components: radiantplus; Flags: ignoreversion; Check: Wanted('RadiantPlus')
Source: "..\plugins\RadiantPlus\dxvk\d3d11.dll"; DestDir: "{app}\bin\plugins\RadiantPlus\dxvk"; Components: radiantplus; Flags: ignoreversion; Check: Wanted('RadiantPlus')
Source: "..\plugins\RadiantPlus\dxvk\LICENSE.txt"; DestDir: "{app}\bin\plugins\RadiantPlus\dxvk"; Components: radiantplus; Flags: ignoreversion; Check: Wanted('RadiantPlus')
Source: "..\plugins\RadiantPlus\dxvk\version.txt"; DestDir: "{app}\bin\plugins\RadiantPlus\dxvk"; Components: radiantplus; Flags: ignoreversion; Check: Wanted('RadiantPlus')
Source: "..\plugins\RadiantPlus\dxvk\dxgi.dll"; DestDir: "{app}\bin\plugins\APEPlus\dxvk"; Components: apeplus and not radiantplus; Flags: ignoreversion; Check: Wanted('APEPlus')
Source: "..\plugins\RadiantPlus\dxvk\d3d11.dll"; DestDir: "{app}\bin\plugins\APEPlus\dxvk"; Components: apeplus and not radiantplus; Flags: ignoreversion; Check: Wanted('APEPlus')
Source: "..\plugins\RadiantPlus\dxvk\LICENSE.txt"; DestDir: "{app}\bin\plugins\APEPlus\dxvk"; Components: apeplus and not radiantplus; Flags: ignoreversion; Check: Wanted('APEPlus')
Source: "..\plugins\RadiantPlus\dxvk\version.txt"; DestDir: "{app}\bin\plugins\APEPlus\dxvk"; Components: apeplus and not radiantplus; Flags: ignoreversion; Check: Wanted('APEPlus')
Source: "{#Bin}\plugins\APEPlus\APEPlus.dll"; DestDir: "{app}\bin\plugins\APEPlus"; Components: apeplus; Flags: ignoreversion; Check: Wanted('APEPlus')
Source: "..\plugins\APEPlus\APEPlus.ini"; DestDir: "{app}\bin\plugins\APEPlus"; Components: apeplus; Flags: onlyifdoesntexist; Check: Wanted('APEPlus')
Source: "..\plugins\APEPlus\README.md"; DestDir: "{app}\bin\plugins\APEPlus"; Components: apeplus; Flags: ignoreversion; Check: Wanted('APEPlus')
Source: "..\themes\*"; DestDir: "{app}\bin\plugins\APEPlus\themes"; Components: apeplus; Flags: ignoreversion recursesubdirs; Check: Wanted('APEPlus')
Source: "..\plugins\APEPlus\deffiles\*.awi"; DestDir: "{app}\bin\plugins\APEPlus\deffiles"; Components: apeplus; Flags: ignoreversion; Check: Wanted('APEPlus')
Source: "{#Bin}\plugins\LauncherPlus\*.dll"; DestDir: "{app}\bin\plugins\LauncherPlus"; Components: buildplus; Flags: ignoreversion; Check: Wanted('LauncherPlus')
Source: "{#Bin}\plugins\LauncherPlus\*.exe"; DestDir: "{app}\bin\plugins\LauncherPlus"; Components: buildplus; Flags: ignoreversion; Check: Wanted('LauncherPlus')
Source: "..\plugins\LauncherPlus\defender_exclusions.ps1"; DestDir: "{app}\bin\plugins\LauncherPlus"; Components: buildplus; Flags: ignoreversion; Check: Wanted('LauncherPlus')
Source: "..\plugins\LauncherPlus\LauncherPlus.ini"; DestDir: "{app}\bin\plugins\LauncherPlus"; Components: buildplus; Flags: onlyifdoesntexist; Check: Wanted('LauncherPlus')
Source: "..\plugins\LauncherPlus\README.md"; DestDir: "{app}\bin\plugins\LauncherPlus"; Components: buildplus; Flags: ignoreversion; Check: Wanted('LauncherPlus')
Source: "{#Bin}\plugins\PlayTest\PlayTest.dll"; DestDir: "{app}\bin\plugins\PlayTest"; Components: playtest; Flags: ignoreversion; Check: Wanted('PlayTest')
Source: "{#Bin}\plugins\PlayTest\BO3LiveBridge.dll"; DestDir: "{app}\bin\plugins\PlayTest"; Components: playtest; Flags: ignoreversion skipifsourcedoesntexist; Check: Wanted('PlayTest')
Source: "..\plugins\PlayTest\PlayTest.ini"; DestDir: "{app}\bin\plugins\PlayTest"; Components: playtest; Flags: onlyifdoesntexist; Check: Wanted('PlayTest')
Source: "..\plugins\PlayTest\README.md"; DestDir: "{app}\bin\plugins\PlayTest"; Components: playtest; Flags: ignoreversion; Check: Wanted('PlayTest')
Source: "..\plugins\PlayTest\defender_playtest.ps1"; DestDir: "{app}\bin\plugins\PlayTest"; Components: playtest; Flags: ignoreversion; Check: Wanted('PlayTest')

[INI]
; the loader loads the installed host
Filename: "{app}\bin\RadiantLoader.ini"; Section: "loader"; Key: "dll"; String: "{app}\bin\plugins\RadiantPlugins.dll"; Components: core; Check: FullRun
Filename: "{app}\bin\RadiantLoader.ini"; Section: "loader"; Key: "enabled"; String: "1"; Components: core; Check: FullRun
Filename: "{app}\bin\RadiantLoader.ini"; Section: "loader"; Key: "process"; String: "Radiant_modtools.exe,asseteditor_modtools.exe,modlauncher.exe"; Components: core; Check: FullRun

[InstallDelete]
; never in add mode (the editors are open), a plugin unticked on an update stops loading, its settings stay
Type: files; Name: "{app}\bin\plugins\TeamCreate\TeamCreate.dll"; Components: not teamcreate; Check: FullRun
Type: files; Name: "{app}\bin\plugins\RadiantPlus\RadiantPlus.dll"; Components: not radiantplus; Check: FullRun
Type: files; Name: "{app}\bin\plugins\APEPlus\APEPlus.dll"; Components: not apeplus; Check: FullRun
Type: files; Name: "{app}\bin\plugins\LauncherPlus\LauncherPlus.dll"; Components: not buildplus; Check: FullRun
Type: files; Name: "{app}\bin\plugins\PlayTest\PlayTest.dll"; Components: not playtest; Check: FullRun

; old per-plugin theme images (themes live in bin\themes)
Type: files; Name: "{app}\bin\plugins\RadiantPlus\themes\images\*.png"; Components: radiantplus; Check: FullRun
Type: files; Name: "{app}\bin\plugins\APEPlus\themes\images\*.png"; Components: apeplus; Check: FullRun
; old plugin names (Better*): the programs go, their settings stay for the host to carry over
Type: files; Name: "{app}\bin\plugins\BetterRadiant\BetterRadiant.dll"; Check: FullRun
Type: files; Name: "{app}\bin\plugins\BetterAPE\BetterAPE.dll"; Check: FullRun
Type: files; Name: "{app}\bin\plugins\BetterBuild\*.dll"; Check: FullRun
Type: files; Name: "{app}\bin\plugins\BetterBuild\*.exe"; Check: FullRun
; old BuildPlus: the same
Type: files; Name: "{app}\bin\plugins\BuildPlus\*.dll"; Check: FullRun
Type: files; Name: "{app}\bin\plugins\BuildPlus\*.exe"; Check: FullRun

[UninstallDelete]
Type: files; Name: "{app}\bin\RadiantLoader.ini"
Type: files; Name: "{app}\bin\RadiantLoader.log"
Type: files; Name: "{app}\bin\plugins\RadiantPlugins.log"
Type: files; Name: "{app}\bin\plugins\RadiantPlugins.ape.log"
Type: files; Name: "{app}\bin\plugins\RadiantPlugins.launcher.log"
Type: files; Name: "{app}\bin\plugins\plugins.ini"

[Code]
function IsGameDir(const Dir: String): Boolean;
begin
  Result := (Dir <> '') and FileExists(AddBackslash(Dir) + 'bin\Radiant_modtools.exe');
end;

{ The game folder is TA_TOOLS_PATH: this process, then the saved user and machine environment }
function ToolsPath(): String;
var
  S: String;
begin
  S := GetEnv('TA_TOOLS_PATH');
  if not IsGameDir(RemoveBackslashUnlessRoot(Trim(S))) then
    if not RegQueryStringValue(HKCU, 'Environment', 'TA_TOOLS_PATH', S) or not IsGameDir(RemoveBackslashUnlessRoot(Trim(S))) then
      if not RegQueryStringValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', 'TA_TOOLS_PATH', S) then
        S := '';
  Result := RemoveBackslashUnlessRoot(Trim(S));
end;

function DefaultGameDir(Param: String): String;
begin
  Result := ToolsPath();
  if Result = '' then
    Result := ExpandConstant('{commonpf32}\Steam\steamapps\common\Call of Duty Black Ops III');
end;

{ Why this folder cannot be used, or '' when it can (checked on the folder page and before installing) }
function GameDirProblem(const Dir: String): String;
var
  Bin: String;
begin
  Result := '';
  if not IsGameDir(Dir) then
  begin
    Result := 'This folder does not contain bin\Radiant_modtools.exe:' + #13#10 + Dir + #13#10#13#10 +
              'Pick the Call of Duty: Black Ops III folder with the mod tools installed. If the mod tools are not installed yet, install them first (Steam: Library > Tools > Call of Duty: Black Ops III - Mod Tools); their installer sets TA_TOOLS_PATH and Setup then finds the folder by itself.';
    exit;
  end;
  { never replace someone else's d3d9.dll (ReShade, DXVK) }
  Bin := AddBackslash(Dir) + 'bin\';
  if FileExists(Bin + 'd3d9.dll') and not FileExists(Bin + 'RadiantLoader.ini') then
    Result := 'There is already a d3d9.dll in' + #13#10 + Bin + #13#10 +
              'that does not belong to MTPlugins (ReShade, DXVK or another wrapper?). MTPlugins needs that file name for its loader.' + #13#10#13#10 +
              'Remove or rename that d3d9.dll, then run Setup again.';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Problem: String;
begin
  Result := True;
  { silent installs skip the page, PrepareToInstall refuses instead }
  if (CurPageID <> wpSelectDir) or WizardSilent then exit;
  Problem := GameDirProblem(WizardDirValue);
  if Problem <> '' then
  begin
    MsgBox(Problem, mbError, MB_OK);
    Result := False;
  end;
end;

{ Add mode: /ADDPLUGIN=Name[,Name] (plugin folder names), run by Browse plugins with the kept Setup.
  Only the named plugins' files are copied, the editors stay open and the loader and host are left alone. }
function AddParam(): String;
begin
  Result := Lowercase(Trim(ExpandConstant('{param:ADDPLUGIN|}')));
end;

function FullRun(): Boolean;
begin
  Result := AddParam() = '';
end;

function Adding(const Name: String): Boolean;
begin
  Result := Pos(',' + Lowercase(Name) + ',', ',' + AddParam() + ',') > 0;
end;

function Wanted(const Name: String): Boolean;
begin
  Result := FullRun or Adding(Name);
end;

function AddsThemes(): Boolean;
begin
  Result := Adding('RadiantPlus') or Adding('APEPlus') or Adding('LauncherPlus');
end;

function KeepSetup(): Boolean;
begin
  Result := FullRun and (CompareText(ExpandConstant('{srcexe}'), ExpandConstant('{app}\bin\plugins\uninstall\MTPlugins-Setup.exe')) <> 0);
end;

{ Why add mode cannot run, or '' }
function AddProblem(): String;
var
  Rest: String;
begin
  Result := '';
  if not FileExists(AddBackslash(WizardDirValue) + 'bin\plugins\RadiantPlugins.dll') then
  begin
    Result := '/ADDPLUGIN adds a plugin to an installed MTPlugins, and there is none in' + #13#10 + WizardDirValue;
    exit;
  end;
  Rest := ',' + AddParam() + ',';
  StringChangeEx(Rest, ',teamcreate,', ',', True);
  StringChangeEx(Rest, ',radiantplus,', ',', True);
  StringChangeEx(Rest, ',apeplus,', ',', True);
  StringChangeEx(Rest, ',launcherplus,', ',', True);
  StringChangeEx(Rest, ',playtest,', ',', True);
  if Rest <> ',' then
    Result := '/ADDPLUGIN names a plugin this Setup does not have: ' + Copy(Rest, 2, Length(Rest) - 2);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := GameDirProblem(WizardDirValue);
  if (Result = '') and not FullRun then Result := AddProblem();
end;

{ The DXVK switch copies dxgi.dll and d3d11.dll into bin: remove them only when they still match the shipped copies (SHA-256) }
procedure RemoveDxvkCopy(const Name: String);
var
  Bin, Shipped: String;
begin
  Bin := ExpandConstant('{app}\bin\');
  DeleteFile(Bin + Name + '.dxvk-off');
  Shipped := Bin + 'plugins\RadiantPlus\dxvk\' + Name;
  if not FileExists(Shipped) then Shipped := Bin + 'plugins\APEPlus\dxvk\' + Name;
  if FileExists(Bin + Name) and FileExists(Shipped) then
    try
      if GetSHA256OfFile(Bin + Name) = GetSHA256OfFile(Shipped) then DeleteFile(Bin + Name);
    except
    end;
end;

{ Windows Defender tasks. Returns the PowerShell exit code, -1 if it did not start. Failure is logged, never an install error }
function RunPowerShell(const Params: String): Integer;
var
  Code: Integer;
begin
  if Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
          '-NoProfile -NonInteractive -ExecutionPolicy Bypass ' + Params, '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Result := Code
  else
    Result := -1;
end;

{ Verb is Add or Remove: a folder exclusion for Dir }
function DefenderFolder(const Verb, Dir: String): Integer;
var
  Quoted: String;
begin
  Quoted := Dir;
  StringChangeEx(Quoted, '''', '''''', True);
  Result := RunPowerShell('-Command "try { ' + Verb + '-MpPreference -ExclusionPath ''' + Quoted +
                          ''' -ErrorAction Stop; exit 0 } catch { exit 1 }"');
end;

{ Mode add or remove: 0 done, 3 Defender is not the active antivirus, -2 no script }
function DefenderBuildTools(const Mode: String): Integer;
var
  Script: String;
begin
  Script := ExpandConstant('{app}\bin\plugins\LauncherPlus\defender_exclusions.ps1');
  if FileExists(Script) then
    Result := RunPowerShell('-File "' + Script + '" ' + Mode + ' "' + ExpandConstant('{app}\bin') + '"')
  else
    Result := -2;
end;

var
  PlayTestExcluded: Boolean;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
  Ini: String;
begin
  { before the files are copied. Add mode has no task page: adding Play Test acts like the ticked task }
  if (CurStep = ssInstall) and ((FullRun and WizardIsTaskSelected('defenderplaytest')) or (IsAdmin and Adding('PlayTest'))) then
  begin
    Code := DefenderFolder('Add', ExpandConstant('{app}\bin\plugins\PlayTest'));
    Log(Format('Defender folder exclusion for Play Test: %d', [Code]));
    PlayTestExcluded := Code = 0;
  end;
  { PlayTest.ini [defender] is read by Play Test's own offer and repair }
  if (CurStep = ssPostInstall) and PlayTestExcluded then
  begin
    Ini := ExpandConstant('{app}\bin\plugins\PlayTest\PlayTest.ini');
    SetIniString('defender', 'exclusion', '1', Ini);
    SetIniString('defender', 'asked', '1', Ini);
  end;
  { not in add mode (Launcher+ then makes its own offer) }
  if (CurStep = ssPostInstall) and FullRun and WizardIsComponentSelected('buildplus') and IsAdmin then
  begin
    { LauncherPlus.ini [defender] is read by the plugin's menu tick and offer }
    Ini := ExpandConstant('{app}\bin\plugins\LauncherPlus\LauncherPlus.ini');
    if WizardIsTaskSelected('defenderbuild') then
    begin
      Code := DefenderBuildTools('add');
      Log(Format('Defender process exclusions for BuildPlus: %d', [Code]));
      if Code = 0 then
      begin
        SetIniString('defender', 'exclusions', '1', Ini);
        SetIniString('defender', 'applied', GetDateTimeString('yyyy-mm-dd hh:nn', '-', ':'), Ini);
      end;
      { any other failure leaves the plugin's own offer }
      if (Code = 0) or (Code = 3) then SetIniString('defender', 'asked', '1', Ini);
    end
    else
      SetIniString('defender', 'asked', '1', Ini);   { declined here: the launcher does not ask again }
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    { before the script goes with the BuildPlus folder }
    if IsAdmin then
    begin
      DefenderBuildTools('remove');
      DefenderFolder('Remove', ExpandConstant('{app}\bin\plugins\PlayTest'));
    end;
    RemoveDxvkCopy('dxgi.dll');
    RemoveDxvkCopy('d3d11.dll');
  end;
end;
