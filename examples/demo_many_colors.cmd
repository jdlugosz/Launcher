@echo off
setlocal DisableDelayedExpansion
set "launcher=%~dp0..\Launcher.exe"
if exist "%launcher%" goto run
set "launcher="
for %%F in ("%~dp0..\Launcher-R*.exe") do if exist "%%~fF" (
    if defined launcher goto ambiguous
    set "launcher=%%~fF"
)
if not defined launcher (
    echo A Launcher executable was not found in the parent folder.
    pause
    exit /b 1
)
:run
start "" "%launcher%" --color-demo
exit /b %errorlevel%
:ambiguous
echo More than one versioned Launcher was found. Run the intended EXE with:
echo --color-demo
pause
exit /b 1
