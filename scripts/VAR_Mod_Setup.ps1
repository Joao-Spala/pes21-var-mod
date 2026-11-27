# PES 2021 VAR Mod - setup script (optional: the README also explains the manual installation)
#
# What it does, and nothing else:
#   - finds the Sider folder (the folder with sider.dll), or asks for it
#   - install: copies modules\var_mod.lua and modules\var_mod\var_core.dll into <Sider folder>\modules\
#   - edits sider.ini (and any sider-*.ini variant next to it) to contain:
#         lua.enabled = 1
#         luajit.ext.enabled = 1
#         lua.module = "var_mod.lua"
#     after saving a dated copy of each file next to it (sider.ini.before-var-mod-<date>.bak, never overwritten)
#   - enable: writes the same three sider.ini lines as install, without copying files
#   - disable: comments out the lua.module line (the files stay)
#   - uninstall: removes the lua.module line, modules\var_mod.lua and the modules\var_mod\ folder, after saving
#     your settings as var_mod.ini.before-uninstall-<date>.bak next to sider.ini (lua.enabled and luajit.ext.enabled stay 1)
#   - status: only reads
#   - update (menu option 5): the only option that uses the network. It reads the latest release of
#     github.com/Joao-Spala/pes21-var-mod (one request, nothing about you or your PC is sent) and compares it with
#     the installed version. Only if you answer y it downloads the release zip from that page into a temporary
#     folder, checks its SHA256 against the one GitHub publishes for it (refuses when they differ or none is
#     published), saves the installed var_mod.lua and var_core.dll in <Sider folder>\var_mod-backup-<version>-<date>\
#     and installs the new ones like option 1. The temporary folder is deleted at the end.
# It changes nothing while PES 2021 is open. No registry, no other files.
#
# Usage: VAR_Mod_Setup.bat  (menu)  or  powershell -File VAR_Mod_Setup.ps1 -Action install|enable|disable|uninstall|status|update [-SiderDir <path>]

param(
    [ValidateSet("menu", "install", "enable", "disable", "uninstall", "status", "update")]
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
$ReleaseApi = "https://api.github.com/repos/Joao-Spala/pes21-var-mod/releases/latest"
$DownloadPrefix = "https://github.com/Joao-Spala/pes21-var-mod/releases/download/"
$HttpHeaders = @{ "User-Agent" = "pes21-var-mod-setup"; "Accept" = "application/vnd.github+json" }
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
        throw "Close PES 2021 before changing the VAR Mod installation (install, enable, disable or uninstall)."
    }
}

function Resolve-SiderDir([string]$hint) {
    if (-not [string]::IsNullOrWhiteSpace($hint)) {
        $cleanHint = $hint.Trim().Trim('"').Trim("'")
        if (-not (Test-Path -LiteralPath $cleanHint -PathType Container)) {
            throw "The given Sider folder does not exist: $cleanHint"
        }
        $hasSider = (Test-Path -LiteralPath (Join-Path $cleanHint "sider.dll")) -or (Test-Path -LiteralPath (Join-Path $cleanHint "sider.ini"))
        if (-not $hasSider) {
            throw "sider.dll or sider.ini not found in: $cleanHint"
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
    Write-Host "The Sider folder was not found automatically."
    $manual = (Read-Host "Enter the full path of the folder that contains sider.dll").Trim().Trim('"').Trim("'")
    if (-not $manual -or -not (Test-Path -LiteralPath (Join-Path $manual "sider.dll"))) {
        throw "sider.dll not found in: $manual"
    }
    return (Resolve-Path -LiteralPath $manual).Path
}

function Get-TargetInis([string]$dir) {
    # sider.ini plus the variants a launcher may switch between (sider-<name>.ini). Backups never match.
    $inis = @()
    $main = Join-Path $dir "sider.ini"
    if (Test-Path -LiteralPath $main) { $inis += $main }
    foreach ($f in @(Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^sider-[^\/]+\.ini$' } | Sort-Object Name)) {
        $inis += $f.FullName
    }
    return $inis
}

function Get-FreeBackupPath([string]$prefix) {
    # Never overwrite an earlier backup, even two runs in the same second
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $path = "$prefix-$stamp.bak"
    $n = 2
    while (Test-Path -LiteralPath $path) { $path = "$prefix-$stamp-$n.bak"; $n++ }
    return $path
}

function Backup-Ini([string]$iniPath) {
    $bak = Get-FreeBackupPath "$iniPath.before-var-mod"
    Copy-Item -LiteralPath $iniPath -Destination $bak
    Write-Host "[backup] Created: $bak"
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
            if ($firstLuaIdx -lt 0) { $firstLuaIdx = $list.Count - 1 }
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
        Write-Host "[ini] Updated ($(if ($enable) { 'ENABLED' } else { 'DISABLED' })): $iniPath"
    } else {
        Write-Host "[ini] No changes needed: $iniPath"
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
        Write-Host "[ini] Entry removed: $iniPath"
    }
}

function Show-Status([string]$dir) {
    $dstLua = Join-Path $dir "modules\var_mod.lua"
    $dstDll = Join-Path $dir "modules\var_mod\var_core.dll"
    Write-Host ""
    Write-Host "=== Status PES 2021 VAR Mod ==="
    Write-Host "Sider folder: $dir"
    Write-Host "var_mod.lua : $(if (Test-Path -LiteralPath $dstLua) { 'Installed' } else { 'Missing' })"
    Write-Host "var_core.dll: $(if (Test-Path -LiteralPath $dstDll) { 'Installed' } else { 'Missing' })"
    foreach ($ini in (Get-TargetInis $dir)) {
        $raw = (Read-IniFile $ini).Lines
        $state = "Not registered"
        foreach ($ln in $raw) {
            if ($ln -match '^\s*lua\.module\s*=\s*"var_mod\.lua"(?:\s*;.*)?\s*$') { $state = "ENABLED"; break }
            if ($ln -match '^\s*;+\s*lua\.module\s*=\s*"var_mod\.lua"(?:\s*;.*)?\s*$') { $state = "DISABLED" }
        }
        Write-Host ("{0,-22}: {1}" -f (Split-Path -Leaf $ini), $state)
    }
    Write-Host ""
}

function Do-Install([string]$dir, [string]$srcLua = $SrcLua, [string]$srcDll = $SrcDll) {
    Assert-GameClosed
    if (-not (Test-Path -LiteralPath $srcLua) -or -not (Test-Path -LiteralPath $srcDll)) {
        throw "Package files not found ($srcLua / $srcDll). Run native\build.ps1 first."
    }
    $dstModDir = Join-Path $dir "modules"
    $dstCoreDir = Join-Path $dstModDir "var_mod"
    New-Item -ItemType Directory -Force -Path $dstCoreDir | Out-Null

    # DLL first: a new Lua must never run against an old DLL (missing exports)
    Copy-Item -Force -LiteralPath $srcDll -Destination (Join-Path $dstCoreDir "var_core.dll")
    Copy-Item -Force -LiteralPath $srcLua -Destination (Join-Path $dstModDir "var_mod.lua")
    Write-Host "[copy] var_mod.lua and var_core.dll copied to $dstModDir"

    $inis = Get-TargetInis $dir
    if ($inis.Count -eq 0) {
        Write-Warning "No sider.ini found in $dir. Add this line by hand: $ModuleLine"
    }
    foreach ($ini in $inis) {
        Set-IniModuleState $ini $true
    }
    Show-Status $dir
}

function Do-Toggle([string]$dir, [bool]$enable) {
    Assert-GameClosed
    $inis = Get-TargetInis $dir
    if ($inis.Count -eq 0) {
        Write-Warning "No sider.ini found in $dir."
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
        $kept = Get-FreeBackupPath (Join-Path $dir "var_mod.ini.before-uninstall")
        Copy-Item -LiteralPath $userIni -Destination $kept
        Write-Host "[backup] VAR Mod settings kept in: $kept"
    }
    if (Test-Path -LiteralPath $dstLua) { Remove-Item -Force -LiteralPath $dstLua }
    if (Test-Path -LiteralPath $dstCoreDir) { Remove-Item -Recurse -Force -LiteralPath $dstCoreDir }
    Write-Host "[uninstall] VAR Mod files removed from $dir"
    Show-Status $dir
}

function Get-InstalledVersion([string]$dir) {
    $lua = Join-Path $dir "modules\var_mod.lua"
    if (-not (Test-Path -LiteralPath $lua)) { return $null }
    $m = Select-String -LiteralPath $lua -Pattern 'version\s*=\s*"(\d+(\.\d+){0,3})"' | Select-Object -First 1
    if ($m) { return $m.Matches[0].Groups[1].Value }
    return $null
}

function ConvertTo-ModVersion([string]$text) {
    $parts = @(($text.TrimStart('v', 'V') -split '\.') | ForEach-Object { [int]$_ })
    while ($parts.Count -lt 4) { $parts += 0 }
    return New-Object System.Version $parts[0], $parts[1], $parts[2], $parts[3]
}

function Backup-ModFiles([string]$dir, [string]$version) {
    $lua = Join-Path $dir "modules\var_mod.lua"
    $dll = Join-Path $dir "modules\var_mod\var_core.dll"
    if (-not (Test-Path -LiteralPath $lua) -and -not (Test-Path -LiteralPath $dll)) { return }
    $label = if ($version) { $version } else { "unknown" }
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $bak = Join-Path $dir "var_mod-backup-$label-$stamp"
    $n = 2
    while (Test-Path -LiteralPath $bak) { $bak = Join-Path $dir "var_mod-backup-$label-$stamp-$n"; $n++ }
    New-Item -ItemType Directory -Path $bak | Out-Null
    if (Test-Path -LiteralPath $lua) { Copy-Item -LiteralPath $lua -Destination $bak }
    if (Test-Path -LiteralPath $dll) { Copy-Item -LiteralPath $dll -Destination $bak }
    Write-Host "[backup] Installed files saved in: $bak"
}

function Do-Update([string]$dir) {
    Assert-GameClosed
    $installed = Get-InstalledVersion $dir
    Write-Host ""
    Write-Host "Installed version: $(if ($installed) { $installed } else { 'not installed' })"
    Write-Host "Reading the latest release on GitHub (nothing about you or your PC is sent)..."
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    try {
        $rel = Invoke-RestMethod -Uri $ReleaseApi -Headers $HttpHeaders -TimeoutSec 20 -UseBasicParsing
    } catch {
        throw "GitHub could not be reached ($($_.Exception.Message)). Nothing was changed."
    }
    $tag = [string]$rel.tag_name
    if ($tag -notmatch '^[vV]?\d+(\.\d+){0,3}$') { throw "Unexpected release tag '$tag'. Nothing was changed." }
    $latest = $tag.TrimStart('v', 'V')
    Write-Host "Latest release: $latest"
    if ($installed -and (ConvertTo-ModVersion $latest) -le (ConvertTo-ModVersion $installed)) {
        Write-Host "You already have the latest version."
        return
    }

    $asset = @($rel.assets | Where-Object { $_.name -match '^PES2021_VAR_Mod_v[0-9.]+\.zip$' }) | Select-Object -First 1
    if (-not $asset) { throw "The release $latest has no package zip. Nothing was downloaded." }
    $url = [string]$asset.browser_download_url
    if (-not $url.StartsWith($DownloadPrefix, [System.StringComparison]::Ordinal)) {
        throw "Unexpected download address '$url'. Nothing was downloaded."
    }
    # SHA256 published by GitHub for the asset, else a line of the release notes naming the zip and its hash
    $expected = $null
    if ([string]$asset.digest -match '^sha256:([0-9a-fA-F]{64})$') {
        $expected = $Matches[1]
    } elseif ([string]$rel.body -match ('(?m)' + [regex]::Escape([string]$asset.name) + '\W+([0-9a-fA-F]{64})\b')) {
        $expected = $Matches[1]
    }
    if (-not $expected) { throw "The release publishes no SHA256 for $($asset.name). Nothing was downloaded." }

    $sizeMb = "{0:N1}" -f ([double]$asset.size / 1MB)
    $answer = Read-Host "Download $($asset.name) ($sizeMb MB) from GitHub and install version $latest? (y/N)"
    if ($answer -notmatch '^[yYsS]$') {
        Write-Host "Nothing was downloaded."
        return
    }

    $work = Join-Path ([System.IO.Path]::GetTempPath()) ("var_mod_update_" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $work | Out-Null
    try {
        $zip = Join-Path $work "package.zip"
        $oldProgress = $ProgressPreference
        $ProgressPreference = "SilentlyContinue" # the progress bar of PowerShell 5.1 makes the download much slower
        try {
            Invoke-WebRequest -Uri $url -OutFile $zip -Headers @{ "User-Agent" = "pes21-var-mod-setup" } -TimeoutSec 180 -UseBasicParsing
        } finally {
            $ProgressPreference = $oldProgress
        }
        $actual = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
        if ($actual.ToUpperInvariant() -ne $expected.ToUpperInvariant()) {
            throw "SHA256 of the download ($actual) differs from the published one ($expected). It was discarded, nothing was installed."
        }
        Write-Host "[verify] SHA256 matches the one published for the release: $actual"

        $extract = Join-Path $work "package"
        Expand-Archive -LiteralPath $zip -DestinationPath $extract
        $dll = Get-ChildItem -LiteralPath $extract -Recurse -File -Filter "var_core.dll" |
               Where-Object { $_.Directory.Name -eq "var_mod" -and $_.Directory.Parent.Name -eq "modules" } |
               Select-Object -First 1
        if (-not $dll) { throw "modules\var_mod\var_core.dll is missing from the package. Nothing was installed." }
        $lua = Join-Path $dll.Directory.Parent.FullName "var_mod.lua"
        if (-not (Test-Path -LiteralPath $lua)) { throw "modules\var_mod.lua is missing from the package. Nothing was installed." }

        Assert-GameClosed # the download may have taken a while
        Backup-ModFiles $dir $installed
        Do-Install $dir $lua $dll.FullName
        Write-Host "[update] Version $latest installed. Your settings (var_mod.ini) were kept."
        Write-Host "         The new setup script and the notes of this version are in the release page:"
        Write-Host "         https://github.com/Joao-Spala/pes21-var-mod/releases/latest"
    } finally {
        Remove-Item -Recurse -Force -LiteralPath $work -ErrorAction SilentlyContinue
    }
}

$targetDir = Resolve-SiderDir $SiderDir

switch ($Action) {
    "install"   { Do-Install $targetDir }
    "enable"    { Do-Toggle $targetDir $true }
    "disable"   { Do-Toggle $targetDir $false }
    "uninstall" { Do-Uninstall $targetDir }
    "status"    { Show-Status $targetDir }
    "update"    { Do-Update $targetDir }
    "menu" {
        while ($true) {
            Show-Status $targetDir
            Write-Host "1 - Install or update the VAR Mod (and enable it in Sider)"
            Write-Host "2 - Enable the VAR Mod in Sider"
            Write-Host "3 - Disable the VAR Mod in Sider (keeps the files)"
            Write-Host "4 - Uninstall completely"
            Write-Host "5 - Check for updates (downloads only if you confirm, checks the SHA256)"
            Write-Host "0 - Exit"
            $choice = Read-Host "Choose an option"
            try {
                switch ($choice) {
                    "1" { Do-Install $targetDir }
                    "2" { Do-Toggle $targetDir $true }
                    "3" { Do-Toggle $targetDir $false }
                    "4" { Do-Uninstall $targetDir }
                    "5" { Do-Update $targetDir }
                    "0" { break }
                    default { Write-Host "Invalid option." }
                }
            } catch {
                # An error (for example the game still open) goes back to the menu instead of closing it
                Write-Host ""
                Write-Host "[error] $($_.Exception.Message)"
            }
            if ($choice -eq "0") { break }
        }
    }
}
