; MLV Drive's installer (Inno Setup 6). Built by `.\build.ps1 setup`, which passes:
;   /DAppVersion=0.2.0  /DStage=<build\stage>  /DRoot=<the project>  /DDist=<dist>
;
; Default or custom installation: the tray app (GUI) and the command-line drive (CLI), at least
; one of them; mlvdrive_tool optional. Per user (no administrator rights) unless "for all users" is
; chosen. WinFsp, which the drive needs, is checked for; if it is missing (or older than 2.1) the
; user is offered to download the latest version from winfsp.dev, which is installed only if
; it is signed by WinFsp's publisher (NAVIMATICS LLC).
;
; Test hook: /TESTWINFSP finds, downloads and checks WinFsp as if it were missing, but doesn't
; install it (results in the setup log: /LOG=file).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Root
  #define Root ".."
#endif
#ifndef Stage
  #define Stage Root + "\build\stage"
#endif
#ifndef Dist
  #define Dist Root + "\dist"
#endif

[Setup]
AppId={{6F1C2B7E-3D49-4C8A-9B5E-0A7D2E4F8C31}
AppName=MLV Drive
AppVersion={#AppVersion}
AppVerName=MLV Drive {#AppVersion}
AppComments=Magic Lantern MLV clips as a drive of CinemaDNG sequences
VersionInfoVersion={#AppVersion}
VersionInfoDescription=MLV Drive setup
DefaultDirName={autopf}\MLV Drive
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile={#Stage}\LICENSE
SetupIconFile={#Root}\assets\mlvdrive.ico
UninstallDisplayIcon={app}\mlvdrive.ico
UninstallDisplayName=MLV Drive
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
OutputDir={#Dist}
OutputBaseFilename=MLVDrive-{#AppVersion}-setup
; Programs of an earlier version still running are asked to close (the tray app unmounts then).
CloseApplications=yes
RestartApplications=no
; PATH may change (the command-line programs).
ChangesEnvironment=yes

[Types]
Name: "default"; Description: "Default installation: the tray app and the command-line drive"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "gui"; Description: "MLV Drive tray app: mount, unmount and settings from the notification area"; Types: default custom
Name: "cli"; Description: "Command-line drive (mlvdrive.exe): mount, unmount and rescan from a console or scripts"; Types: default custom
Name: "tool"; Description: "mlvdrive_tool.exe: clip details, DNG and WAV export, checks and benchmarks (no drive)"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Components: gui; Flags: unchecked
Name: "autostart"; Description: "Start MLV Drive with Windows"; Components: gui; Flags: unchecked
Name: "addpath"; Description: "Add the command-line programs to PATH"; Components: cli tool

[Files]
Source: "{#Stage}\mlvdrive_tray.exe"; DestDir: "{app}"; Components: gui; Flags: ignoreversion
Source: "{#Stage}\mlvdrive.exe"; DestDir: "{app}"; Components: cli; Flags: ignoreversion
Source: "{#Stage}\mlvdrive_tool.exe"; DestDir: "{app}"; Components: tool; Flags: ignoreversion
Source: "{#Stage}\pixelmaps\*"; DestDir: "{app}\pixelmaps"; Flags: ignoreversion recursesubdirs
Source: "{#Stage}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\assets\mlvdrive.ico"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
; Programs since renamed, the script installer's files, and programs not chosen this time.
Type: files; Name: "{app}\mlvdrive_cli.exe"
Type: files; Name: "{app}\install.ps1"
Type: files; Name: "{app}\install.cmd"
Type: files; Name: "{app}\uninstall.cmd"
Type: files; Name: "{app}\mlvdrive_tray.exe"; Components: not gui
Type: files; Name: "{app}\mlvdrive.exe"; Components: not cli
Type: files; Name: "{app}\mlvdrive_tool.exe"; Components: not tool

[Icons]
Name: "{autoprograms}\MLV Drive"; Filename: "{app}\mlvdrive_tray.exe"; Comment: "MLV clips as a drive of CinemaDNG sequences"; Components: gui
Name: "{autodesktop}\MLV Drive"; Filename: "{app}\mlvdrive_tray.exe"; Comment: "MLV clips as a drive of CinemaDNG sequences"; Tasks: desktopicon

[Registry]
; The same value the tray app's "Start with Windows" sets.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "MLV Drive"; ValueData: """{app}\mlvdrive_tray.exe"""; Tasks: autostart

[Run]
Filename: "{app}\mlvdrive_tray.exe"; Description: "Start MLV Drive"; Flags: postinstall nowait skipifsilent; Components: gui

[Code]
const
  MinWinFsp = '2.1';
  WinFspPage = 'https://winfsp.dev/rel/';
  WinFspLink = 'https://github.com/winfsp/winfsp/releases/download/';
  TrayClass = 'MLVDriveTray';
  QuitCommand = 116;  { the tray menu's Quit: unmounts and exits }
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';

var
  WinFspChoice: TInputOptionWizardPage;
  Progress: TOutputProgressWizardPage;

{ --- WinFsp --- }

{ The installed WinFsp's version ('2.1.25156'), or '' if there is none. }
function WinFspVersion(): String;
var
  Dir, Version: String;
begin
  Result := '';
  if RegQueryStringValue(HKLM32, 'SOFTWARE\WinFsp', 'InstallDir', Dir) then
    if GetVersionNumbersString(AddBackslash(Dir) + 'bin\winfsp-x64.dll', Version) then
      Result := Version;
end;

{ Compares dotted versions: <0, 0, >0. }
function CompareVersions(A, B: String): Integer;
var
  X, Y, P: Integer;
begin
  Result := 0;
  while (Result = 0) and ((A <> '') or (B <> '')) do begin
    P := Pos('.', A); if P = 0 then P := Length(A) + 1;
    X := StrToIntDef(Copy(A, 1, P - 1), 0); Delete(A, 1, P);
    P := Pos('.', B); if P = 0 then P := Length(B) + 1;
    Y := StrToIntDef(Copy(B, 1, P - 1), 0); Delete(B, 1, P);
    if X < Y then Result := -1 else if X > Y then Result := 1;
  end;
end;

function TestingWinFsp(): Boolean;
begin
  Result := ExpandConstant('{param:TESTWINFSP|*}') <> '*';
  if not Result then Result := Pos('/TESTWINFSP', Uppercase(GetCmdTail)) > 0;
end;

function WinFspNeeded(): Boolean;
var
  V: String;
begin
  V := WinFspVersion();
  Result := TestingWinFsp() or (V = '') or (CompareVersions(V, MinWinFsp) < 0);
end;

function OnDownloadProgress(const Url, FileName: String; const Done, Total: Int64): Boolean;
begin
  if (Total > 0) and not WizardSilent then
    Progress.SetProgress(Integer(Done div 1024), Integer(Total div 1024));
  Result := True;
end;

{ winfsp.dev's download page links the latest installer: the first link to
  github.com/winfsp/winfsp/releases/download/.../winfsp-<version>.msi. '' if there is none. }
function LatestWinFspUrl(): String;
var
  Page: AnsiString;
  S: String;
  P, E: Integer;
begin
  Result := '';
  DownloadTemporaryFile(WinFspPage, 'winfsp-rel.html', '', nil);
  if not LoadStringFromFile(ExpandConstant('{tmp}\winfsp-rel.html'), Page) then Exit;
  S := String(Page);
  P := Pos(WinFspLink, S);
  if P = 0 then Exit;
  S := Copy(S, P, 300);
  E := Pos('.msi', S);
  if E = 0 then Exit;
  S := Copy(S, 1, E + 3);
  { Nothing but the link: no quote or space inside, and a WinFsp installer's name. }
  if (Pos('"', S) > 0) or (Pos('''', S) > 0) or (Pos(' ', S) > 0) or (Pos('<', S) > 0) then Exit;
  if Pos('/winfsp-', S) = 0 then Exit;
  Result := S;
end;

{ Whether a file carries a valid signature of WinFsp's publisher. }
function SignedByWinFsp(const FileName: String): Boolean;
var
  Code: Integer;
  Diag: AnsiString;
begin
  Result := Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    { Its own module explicitly: started from PowerShell 7, setup passes on a module path that
      makes Windows PowerShell fail to load it. }
    '-NoProfile -ExecutionPolicy Bypass -Command "Import-Module (Join-Path $PSHOME ''Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1''); ' +
    '$s = Get-AuthenticodeSignature -LiteralPath ''' + FileName + '''; ' +
    'Set-Content -LiteralPath ''' + FileName + '.sig.txt'' -Value ([string]$s.Status + '' | '' + $s.SignerCertificate.Subject + '' | '' + [string]$Error[0]); ' +
    'if ($s.Status -eq ''Valid'' -and $s.SignerCertificate.Subject -like ''CN=NAVIMATICS LLC,*'') { exit 0 } else { exit 1 }"',
    '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
  Log('WinFsp: signature check exit ' + IntToStr(Code));
  if LoadStringFromFile(FileName + '.sig.txt', Diag) then Log('WinFsp: signature: ' + String(Diag));
end;

{ Downloads, checks and (unless testing) installs the latest WinFsp. Never fails the setup:
  MLV Drive installs anyway, and the tray app keeps pointing to WinFsp. }
procedure InstallWinFsp();
var
  Url, Msi, Why: String;
  Code: Integer;
begin
  Why := '';
  if not WizardSilent then begin
    Progress.SetText('Getting WinFsp from winfsp.dev...', WinFspPage);
    Progress.SetProgress(0, 0);
    Progress.Show;
  end;
  try
    try
      Url := LatestWinFspUrl();
      Log('WinFsp: the download page links ' + Url);
      if Url = '' then
        Why := 'The WinFsp download link wasn''t found on winfsp.dev.'
      else begin
        if not WizardSilent then Progress.SetText('Downloading WinFsp...', Url);
        DownloadTemporaryFile(Url, 'winfsp.msi', '', @OnDownloadProgress);
        Msi := ExpandConstant('{tmp}\winfsp.msi');
        if not SignedByWinFsp(Msi) then
          Why := 'The downloaded WinFsp installer isn''t signed by WinFsp''s publisher, so it wasn''t run.'
        else if TestingWinFsp() then
          Log('WinFsp: downloaded and signed by NAVIMATICS LLC (test: not installed)')
        else begin
          if not WizardSilent then Progress.SetText('Installing WinFsp...', 'Windows asks for administrator rights.');
          if IsAdminInstallMode then
            Exec('msiexec.exe', '/i "' + Msi + '" /passive /norestart', '', SW_SHOW, ewWaitUntilTerminated, Code)
          else if not ShellExec('runas', 'msiexec.exe', '/i "' + Msi + '" /passive /norestart', '', SW_SHOW,
                                ewWaitUntilTerminated, Code) then
            Code := -1;
          Log('WinFsp: msiexec ended with ' + IntToStr(Code));
          if (WinFspVersion() = '') or (CompareVersions(WinFspVersion(), MinWinFsp) < 0) then
            Why := 'WinFsp wasn''t installed (cancelled, or the installer failed).';
        end;
      end;
    except
      Why := 'WinFsp couldn''t be downloaded: ' + GetExceptionMessage;
    end;
  finally
    if not WizardSilent then Progress.Hide;
  end;
  if Why <> '' then begin
    Log('WinFsp: ' + Why);
    if not WizardSilent then
      if MsgBox(Why + #13#10#13#10 + 'MLV Drive is installed anyway; it needs WinFsp to show the drive. ' +
                'Open winfsp.dev to download it yourself?', mbError, MB_YESNO) = IDYES then
        ShellExec('open', WinFspPage, '', '', SW_SHOWNORMAL, ewNoWait, Code);
  end;
end;

{ --- PATH --- }

function EnvRoot(): Integer;
begin
  if IsAdminInstallMode then Result := HKLM else Result := HKCU;
end;

function EnvKey(): String;
begin
  if IsAdminInstallMode then Result := 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
  else Result := 'Environment';
end;

procedure ChangePath(const Dir: String; Add: Boolean);
var
  Path: String;
  P: Integer;
begin
  if not RegQueryStringValue(EnvRoot(), EnvKey(), 'Path', Path) then Path := '';
  P := Pos(';' + Uppercase(Dir) + ';', ';' + Uppercase(Path) + ';');
  if Add and (P = 0) then begin
    if (Path <> '') and (Path[Length(Path)] <> ';') then Path := Path + ';';
    RegWriteExpandStringValue(EnvRoot(), EnvKey(), 'Path', Path + Dir);
  end else if (not Add) and (P > 0) then begin
    Delete(Path, P, Length(Dir) + 1);  { the entry and the ';' after it (or before, at the end) }
    if (Length(Path) > 0) and (Path[Length(Path)] = ';') then Delete(Path, Length(Path), 1);
    RegWriteExpandStringValue(EnvRoot(), EnvKey(), 'Path', Path);
  end;
end;

{ --- The wizard --- }

procedure InitializeWizard();
begin
  WinFspChoice := CreateInputOptionPage(wpSelectTasks, 'WinFsp',
    'MLV Drive needs WinFsp, a free and open-source driver, to show the drive.',
    'WinFsp isn''t installed on this computer (or is older than ' + MinWinFsp + '). Setup can download the ' +
    'latest version from its official site, winfsp.dev, and install it (Windows will ask for administrator ' +
    'rights). The installer is run only if it carries WinFsp''s publisher''s signature.',
    False, False);
  WinFspChoice.Add('Download and install the latest WinFsp now (recommended)');
  WinFspChoice.Values[0] := True;
  Progress := CreateOutputProgressPage('WinFsp', 'Getting WinFsp, which MLV Drive needs for the drive.');
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = WinFspChoice.ID) and not WinFspNeeded();
end;

function NoProgramChosen(): Boolean;
begin
  Result := not WizardIsComponentSelected('gui') and not WizardIsComponentSelected('cli');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectComponents) and NoProgramChosen() then begin
    MsgBox('Choose the tray app, the command-line drive, or both: one of them shows the drive.', mbError, MB_OK);
    Result := False;
  end;
  { Before the files: WinFsp, if the user asked for it. }
  if (CurPageID = wpReady) and WinFspNeeded() and WinFspChoice.Values[0] then
    InstallWinFsp();
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  { The components rule again, in case the page was passed over. }
  if NoProgramChosen() then
    Result := 'Choose the tray app (gui), the command-line drive (cli), or both.';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    if WizardIsTaskSelected('addpath') then ChangePath(ExpandConstant('{app}'), True);
end;

{ --- Uninstall --- }

{ Whether this installation's tray app is running (one runs at a time, but it may be another
  installation's: that one is left alone). }
function TrayRunningFromApp(): Boolean;
var
  Locator, Service, Items: Variant;
  Path: String;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Path := ExpandConstant('{app}\mlvdrive_tray.exe');
    StringChangeEx(Path, '\', '\\', True);
    Items := Service.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE ExecutablePath = ''' + Path + '''');
    Result := Items.Count > 0;
  except
    Log('Couldn''t list the running programs: ' + GetExceptionMessage);
  end;
end;

function InitializeUninstall(): Boolean;
var
  Wnd: HWND;
  I: Integer;
begin
  { The tray app unmounts and exits (its menu's Quit). }
  Wnd := 0;
  if TrayRunningFromApp() then Wnd := FindWindowByClassName(TrayClass);
  if Wnd <> 0 then begin
    PostMessage(Wnd, $0111, QuitCommand, 0);
    for I := 1 to 50 do begin
      if FindWindowByClassName(TrayClass) = 0 then Break;
      Sleep(200);
    end;
  end;
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Command: String;
begin
  if CurUninstallStep = usUninstall then begin
    ChangePath(ExpandConstant('{app}'), False);
    { "Start with Windows", if it starts this installation's tray app. }
    if RegQueryStringValue(HKCU, RunKey, 'MLV Drive', Command) then
      if Pos(Uppercase(AddBackslash(ExpandConstant('{app}'))), Uppercase(Command)) > 0 then
        RegDeleteValue(HKCU, RunKey, 'MLV Drive');
  end;
  if CurUninstallStep = usPostUninstall then
    if not UninstallSilent then
      if MsgBox('Delete MLV Drive''s settings too (' + ExpandConstant('{userappdata}\MLV Drive') + ')?' + #13#10 +
                'Keep them if you will install MLV Drive again. (WinFsp stays installed: other programs may use it.)',
                mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(ExpandConstant('{userappdata}\MLV Drive'), True, True, True);
end;
