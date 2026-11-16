@echo off
setlocal

set "BUILD_PRESET=release"
if not "%~1"=="" set "BUILD_PRESET=%~1"
if /i "%BUILD_PRESET%"=="Debug" set "BUILD_PRESET=debug"
if /i "%BUILD_PRESET%"=="Release" set "BUILD_PRESET=release"
if /i "%BUILD_PRESET%"=="RelWithDebInfo" set "BUILD_PRESET=relwithdebinfo"
if not "%BUILD_PRESET%"=="debug" if not "%BUILD_PRESET%"=="release" if not "%BUILD_PRESET%"=="relwithdebinfo" (
    echo Usage: build.cmd [Debug^|Release^|RelWithDebInfo] [modules^|headers]
    exit /b 2
)
set "CONFIGURE_PRESET=windows-x64"
set "BUILD_CONFIGURATION=Release"
if "%BUILD_PRESET%"=="debug" set "BUILD_CONFIGURATION=Debug"
if "%BUILD_PRESET%"=="relwithdebinfo" set "BUILD_CONFIGURATION=RelWithDebInfo"
set "BUILD_MODE=modules"
if not "%~2"=="" set "BUILD_MODE=%~2"
if /i "%BUILD_MODE%"=="headers" (
    set "CONFIGURE_PRESET=windows-x64-headers"
    set "BUILD_PRESET=%BUILD_PRESET%-headers"
) else if /i not "%BUILD_MODE%"=="modules" (
    echo Usage: build.cmd [Debug^|Release^|RelWithDebInfo] [modules^|headers]
    exit /b 2
)

rem --stop checks the full executable path and waits for graceful shutdown.
for %%D in (windows-x64 windows-x64-headers) do for %%C in (Debug Release RelWithDebInfo) do (
    if exist "%~dp0build\%%D\%%C\Launcher.exe" (
        "%~dp0build\%%D\%%C\Launcher.exe" --stop
        if errorlevel 1 (
            echo Launcher could not close cleanly. The build has been canceled.
            exit /b 1
        )
    )
)

if not defined VCPKG_ROOT (
    for /f "tokens=2,*" %%A in ('reg query "HKCU\Environment" /v VCPKG_ROOT 2^>nul') do set "VCPKG_ROOT=%%B"
)
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
    echo Set VCPKG_ROOT to your installed vcpkg directory before building.
    exit /b 1
)
set "LAUNCHER_VCPKG_ROOT=%VCPKG_ROOT%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Microsoft Visual Studio Build Tools could not be found.
    exit /b 1
)
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"
if not defined VS_INSTALL (
    echo Install Microsoft Visual Studio's Desktop development with C++ workload.
    exit /b 1
)
call "%VS_INSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
rem Visual Studio's vcpkg extension replaces VCPKG_ROOT with its bundled checkout.
set "VCPKG_ROOT=%LAUNCHER_VCPKG_ROOT%"
set "PATH=%VCPKG_ROOT%;%PATH%"

set "CMAKE_EXE=%VS_INSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CTEST_EXE=%VS_INSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
set "NINJA_DIR=%VS_INSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if not exist "%CMAKE_EXE%" (
    for /f "delims=" %%I in ('where cmake.exe 2^>nul') do if not defined FOUND_CMAKE set "FOUND_CMAKE=%%I"
)
if defined FOUND_CMAKE set "CMAKE_EXE=%FOUND_CMAKE%"
if not exist "%CTEST_EXE%" (
    for /f "delims=" %%I in ('where ctest.exe 2^>nul') do if not defined FOUND_CTEST set "FOUND_CTEST=%%I"
)
if defined FOUND_CTEST set "CTEST_EXE=%FOUND_CTEST%"
if not exist "%CMAKE_EXE%" (
    echo CMake was not found. Install the C++ CMake tools component or put CMake on PATH.
    exit /b 1
)
if not exist "%CTEST_EXE%" (
    echo CTest was not found beside CMake or on PATH.
    exit /b 1
)
if exist "%NINJA_DIR%\ninja.exe" set "PATH=%NINJA_DIR%;%PATH%"
where ninja.exe >nul 2>nul
if errorlevel 1 (
    echo Ninja was not found. Install the C++ CMake tools component or put Ninja on PATH.
    exit /b 1
)

pushd "%~dp0"
if errorlevel 1 exit /b 1
"%CMAKE_EXE%" --preset "%CONFIGURE_PRESET%"
if errorlevel 1 goto failed
"%CMAKE_EXE%" --build --preset "%BUILD_PRESET%"
if errorlevel 1 goto failed
"%CTEST_EXE%" --preset "%BUILD_PRESET%"
if errorlevel 1 goto failed
popd
echo Build and configuration/layout tests passed.
if /i "%BUILD_MODE%"=="headers" (
    echo Header fallback: build\windows-x64-headers\%BUILD_CONFIGURATION%\Launcher.exe
) else (
    echo Run Run-Launcher.cmd to show Launcher.
)
exit /b 0

:failed
set "BUILD_RESULT=%errorlevel%"
popd
exit /b %BUILD_RESULT%
