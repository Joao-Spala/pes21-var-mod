param(
    [switch]$SkipTests,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$RepoRoot  = Split-Path -Parent $PSScriptRoot
$NativeDir = $PSScriptRoot
$SrcDir    = Join-Path $NativeDir "src"
$ImguiDir  = Join-Path $NativeDir "third_party\imgui"
$TestDir   = Join-Path $NativeDir "tests"
$BuildTmp  = Join-Path $NativeDir "build_tmp"
$DistDir   = Join-Path $RepoRoot "dist"
$DistMod   = Join-Path $DistDir "modules"
$DistCore  = Join-Path $DistMod "var_mod"

function Find-Compiler {
    $cmd = Get-Command "x86_64-w64-mingw32-clang++.exe" -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $candidates = @(
        "C:\Users\$env:USERNAME\AppData\Local\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\*\bin\x86_64-w64-mingw32-clang++.exe",
        "C:\Program Files\LLVM-MinGW\bin\x86_64-w64-mingw32-clang++.exe",
        "C:\msys64\ucrt64\bin\g++.exe"
    )
    foreach ($pat in $candidates) {
        $found = Get-Item $pat -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    throw "Compilador C++ x64 (LLVM-MinGW clang++ / g++) nao encontrado no sistema."
}

if ($Clean -and (Test-Path $BuildTmp)) {
    Remove-Item -Recurse -Force $BuildTmp
}

New-Item -ItemType Directory -Force -Path $BuildTmp | Out-Null
New-Item -ItemType Directory -Force -Path $DistCore | Out-Null

$Compiler = Find-Compiler
Write-Host "[build] Compilador: $Compiler"

$CommonFlags = @(
    "-std=c++20",
    "-O2",
    "-mcx16",
    "-ffunction-sections",
    "-fdata-sections",
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    "-DUNICODE",
    "-D_UNICODE",
    "-DNOMINMAX",
    "-DWIN32_LEAN_AND_MEAN",
    "-I$SrcDir",
    "-I$ImguiDir"
)

$ImguiSources = @(
    (Join-Path $ImguiDir "imgui.cpp"),
    (Join-Path $ImguiDir "imgui_draw.cpp"),
    (Join-Path $ImguiDir "imgui_tables.cpp"),
    (Join-Path $ImguiDir "imgui_widgets.cpp"),
    (Join-Path $ImguiDir "imgui_impl_dx11.cpp")
)

$CoreSources = @(
    (Join-Path $SrcDir "logger.cpp"),
    (Join-Path $SrcDir "i18n.cpp"),
    (Join-Path $SrcDir "aob_scanner.cpp"),
    (Join-Path $SrcDir "inline_hook.cpp"),
    (Join-Path $SrcDir "heap_guard.cpp"),
    (Join-Path $SrcDir "var_state.cpp"),
    (Join-Path $SrcDir "match_hooks.cpp"),
    (Join-Path $SrcDir "input_manager.cpp"),
    (Join-Path $SrcDir "var_hud.cpp"),
    (Join-Path $SrcDir "var_hud_render.cpp"),
    (Join-Path $SrcDir "dx11_hook.cpp"),
    (Join-Path $SrcDir "update_checker.cpp"),
    (Join-Path $SrcDir "engine_guards.cpp"),
    (Join-Path $SrcDir "dllmain.cpp")
)

function Compile-Object($srcFile, $extraFlags) {
    $baseName = [System.IO.Path]::GetFileNameWithoutExtension($srcFile)
    $objFile  = Join-Path $BuildTmp ($baseName + ".o")
    $needBuild = $true
    if (Test-Path $objFile) {
        $srcTime = (Get-Item $srcFile).LastWriteTimeUtc
        $objTime = (Get-Item $objFile).LastWriteTimeUtc
        # Always rebuild project sources when headers may have changed. imgui objects are cached, but only
        # while they are newer than the imgui sources AND headers (imconfig.h changes IM_ASSERT for all of them)
        if ($srcFile.StartsWith($ImguiDir) -and $objTime -ge $srcTime -and $objTime -ge $ImguiHeadersTime) {
            $needBuild = $false
        }
    }
    if ($needBuild) {
        Write-Host "[cc] $baseName.cpp"
        $argsList = @() + $CommonFlags + $extraFlags + @("-c", $srcFile, "-o", $objFile)
        & $Compiler @argsList
        if ($LASTEXITCODE -ne 0) {
            throw "Erro ao compilar $srcFile"
        }
    }
    return $objFile
}

$ImguiHeadersTime = (Get-ChildItem -LiteralPath $ImguiDir -Filter "*.h" | Measure-Object -Property LastWriteTimeUtc -Maximum).Maximum
# The imgui object cache is also keyed on its compile flags: a flag change rebuilds those objects
$ImguiFlagsKey = "-O2 -Os -Wno-deprecated-declarations"
$ImguiFlagsStamp = Join-Path $BuildTmp "imgui_flags.txt"
if (-not (Test-Path $ImguiFlagsStamp) -or ((Get-Content -LiteralPath $ImguiFlagsStamp -Raw).Trim() -ne $ImguiFlagsKey)) {
    Get-ChildItem -LiteralPath $BuildTmp -Filter "imgui*.o" -ErrorAction SilentlyContinue | Remove-Item -Force
    Set-Content -LiteralPath $ImguiFlagsStamp -Value $ImguiFlagsKey -Encoding ascii
}
$ImguiObjs = @()
foreach ($f in $ImguiSources) {
    # Size over speed for Dear ImGui: same HUD frame time measured (62 us), 52 KB smaller DLL
    $ImguiObjs += Compile-Object $f @("-Wno-deprecated-declarations", "-Os")
}

$CoreObjs = @()
foreach ($f in $CoreSources) {
    $CoreObjs += Compile-Object $f @("-Werror=return-type")
}

$OutDll = Join-Path $BuildTmp "var_core.dll"
$DistDll = Join-Path $DistCore "var_core.dll"
Write-Host "[link] $OutDll"
$LinkArgs = @(
    "-shared",
    "-s",
    "-static-libgcc",
    "-static-libstdc++",
    "-Wl,--gc-sections",
    "-Wl,--out-implib,$(Join-Path $BuildTmp 'libvar_core.a')",
    "-o", $OutDll
) + $CoreObjs + $ImguiObjs + @(
    "-luser32",
    "-lwinhttp",
    "-lbcrypt"
)

& $Compiler @LinkArgs
if ($LASTEXITCODE -ne 0) {
    throw "Erro ao linkar var_core.dll"
}

Write-Host "[ok] var_core.dll compilada em: $OutDll"

if (-not $SkipTests) {
    $TestSrc = Join-Path $TestDir "test_var_core.cpp"
    if (Test-Path $TestSrc) {
        $TestExe = Join-Path $BuildTmp "test_var_core.exe"
        Write-Host "[test] Compilando $TestExe"
        $TestCoreObjs = @(
            (Join-Path $BuildTmp "logger.o"),
            (Join-Path $BuildTmp "i18n.o"),
            (Join-Path $BuildTmp "aob_scanner.o"),
            (Join-Path $BuildTmp "inline_hook.o"),
            (Join-Path $BuildTmp "heap_guard.o"),
            (Join-Path $BuildTmp "var_state.o"),
            (Join-Path $BuildTmp "input_manager.o"),
            (Join-Path $BuildTmp "var_hud.o"),
            (Join-Path $BuildTmp "var_hud_render.o"),
            (Join-Path $BuildTmp "update_checker.o"),
            (Join-Path $BuildTmp "engine_guards.o")
        )
        $TestImguiObjs = @(
            (Join-Path $BuildTmp "imgui.o"),
            (Join-Path $BuildTmp "imgui_draw.o"),
            (Join-Path $BuildTmp "imgui_tables.o"),
            (Join-Path $BuildTmp "imgui_widgets.o")
        )
        $TestArgs = @() + $CommonFlags + @(
            "-static-libgcc",
            "-static-libstdc++",
            $TestSrc
        ) + $TestCoreObjs + $TestImguiObjs + @(
            "-luser32",
            "-lwinhttp",
            "-lbcrypt",
            "-lgdi32",
            "-o", $TestExe
        )
        & $Compiler @TestArgs
        if ($LASTEXITCODE -ne 0) {
            throw "Erro ao compilar test_var_core.exe"
        }

        Write-Host "[test] Executando suite de testes..."
        & $TestExe
        if ($LASTEXITCODE -ne 0) {
            throw "Suite de testes falhou (exit code $LASTEXITCODE)"
        }
    }
}

# Stage the distribution only now: dist/ never holds an untested build (the installer copies from it)
Copy-Item -Force $OutDll $DistDll
Copy-Item -Force (Join-Path $RepoRoot "sider\modules\var_mod.lua") (Join-Path $DistMod "var_mod.lua")
if (Test-Path (Join-Path $RepoRoot "scripts\VAR_Mod_Setup.ps1")) {
    Copy-Item -Force (Join-Path $RepoRoot "scripts\VAR_Mod_Setup.ps1") (Join-Path $DistDir "VAR_Mod_Setup.ps1")
}
if (Test-Path (Join-Path $RepoRoot "scripts\VAR_Mod_Setup.bat")) {
    Copy-Item -Force (Join-Path $RepoRoot "scripts\VAR_Mod_Setup.bat") (Join-Path $DistDir "VAR_Mod_Setup.bat")
}
Write-Host "[ok] Distribuicao pronta em: $DistDll"
