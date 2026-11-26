<#
.SYNOPSIS
    Liga ou desliga o modo offline nativo da Steam para jogar PES 2021 sem internet.

.DESCRIPTION
    Edita apenas os campos WantsOfflineMode e SkipOfflineModeWarning da conta com
    AutoLogin em <Steam>\config\loginusers.vdf, que sao os mesmos campos que a Steam
    grava ao usar "Steam > Ficar offline". Nao toca no jogo, no exe nem em DLLs.

    A Steam precisa estar fechada durante a edicao, porque ela reescreve esse arquivo
    ao sair. O script fecha a Steam, cria backup, edita e reabre.

.PARAMETER Modo
    Offline: liga o modo offline. Online: volta ao normal. Status: so mostra o estado.

.EXAMPLE
    .\Steam_Modo_Offline.ps1 -Modo Offline
#>
param(
    [ValidateSet('Offline', 'Online', 'Status')]
    [string]$Modo = 'Status',
    [switch]$NaoAbrirSteam
)

$ErrorActionPreference = 'Stop'

$steamPath = (Get-ItemProperty 'HKCU:\Software\Valve\Steam').SteamPath -replace '/', '\'
$steamExe  = Join-Path $steamPath 'steam.exe'
$vdf       = Join-Path $steamPath 'config\loginusers.vdf'
if (-not (Test-Path $vdf)) { throw "loginusers.vdf nao encontrado em $vdf" }

function Get-AutoLoginUser([string]$texto) {
    $m = [regex]::Matches($texto, '"(\d{17})"\s*\{([^{}]*)\}')
    foreach ($u in $m) {
        if ($u.Groups[2].Value -match '"AutoLogin"\s*"1"') { return $u }
    }
    if ($m.Count -eq 1) { return $m[0] }
    throw 'Nenhuma conta com AutoLogin=1. Faca login na Steam com "Lembrar senha" marcado.'
}

function Show-Status {
    $t = Get-Content $vdf -Raw
    $u = Get-AutoLoginUser $t
    $b = $u.Groups[2].Value
    $get = { param($k) if ($b -match "`"$k`"\s*`"(\d)`"") { $Matches[1] } else { '(ausente)' } }
    Write-Host "Conta:                  $($u.Groups[1].Value)"
    Write-Host "RememberPassword:       $(& $get 'RememberPassword')"
    Write-Host "AutoLogin:              $(& $get 'AutoLogin')"
    Write-Host "WantsOfflineMode:       $(& $get 'WantsOfflineMode')"
    Write-Host "SkipOfflineModeWarning: $(& $get 'SkipOfflineModeWarning')"
}

if ($Modo -eq 'Status') { Show-Status; return }

$valor = if ($Modo -eq 'Offline') { '1' } else { '0' }

# 1. Fechar a Steam (ela reescreve o vdf ao sair)
if (Get-Process steam -ErrorAction SilentlyContinue) {
    Write-Host 'Fechando a Steam...'
    Start-Process $steamExe -ArgumentList '-shutdown'
    $limite = (Get-Date).AddSeconds(30)
    while ((Get-Process steam -ErrorAction SilentlyContinue) -and (Get-Date) -lt $limite) {
        Start-Sleep -Milliseconds 500
    }
    if (Get-Process steam -ErrorAction SilentlyContinue) {
        throw 'A Steam nao fechou em 30s. Feche manualmente e rode de novo.'
    }
    Start-Sleep -Seconds 1
}

# 2. Backup
$backup = "$vdf.antes-$($Modo.ToLower())-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
Copy-Item $vdf $backup
Write-Host "Backup: $backup"

# 3. Editar somente o bloco da conta com AutoLogin
$texto = Get-Content $vdf -Raw
$u = Get-AutoLoginUser $texto
$bloco = $u.Groups[2].Value
if ($bloco -notmatch '"RememberPassword"\s*"1"') {
    Write-Warning 'RememberPassword nao esta em 1. O modo offline exige login salvo. Entre online com "Lembrar senha" marcado antes.'
}
foreach ($k in 'WantsOfflineMode', 'SkipOfflineModeWarning') {
    if ($bloco -match "`"$k`"\s*`"\d`"") {
        $bloco = [regex]::Replace($bloco, "(`"$k`"\s*`")\d(`")", "`${1}$valor`${2}")
    } else {
        $bloco = $bloco.TrimEnd() + "`n`t`t`"$k`"`t`t`"$valor`"`n`t"
    }
}
$novo = $texto.Substring(0, $u.Groups[2].Index) + $bloco + $texto.Substring($u.Groups[2].Index + $u.Groups[2].Length)
[IO.File]::WriteAllText($vdf, $novo, (New-Object Text.UTF8Encoding($false)))

Write-Host "Modo $Modo aplicado."
Show-Status

# 4. Reabrir a Steam
if (-not $NaoAbrirSteam) {
    Start-Process $steamExe
    Write-Host 'Steam reaberta.'
}
