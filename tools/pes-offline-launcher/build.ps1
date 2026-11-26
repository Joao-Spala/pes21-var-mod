<#
.SYNOPSIS
    Compila o PES 2021 Offline Launcher com o csc do .NET Framework 4 (sem SDK).
.PARAMETER BmpesDir
    Pasta do BMPES (onde fica o sider.exe). Se omitida, e descoberta pelas bibliotecas da Steam.
.PARAMETER Instalar
    Copia o exe para a pasta do BMPES (arquivo novo, nao substitui nada do patch).
#>
param([string]$BmpesDir, [switch]$Instalar)

$ErrorActionPreference = 'Stop'
$csc  = Join-Path ([Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory()) 'csc.exe'
$here = $PSScriptRoot
$out  = Join-Path $here '..\..\dist\pes-offline-launcher'
$exe  = Join-Path $out 'PES Offline Launcher.exe'

if (-not $BmpesDir) {
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam').SteamPath -replace '/', '\'
    $libs  = @(Join-Path $steam 'steamapps')
    $vdf   = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        $libs += [regex]::Matches((Get-Content $vdf -Raw), '"path"\s*"([^"]+)"') |
                 ForEach-Object { [IO.Path]::Combine(($_.Groups[1].Value -replace '\\\\', '\'), 'steamapps') }
    }
    # [IO.Path]::Combine e nao Join-Path: bibliotecas em drives desconectados nao podem abortar o build
    $BmpesDir = $libs | ForEach-Object { [IO.Path]::Combine($_, 'common\BMPES') } |
                Where-Object { [IO.File]::Exists([IO.Path]::Combine($_, 'sider.exe')) } | Select-Object -First 1
}

New-Item -ItemType Directory -Force $out | Out-Null

$cscArgs = @('/nologo', '/target:winexe', '/optimize+', '/platform:anycpu',
             "/out:$exe",
             '/r:System.dll', '/r:System.Core.dll', '/r:System.Drawing.dll', '/r:System.Windows.Forms.dll')
if ($BmpesDir -and (Test-Path (Join-Path $BmpesDir 'icon.ico'))) { $cscArgs += "/win32icon:$(Join-Path $BmpesDir 'icon.ico')" }
$cscArgs += (Join-Path $here 'PesOfflineLauncher.cs')

& $csc @cscArgs
if ($LASTEXITCODE -ne 0) { throw "csc falhou ($LASTEXITCODE)" }
Write-Host "OK: $exe"

if ($Instalar) {
    if (-not $BmpesDir) { throw 'Pasta do BMPES nao encontrada. Informe -BmpesDir.' }
    $dest = Join-Path $BmpesDir 'PES Offline Launcher.exe'
    if ((Test-Path $dest) -and -not (Test-Path "$dest.bak")) { Copy-Item $dest "$dest.bak" }
    Copy-Item $exe $dest -Force
    Write-Host "Instalado: $dest"
}
