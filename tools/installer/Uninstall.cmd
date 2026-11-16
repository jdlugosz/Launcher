@echo off
rem Detect the caller mode before disabling expansion; paths may contain literal !.
if "!"=="" (
  setlocal DisableDelayedExpansion
  set "LauncherInheritedDelayed=1"
) else (
  setlocal DisableDelayedExpansion
  set "LauncherInheritedDelayed=0"
)
set "LauncherMaintenanceDirectory=%~1"
if not defined LauncherMaintenanceDirectory set "LauncherMaintenanceDirectory=%~dp0."
for %%D in ("%LauncherMaintenanceDirectory%\.") do set "LauncherMaintenanceDirectory=%%~fD"
set "LauncherMaintenanceScript=%~dp0Maintenance.ps1"
set "LauncherMaintenanceScope=%~2"
if not exist "%~dp0Maintenance.ps1" (
  echo Launcher maintenance files are missing. Extract the complete download and run setup to repair this installation.
  pause
  exit /b 2
)
set "PSModulePath="
set "LauncherMaintenancePowerShell=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "LauncherMaintenancePowerShell=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%LauncherMaintenancePowerShell%" (
  echo Windows PowerShell could not be found. Launcher was not removed.
  pause
  exit /b 2
)
rem GOTO ends this batch before removal deletes it, restoring the caller mode.
rem Escape bangs only when that caller will expand them; PowerShell resets its module path.
if "%LauncherInheritedDelayed%"=="1" (
  set "LauncherMaintenancePowerShell=%LauncherMaintenancePowerShell:!=^!%"
  set "LauncherMaintenanceScript=%LauncherMaintenanceScript:!=^!%"
  set "LauncherMaintenanceDirectory=%LauncherMaintenanceDirectory:!=^!%"
  if defined LauncherMaintenanceScope set "LauncherMaintenanceScope=%LauncherMaintenanceScope:!=^!%"
)
if not defined LauncherMaintenanceScope (
  (goto) 2>nul & "%LauncherMaintenancePowerShell%" -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File "%LauncherMaintenanceScript%" -Action Uninstall -InstallDirectory "%LauncherMaintenanceDirectory%" -Interactive
) else (
  (goto) 2>nul & "%LauncherMaintenancePowerShell%" -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File "%LauncherMaintenanceScript%" -Action Uninstall -InstallDirectory "%LauncherMaintenanceDirectory%" -Scope "%LauncherMaintenanceScope%" -Interactive
)