@echo off
REM PES 2021 VAR Mod setup: runs VAR_Mod_Setup.ps1 next to this file (plain text, readable in any editor).
chcp 65001 >nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0VAR_Mod_Setup.ps1" %*
if errorlevel 1 pause
