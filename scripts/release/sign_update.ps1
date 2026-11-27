# PES 2021 VAR Mod - signing of the automatic update (maintainer only, not part of the mod package)
#
# The mod installs a new version by itself only when the release carries update_manifest.txt signed with the
# maintainer's private key (ECDSA P-256 over SHA-256). The public key is compiled into var_core.dll
# (native/src/update_checker.cpp, kUpdatePublicKey). A release without a valid signature is never installed
# automatically, so a compromised GitHub account alone cannot push code to players.
#
#   -NewKey   creates the key pair once. The private key goes to -KeyFile (outside the repository, back it up:
#             without it no later version can be installed automatically) and the public key is printed as the
#             C++ array to paste into update_checker.cpp.
#   default   builds update_manifest.txt and update_manifest.sig for the files in dist\ and copies var_core.dll and
#             var_mod.lua next to them in -OutDir. Upload the four files to the release together with the zip.
#
# Usage: powershell -File sign_update.ps1 -Version 4.5.1 -OutDir <folder>     or     sign_update.ps1 -NewKey

param(
    [switch]$NewKey,
    [string]$Version = "",
    [string]$OutDir = "",
    [string]$KeyFile = (Join-Path $env:USERPROFILE ".pes21-var-mod\update_signing_key.txt"),
    [string]$DistDir = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "dist")
)

$ErrorActionPreference = "Stop"
$Curve = [System.Security.Cryptography.ECCurve+NamedCurves]::nistP256
$Sha256Name = [System.Security.Cryptography.HashAlgorithmName]::SHA256

function ConvertTo-Hex([byte[]]$b) { return -join ($b | ForEach-Object { $_.ToString("x2") }) }
function ConvertFrom-Hex([string]$h) {
    $h = $h.Trim()
    $out = New-Object byte[] ($h.Length / 2)
    for ($i = 0; $i -lt $out.Length; $i++) { $out[$i] = [Convert]::ToByte($h.Substring($i * 2, 2), 16) }
    return $out
}

function New-PublicEcdsa([byte[]]$x, [byte[]]$y) {
    $p = New-Object System.Security.Cryptography.ECParameters
    $p.Curve = $Curve
    $q = New-Object System.Security.Cryptography.ECPoint
    $q.X = $x
    $q.Y = $y
    $p.Q = $q
    $k = [System.Security.Cryptography.ECDsa]::Create()
    $k.ImportParameters($p)
    return $k
}

function Write-PublicKeyArray([byte[]]$x, [byte[]]$y) {
    $all = [byte[]]($x + $y)
    $lines = @()
    for ($i = 0; $i -lt 64; $i += 16) {
        $lines += "    " + ((0..15 | ForEach-Object { "0x" + $all[$i + $_].ToString("X2") }) -join ", ") + ","
    }
    Write-Host "constexpr uint8_t kUpdatePublicKey[64] = { // X then Y"
    $lines | ForEach-Object { Write-Host $_ }
    Write-Host "};"
}

if ($NewKey) {
    if (Test-Path -LiteralPath $KeyFile) { throw "A key already exists at $KeyFile. It is never replaced." }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $KeyFile) | Out-Null
    $k = [System.Security.Cryptography.ECDsa]::Create($Curve)
    $p = $k.ExportParameters($true)
    $text = "# PES 2021 VAR Mod update signing key (ECDSA P-256). PRIVATE: never commit or share. Back it up.`n" +
            "d = $(ConvertTo-Hex $p.D)`nx = $(ConvertTo-Hex $p.Q.X)`ny = $(ConvertTo-Hex $p.Q.Y)`n"
    [System.IO.File]::WriteAllText($KeyFile, $text, [System.Text.Encoding]::ASCII)
    Write-Host "[key] Private key written to $KeyFile"
    Write-PublicKeyArray $p.Q.X $p.Q.Y
    return
}

if (-not ($Version -match '^\d+(\.\d+){1,3}$')) { throw "Pass -Version (for example 4.5.1)." }
if (-not $OutDir) { throw "Pass -OutDir." }
if (-not (Test-Path -LiteralPath $KeyFile)) { throw "Signing key not found at $KeyFile." }

$vals = @{}
foreach ($ln in [System.IO.File]::ReadAllLines($KeyFile)) {
    if ($ln -match '^\s*([dxy])\s*=\s*([0-9a-f]{64})\s*$') { $vals[$Matches[1]] = ConvertFrom-Hex $Matches[2] }
}
if ($vals.Count -ne 3) { throw "Malformed key file $KeyFile." }
$priv = New-Object System.Security.Cryptography.ECParameters
$priv.Curve = $Curve
$priv.D = $vals["d"]
$q = New-Object System.Security.Cryptography.ECPoint
$q.X = $vals["x"]
$q.Y = $vals["y"]
$priv.Q = $q
$signer = [System.Security.Cryptography.ECDsa]::Create()
$signer.ImportParameters($priv)

$dll = Join-Path $DistDir "modules\var_mod\var_core.dll"
$lua = Join-Path $DistDir "modules\var_mod.lua"
foreach ($f in @($dll, $lua)) { if (-not (Test-Path -LiteralPath $f)) { throw "Missing $f. Run native\build.ps1 first." } }
# The files must be the version being signed
$luaText = [System.IO.File]::ReadAllText($lua)
if ($luaText -notmatch ('version\s*=\s*"' + [regex]::Escape($Version) + '"')) { throw "var_mod.lua is not version $Version." }
$dllBytes = [System.IO.File]::ReadAllBytes($dll)
if ([System.Text.Encoding]::ASCII.GetString($dllBytes).IndexOf($Version) -lt 0) { throw "var_core.dll does not contain version $Version." }

function Get-Sha([string]$path) { return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
$manifest = "pes21-var-mod update 1`n" +
            "version = $Version`n" +
            "var_core.dll = $(Get-Sha $dll) $((Get-Item -LiteralPath $dll).Length)`n" +
            "var_mod.lua = $(Get-Sha $lua) $((Get-Item -LiteralPath $lua).Length)`n"
$manifestBytes = [System.Text.Encoding]::ASCII.GetBytes($manifest)
$sig = $signer.SignData($manifestBytes, $Sha256Name)
$verifier = New-PublicEcdsa $vals["x"] $vals["y"]
if (-not $verifier.VerifyData($manifestBytes, $sig, $Sha256Name)) { throw "Signature check failed." }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
[System.IO.File]::WriteAllBytes((Join-Path $OutDir "update_manifest.txt"), $manifestBytes)
[System.IO.File]::WriteAllText((Join-Path $OutDir "update_manifest.sig"), (ConvertTo-Hex $sig) + "`n", [System.Text.Encoding]::ASCII)
Copy-Item -Force -LiteralPath $dll -Destination (Join-Path $OutDir "var_core.dll")
Copy-Item -Force -LiteralPath $lua -Destination (Join-Path $OutDir "var_mod.lua")
Write-Host "[sign] update_manifest.txt, update_manifest.sig, var_core.dll and var_mod.lua written to $OutDir"
Write-Host $manifest
