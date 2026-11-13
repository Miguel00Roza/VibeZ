; VibeZ client installer (per-user, no administrator rights required).
;
; Build it with installer\build-installer.ps1, which stages a clean Release
; deployment in out\deploy\client and passes the version in:
;
;   ISCC.exe /DAppVersion=0.1.0 installer\VibeZ.iss
;
; Only the GUI client is packaged. The server is a separate, independent
; product and must never be shipped inside the player's installer.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef DeployDir
  #define DeployDir "..\out\deploy\client"
#endif

#if !DirExists(AddBackslash(SourcePath) + DeployDir)
  #error The staged client is missing. Run installer\build-installer.ps1 first.
#endif

#define AppName "VibeZ"
#define AppExe "VibeZ.exe"
#define AppPublisher "VibeZ"
; The doubled brace is the documented way to keep a literal "{" in AppId.
#define AppIdValue "{{6F1B9C24-71A4-4C0E-9B57-2E5C3D8A4F01}"

[Setup]
; A stable AppId keeps upgrades and the uninstall entry in sync.
AppId={#AppIdValue}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}.0
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} - voz e tela compartilhada
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}.0

; Per-user installation: everything stays inside the user profile, so the
; installer never asks for elevation and works on locked-down machines.
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
UsePreviousAppDir=yes
UsePreviousTasks=yes

ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

OutputDir=..\out\installer
OutputBaseFilename={#AppName}-Setup-{#AppVersion}
SetupIconFile=vibez.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName} {#AppVersion}

Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
WizardSizePercent=110
ShowLanguageDialog=auto
SetupLogging=yes
CloseApplications=yes
RestartApplications=no
AllowNoIcons=yes
DisableFinishedPage=no

[Languages]
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
brazilianportuguese.CreateDesktopIcon=Criar um atalho na &área de trabalho
english.CreateDesktopIcon=Create a &desktop shortcut
brazilianportuguese.LaunchApp=Iniciar o {#AppName} agora
english.LaunchApp=Launch {#AppName} now
brazilianportuguese.AdditionalIcons=Atalhos:
english.AdditionalIcons=Shortcuts:

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#DeployDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"
Name: "{group}\Desinstalar {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchApp}"; Flags: nowait postinstall skipifsilent
