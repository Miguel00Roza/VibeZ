# Builds the VibeZ installers.
#
#   powershell -ExecutionPolicy Bypass -File installer\build-installer.ps1
#   powershell -ExecutionPolicy Bypass -File installer\build-installer.ps1 -Product Server
#   powershell -ExecutionPolicy Bypass -File installer\build-installer.ps1 -Product All -TestInstall
#
# Each product is staged from the Release build into a clean directory (the
# shared build directory also holds Debug DLLs, which must never reach users),
# audited, and then compiled with Inno Setup 6.
[CmdletBinding()]
param(
    [ValidateSet('Client', 'Server', 'All')]
    [string]$Product = 'Client',
    [string]$AppVersion = '0.1.0',
    [string]$BuildDir = 'out/build/vibez-windows-debug',
    [string]$Iscc,
    [switch]$SkipBuild,
    [switch]$SkipDependencyCheck,
    # Installs each result silently into a temporary directory, checks it, then
    # uninstalls again. Leaves the machine clean.
    [switch]$TestInstall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {

. (Join-Path $PSScriptRoot 'Common.ps1')

$script:cmake = $null
$script:iscc = $null

function Get-CMake {
    if ($script:cmake) { return $script:cmake }
    $script:cmake = Resolve-Tool -Name 'cmake' -Candidates @(
        'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    )
    if (-not $script:cmake) { Fail 'cmake.exe nao encontrado no PATH nem na instalacao do Visual Studio.' }
    return $script:cmake
}

function Get-Iscc {
    if ($script:iscc) { return $script:iscc }
    $script:iscc = Get-InnoCompiler -Explicit $Iscc
    if (-not $script:iscc) {
        Fail 'ISCC.exe (Inno Setup 6) nao encontrado. Instale com: winget install JRSoftware.InnoSetup'
    }
    return $script:iscc
}

function Invoke-BuildTarget {
    param([string]$Target, [string]$RunningProcess)
    if ($SkipBuild) { return }
    $running = Get-Process -Name $RunningProcess -ErrorAction SilentlyContinue
    if ($running) {
        Fail "Feche o $RunningProcess em execucao (PID $(($running | Select-Object -ExpandProperty Id) -join ', ')) antes de compilar."
    }
    Write-Section "Compilando $Target (Release)"
    & (Get-CMake) --build $BuildDir --config Release --target $Target | Out-Host
    if ($LASTEXITCODE -ne 0) { Fail "A compilacao de $Target falhou (codigo $LASTEXITCODE)." }
}

function New-StagingDirectory {
    param([string]$Path)
    if (Test-Path $Path) { Remove-Item $Path -Recurse -Force }
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
    New-Item -ItemType Directory -Path (Split-Path $Path -Parent) -Force | Out-Null
}

function Invoke-InnoSetup {
    param([string]$Script, [string]$InstallerName)
    Write-Section 'Compilando o instalador'
    $outputDir = Join-Path $repoRoot 'out/installer'
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
    & (Get-Iscc) "/DAppVersion=$AppVersion" (Join-Path $PSScriptRoot $Script) | Out-Host
    if ($LASTEXITCODE -ne 0) { Fail "O Inno Setup falhou (codigo $LASTEXITCODE)." }
    $installer = Join-Path $outputDir $InstallerName
    if (-not (Test-Path $installer)) { Fail "Instalador nao gerado: $installer" }
    $item = Get-Item $installer
    Write-Host ''
    Write-Host "Instalador: $($item.FullName)" -ForegroundColor Green
    Write-Host ("Tamanho:    {0:N1} MB" -f ($item.Length / 1MB)) -ForegroundColor Green
    return $installer
}

# ---------------------------------------------------------------------------
# Client
# ---------------------------------------------------------------------------
function New-ClientPackage {
    Invoke-BuildTarget -Target 'VibeZClient' -RunningProcess 'VibeZ'

    $deployDir = 'out/deploy/client'
    Write-Section "Preparando $deployDir"
    New-StagingDirectory -Path $deployDir

    $vcpkgBin = 'vcpkg_installed/x64-windows/bin'
    $qtPlugins = 'vcpkg_installed/x64-windows/Qt6/plugins'

    # Exactly the runtime the client loads: Qt widgets stack, Opus and the
    # third-party libraries referenced by them.
    $runtimeDlls = @(
        'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll',
        'opus.dll',
        # icuuc.dll is not bundled: Windows 10/11 ship it in System32 (verified by
        # hash), so packaging it would only risk shipping an outdated copy.
        'z.dll', 'zstd.dll', 'double-conversion.dll', 'pcre2-16.dll',
        'libpng16.dll', 'md4c.dll', 'freetype.dll', 'bz2.dll',
        'jpeg62.dll', 'turbojpeg.dll',
        'brotlidec.dll', 'brotlicommon.dll', 'brotlienc.dll'
    )

    $builtExe = Join-Path $BuildDir 'Release/VibeZ.exe'
    if (-not (Test-Path $builtExe)) { Fail "Binario nao encontrado: $builtExe" }
    if (-not (Test-Path 'installer/vibez.ico')) { Fail 'installer/vibez.ico ausente. Rode installer\make-icon.ps1.' }

    Copy-Item -Path $builtExe -Destination $deployDir -Force
    foreach ($dll in $runtimeDlls) {
        $source = Join-Path $vcpkgBin $dll
        if (-not (Test-Path $source)) { Fail "DLL de runtime ausente: $source" }
        Copy-Item -Path $source -Destination $deployDir -Force
    }
    Copy-CrtRuntime -Destination $deployDir

    # Qt plugins live in subdirectories that QCoreApplication resolves relative
    # to the executable; missing "platforms" means the app cannot start at all.
    $pluginDirs = @('platforms', 'styles', 'imageformats', 'iconengines', 'generic')
    foreach ($pluginDir in $pluginDirs) {
        $source = Join-Path $qtPlugins $pluginDir
        if (-not (Test-Path $source)) {
            if ($pluginDir -eq 'platforms') { Fail "Plugins do Qt ausentes: $source" }
            Write-Host "  aviso: plugin opcional ausente: $pluginDir"
            continue
        }
        Copy-Item -Path $source -Destination $deployDir -Recurse -Force
    }

    # vcpkg keeps program databases beside the release plugins; they would add
    # ~47 MB to the installer and help nobody.
    Get-ChildItem $deployDir -Recurse -File -Include '*.pdb', '*.lib', '*.exp', '*.ilk' | Remove-Item -Force

    Write-Section 'Validando o pacote do cliente'
    $staged = Assert-Package -DeployDir $deployDir `
        -Forbidden @('VibeZServer.exe', 'VibeZConsole.exe', 'vc_redist.x64.exe') `
        -SkipDependencyCheck:$SkipDependencyCheck

    $exeInfo = (Get-Item (Join-Path $deployDir 'VibeZ.exe')).VersionInfo
    Write-Host "  VibeZ.exe $($exeInfo.FileVersion) / $($exeInfo.FileDescription)"
    $totalSize = ($staged | Measure-Object -Property Length -Sum).Sum
    Write-Host ("  conteudo: {0} arquivos, {1:N1} MB" -f $staged.Count, ($totalSize / 1MB))

    return Invoke-InnoSetup -Script 'VibeZ.iss' -InstallerName "VibeZ-Setup-$AppVersion.exe"
}

# ---------------------------------------------------------------------------
# Server
# ---------------------------------------------------------------------------
function New-ServerPackage {
    Invoke-BuildTarget -Target 'VibeZServer' -RunningProcess 'VibeZServer'

    $deployDir = 'out/deploy/server'
    Write-Section "Preparando $deployDir"
    New-StagingDirectory -Path $deployDir

    $builtExe = Join-Path $BuildDir 'Release/VibeZServer.exe'
    if (-not (Test-Path $builtExe)) { Fail "Binario nao encontrado: $builtExe" }
    if (-not (Test-Path 'installer/vibez-server.ico')) { Fail 'installer/vibez-server.ico ausente. Rode installer\make-icon.ps1.' }

    Copy-Item -Path $builtExe -Destination $deployDir -Force
    $opus = Join-Path 'vcpkg_installed/x64-windows/bin' 'opus.dll'
    if (-not (Test-Path $opus)) { Fail "DLL de runtime ausente: $opus" }
    Copy-Item -Path $opus -Destination $deployDir -Force
    Copy-CrtRuntime -Destination $deployDir

    Write-Section 'Validando o pacote do servidor'
    $staged = Assert-Package -DeployDir $deployDir `
        -Forbidden @('VibeZ.exe', 'VibeZConsole.exe', 'vc_redist.x64.exe', 'Qt6Core.dll') `
        -SkipDependencyCheck:$SkipDependencyCheck

    $exeInfo = (Get-Item (Join-Path $deployDir 'VibeZServer.exe')).VersionInfo
    Write-Host "  VibeZServer.exe $($exeInfo.FileVersion) / $($exeInfo.FileDescription)"
    $totalSize = ($staged | Measure-Object -Property Length -Sum).Sum
    Write-Host ("  conteudo: {0} arquivos, {1:N1} MB" -f $staged.Count, ($totalSize / 1MB))

    return Invoke-InnoSetup -Script 'VibeZServer.iss' -InstallerName "VibeZServer-Setup-$AppVersion.exe"
}

# ---------------------------------------------------------------------------
# Silent install round trips
# ---------------------------------------------------------------------------
function Get-FreeTcpPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
    $listener.Stop()
    return $port
}

function Test-ClientInstaller {
    param([string]$Installer)
    Write-Section 'Teste de instalacao silenciosa do cliente'
    $testDir = Join-Path $env:TEMP 'vibez-client-install-test'
    if (Test-Path $testDir) { Remove-Item $testDir -Recurse -Force }
    $errors = [System.Collections.Generic.List[string]]::new()
    $installOk = $false
    $previousDebugPlugins = $env:QT_DEBUG_PLUGINS

    try {
        $installLog = Join-Path $env:TEMP 'vibez-client-install.log'
        $install = Start-Process -FilePath $Installer -ArgumentList @(
            '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-',
            "/DIR=$testDir", "/LOG=$installLog"
        ) -PassThru -Wait
        if ($install.ExitCode -ne 0) {
            $errors.Add("instalacao silenciosa falhou (codigo $($install.ExitCode)); veja $installLog")
        } else {
            $installOk = $true
            foreach ($relative in @('VibeZ.exe', 'opus.dll', 'Qt6Core.dll', 'platforms\qwindows.dll', 'msvcp140.dll', 'vcruntime140.dll')) {
                if (-not (Test-Path (Join-Path $testDir $relative))) { $errors.Add("falta $relative") }
            }
            foreach ($unexpected in @('VibeZServer.exe', 'vc_redist.x64.exe')) {
                if (Test-Path (Join-Path $testDir $unexpected)) { $errors.Add("nao deveria conter $unexpected") }
            }

            # Starts the installed executable from its own directory with Qt's
            # plugin diagnostics on: a missing platform plugin aborts before any
            # window exists, which no static check can prove.
            $probeLog = Join-Path $env:TEMP 'vibez-client-runtime-probe.log'
            Remove-Item $probeLog -ErrorAction SilentlyContinue
            $env:QT_DEBUG_PLUGINS = '1'
            $probe = Start-Process -FilePath (Join-Path $testDir 'VibeZ.exe') -WorkingDirectory $testDir `
                -RedirectStandardError $probeLog -PassThru
            Start-Sleep -Seconds 5
            $exited = $probe.HasExited
            if (-not $exited) { Stop-Process -Id $probe.Id -Force -ErrorAction SilentlyContinue }
            $env:QT_DEBUG_PLUGINS = $previousDebugPlugins
            $probeText = if (Test-Path $probeLog) { Get-Content $probeLog -Raw } else { '' }
            if ($exited) {
                $errors.Add("o VibeZ instalado encerrou ao iniciar:`n$probeText")
            } elseif ($probeText -notmatch 'platforms[\\/]qwindows') {
                $errors.Add("o plugin de plataforma do Qt nao foi carregado:`n$probeText")
            } elseif ($probeText -match '(?i)(could not load|cannot load|no Qt platform plugin)') {
                $errors.Add("falha ao carregar plugins do Qt:`n$probeText")
            }

            $shortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\VibeZ\VibeZ.lnk'
            if (-not (Test-Path $shortcut)) { $errors.Add('atalho do Menu Iniciar nao encontrado') }
        }

        $installedSize = (Get-ChildItem $testDir -Recurse -File -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum
        if ($installOk) { Write-Host ("  instalado em {0} ({1:N1} MB)" -f $testDir, ($installedSize / 1MB)) }
    } catch {
        $errors.Add($_.Exception.Message)
    } finally {
        if ($null -eq $previousDebugPlugins) { Remove-Item Env:\QT_DEBUG_PLUGINS -ErrorAction SilentlyContinue }
        else { $env:QT_DEBUG_PLUGINS = $previousDebugPlugins }

        if ($installOk) {
            $uninstaller = Join-Path $testDir 'unins000.exe'
            if (Test-Path $uninstaller) {
                $uninstall = Start-Process -FilePath $uninstaller -ArgumentList @(
                    '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
                ) -PassThru -Wait
                Start-Sleep -Seconds 1
                if ($uninstall.ExitCode -ne 0) { $errors.Add("desinstalacao falhou (codigo $($uninstall.ExitCode))") }
                if (Test-Path (Join-Path $testDir 'VibeZ.exe')) { $errors.Add('a desinstalacao deixou o executavel para tras') }
            } else {
                $errors.Add('desinstalador ausente apos a instalacao')
            }
            Remove-Item $testDir -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    if ($errors.Count -gt 0) { Fail ("Teste do cliente falhou:`n  " + ($errors -join "`n  ")) }
    Write-Host '  instalacao, inicializacao, atalho e desinstalacao OK' -ForegroundColor Green
}

function Test-ServerInstaller {
    param([string]$Installer)
    Write-Section 'Teste de instalacao silenciosa do servidor'
    $testDir = Join-Path $env:TEMP 'vibez-server-install-test'
    if (Test-Path $testDir) { Remove-Item $testDir -Recurse -Force }
    $voicePort = Get-FreeTcpPort
    $webPort = Get-FreeTcpPort
    while ($webPort -eq $voicePort) { $webPort = Get-FreeTcpPort }

    $errors = [System.Collections.Generic.List[string]]::new()
    $installOk = $false
    $server = $null

    try {
        $installLog = Join-Path $env:TEMP 'vibez-server-install.log'
        $install = Start-Process -FilePath $Installer -ArgumentList @(
            '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-',
            "/DIR=$testDir", "/VOICEPORT=$voicePort", "/WEBPORT=$webPort", "/LOG=$installLog"
        ) -PassThru -Wait
        if ($install.ExitCode -ne 0) {
            $errors.Add("instalacao silenciosa falhou (codigo $($install.ExitCode)); veja $installLog")
        } else {
            $installOk = $true
            foreach ($relative in @('VibeZServer.exe', 'opus.dll', 'vibez-server.ini', 'msvcp140.dll', 'vcruntime140.dll')) {
                if (-not (Test-Path (Join-Path $testDir $relative))) { $errors.Add("falta $relative") }
            }
            foreach ($unexpected in @('VibeZ.exe', 'Qt6Core.dll', 'vc_redist.x64.exe')) {
                if (Test-Path (Join-Path $testDir $unexpected)) { $errors.Add("nao deveria conter $unexpected") }
            }

            $ini = Join-Path $testDir 'vibez-server.ini'
            if (Test-Path $ini) {
                $contents = Get-Content $ini -Raw
                if ($contents -notmatch "voice_port=$voicePort") { $errors.Add("voice_port=$voicePort nao foi gravada no INI") }
                if ($contents -notmatch "web_port=$webPort") { $errors.Add("web_port=$webPort nao foi gravada no INI") }
            }

            # The executable must read the INI that the installer just wrote.
            $printOut = (& (Join-Path $testDir 'VibeZServer.exe') --print-config 2>&1 | Out-String)
            if ($printOut -notmatch "voice_port=$voicePort") { $errors.Add("--print-config nao leu a porta do app`n$printOut") }
            if ($printOut -notmatch "web_port=$webPort") { $errors.Add("--print-config nao leu a porta do site`n$printOut") }

            # And it must actually listen on both configured ports.
            $serverOut = Join-Path $env:TEMP 'vibez-server-install-test.out.log'
            $serverErr = Join-Path $env:TEMP 'vibez-server-install-test.err.log'
            Remove-Item $serverOut, $serverErr -ErrorAction SilentlyContinue
            $server = Start-Process -FilePath (Join-Path $testDir 'VibeZServer.exe') -WorkingDirectory $testDir `
                -WindowStyle Hidden -RedirectStandardOutput $serverOut -RedirectStandardError $serverErr -PassThru

            $deadline = (Get-Date).AddSeconds(20)
            $voiceUp = $false
            $webUp = $false
            while ((Get-Date) -lt $deadline -and -not ($voiceUp -and $webUp)) {
                if ($server.HasExited) { break }
                if (-not $voiceUp) {
                    $voiceUp = Test-NetConnection -ComputerName '127.0.0.1' -Port $voicePort -InformationLevel Quiet -WarningAction SilentlyContinue
                }
                if (-not $webUp) {
                    try {
                        $response = Invoke-WebRequest "http://127.0.0.1:$webPort/api/streams" -UseBasicParsing -TimeoutSec 2
                        $webUp = $response.StatusCode -eq 200
                    } catch {
                        $webUp = $false
                    }
                }
                if (-not ($voiceUp -and $webUp)) { Start-Sleep -Milliseconds 400 }
            }
            if ($server.HasExited) {
                $errors.Add("o servidor encerrou ao iniciar:`n$(Get-Content $serverErr -Raw -ErrorAction SilentlyContinue)")
            }
            if (-not $voiceUp) { $errors.Add("a porta do app ($voicePort) nao respondeu") }
            if (-not $webUp) { $errors.Add("a porta do site ($webPort) nao respondeu") }
        }
    } catch {
        $errors.Add("$($_.Exception.Message)`n  em: $($_.ScriptStackTrace)")
    } finally {
        if ($server -and -not $server.HasExited) {
            Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
            Start-Sleep -Milliseconds 500
        }
        if ($installOk) {
            $uninstaller = Join-Path $testDir 'unins000.exe'
            if (Test-Path $uninstaller) {
                $uninstall = Start-Process -FilePath $uninstaller -ArgumentList @(
                    '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
                ) -PassThru -Wait
                Start-Sleep -Seconds 1
                if ($uninstall.ExitCode -ne 0) { $errors.Add("desinstalacao falhou (codigo $($uninstall.ExitCode))") }
                if (Test-Path (Join-Path $testDir 'VibeZServer.exe')) { $errors.Add('a desinstalacao deixou o executavel para tras') }
                if (Test-Path (Join-Path $testDir 'vibez-server.ini')) { $errors.Add('a desinstalacao deixou o INI para tras') }
            } else {
                $errors.Add('desinstalador ausente apos a instalacao')
            }
            Remove-Item $testDir -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    if ($errors.Count -gt 0) { Fail ("Teste do servidor falhou:`n  " + ($errors -join "`n  ")) }
    Write-Host ("  instalacao, portas {0}/{1}, inicializacao e desinstalacao OK" -f $voicePort, $webPort) -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
$products = switch ($Product) {
    'All' { @('Client', 'Server') }
    default { @($Product) }
}

$installers = [ordered]@{}
foreach ($item in $products) {
    if ($item -eq 'Client') { $installers.Client = New-ClientPackage }
    else { $installers.Server = New-ServerPackage }
}

if ($TestInstall) {
    if ($installers.Contains('Client')) { Test-ClientInstaller -Installer $installers.Client }
    if ($installers.Contains('Server')) { Test-ServerInstaller -Installer $installers.Server }
}

Write-Host ''
Write-Host 'Concluido:' -ForegroundColor Cyan
foreach ($name in $installers.Keys) {
    $file = Get-Item $installers[$name]
    Write-Host ("  {0,-7} {1} ({2:N1} MB)" -f $name, $file.FullName, ($file.Length / 1MB))
}

} finally {
    Pop-Location
}
