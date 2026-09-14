; DYMO roll patch - installer script (Inno Setup 6)
; Per-user install (no admin needed), optional Windows-startup registration.

#define MyAppName "DYMO roll patch"
#define MyAppVersion "1.0"
#define MyAppPublisher "haadeej"
#define MyAppExeName "dymo.exe"

[Setup]
AppId={{833C64E2-546A-43C7-85C1-6F4CB1CA8D2E}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\dymo-patch
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputBaseFilename=dymo-roll-patch-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: "startup"; Description: "Start automatically when I log in (Windows startup)"; GroupDescription: "Startup:"; Flags: checkedonce
Name: "launch"; Description: "Launch {#MyAppName} now"; GroupDescription: "After install:"; Flags: unchecked

[Files]
Source: "..\release\dymo.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release\dnlib.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release\dymo.exe.config"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Registry]
; Same entry the app itself writes via "Add to Windows startup…"
Root: hkcu; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "dymo_roll_patch"; ValueData: """{app}\{#MyAppExeName}"" --tray"; \
    Flags: uninsdeletevalue; Tasks: startup

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: launch

[Run]
Filename: "{app}\{#MyAppExeName}"; Parameters: "--tray"; Description: "Launch {#MyAppName}"; \
    Flags: nowait postinstall skipifsilent; Tasks: launch

[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\dymo_roll.flag"
