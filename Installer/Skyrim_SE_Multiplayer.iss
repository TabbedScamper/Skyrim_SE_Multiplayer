; SPDX-License-Identifier: GPL-3.0-only
; Skyrim SE Multiplayer is based on Skyrim Together Reborn by Tilted Phoques.
#ifndef ReleaseVersion
  #error ReleaseVersion is required
#endif
#ifndef StageDir
  #error StageDir is required
#endif
#ifndef RedistPath
  #error RedistPath is required
#endif

[Setup]
AppId={{B10F64A8-8E0A-41D9-A587-B8D845F33736}
AppName=Skyrim SE Multiplayer
AppVersion={#ReleaseVersion}
AppPublisher=Skyrim SE Multiplayer contributors and Tilted Phoques
DefaultDirName={code:GameDirectory}
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
OutputBaseFilename=Skyrim_SE_Multiplayer-{#ReleaseVersion}-setup
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
UninstallDisplayIcon={app}\Data\SkyrimTogetherReborn\Skyrim_SE_Multiplayer.exe
Compression=lzma2
SolidCompression=yes
CloseApplications=yes

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}\Data"; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#RedistPath}"; DestDir: "{tmp}"; DestName: "vc_redist.x64.exe"; Flags: deleteafterinstall

[Icons]
Name: "{autoprograms}\Skyrim SE Multiplayer"; Filename: "{app}\Data\SkyrimTogetherReborn\Skyrim_SE_Multiplayer.exe"
Name: "{autodesktop}\Skyrim SE Multiplayer"; Filename: "{app}\Data\SkyrimTogetherReborn\Skyrim_SE_Multiplayer.exe"; Tasks: desktopicon

[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Microsoft Visual C++ runtime"; Flags: waituntilterminated; Check: NeedsRedist

[Code]
function TrimQuotes(const Text: String): String;
begin
  Result := Trim(Text);
  if (Length(Result) >= 2) and (Result[1] = '"') and (Result[Length(Result)] = '"') then
    Result := Copy(Result, 2, Length(Result) - 2);
end;

function LibraryHasGame(const LibraryPath: String): Boolean;
begin
  Result := FileExists(AddBackslash(LibraryPath) +
    'steamapps\appmanifest_489830.acf') and
    FileExists(AddBackslash(LibraryPath) +
    'steamapps\common\Skyrim Special Edition\SkyrimSE.exe');
end;

function FindSteamGame(): String;
var
  SteamPath, Vdf, Line, Candidate: String;
  Lines: TArrayOfString;
  I, J: Integer;
  HasApp: Boolean;
begin
  Result := '';
  if not RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', SteamPath) then
    Exit;
  StringChangeEx(SteamPath, '/', '\', True);
  if LibraryHasGame(SteamPath) then
  begin
    Result := AddBackslash(SteamPath) + 'steamapps\common\Skyrim Special Edition';
    Exit;
  end;
  Vdf := AddBackslash(SteamPath) + 'steamapps\libraryfolders.vdf';
  if not LoadStringsFromFile(Vdf, Lines) then Exit;
  Candidate := '';
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    Line := Trim(Lines[I]);
    if Pos('"path"', Line) > 0 then
    begin
      Candidate := Trim(Copy(Line, Pos('"path"', Line) + 6, Length(Line)));
      Candidate := TrimQuotes(Candidate);
      StringChangeEx(Candidate, '\\', '\', True);
      HasApp := False;
      for J := I + 1 to GetArrayLength(Lines) - 1 do
      begin
        if Pos('"path"', Lines[J]) > 0 then Break;
        if Pos('"489830"', Lines[J]) > 0 then HasApp := True;
      end;
      if HasApp and LibraryHasGame(Candidate) then
      begin
        Result := AddBackslash(Candidate) + 'steamapps\common\Skyrim Special Edition';
        Exit;
      end;
    end;
  end;
end;

function GameDirectory(Param: String): String;
begin
  Result := FindSteamGame();
  if Result = '' then
    Result := ExpandConstant('{autopf}\Steam\steamapps\common\Skyrim Special Edition');
end;

function NeedsRedist(): Boolean;
var
  Installed: Cardinal;
begin
  Result := not RegQueryDWordValue(HKLM64,
    'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64',
    'Installed', Installed) or (Installed <> 1);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Version, Probe: String;
begin
  Result := True;
  if CurPageID <> wpSelectDir then Exit;
  if not FileExists(AddBackslash(WizardDirValue) + 'SkyrimSE.exe') then
  begin
    MsgBox('Choose the Skyrim Special Edition folder containing SkyrimSE.exe.',
      mbError, MB_OK);
    Result := False;
    Exit;
  end;
  if (not GetVersionNumbersString(AddBackslash(WizardDirValue) + 'SkyrimSE.exe', Version)) or
     (Version <> '1.7.104.0') then
    MsgBox('This build targets SkyrimSE.exe 1.7.104.0. Your game version is ' +
      Version + '. You may continue, but the mod may not work.', mbInformation, MB_OK);
  Probe := AddBackslash(WizardDirValue) + 'Data\ssm-install-write-check.tmp';
  if not SaveStringToFile(Probe, '', False) then
  begin
    MsgBox('This Skyrim Data folder is not writable by your account. Choose a writable Steam library.',
      mbError, MB_OK);
    Result := False;
  end
  else DeleteFile(Probe);
end;

procedure UpdatePlugins(RemoveEntries: Boolean);
var
  Path, Backup: String;
  Lines, NewLines: TArrayOfString;
  I, Count: Integer;
begin
  Path := ExpandConstant('{localappdata}\Skyrim Special Edition\plugins.txt');
  Backup := Path + '.ssm-backup';
  if not FileExists(Path) then Exit;
  if not RemoveEntries and not FileExists(Backup) then
    FileCopy(Path, Backup, False);
  if not LoadStringsFromFile(Path, Lines) then Exit;
  SetArrayLength(NewLines, GetArrayLength(Lines) + 2);
  Count := 0;
  for I := 0 to GetArrayLength(Lines) - 1 do
    if (CompareText(Trim(Lines[I]), '*SkyrimSEMultiplayer.esp') <> 0) and
       (CompareText(Trim(Lines[I]), '*SkyrimSEMultiplayerQuestPatches.esp') <> 0) then
    begin
      NewLines[Count] := Lines[I];
      Count := Count + 1;
    end;
  if not RemoveEntries then
  begin
    NewLines[Count] := '*SkyrimSEMultiplayer.esp';
    NewLines[Count + 1] := '*SkyrimSEMultiplayerQuestPatches.esp';
    Count := Count + 2;
  end;
  SetArrayLength(NewLines, Count);
  SaveStringsToFile(Path, NewLines, False);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then UpdatePlugins(False);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then UpdatePlugins(True);
end;
