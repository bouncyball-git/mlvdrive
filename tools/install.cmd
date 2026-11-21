@echo off
rem Installs MLV Drive for the current user (see install.ps1) and starts it.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Start
pause
