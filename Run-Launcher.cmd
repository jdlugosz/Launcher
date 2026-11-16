@echo off
setlocal
if "%~1"=="" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\run-installed-launcher.ps1"
) else (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\run-installed-launcher.ps1" -ConfigurationPath "%~1"
)
exit /b %errorlevel%
