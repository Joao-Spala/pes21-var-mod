@echo off
setlocal
echo.
echo  Steam - modo offline para PES 2021
echo  ----------------------------------
echo  1. Ligar modo offline
echo  2. Voltar ao modo online
echo  3. Ver status
echo.
choice /c 123 /n /m "Escolha [1-3]: "
if errorlevel 3 set MODO=Status& goto run
if errorlevel 2 set MODO=Online& goto run
set MODO=Offline
:run
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Steam_Modo_Offline.ps1" -Modo %MODO%
echo.
pause
