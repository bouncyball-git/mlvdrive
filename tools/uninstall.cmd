@echo off
rem Uninstalls MLV Drive (see install.ps1); the settings are kept.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall
pause
