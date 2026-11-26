param(
    [ValidateSet("menu", "install", "enable", "disable", "uninstall", "status")]
    [string]$Action = "menu",
    [string]$SiderDir = ""
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = if (Test-Path -LiteralPath (Join-Path $ScriptDir "modules\var_mod\var_core.dll")) { $ScriptDir }
             elseif (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $ScriptDir) "dist\modules\var_mod\var_core.dll")) { Join-Path (Split-Path -Parent $ScriptDir) "dist" }
             else { $ScriptDir }

$SrcLua = Join-Path $RepoRoot "modules\var_mod.lua"
$SrcDll = Join-Path $RepoRoot "modules\var_mod\var_core.dll"
$ModuleLine = 'lua.module = "var_mod.lua"'
$Latin1 = [System.Text.Encoding]::GetEncoding(28591)

# sider.ini is read and written as raw bytes (Latin1 maps every byte 1:1), keeping a UTF-8 BOM, the line
# break style of the file and whether it ends with a line break. Mixed line breaks become the first style found.
function Read-IniFile([string]$path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $bom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $start = if ($bom) { 3 } else { 0 }
    $text = $Latin1.GetString($bytes, $start, $bytes.Length - $start)
    $nl = if ($text.Contains("`r`n")) { "`r`n" } elseif ($text.Contains("`n")) { "`n" } else { "`r`n" }
    $trailing = $text.EndsWith("`n")
    if ($trailing) {
        $text = $text.Substring(0, $text.Length - 1)
        if ($text.EndsWith("`r")) { $text = $text.Substring(0, $text.Length - 1) }
    }
    $lines = if ($text.Length -eq 0) { @() } else { $text -split "`r?`n" }
    return [pscustomobject]@{ Lines = [string[]]$lines; Bom = $bom; NewLine = $nl; Trailing = $trailing }
}

function Write-IniFile([string]$path, $ini, [string[]]$lines) {
    $text = [string]::Join($ini.NewLine, $lines)
    if ($ini.Trailing) { $text += $ini.NewLine }
    $body = $Latin1.GetBytes($text)
    if ($ini.Bom) {
        $out = New-Object byte[] ($body.Length + 3)
        $out[0] = 0xEF; $out[1] = 0xBB; $out[2] = 0xBF
        [System.Array]::Copy($body, 0, $out, 3, $body.Length)
        $body = $out
    }
    [System.IO.File]::WriteAllBytes($path, $body)
}

function Assert-GameClosed {
    # var_core.dll stays mapped (and pinned) while the game runs: copying or deleting it would fail halfway
    if (Get-Process -Name "PES2021" -ErrorAction SilentlyContinue) {
        throw "Feche o PES 2021 antes de instalar, atualizar ou desinstalar o VAR Mod."
    }
}

function Resolve-SiderDir([string]$hint) {
    if (-not [string]::IsNullOrWhiteSpace($hint)) {
        $cleanHint = $hint.Trim().Trim('"').Trim("'")
        if (-not (Test-Path -LiteralPath $cleanHint -PathType Container)) {
            throw "Pasta Sider informada nao existe: $cleanHint"
        }
        $hasSider = (Test-Path -LiteralPath (Join-Path $cleanHint "sider.dll")) -or (Test-Path -LiteralPath (Join-Path $cleanHint "sider.ini"))
        if (-not $hasSider) {
            throw "sider.dll ou sider.ini nao encontrado em: $cleanHint"
        }
        return (Resolve-Path -LiteralPath $cleanHint).Path
    }

    $candidates = @($ScriptDir, (Split-Path -Parent $ScriptDir))
    # Steam libraries on any drive (default install and extra libraries)
    $steamRoots = @("C:\Program Files (x86)\Steam")
    foreach ($drv in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
        $steamRoots += (Join-Path $drv.Root "SteamLibrary")
    }
    # Patch folders first (any game folder of the library that ships its own Sider): with a patch installed, the
    # patch's Sider is the one the game is launched with. The vanilla Sider folder comes after them.
    foreach ($root in $steamRoots) {
        $common = Join-Path $root "steamapps\common"
        if (Test-Path -LiteralPath $common) {
            foreach ($d in (Get-ChildItem -LiteralPath $common -Directory -ErrorAction SilentlyContinue)) {
                if ($d.Name -ne "eFootball PES 2021") { $candidates += $d.FullName }
            }
        }
    }
    foreach ($root in $steamRoots) { $candidates += (Join-Path $root "steamapps\common\eFootball PES 2021\sider") }
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath (Join-Path $c "sider.dll"))) {
            return (Resolve-Path -LiteralPath $c).Path
        }
    }

    Write-Host ""
    Write-Host "Pasta do Sider nao detectada automaticamente."
    $manual = (Read-Host "Digite o caminho completo da pasta onde fica o sider.dll").Trim().Trim('"').Trim("'")
    if (-not $manual -or -not (Test-Path -LiteralPath (Join-Path $manual "sider.dll"))) {
        throw "sider.dll nao encontrado em: $manual"
    }
    return (Resolve-Path -LiteralPath $manual).Path
}

function Get-TargetInis([string]$dir) {
    $inis = @()
    foreach ($name in @("sider.ini", "sider-normal.ini", "sider-capacidade.ini")) {
        $p = Join-Path $dir $name
        if (Test-Path -LiteralPath $p) { $inis += $p }
    }
    return $inis
}

function Backup-Ini([string]$iniPath) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $bak = "$iniPath.antes-var-mod-$stamp.bak"
    Copy-Item -Force -LiteralPath $iniPath -Destination $bak
    Write-Host "[backup] Criado: $bak"
}

function Set-IniModuleState([string]$iniPath, [bool]$enable) {
    $ini = Read-IniFile $iniPath
    $lines = $ini.Lines
    $list = [System.Collections.Generic.List[string]]::new()
    $found = $false
    $lastLuaIdx = -1
    $firstLuaIdx = -1
    $luajitOn = $false
    $luaOn = $false

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
        } elseif ($enable -and $ln -match '^\s*;*\s*luajit\.ext\.enabled\s*=\s*\d+\s*(;.*)?$') {
            # Covers "= 0", a trailing comment and a commented-out line: the module needs LuaJIT FFI
            if (-not $luajitOn) { $list.Add("luajit.ext.enabled = 1") }
            $luajitOn = $true
        } elseif ($enable -and $ln -match '^\s*;*\s*lua\.enabled\s*=\s*\d+\s*(;.*)?$') {
            # Without Lua the module is never loaded and nothing tells the user why
            if (-not $luaOn) { $list.Add("lua.enabled = 1") }
            $luaOn = $true
        } else {
            $list.Add($ln)
            if ($ln -match '^\s*lua\.module\s*=') {
                $lastLuaIdx = $list.Count - 1
                if ($firstLuaIdx -lt 0) { $firstLuaIdx = $list.Count - 1 }
            }
        }
    }

    if ($enable -and -not $luaOn) {
        if ($firstLuaIdx -ge 0) {
            $list.Insert($firstLuaIdx, "lua.enabled = 1")
            $firstLuaIdx++
            if ($lastLuaIdx -ge 0) { $lastLuaIdx++ }
        } else {
            $list.Add("lua.enabled = 1")
        }
    }

    if ($enable -and -not $luajitOn) {
        # Key missing entirely: add it before the first module line (or at the end)
        if ($firstLuaIdx -ge 0) {
            $list.Insert($firstLuaIdx, "luajit.ext.enabled = 1")
            if ($lastLuaIdx -ge $firstLuaIdx) { $lastLuaIdx++ }
        } else {
            $list.Add("luajit.ext.enabled = 1")
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
        Write-IniFile $iniPath $ini $list.ToArray()
        Write-Host "[ini] Atualizado ($(if ($enable) { 'ATIVADO' } else { 'DESATIVADO' })): $iniPath"
    } else {
        Write-Host "[ini] Sem alteracoes necessarias: $iniPath"
    }
}

function Remove-IniModule([string]$iniPath) {
    $ini = Read-IniFile $iniPath
    $lines = $ini.Lines
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
        Write-IniFile $iniPath $ini $list.ToArray()
        Write-Host "[ini] Entrada removida: $iniPath"
    }
}

function Show-Status([string]$dir) {
    $dstLua = Join-Path $dir "modules\var_mod.lua"
    $dstDll = Join-Path $dir "modules\var_mod\var_core.dll"
    Write-Host ""
    Write-Host "=== Status PES 2021 VAR Mod ==="
    Write-Host "Pasta Sider : $dir"
    Write-Host "var_mod.lua : $(if (Test-Path -LiteralPath $dstLua) { 'Instalado' } else { 'Ausente' })"
    Write-Host "var_core.dll: $(if (Test-Path -LiteralPath $dstDll) { 'Instalado' } else { 'Ausente' })"
    foreach ($ini in (Get-TargetInis $dir)) {
        $raw = (Read-IniFile $ini).Lines
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
    Assert-GameClosed
    if (-not (Test-Path -LiteralPath $SrcLua) -or -not (Test-Path -LiteralPath $SrcDll)) {
        throw "Arquivos de distribuicao nao encontrados ($SrcLua / $SrcDll). Execute native\build.ps1 antes."
    }
    $dstModDir = Join-Path $dir "modules"
    $dstCoreDir = Join-Path $dstModDir "var_mod"
    New-Item -ItemType Directory -Force -Path $dstCoreDir | Out-Null

    # DLL first: a new Lua must never run against an old DLL (missing exports)
    Copy-Item -Force -LiteralPath $SrcDll -Destination (Join-Path $dstCoreDir "var_core.dll")
    Copy-Item -Force -LiteralPath $SrcLua -Destination (Join-Path $dstModDir "var_mod.lua")
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
    Assert-GameClosed
    foreach ($ini in (Get-TargetInis $dir)) {
        Remove-IniModule $ini
    }
    $dstLua = Join-Path $dir "modules\var_mod.lua"
    $dstCoreDir = Join-Path $dir "modules\var_mod"
    # The user's settings (shortcuts, language) are kept next to sider.ini before the folder goes away
    $userIni = Join-Path $dstCoreDir "var_mod.ini"
    if (Test-Path -LiteralPath $userIni) {
        $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
        $kept = Join-Path $dir "var_mod.ini.antes-desinstalar-$stamp.bak"
        Copy-Item -Force -LiteralPath $userIni -Destination $kept
        Write-Host "[backup] Configuracao do VAR Mod guardada em: $kept"
    }
    if (Test-Path -LiteralPath $dstLua) { Remove-Item -Force -LiteralPath $dstLua }
    if (Test-Path -LiteralPath $dstCoreDir) { Remove-Item -Recurse -Force -LiteralPath $dstCoreDir }
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
            try {
                switch ($choice) {
                    "1" { Do-Install $targetDir }
                    "2" { Do-Toggle $targetDir $true }
                    "3" { Do-Toggle $targetDir $false }
                    "4" { Do-Uninstall $targetDir }
                    "0" { break }
                    default { Write-Host "Opcao invalida." }
                }
            } catch {
                # An error (for example the game still open) goes back to the menu instead of closing it
                Write-Host ""
                Write-Host "[erro] $($_.Exception.Message)"
            }
            if ($choice -eq "0") { break }
        }
    }
}
