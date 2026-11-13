; VibeZ Server installer (per-user, no administrator rights required).
;
; Build it with installer\build-installer.ps1 -Product Server, which stages the
; Release server deployment in out\deploy\server and passes the version in:
;
;   ISCC.exe /DAppVersion=0.1.0 installer\VibeZServer.iss
;
; The wizard writes vibez-server.ini with the two ports. The server reads that
; file on startup; editing it and restarting is enough to change ports later.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef DeployDir
  #define DeployDir "..\out\deploy\server"
#endif

#if !DirExists(AddBackslash(SourcePath) + DeployDir)
  #error The staged server is missing. Run installer\build-installer.ps1 -Product Server first.
#endif

#define AppName "VibeZ Server"
#define AppExe "VibeZServer.exe"
#define AppPublisher "VibeZ"
#define AppIdValue "{{3E7A5D18-9C42-4B6F-8A21-5D0C7E9B1A34}"
#define DefaultVoicePort "28765"
#define DefaultWebPort "8080"

[Setup]
AppId={#AppIdValue}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}.0
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} - servidor de voz e tela
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}.0

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
OutputBaseFilename=VibeZServer-Setup-{#AppVersion}
SetupIconFile=vibez-server.ico
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
brazilianportuguese.PortsTitle=Portas do servidor
brazilianportuguese.PortsSubtitle=Escolha as portas do app e do site
english.PortsTitle=Server ports
english.PortsSubtitle=Choose the app and website ports
brazilianportuguese.PortsDescription=A porta do app transporta voz, vídeo e controle (TCP e UDP). A porta do site serve a página do visualizador no navegador. Libere as duas no firewall para acesso fora deste PC.
english.PortsDescription=The app port carries voice, video and control (TCP and UDP). The website port serves the browser viewer page. Allow both through the firewall for access outside this PC.
brazilianportuguese.VoicePortLabel=Porta do app (voz e tela):
english.VoicePortLabel=App port (voice and screen):
brazilianportuguese.WebPortLabel=Porta do site (navegador):
english.WebPortLabel=Website port (browser):
brazilianportuguese.VoicePortInvalid=A porta do app precisa ser um número entre 1 e 65535.
english.VoicePortInvalid=The app port must be a number between 1 and 65535.
brazilianportuguese.WebPortInvalid=A porta do site precisa ser um número entre 1 e 65535.
english.WebPortInvalid=The website port must be a number between 1 and 65535.
brazilianportuguese.PortsSame=O app e o site não podem usar a mesma porta.
english.PortsSame=The app and the website cannot use the same port.
brazilianportuguese.IniWriteError=Não foi possível gravar o arquivo de configuração. O servidor usará as portas padrão (28765 e 8080).
english.IniWriteError=The settings file could not be written. The server will use the default ports (28765 and 8080).

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#DeployDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
; The settings file is written by the wizard below; remove it with the product
; so a later install starts from a clean state.
Type: files; Name: "{app}\vibez-server.ini"

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"
Name: "{group}\Abrir o visualizador no navegador"; Filename: "http://127.0.0.1:{code:GetWebPort}"
Name: "{group}\Desinstalar {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchApp}"; Flags: nowait postinstall skipifsilent

[Code]
var
  PortPage: TInputQueryWizardPage;

function ReadExistingPort(const Key: String; const Fallback: String): String;
var
  Value: String;
begin
  // {app} is not expanded yet during InitializeWizard, so read the default
  // install location. A repair/upgrade install keeps the previous ports.
  Value := GetIniString('server', Key, '',
    ExpandConstant('{localappdata}\Programs\{#AppName}\vibez-server.ini'));
  if Value = '' then
    Result := Fallback
  else
    Result := Value;
end;

procedure InitializeWizard;
begin
  PortPage := CreateInputQueryPage(wpSelectDir,
    ExpandConstant('{cm:PortsTitle}'), ExpandConstant('{cm:PortsSubtitle}'),
    ExpandConstant('{cm:PortsDescription}'));
  PortPage.Add(ExpandConstant('{cm:VoicePortLabel}'), False);
  PortPage.Add(ExpandConstant('{cm:WebPortLabel}'), False);

  // Command line values win (used by silent installs and by the build script),
  // then the previous configuration, then the defaults.
  PortPage.Values[0] := ExpandConstant('{param:voiceport|' + ReadExistingPort('voice_port', '{#DefaultVoicePort}') + '}');
  PortPage.Values[1] := ExpandConstant('{param:webport|' + ReadExistingPort('web_port', '{#DefaultWebPort}') + '}');
end;

function TryPortText(const Text: String; out Port: Integer): Boolean;
begin
  Port := StrToIntDef(Trim(Text), -1);
  Result := (Port >= 1) and (Port <= 65535);
end;

function ValidatePorts: String;
var
  Voice, Web: Integer;
begin
  Result := '';
  if not TryPortText(PortPage.Values[0], Voice) then
    Result := ExpandConstant('{cm:VoicePortInvalid}')
  else if not TryPortText(PortPage.Values[1], Web) then
    Result := ExpandConstant('{cm:WebPortInvalid}')
  else if Voice = Web then
    Result := ExpandConstant('{cm:PortsSame}');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Message: String;
begin
  Result := True;
  if CurPageID = PortPage.ID then begin
    Message := ValidatePorts;
    if Message <> '' then begin
      MsgBox(Message, mbError, MB_OK);
      Result := False;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  // Silent installs never visit the wizard page, so validate here as well.
  Result := ValidatePorts;
end;

function GetVoicePort(Param: String): String;
begin
  Result := PortPage.Values[0];
end;

function GetWebPort(Param: String): String;
begin
  Result := PortPage.Values[1];
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  IniPath: String;
  Contents: String;
begin
  if CurStep <> ssPostInstall then
    Exit;

  IniPath := ExpandConstant('{app}\vibez-server.ini');
  Contents :=
    '; VibeZ Server - gerado pelo instalador.' + #13#10 +
    '; Edite as portas e reinicie o servidor para aplicar.' + #13#10 +
    '[server]' + #13#10 +
    'voice_port=' + PortPage.Values[0] + #13#10 +
    'web_port=' + PortPage.Values[1] + #13#10 +
    'web_bind=0.0.0.0' + #13#10 +
    'web_enabled=1' + #13#10;
  if not SaveStringToFile(IniPath, Contents, False) then
    MsgBox(ExpandConstant('{cm:IniWriteError}') + #13#10 + IniPath, mbError, MB_OK);
end;
