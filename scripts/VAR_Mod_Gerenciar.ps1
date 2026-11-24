param(
    [ValidateSet("menu", "install", "enable", "disable", "uninstall", "status")]
    [string]$Action = "menu",
    [string]$SiderDir = ""
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = if (Test-Path (Join-Path $ScriptDir "modules\var_mod\var_core.dll")) { $ScriptDir }
             elseif (Test-Path (Join-Path (Split-Path -Parent $ScriptDir) "dist\modules\var_mod\var_core.dll")) { Join-Path (Split-Path -Parent $ScriptDir) "dist" }
             else { $ScriptDir }

$SrcLua = Join-Path $RepoRoot "modules\var_mod.lua"
$SrcDll = Join-Path $RepoRoot "modules\var_mod\var_core.dll"
$ModuleLine = 'lua.module = "var_mod.lua"'
$Latin1 = [System.Text.Encoding]::GetEncoding(28591)

function Resolve-SiderDir([string]$hint) {
    if (-not [string]::IsNullOrWhiteSpace($hint)) {
        $cleanHint = $hint.Trim().Trim('"').Trim("'")
        if (-not (Test-Path $cleanHint -PathType Container)) {
            throw "Pasta Sider informada nao existe: $cleanHint"
        }
        $hasSider = (Test-Path (Join-Path $cleanHint "sider.dll")) -or (Test-Path (Join-Path $cleanHint "sider.ini"))
        if (-not $hasSider) {
            throw "sider.dll ou sider.ini nao encontrado em: $cleanHint"
        }
        return (Resolve-Path $cleanHint).Path
    }

    $candidates = @($ScriptDir, (Split-Path -Parent $ScriptDir))
    # Steam libraries on any drive (default install and extra libraries), vanilla Sider and BMPES
    $steamRoots = @("C:\Program Files (x86)\Steam")
    foreach ($drv in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
        $steamRoots += (Join-Path $drv.Root "SteamLibrary")
    }
    foreach ($root in $steamRoots) {
        $candidates += (Join-Path $root "steamapps\common\eFootball PES 2021\sider")
        $candidates += (Join-Path $root "steamapps\common\BMPES")
    }
    foreach ($c in $candidates) {
        if ($c -and (Test-Path (Join-Path $c "sider.dll"))) {
            return (Resolve-Path $c).Path
        }
    }

    Write-Host ""
    Write-Host "Pasta do Sider nao detectada automaticamente."
    $manual = (Read-Host "Digite o caminho completo da pasta onde fica o sider.dll").Trim().Trim('"').Trim("'")
    if (-not $manual -or -not (Test-Path (Join-Path $manual "sider.dll"))) {
        throw "sider.dll nao encontrado em: $manual"
    }
    return (Resolve-Path $manual).Path
}

function Get-TargetInis([string]$dir) {
    $inis = @()
    foreach ($name in @("sider.ini", "sider-normal.ini", "sider-capacidade.ini")) {
        $p = Join-Path $dir $name
        if (Test-Path $p) { $inis += $p }
    }
    return $inis
}

function Backup-Ini([string]$iniPath) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $bak = "$iniPath.antes-var-mod-$stamp.bak"
    Copy-Item -Force $iniPath $bak
    Write-Host "[backup] Criado: $bak"
}

function Set-IniModuleState([string]$iniPath, [bool]$enable) {
    $lines = [System.IO.File]::ReadAllLines($iniPath, $Latin1)
    $list = [System.Collections.Generic.List[string]]::new()
    $found = $false
    $lastLuaIdx = -1

    for ($i = 0; $i -lt $lines.Length; $i++) {
        $ln = $lines[$i]
        if ($ln -match '^\s*;*\s*lua\.module\s*=\s*"var_mod\.lua"(?:\s*;.*)?\s*$') {
            if ($found) {
                continue # Deduplicate any repeated var_mod.lua entries
            }
            $found = $true
            if ($enable) {
                $list.Add($ModuleLine)
            } else {
                $list.Add(";" + $ModuleLine)
            }
            $lastLuaIdx = $list.Count - 1
        } elseif ($enable -and $ln -match '^\s*;?\s*luajit\.ext\.enabled\s*=\s*0\s*$') {
            $list.Add("luajit.ext.enabled = 1")
        } else {
            $list.Add($ln)
            if ($ln -match '^\s*lua\.module\s*=') {
                $lastLuaIdx = $list.Count - 1
            }
        }
    }

    if ($enable -and -not $found) {
        if ($lastLuaIdx -ge 0) {
            $list.Insert($lastLuaIdx + 1, $ModuleLine)
        } else {
            $list.Add("")
            $list.Add($ModuleLine)
        }
    }

    $changed = ($list.Count -ne $lines.Length)
    if (-not $changed) {
        for ($i = 0; $i -lt $lines.Length; $i++) {
            if ($list[$i] -ne $lines[$i]) { $changed = $true; break }
        }
    }

    if ($changed) {
        Backup-Ini $iniPath
        [System.IO.File]::WriteAllLines($iniPath, $list.ToArray(), $Latin1)
        Write-Host "[ini] Atualizado ($(if ($enable) { 'ATIVADO' } else { 'DESATIVADO' })): $iniPath"
    } else {
        Write-Host "[ini] Sem alteracoes necessarias: $iniPath"
    }
}

function Remove-IniModule([string]$iniPath) {
    $lines = [System.IO.File]::ReadAllLines($iniPath, $Latin1)
    $list = [System.Collections.Generic.List[string]]::new()
    $removed = $false
    foreach ($ln in $lines) {
        if ($ln -match '^\s*;*\s*lua\.module\s*=\s*"var_mod\.lua"(?:\s*;.*)?\s*$') {
            $removed = $true
            continue
        }
        $list.Add($ln)
    }
    if ($removed) {
        Backup-Ini $iniPath
        [System.IO.File]::WriteAllLines($iniPath, $list.ToArray(), $Latin1)
        Write-Host "[ini] Entrada removida: $iniPath"
    }
}

function Show-Status([string]$dir) {
    $dstLua = Join-Path $dir "modules\var_mod.lua"
    $dstDll = Join-Path $dir "modules\var_mod\var_core.dll"
    Write-Host ""
    Write-Host "=== Status PES 2021 VAR Mod ==="
    Write-Host "Pasta Sider : $dir"
    Write-Host "var_mod.lua : $(if (Test-Path $dstLua) { 'Instalado' } else { 'Ausente' })"
    Write-Host "var_core.dll: $(if (Test-Path $dstDll) { 'Instalado' } else { 'Ausente' })"
    foreach ($ini in (Get-TargetInis $dir)) {
        $raw = [System.IO.File]::ReadAllLines($ini, $Latin1)
        $state = "Nao registrado"
        foreach ($ln in $raw) {
            if ($ln -match '^\s*lua\.module\s*=\s*"var_mod\.lua"(?:\s*;.*)?\s*$') { $state = "ATIVADO"; break }
            if ($ln -match '^\s*;+\s*lua\.module\s*=\s*"var_mod\.lua"(?:\s*;.*)?\s*$') { $state = "DESATIVADO" }
        }
        Write-Host ("{0,-22}: {1}" -f (Split-Path -Leaf $ini), $state)
    }
    Write-Host ""
}

function Do-Install([string]$dir) {
    if (-not (Test-Path $SrcLua) -or -not (Test-Path $SrcDll)) {
        throw "Arquivos de distribuicao nao encontrados ($SrcLua / $SrcDll). Execute native\build.ps1 antes."
    }
    $dstModDir = Join-Path $dir "modules"
    $dstCoreDir = Join-Path $dstModDir "var_mod"
    New-Item -ItemType Directory -Force -Path $dstCoreDir | Out-Null

    Copy-Item -Force $SrcLua (Join-Path $dstModDir "var_mod.lua")
    Copy-Item -Force $SrcDll (Join-Path $dstCoreDir "var_core.dll")
    Write-Host "[copy] var_mod.lua e var_core.dll copiados para $dstModDir"

    $inis = Get-TargetInis $dir
    if ($inis.Count -eq 0) {
        Write-Warning "Nenhum sider.ini encontrado em $dir. Adicione manualmente: $ModuleLine"
    }
    foreach ($ini in $inis) {
        Set-IniModuleState $ini $true
    }
    Show-Status $dir
}

function Do-Toggle([string]$dir, [bool]$enable) {
    $inis = Get-TargetInis $dir
    if ($inis.Count -eq 0) {
        Write-Warning "Nenhum sider.ini encontrado em $dir."
    }
    foreach ($ini in $inis) {
        Set-IniModuleState $ini $enable
    }
    Show-Status $dir
}

function Do-Uninstall([string]$dir) {
    foreach ($ini in (Get-TargetInis $dir)) {
        Remove-IniModule $ini
    }
    $dstLua = Join-Path $dir "modules\var_mod.lua"
    $dstCoreDir = Join-Path $dir "modules\var_mod"
    if (Test-Path $dstLua) { Remove-Item -Force $dstLua }
    if (Test-Path $dstCoreDir) { Remove-Item -Recurse -Force $dstCoreDir }
    Write-Host "[uninstall] Arquivos do VAR Mod removidos de $dir"
    Show-Status $dir
}

$targetDir = Resolve-SiderDir $SiderDir

switch ($Action) {
    "install"   { Do-Install $targetDir }
    "enable"    { Do-Toggle $targetDir $true }
    "disable"   { Do-Toggle $targetDir $false }
    "uninstall" { Do-Uninstall $targetDir }
    "status"    { Show-Status $targetDir }
    "menu" {
        while ($true) {
            Show-Status $targetDir
            Write-Host "1 - Instalar ou atualizar VAR Mod (e ativar no Sider)"
            Write-Host "2 - Ativar VAR Mod no Sider"
            Write-Host "3 - Desativar VAR Mod no Sider (mantem arquivos)"
            Write-Host "4 - Desinstalar completamente"
            Write-Host "0 - Sair"
            $choice = Read-Host "Escolha uma opcao"
            switch ($choice) {
                "1" { Do-Install $targetDir }
                "2" { Do-Toggle $targetDir $true }
                "3" { Do-Toggle $targetDir $false }
                "4" { Do-Uninstall $targetDir }
                "0" { break }
                default { Write-Host "Opcao invalida." }
            }
            if ($choice -eq "0") { break }
        }
    }
}
