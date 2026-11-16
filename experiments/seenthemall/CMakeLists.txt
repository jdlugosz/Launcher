cmake_minimum_required(VERSION 4.3)
# This opt-in experiment uses the installed CMake 4.3 experimental gate.
# A different CMake minor version requires its documented gate value.
if(NOT CMAKE_VERSION VERSION_LESS 4.4)
    message(FATAL_ERROR "Update the experimental import-std gate for this CMake version.")
endif()
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
project(SeenThemAllProbe LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This adapter is tested with x64 MSVC on Windows.")
endif()

# These also reach CMake's generated standard-module target. Target-only
# /utf-8 causes a _UTF8 environment mismatch with that target under /WX.
string(APPEND CMAKE_CXX_FLAGS " /utf-8 /Zc:__cplusplus /Zc:preprocessor /permissive-")
set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "/std:c++23preview")
set(CMAKE_CXX23_EXTENSION_COMPILE_OPTION "/std:c++23preview")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
get_filename_component(LAUNCHER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(LAUNCHER_DEPENDENCY_INCLUDE
    "${LAUNCHER_ROOT}/build/windows-x64/vcpkg_installed/x64-windows-static/include"
    CACHE PATH "Installed dependency include directory")
if(NOT EXISTS "${LAUNCHER_DEPENDENCY_INCLUDE}/toml++/toml.hpp")
    message(FATAL_ERROR "Install the Launcher manifest dependencies first.")
endif()
add_executable(SeenThemAllProbe probe.cpp)
target_compile_options(SeenThemAllProbe PRIVATE /W4 /WX /Zi)
target_compile_definitions(SeenThemAllProbe PRIVATE
    NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_include_directories(SeenThemAllProbe PRIVATE "${LAUNCHER_ROOT}/include")
target_include_directories(SeenThemAllProbe SYSTEM PRIVATE "${LAUNCHER_DEPENDENCY_INCLUDE}")
set_property(TARGET SeenThemAllProbe PROPERTY CXX_MODULE_STD ON)
target_precompile_headers(SeenThemAllProbe PRIVATE "${LAUNCHER_ROOT}/include/launcher/windows_pch.hpp")
enable_testing()
add_test(NAME SeenThemAll.Interop COMMAND SeenThemAllProbe)
set_tests_properties(SeenThemAll.Interop PROPERTIES WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
