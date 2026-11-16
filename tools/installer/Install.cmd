@echo off
setlocal
if "%~1"=="" if exist "%~dp0Launcher Setup.exe" goto native_setup
rem Windows PowerShell must use its own modules when called from PowerShell 7.
set "PSModulePath="
set "LauncherSetupPowerShell=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "LauncherSetupPowerShell=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%LauncherSetupPowerShell%" -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0Install.ps1" %*
exit /b %errorlevel%

:native_setup
start "" /wait "%~dp0Launcher Setup.exe"
exit /b %errorlevel%
