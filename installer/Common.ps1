# Shared helpers for the VibeZ installers. Dot-source this file:
#
#   . (Join-Path $PSScriptRoot 'Common.ps1')
#
# Every helper reports fatal problems through Fail, which prints the message and
# exits with a non-zero code.

function Fail {
    param([string]$Message)
    Write-Host "ERRO: $Message" -ForegroundColor Red
    exit 1
}

function Write-Section {
    param([string]$Message)
    Write-Host ''
    Write-Host "== $Message" -ForegroundColor Cyan
}

function Resolve-Tool {
    param([string]$Name, [string[]]$Candidates)
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) { return $candidate }
    }
    return $null
}

function Get-DumpbinPath {
    $dumpbin = Resolve-Tool -Name 'dumpbin' -Candidates @()
    if ($dumpbin) { return $dumpbin }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -products * -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' 2>$null | Select-Object -First 1
        if ($found) { return $found }
    }
    return $null
}

function Get-BinaryImports {
    param([string]$Dumpbin, [string]$Path)
    & $Dumpbin /nologo /DEPENDENTS $Path 2>$null |
        Select-String -Pattern '^\s+(\S+\.dll)$' |
        ForEach-Object { $_.Matches[0].Groups[1].Value }
}

# Copies the app-local C++ runtime from the Visual Studio redistributable
# directory, so no vcredist install and no administrator rights are needed.
function Copy-CrtRuntime {
    param([string]$Destination)
    $roots = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT' -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch 'onecore' -and (Test-Path (Join-Path $_.FullName 'msvcp140.dll')) } |
        Sort-Object FullName -Descending
    if (-not $roots) { Fail 'Nao encontrei o runtime C++ (Microsoft.VC*.CRT) do Visual Studio.' }
    $root = $roots[0]
    Get-ChildItem (Join-Path $root.FullName '*.dll') |
        Where-Object { $_.Name -notmatch '(?i)(clr|vccorlib)' } |
        ForEach-Object { Copy-Item -Path $_.FullName -Destination $Destination -Force }
    Write-Host "  runtime C++: $($root.FullName)"
}

function Get-InnoCompiler {
    param([string]$Explicit)
    if ($Explicit) { return $Explicit }
    if ($env:INNO_SETUP_ISCC) { return $env:INNO_SETUP_ISCC }
    return Resolve-Tool -Name 'ISCC' -Candidates @(
        'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
        'C:\Program Files\Inno Setup 6\ISCC.exe',
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
    )
}

# Rejects a staged package that contains Debug binaries, files that belong to a
# different product, or an import that nothing in the folder can satisfy.
# Returns the staged files.
function Assert-Package {
    param(
        [Parameter(Mandatory)][string]$DeployDir,
        [string[]]$Forbidden = @(),
        [switch]$SkipDependencyCheck
    )

    $staged = @(Get-ChildItem $DeployDir -Recurse -File)
    if ($staged.Count -eq 0) { Fail "Pacote vazio: $DeployDir" }

    $dumpbin = Get-DumpbinPath

    # Debug MSVC/Qt binaries import the *D runtime; file names cannot be trusted
    # because vcpkg keeps the release names in its debug tree too.
    $debugs = @()
    if ($dumpbin) {
        foreach ($binary in $staged | Where-Object { $_.Extension -in @('.exe', '.dll') }) {
            $imports = Get-BinaryImports -Dumpbin $dumpbin -Path $binary.FullName
            if ($imports | Where-Object { $_ -match '(?i)^(msvcp140|vcruntime140|concrt140).*d\.dll$|^ucrtbased\.dll$' }) {
                $debugs += $binary.Name
            }
        }
    } else {
        $releaseNames = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($releaseRoot in @('vcpkg_installed/x64-windows/bin', 'vcpkg_installed/x64-windows/Qt6/plugins')) {
            if (-not (Test-Path $releaseRoot)) { continue }
            foreach ($file in Get-ChildItem $releaseRoot -Recurse -Filter '*.dll' -File) { [void]$releaseNames.Add($file.Name) }
        }
        $debugOnly = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($debugRoot in @('vcpkg_installed/x64-windows/debug/bin', 'vcpkg_installed/x64-windows/debug/Qt6/plugins')) {
            if (-not (Test-Path $debugRoot)) { continue }
            foreach ($file in Get-ChildItem $debugRoot -Recurse -Filter '*.dll' -File) {
                if (-not $releaseNames.Contains($file.Name)) { [void]$debugOnly.Add($file.Name) }
            }
        }
        $debugs = @($staged | Where-Object { $debugOnly.Contains($_.Name) } | ForEach-Object { $_.Name })
    }
    if ($debugs.Count -gt 0) { Fail "DLLs de Debug no pacote: $($debugs -join ', ')" }

    $unexpected = @($staged | Where-Object { $Forbidden -contains $_.Name })
    if ($unexpected.Count -gt 0) { Fail "Arquivos que nao pertencem a este produto: $($unexpected.Name -join ', ')" }

    if (-not $SkipDependencyCheck) {
        if (-not $dumpbin) {
            Write-Host '  aviso: dumpbin nao encontrado; auditoria de dependencias ignorada.' -ForegroundColor Yellow
        } else {
            # The MSVC runtime is app-local, so it must not be treated as an OS
            # component just because the build machine has it in System32.
            $localRuntime = '^(msvcp140|vcruntime140|concrt140)'
            $system32 = Join-Path $env:SystemRoot 'System32'
            $stagedNames = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            foreach ($file in $staged) { [void]$stagedNames.Add($file.Name) }

            $missing = @()
            foreach ($binary in $staged | Where-Object { $_.Extension -in @('.exe', '.dll') }) {
                foreach ($import in (Get-BinaryImports -Dumpbin $dumpbin -Path $binary.FullName | Sort-Object -Unique)) {
                    if ($stagedNames.Contains($import)) { continue }
                    if ($import -match $localRuntime) { $missing += "$($binary.Name) -> $import"; continue }
                    if ($import -match '^(api-ms-win|ext-ms-)') { continue }
                    if (Test-Path (Join-Path $system32 $import)) { continue }
                    $missing += "$($binary.Name) -> $import"
                }
            }
            if ($missing) { Fail "Dependencias nao satisfeitas:`n  $($missing -join "`n  ")" }
            Write-Host "  dependencias OK ($($staged.Count) arquivos)"
        }
    }

    return $staged
}
