#define AppVersion "0.13.6"
#define RepoRoot SourcePath + ".."
[Setup]
AppId={{DFA5DE34-4EA9-4ECC-B99A-8F79EE9357C8}
AppName=HiveDesk
AppVersion={#AppVersion}
AppPublisher=HiveDesk
AppComments=Personal LAN desktop switching
VersionInfoVersion=0.13.6.0
VersionInfoDescription=HiveDesk Setup
VersionInfoProductName=HiveDesk
SetupIconFile={#RepoRoot}\resources\HiveDesk.ico
DefaultDirName={autopf}\HiveDesk
DefaultGroupName=HiveDesk
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#RepoRoot}\dist
OutputBaseFilename=HiveDesk-{#AppVersion}-windows-x64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
UninstallDisplayIcon={app}\HiveDesk.exe

[Files]
Source: "{#RepoRoot}\build\ffmpeg\avcodec-63.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\build\ffmpeg\avutil-61.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\build\ffmpeg\COPYING.LGPLv2.1"; DestDir: "{app}\licenses\ffmpeg"; Flags: ignoreversion
Source: "{#RepoRoot}\build\ffmpeg\ffmpeg-n9.0.2.tar.gz"; DestDir: "{app}\licenses\ffmpeg"; Flags: ignoreversion
Source: "{#RepoRoot}\third_party\ffmpeg\SOURCE.md"; DestDir: "{app}\licenses\ffmpeg"; Flags: ignoreversion
Source: "{#RepoRoot}\scripts\build-codec.ps1"; DestDir: "{app}\licenses\ffmpeg"; Flags: ignoreversion
Source: "{#RepoRoot}\scripts\build-codec.sh"; DestDir: "{app}\licenses\ffmpeg"; Flags: ignoreversion
Source: "{#RepoRoot}\build\HiveDesk.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\build\HiveDeskService.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\build\HiveDeskUpdater.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\build\HiveDeskGpuPolicy.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\build\HiveDeskHost.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\build\HiveDeskCheck.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\build\HiveDeskPathCheck.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\build\HiveDeskRemoteVideoCheck.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\build\HiveDeskMediaProbe.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\build\HiveDeskRemoteMediaProbe.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\third_party\onevpl\LICENSE"; DestDir: "{app}\licenses"; DestName: "Intel-oneVPL-LICENSE.txt"; Flags: ignoreversion
Source: "{#RepoRoot}\third_party\amf\LICENSE.txt"; DestDir: "{app}\licenses"; DestName: "AMD-AMF-LICENSE.txt"; Flags: ignoreversion
Source: "{#RepoRoot}\build\settings\*"; DestDir: "{app}\settings"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RepoRoot}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\third_party\NOTICE.md"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "{#RepoRoot}\third_party\NVIDIA-HEADERS-LICENSES.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "{#RepoRoot}\third_party\nvapi\LICENSE.txt"; DestDir: "{app}\licenses"; DestName: "NVIDIA-NVAPI-LICENSE.txt"; Flags: ignoreversion
Source: "{#RepoRoot}\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\HiveDesk"; Filename: "{app}\HiveDesk.exe"; Parameters: "--settings"; IconFilename: "{app}\HiveDesk.exe"
Name: "{autodesktop}\HiveDesk"; Filename: "{app}\HiveDesk.exe"; Parameters: "--settings"; IconFilename: "{app}\HiveDesk.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[InstallDelete]
Type: files; Name: "{app}\licenses\CUDA-LICENSE.txt"

[Run]
Filename: "{app}\HiveDeskService.exe"; Parameters: "--install"; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""HiveDesk LAN TCP"""; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""HiveDesk LAN Discovery"""; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""HiveDesk LAN TCP"" dir=in action=allow program=""{app}\HiveDesk.exe"" protocol=TCP localport=47980 profile=private,domain remoteip=localsubnet"; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""HiveDesk LAN Discovery"" dir=in action=allow program=""{app}\HiveDesk.exe"" protocol=UDP localport=47979 profile=private,domain remoteip=localsubnet"; Flags: runhidden waituntilterminated
Filename: "{app}\HiveDesk.exe"; Parameters: "--quiet"; Flags: nowait runasoriginaluser runhidden; Check: NotOtaUpdate

[Registry]
Root: HKLM; Subkey: "Software\HiveDesk"; ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "HiveDesk"; Flags: uninsdeletevalue

[UninstallRun]
Filename: "{app}\HiveDeskService.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveHiveDeskService"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""HiveDesk LAN TCP"""; Flags: runhidden waituntilterminated; RunOnceId: "RemoveHiveDeskTCP"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""HiveDesk LAN Discovery"""; Flags: runhidden waituntilterminated; RunOnceId: "RemoveHiveDeskUDP"

[Code]
function NotOtaUpdate: Boolean;
begin
  Result := ExpandConstant('{param:OTA|0}') <> '1';
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ServicePath: String;
  ExitCode: Integer;
begin
  Result := '';
  ServicePath := ExpandConstant('{app}\HiveDeskService.exe');
  if FileExists(ServicePath) then
    if not Exec(ServicePath, '--uninstall', '', SW_HIDE, ewWaitUntilTerminated, ExitCode) then
      Result := 'Could not stop the previous HiveDesk service.'
    else if ExitCode <> 0 then
      Result := 'Could not stop the previous HiveDesk service. Error ' + IntToStr(ExitCode);
end;
