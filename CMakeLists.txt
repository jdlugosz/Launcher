cmake_minimum_required(VERSION 3.25)

option(LAUNCHER_USE_STD_MODULE "Use the MSVC C++ standard-library module" ON)
option(LAUNCHER_VERSION_TEST_HOOKS "Build an isolated instrumented version-discovery scenario application" OFF)
option(LAUNCHER_RELEASE_PACKAGE "Package finalized public-release terms instead of prerelease evaluation terms" OFF)
set(LAUNCHER_JMDLIB_CPP_SOURCE_DIR "" CACHE PATH "Override the pinned JMDlib.Cpp source checkout")
if(LAUNCHER_USE_STD_MODULE)
    if(CMAKE_VERSION VERSION_LESS 4.3 OR NOT CMAKE_VERSION VERSION_LESS 4.4)
        message(FATAL_ERROR
            "The standard-module build requires CMake 4.3. Use LAUNCHER_USE_STD_MODULE=OFF "
            "for the header build, or update the experimental import-std gate for this CMake version.")
    endif()
    # This CMake 4.3 gate must precede compiler discovery in project().
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
endif()

project(Launcher VERSION 1.1.0 LANGUAGES CXX RC)

set(LAUNCHER_DISTRIBUTION_KIND "Prerelease" CACHE STRING "Distribution kind: Prerelease, Beta or Special (Release requires LAUNCHER_RELEASE_PACKAGE)")
set_property(CACHE LAUNCHER_DISTRIBUTION_KIND PROPERTY STRINGS Prerelease Beta Special)
set(LAUNCHER_PACKAGE_REVISION "0" CACHE STRING "Reserved fourth version component; zero for ordinary development builds")
if(NOT LAUNCHER_PACKAGE_REVISION MATCHES "^(0|[1-9][0-9]*)$" OR LAUNCHER_PACKAGE_REVISION GREATER 65535)
    message(FATAL_ERROR "LAUNCHER_PACKAGE_REVISION must be an integer from 0 to 65535.")
endif()
if(LAUNCHER_RELEASE_PACKAGE)
    set(LAUNCHER_DISTRIBUTION_KIND "Release")
    if(NOT LAUNCHER_PACKAGE_REVISION EQUAL 0)
        message(FATAL_ERROR "Final release packages require a zero fourth version component.")
    endif()
elseif(NOT LAUNCHER_DISTRIBUTION_KIND MATCHES "^(Prerelease|Beta|Special)$")
    message(FATAL_ERROR "LAUNCHER_DISTRIBUTION_KIND must be Prerelease, Beta or Special; Release requires LAUNCHER_RELEASE_PACKAGE=ON.")
elseif(LAUNCHER_DISTRIBUTION_KIND STREQUAL "Beta" AND LAUNCHER_PACKAGE_REVISION EQUAL 0)
    message(FATAL_ERROR "Beta packages require a numbered version reserved by LauncherPackageBeta.")
endif()
# The source and vcpkg keep three components; numbered packages reserve the
# fourth before resource compilation in their own build tree.
set(LAUNCHER_BINARY_VERSION "${PROJECT_VERSION}.${LAUNCHER_PACKAGE_REVISION}")
set(LAUNCHER_DISPLAY_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
if(LAUNCHER_PACKAGE_REVISION GREATER 0)
    set(LAUNCHER_DISPLAY_VERSION "${LAUNCHER_BINARY_VERSION}")
elseif(NOT PROJECT_VERSION_PATCH EQUAL 0)
    string(APPEND LAUNCHER_DISPLAY_VERSION ".${PROJECT_VERSION_PATCH}")
endif()
set(LAUNCHER_EXECUTABLE_NAME "Launcher-R${LAUNCHER_DISPLAY_VERSION}.exe")
set(LAUNCHER_PRERELEASE 1)
if(LAUNCHER_DISTRIBUTION_KIND STREQUAL "Release")
    set(LAUNCHER_PRERELEASE 0)
    set(LAUNCHER_RELEASE_STATUS "Final release ${LAUNCHER_DISPLAY_VERSION}")
else()
    math(EXPR LAUNCHER_ODD_MINOR "${PROJECT_VERSION_MINOR} % 2")
    if(LAUNCHER_ODD_MINOR)
        math(EXPR LAUNCHER_PREVIOUS_MINOR "${PROJECT_VERSION_MINOR} - 1")
        set(LAUNCHER_RELEASE_STATUS "${LAUNCHER_DISTRIBUTION_KIND} after ${PROJECT_VERSION_MAJOR}.${LAUNCHER_PREVIOUS_MINOR}")
    else()
        set(LAUNCHER_RELEASE_STATUS "${LAUNCHER_DISTRIBUTION_KIND} for release series ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
    endif()
endif()
set(LAUNCHER_GENERATED_RESOURCES "${CMAKE_CURRENT_BINARY_DIR}/generated-resources")
find_program(LAUNCHER_INSTALLER_POWERSHELL NAMES powershell.exe
    PATHS "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0" NO_DEFAULT_PATH REQUIRED)
set(LAUNCHER_ARCHIVE_BUILD_ID "" CACHE STRING "Catalog build ID embedded in the signed Launcher ZIP")
set(LAUNCHER_ARCHIVE_SIGNING_CERTIFICATE "" CACHE FILEPATH "DER public certificate for signed Launcher ZIPs")
set(LAUNCHER_ARCHIVE_ADDITIONAL_SIGNING_CERTIFICATE "" CACHE FILEPATH "Additional DER public certificate for Launcher ZIP recovery")
if(LAUNCHER_ARCHIVE_SIGNING_CERTIFICATE AND NOT LAUNCHER_ARCHIVE_BUILD_ID)
    set(archive_kind "development")
    if(NOT LAUNCHER_DISTRIBUTION_KIND STREQUAL "Prerelease")
        string(TOLOWER "${LAUNCHER_DISTRIBUTION_KIND}" archive_kind)
    endif()
    string(REPLACE "." "-" archive_version "${LAUNCHER_DISPLAY_VERSION}")
    set(LAUNCHER_ARCHIVE_BUILD_ID "launcher-${archive_kind}-${archive_version}")
endif()
function(launcher_validate_archive_signer path)
    set(encode_archive_signer "${CMAKE_CURRENT_SOURCE_DIR}/tools/encode-archive-signer.ps1")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}" "${encode_archive_signer}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath "${LAUNCHER_INSTALLER_POWERSHELL}"
        -NoProfile -ExecutionPolicy Bypass -File "${encode_archive_signer}" -Path "${path}"
        OUTPUT_VARIABLE certificate_hex OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(ARGC GREATER 1)
        set(${ARGV1} "${certificate_hex}" PARENT_SCOPE)
    endif()
endfunction()
if(LAUNCHER_ARCHIVE_BUILD_ID OR LAUNCHER_ARCHIVE_SIGNING_CERTIFICATE OR LAUNCHER_ARCHIVE_ADDITIONAL_SIGNING_CERTIFICATE)
    string(LENGTH "${LAUNCHER_ARCHIVE_BUILD_ID}" archive_build_id_length)
    if(NOT LAUNCHER_ARCHIVE_BUILD_ID MATCHES "^[a-z0-9][a-z0-9_-]*$" OR
       archive_build_id_length GREATER 64 OR NOT EXISTS "${LAUNCHER_ARCHIVE_SIGNING_CERTIFICATE}")
        message(FATAL_ERROR "Signed archive recovery requires a catalog build ID and a DER public certificate.")
    endif()
    launcher_validate_archive_signer("${LAUNCHER_ARCHIVE_SIGNING_CERTIFICATE}")
    file(SHA256 "${LAUNCHER_ARCHIVE_SIGNING_CERTIFICATE}" LAUNCHER_ARCHIVE_SIGNER_SHA256)
    string(TOUPPER "${LAUNCHER_ARCHIVE_SIGNER_SHA256}" LAUNCHER_ARCHIVE_SIGNER_SHA256)
    set(LAUNCHER_ARCHIVE_ADDITIONAL_SIGNER_HEX "")
    if(LAUNCHER_ARCHIVE_ADDITIONAL_SIGNING_CERTIFICATE)
        launcher_validate_archive_signer("${LAUNCHER_ARCHIVE_ADDITIONAL_SIGNING_CERTIFICATE}" LAUNCHER_ARCHIVE_ADDITIONAL_SIGNER_HEX)
    endif()
    configure_file(tools/installer/ArchiveRecovery.ps1.in "${LAUNCHER_GENERATED_RESOURCES}/ArchiveRecovery.ps1" @ONLY)
    configure_file("${LAUNCHER_ARCHIVE_SIGNING_CERTIFICATE}" "${LAUNCHER_GENERATED_RESOURCES}/ArchiveSigner.cer" COPYONLY)
    file(GENERATE OUTPUT "${LAUNCHER_GENERATED_RESOURCES}/Launcher.archive.json" CONTENT
        "{\"SchemaVersion\":1,\"Package\":\"launcher\",\"Build\":\"${LAUNCHER_ARCHIVE_BUILD_ID}\",\"Platform\":\"windows-x64\",\"Version\":\"${LAUNCHER_DISPLAY_VERSION}\"}\n")
endif()
set(LAUNCHER_UPDATER_INFO_URL "https://updates.dlugosz.enterprises/downloads/shared-updater/" CACHE STRING
    "HTTPS information page for the Shared Updater Client")
set(LAUNCHER_UPDATER_SETUP_URL "" CACHE STRING "HTTPS download of signed Shared Updater Client Setup")
set(LAUNCHER_UPDATER_SIGNER_SHA256 "" CACHE STRING "SHA-256 fingerprint of the updater setup signer certificate")
set(updater_https_url "^https://[A-Za-z0-9.-]+(/[A-Za-z0-9._~/%+/-]*)?$")
if(NOT LAUNCHER_UPDATER_INFO_URL MATCHES "${updater_https_url}")
    message(FATAL_ERROR "LAUNCHER_UPDATER_INFO_URL must be an HTTPS URL with a host and optional path.")
endif()
set(LAUNCHER_UPDATER_BOOTSTRAP_ENABLED 0)
if(NOT LAUNCHER_UPDATER_SETUP_URL STREQUAL "" OR NOT LAUNCHER_UPDATER_SIGNER_SHA256 STREQUAL "")
    string(LENGTH "${LAUNCHER_UPDATER_SIGNER_SHA256}" updater_signer_length)
    if(NOT LAUNCHER_UPDATER_SETUP_URL MATCHES "${updater_https_url}" OR
       NOT LAUNCHER_UPDATER_SIGNER_SHA256 MATCHES "^[0-9A-Fa-f]+$" OR NOT updater_signer_length EQUAL 64)
        message(FATAL_ERROR "Updater setup requires an HTTPS URL and a 64-digit signer certificate SHA-256.")
    endif()
    set(LAUNCHER_UPDATER_BOOTSTRAP_ENABLED 1)
endif()
configure_file(tools/installer/UpdaterBootstrap.ps1.in "${LAUNCHER_GENERATED_RESOURCES}/UpdaterBootstrap.ps1" @ONLY)
include(tools/distribution-license.cmake)
launcher_distribution_license("${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_VERSION}" "${LAUNCHER_DISTRIBUTION_KIND}")
configure_file("${LAUNCHER_LICENSE_FILE}" "${LAUNCHER_GENERATED_RESOURCES}/LICENSE.txt" COPYONLY)
set(LAUNCHER_PALETTE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/basic_colors.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/extra_colors.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/colors-light.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/colors-dark.toml")
set(LAUNCHER_EXAMPLE_FILES ${LAUNCHER_PALETTE_FILES}
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/demo_many_colors.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/demo_many_colors.cmd")
find_program(LAUNCHER_POWERSHELL_EXECUTABLE NAMES pwsh.exe powershell.exe REQUIRED)
find_program(LAUNCHER_HTML_POWERSHELL NAMES pwsh.exe REQUIRED)
# Ninja's cmd shell does not apply pwsh's legacy-child module-path cleanup.
set(LAUNCHER_INSTALLER_CHECK "${CMAKE_COMMAND}" -E env --unset=PSModulePath "${LAUNCHER_INSTALLER_POWERSHELL}")
set(LAUNCHER_COMPACT_COLOR_DIRECTORY "${LAUNCHER_GENERATED_RESOURCES}/colors")
set(LAUNCHER_COMPACT_COLOR_FILES "")
foreach(palette_file IN LISTS LAUNCHER_PALETTE_FILES)
    get_filename_component(palette_filename "${palette_file}" NAME)
    set(compact_palette "${LAUNCHER_COMPACT_COLOR_DIRECTORY}/${palette_filename}")
    list(APPEND LAUNCHER_COMPACT_COLOR_FILES "${compact_palette}")
    add_custom_command(
        OUTPUT "${compact_palette}"
        COMMAND "${LAUNCHER_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/compact-color-resources.ps1"
            -InputFiles "${palette_file}"
            -OutputDirectory "${LAUNCHER_COMPACT_COLOR_DIRECTORY}"
        DEPENDS "${palette_file}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/compact-color-resources.ps1"
        COMMENT "Compacting embedded color resource ${palette_filename}"
        VERBATIM)
endforeach()
add_custom_target(LauncherColorResources DEPENDS ${LAUNCHER_COMPACT_COLOR_FILES})
set(LAUNCHER_DEMO_ICON "${LAUNCHER_GENERATED_RESOURCES}/many-colors.svg")
add_custom_command(OUTPUT "${LAUNCHER_DEMO_ICON}"
    COMMAND "${LAUNCHER_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/build-demo-icon.ps1"
        -PalettePath "${CMAKE_CURRENT_SOURCE_DIR}/basic_colors.toml" -OutputPath "${LAUNCHER_DEMO_ICON}"
    DEPENDS basic_colors.toml tools/build-demo-icon.ps1 VERBATIM)
add_custom_target(LauncherDemoIcon DEPENDS "${LAUNCHER_DEMO_ICON}")
set(LAUNCHER_COLORS_RC "${LAUNCHER_GENERATED_RESOURCES}/colors.generated.rc")
configure_file(resources/color_resources.rc.in "${LAUNCHER_COLORS_RC}" @ONLY)
set_source_files_properties("${LAUNCHER_COLORS_RC}" PROPERTIES OBJECT_DEPENDS
    "${LAUNCHER_COMPACT_COLOR_FILES};${LAUNCHER_DEMO_ICON};${CMAKE_CURRENT_SOURCE_DIR}/examples/demo_many_colors.toml;${CMAKE_CURRENT_SOURCE_DIR}/resources/launcher_resource.h")
configure_file(resources/launcher_version.h.in
    "${LAUNCHER_GENERATED_RESOURCES}/launcher_version.h" @ONLY)
configure_file(resources/app.manifest.in
    "${LAUNCHER_GENERATED_RESOURCES}/launcher.generated.manifest" @ONLY)
configure_file(resources/portable-readme.txt.in
    "${LAUNCHER_GENERATED_RESOURCES}/README.txt" @ONLY)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json" LAUNCHER_MANIFEST_JSON)
string(JSON LAUNCHER_MANIFEST_VERSION GET "${LAUNCHER_MANIFEST_JSON}" version-semver)
if(NOT LAUNCHER_MANIFEST_VERSION STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR "vcpkg's project version must match PROJECT_VERSION.")
endif()

set(LAUNCHER_BUILD_INSTRUMENTATION "" CACHE STRING "Describe special instrumentation, such as externally applied code coverage")
if(LAUNCHER_VERSION_TEST_HOOKS)
    set(LAUNCHER_BUILD_INSTRUMENTATION "version-scenarios" CACHE STRING "Describe special instrumentation, such as externally applied code coverage" FORCE)
endif()
find_package(Git QUIET)
# Run even after a commit without reconfiguration. Only resource objects need
# rebuilding for a fresh stamp; C++ translation units do not include it.
add_custom_target(LauncherBuildStamp
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUTPUT=${LAUNCHER_GENERATED_RESOURCES}/$<CONFIG>/launcher_build.h"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DBUILD_CONFIGURATION=$<CONFIG>"
        "-DRELEASE_VERSION=$<IF:$<BOOL:${LAUNCHER_RELEASE_PACKAGE}>,${PROJECT_VERSION},>"
        "-DBUILD_INSTRUMENTATION=${LAUNCHER_BUILD_INSTRUMENTATION}"
        "-DBUILD_FLAGS=${CMAKE_CXX_FLAGS} ${CMAKE_EXE_LINKER_FLAGS} $<$<CONFIG:Debug>:${CMAKE_CXX_FLAGS_DEBUG} ${CMAKE_EXE_LINKER_FLAGS_DEBUG}>$<$<CONFIG:Release>:${CMAKE_CXX_FLAGS_RELEASE} ${CMAKE_EXE_LINKER_FLAGS_RELEASE}>$<$<CONFIG:RelWithDebInfo>:${CMAKE_CXX_FLAGS_RELWITHDEBINFO} ${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO}>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-build-info.cmake"
    BYPRODUCTS "${LAUNCHER_GENERATED_RESOURCES}/$<CONFIG>/launcher_build.h"
    COMMENT "Recording build time, Git revision and build type"
    VERBATIM)
set(LAUNCHER_RESOURCE_CONFIGURATIONS "${CMAKE_CONFIGURATION_TYPES}")
if(NOT CMAKE_CONFIGURATION_TYPES)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
    endif()
    set(LAUNCHER_RESOURCE_CONFIGURATIONS "${CMAKE_BUILD_TYPE}")
endif()
foreach(configuration IN LISTS LAUNCHER_RESOURCE_CONFIGURATIONS)
    set(resource "${LAUNCHER_GENERATED_RESOURCES}/${configuration}/launcher.generated.rc")
    file(WRITE "${resource}" "#include \"launcher_build.h\"\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/resources/launcher.rc\"\n")
    set_source_files_properties("${resource}" PROPERTIES OBJECT_DEPENDS
        "${LAUNCHER_GENERATED_RESOURCES}/${configuration}/launcher_build.h;${LAUNCHER_GENERATED_RESOURCES}/launcher.generated.manifest;${LAUNCHER_GENERATED_RESOURCES}/launcher_version.h;${LAUNCHER_GENERATED_RESOURCES}/UpdaterBootstrap.ps1;${CMAKE_CURRENT_SOURCE_DIR}/resources/launcher.rc;${CMAKE_CURRENT_SOURCE_DIR}/resources/launcher.ico;${CMAKE_CURRENT_SOURCE_DIR}/resources/launcher-recovery.ico;${CMAKE_CURRENT_SOURCE_DIR}/resources/no-action-ding.wav;${CMAKE_CURRENT_SOURCE_DIR}/resources/launcher_resource.h;${CMAKE_CURRENT_SOURCE_DIR}/resources/pending_overlay.rc;${CMAKE_CURRENT_SOURCE_DIR}/resources/pending_overlay_resource.h;${CMAKE_CURRENT_SOURCE_DIR}/resources/pending-blinds-4.svg;${CMAKE_CURRENT_SOURCE_DIR}/resources/pending-blinds-5.svg;${CMAKE_CURRENT_SOURCE_DIR}/resources/pending-blinds-6.svg")
endforeach()
set(LAUNCHER_RC "${LAUNCHER_GENERATED_RESOURCES}/$<CONFIG>/launcher.generated.rc")
function(launcher_resources target)
    target_sources(${target} PRIVATE "${LAUNCHER_RC}")
    target_include_directories(${target} PRIVATE resources "${LAUNCHER_GENERATED_RESOURCES}")
    add_dependencies(${target} LauncherBuildStamp)
endfunction()

if(DEFINED VCPKG_EXECUTABLE AND NOT EXISTS "${VCPKG_EXECUTABLE}")
    message(FATAL_ERROR "The selected VCPKG_ROOT does not contain vcpkg.exe.")
endif()

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Launcher requires Windows and Microsoft's C++ compiler.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Use the x64 Microsoft compiler environment for Launcher.")
endif()

# CMake's generated std-module target must receive these flags too. Target-only
# /utf-8 causes a _UTF8 environment mismatch when the module is imported.
string(APPEND CMAKE_CXX_FLAGS " /utf-8 /Zc:__cplusplus /Zc:preprocessor /permissive-")
# Resource strings use UTF-8 too, independently of the build machine's ANSI page.
string(APPEND CMAKE_RC_FLAGS " /c65001")

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("/std:c++23preview" LAUNCHER_HAS_CXX23_PREVIEW)
if(NOT LAUNCHER_HAS_CXX23_PREVIEW)
    message(FATAL_ERROR "Launcher requires an MSVC toolset supporting /std:c++23preview (14.51 or later).")
endif()
# Explicitly select MSVC's supported C++23 mode, rather than CMake's /std:c++latest fallback.
set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "/std:c++23preview")
set(CMAKE_CXX23_EXTENSION_COMPILE_OPTION "/std:c++23preview")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_SCAN_FOR_MODULES ${LAUNCHER_USE_STD_MODULE})
if(LAUNCHER_USE_STD_MODULE AND NOT 23 IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
    message(FATAL_ERROR
        "Compiler discovery does not report the C++23 std module. When enabling modules "
        "in an existing header build, reconfigure with --fresh to refresh compiler metadata. "
        "If the selected toolchain still lacks the module, use LAUNCHER_USE_STD_MODULE=OFF.")
endif()

set(jmdlib_source "${LAUNCHER_JMDLIB_CPP_SOURCE_DIR}")
if(jmdlib_source STREQUAL "")
    set(jmdlib_source "${CMAKE_CURRENT_SOURCE_DIR}/vendor/JMDlib.Cpp")
endif()
if(NOT EXISTS "${jmdlib_source}/include/jmd/log.hpp")
    message(FATAL_ERROR "Initialize or update vendor/JMDlib.Cpp, or set LAUNCHER_JMDLIB_CPP_SOURCE_DIR to a checkout with jmd.log.")
endif()
include("${jmdlib_source}/cmake/JMDModules.cmake")
set(LAUNCHER_USE_JMD_MODULES ${LAUNCHER_USE_STD_MODULE})

set(LAUNCHER_GENERATED_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/generated")
function(launcher_use_std_module target)
    if(LAUNCHER_USE_STD_MODULE)
        set_property(TARGET ${target} PROPERTY CXX_MODULE_STD ON)
        # Prefer the adapter generated for this compiler and dependency set.
        target_include_directories(${target} BEFORE PRIVATE "${LAUNCHER_GENERATED_INCLUDE}")
        # This dependency also orders Ninja's module scanner and SDK PCH build
        # after the adapter is ready; the stable headers are ordinary includes.
        add_dependencies(${target} LauncherStdHeaderAdapter)
    endif()
endfunction()

add_library(LauncherOptions INTERFACE)
target_compile_definitions(LauncherOptions INTERFACE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    WINVER=0x0A00 _WIN32_WINNT=0x0A00
    LAUNCHER_USE_STD_MODULE=$<BOOL:${LAUNCHER_USE_STD_MODULE}>)
target_compile_options(LauncherOptions INTERFACE
    "$<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/Zi>")
target_link_options(LauncherOptions INTERFACE
    /DEBUG:FULL /DYNAMICBASE /NXCOMPAT
    "$<$<NOT:$<CONFIG:Debug>>:/OPT:REF>"
    "$<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>")

add_library(LauncherResources STATIC src/resource_module.cpp)
target_include_directories(LauncherResources PUBLIC include)
target_link_libraries(LauncherResources PUBLIC LauncherOptions)
launcher_use_std_module(LauncherResources)

find_package(tomlplusplus CONFIG REQUIRED)
add_library(LauncherCore STATIC
    src/config.cpp src/config_text.cpp src/colors.cpp src/color_resources.cpp src/dock.cpp src/config_storage.cpp src/file_lookup.cpp src/settings.cpp src/log.cpp)
target_include_directories(LauncherCore PUBLIC include PRIVATE resources)
# Every executable using the core receives the same embedded stock palettes.
target_sources(LauncherCore INTERFACE "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${LAUNCHER_COLORS_RC}>")
add_dependencies(LauncherCore LauncherColorResources LauncherDemoIcon)
target_link_libraries(LauncherCore PUBLIC LauncherOptions tomlplusplus::tomlplusplus PRIVATE LauncherResources shell32 ole32)
launcher_use_std_module(LauncherCore)
if(LAUNCHER_USE_JMD_MODULES)
    jmd_use_modules(LauncherCore PUBLIC files log)
    target_compile_definitions(LauncherCore PRIVATE LAUNCHER_USE_JMD_MODULES=1)
else()
    jmd_use_log_headers(LauncherCore)
endif()

# Cache stable SDK declarations privately, separately for each native target and
# configuration. Keep project/third-party headers and module imports outside it.
function(launcher_use_windows_pch target)
    target_precompile_headers(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/include/launcher/windows_pch.hpp>")
endfunction()

add_library(WindowsMessaging STATIC src/window_text_reply.cpp)
target_include_directories(WindowsMessaging PUBLIC include)
target_link_libraries(WindowsMessaging PUBLIC LauncherOptions PRIVATE user32)

add_library(LauncherLaunch STATIC src/launch.cpp src/shell_context_menu.cpp src/restart.cpp src/adobe_cc.cpp src/adobe_cc_registry.cpp
    src/target_version_cache.cpp src/target_versions.cpp src/target_version_source.cpp src/version_discovery.cpp src/version_discovery_runtime.cpp src/item_version_catalog.cpp src/filesystem_error_log.cpp src/filesystem_error_notice.cpp src/app_notification.cpp)
target_include_directories(LauncherLaunch PUBLIC include)
target_link_libraries(LauncherLaunch PUBLIC LauncherOptions WindowsMessaging PRIVATE LauncherCore shell32 ole32 advapi32 user32 version windowsapp runtimeobject)
launcher_use_windows_pch(LauncherLaunch)
launcher_use_std_module(LauncherLaunch)
if(LAUNCHER_USE_JMD_MODULES)
    target_include_directories(LauncherLaunch PRIVATE "${jmdlib_source}/include")
    target_compile_definitions(LauncherLaunch PRIVATE LAUNCHER_USE_JMD_MODULES=1)
endif()

find_package(lunasvg CONFIG REQUIRED)
add_library(LauncherImages STATIC src/icon_images.cpp src/svg_images.cpp src/png.cpp src/pe_resources.cpp src/item_presentation.cpp src/pending_overlay.cpp src/menu_icons.cpp)
target_include_directories(LauncherImages PUBLIC include)
target_link_libraries(LauncherImages PUBLIC LauncherOptions PRIVATE LauncherCore LauncherResources LauncherLaunch windowscodecs shell32 shlwapi ole32 user32 xmllite lunasvg::lunasvg)
launcher_use_windows_pch(LauncherImages)
launcher_use_std_module(LauncherImages)

add_library(LauncherAbout STATIC src/about.cpp)
target_include_directories(LauncherAbout PUBLIC include PRIVATE resources)
target_link_libraries(LauncherAbout PUBLIC LauncherOptions PRIVATE version user32 gdi32 comctl32)
launcher_use_windows_pch(LauncherAbout)
launcher_use_std_module(LauncherAbout)
if(LAUNCHER_USE_JMD_MODULES)
    jmd_use_modules(LauncherAbout about)
    target_compile_definitions(LauncherAbout PRIVATE LAUNCHER_USE_JMD_MODULES=1)
endif()

add_library(LauncherMenus STATIC src/popup_menu.cpp)
target_include_directories(LauncherMenus PUBLIC include)
target_link_libraries(LauncherMenus PUBLIC LauncherOptions PRIVATE user32 gdi32 comctl32 uxtheme msimg32)
launcher_use_windows_pch(LauncherMenus)
launcher_use_std_module(LauncherMenus)

add_executable(Launcher WIN32
    src/main.cpp src/desktop_icons.cpp src/adjustments_dialog.cpp src/about_review_status.cpp src/updater_integration.cpp
    src/default_drawer.cpp)
target_compile_definitions(Launcher PRIVATE
    LAUNCHER_UPDATER_BOOTSTRAP_ENABLED=${LAUNCHER_UPDATER_BOOTSTRAP_ENABLED}
    "LAUNCHER_UPDATER_INFO_URL=L\"${LAUNCHER_UPDATER_INFO_URL}\"")
if(LAUNCHER_USE_JMD_MODULES)
    jmd_use_modules(Launcher updater)
    target_compile_definitions(Launcher PRIVATE LAUNCHER_USE_JMD_MODULES=1)
endif()
if(LAUNCHER_VERSION_TEST_HOOKS)
    set_target_properties(Launcher PROPERTIES OUTPUT_NAME LauncherVersionTest)
    target_sources(Launcher PRIVATE src/version_scenarios.cpp)
    target_compile_definitions(Launcher PRIVATE LAUNCHER_VERSION_TEST_HOOKS=1)
    target_link_libraries(Launcher PRIVATE tomlplusplus::tomlplusplus)
endif()
launcher_resources(Launcher)
target_link_libraries(Launcher PRIVATE LauncherCore LauncherLaunch LauncherImages LauncherAbout LauncherMenus
    d2d1 dwrite windowscodecs shell32 ole32 user32 gdi32 advapi32 uuid dwmapi comctl32 winmm bcrypt)
target_include_directories(Launcher PRIVATE resources "${LAUNCHER_GENERATED_RESOURCES}")
launcher_use_windows_pch(Launcher)
launcher_use_std_module(Launcher)
target_link_options(Launcher PRIVATE /MANIFEST:NO)
add_custom_command(TARGET Launcher POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Launcher.toml"
        "$<TARGET_FILE_DIR:Launcher>/Launcher.default.toml"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt"
        "$<TARGET_FILE_DIR:Launcher>/THIRD-PARTY-NOTICES.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${LAUNCHER_GENERATED_RESOURCES}/LICENSE.txt"
        "$<TARGET_FILE_DIR:Launcher>/LICENSE.txt"
    COMMENT "Copying the bundled default beside Launcher.exe")
add_custom_command(TARGET Launcher POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:Launcher>/examples"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${LAUNCHER_EXAMPLE_FILES}
        "$<TARGET_FILE_DIR:Launcher>/examples"
    COMMENT "Copying readable palettes and the many-colors demo into examples")
add_custom_command(TARGET Launcher POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:Launcher>/resources"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${LAUNCHER_DEMO_ICON}" "$<TARGET_FILE_DIR:Launcher>/resources/many-colors.svg")
# The sample may change on an up-to-date build; never copy over user settings.
add_custom_target(LauncherConfiguration ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Launcher.toml"
        "$<TARGET_FILE_DIR:Launcher>/Launcher.default.toml"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt"
        "$<TARGET_FILE_DIR:Launcher>/THIRD-PARTY-NOTICES.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${LAUNCHER_GENERATED_RESOURCES}/LICENSE.txt"
        "$<TARGET_FILE_DIR:Launcher>/LICENSE.txt"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:Launcher>/examples"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${LAUNCHER_EXAMPLE_FILES}
        "$<TARGET_FILE_DIR:Launcher>/examples"
    DEPENDS Launcher
    COMMENT "Updating the bundled default, license, dependency notices and copyable examples")

set(LAUNCHER_CLEAN_TEST_RUNTIME_COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/clean-test-runtime.cmake")
add_custom_target(LauncherCleanTestRuntime
    COMMAND ${LAUNCHER_CLEAN_TEST_RUNTIME_COMMAND}
    COMMENT "Removing generated test runtime settings"
    VERBATIM)

include(CTest)
if(BUILD_TESTING)
    find_package(Catch2 3 CONFIG REQUIRED)
    add_executable(LauncherTests
        tests/layout_tests.cpp tests/dock_tests.cpp tests/config_storage_tests.cpp tests/file_lookup_tests.cpp tests/config_text_tests.cpp
        tests/settings_tests.cpp tests/colors_tests.cpp tests/log_tests.cpp)
    target_link_libraries(LauncherTests PRIVATE LauncherCore LauncherLaunch Catch2::Catch2WithMain)
    launcher_use_std_module(LauncherTests)
    include(Catch)
    if(LAUNCHER_VERSION_TEST_HOOKS)
        add_executable(LauncherVersionScenarioTests tests/version_scenarios_tests.cpp src/version_scenarios.cpp)
        target_compile_definitions(LauncherVersionScenarioTests PRIVATE LAUNCHER_VERSION_TEST_HOOKS=1)
        target_link_libraries(LauncherVersionScenarioTests PRIVATE LauncherCore LauncherLaunch Catch2::Catch2WithMain tomlplusplus::tomlplusplus advapi32 user32)
        launcher_use_windows_pch(LauncherVersionScenarioTests)
        launcher_use_std_module(LauncherVersionScenarioTests)
        catch_discover_tests(LauncherVersionScenarioTests TEST_PREFIX "Launcher.VersionScenario." PROPERTIES FIXTURES_REQUIRED LauncherTestRuntime)
    endif()
    catch_discover_tests(LauncherTests PROPERTIES FIXTURES_REQUIRED LauncherTestRuntime)
    add_executable(LauncherAboutTests tests/about_tests.cpp)
    launcher_resources(LauncherAboutTests)
    target_compile_definitions(LauncherAboutTests PRIVATE "LAUNCHER_TEST_CONFIGURATION=L\"$<CONFIG>\"")
    target_link_libraries(LauncherAboutTests PRIVATE LauncherAbout Catch2::Catch2WithMain)
    target_link_options(LauncherAboutTests PRIVATE /MANIFEST:NO)
    launcher_use_windows_pch(LauncherAboutTests)
    launcher_use_std_module(LauncherAboutTests)
    add_dependencies(LauncherAboutTests Launcher)
    catch_discover_tests(LauncherAboutTests TEST_PREFIX "Launcher.About." PROPERTIES FIXTURES_REQUIRED LauncherTestRuntime)
    add_executable(LauncherMenuTests tests/popup_menu_tests.cpp)
    launcher_resources(LauncherMenuTests)
    target_link_libraries(LauncherMenuTests PRIVATE LauncherMenus Catch2::Catch2WithMain)
    target_link_options(LauncherMenuTests PRIVATE /MANIFEST:NO)
    launcher_use_windows_pch(LauncherMenuTests)
    launcher_use_std_module(LauncherMenuTests)
    catch_discover_tests(LauncherMenuTests TEST_PREFIX "Launcher.Menus." PROPERTIES FIXTURES_REQUIRED LauncherTestRuntime)
    if(Git_FOUND)
        add_test(NAME Launcher.PackageWorkflow COMMAND "${LAUNCHER_POWERSHELL_EXECUTABLE}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/package-workflow.ps1" -CMake "${CMAKE_COMMAND}")
        add_test(NAME Launcher.ReleaseVersionWorkflow COMMAND "${LAUNCHER_POWERSHELL_EXECUTABLE}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/release-version-workflow.ps1")
        add_test(NAME Launcher.BuildStamp COMMAND "${CMAKE_COMMAND}"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/build-stamp-test/$<CONFIG>"
            "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/build-stamp-output/$<CONFIG>/launcher_build.h"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
            "-DGENERATOR=${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-build-info.cmake"
            "-DRELEASE_CHECK=${CMAKE_CURRENT_SOURCE_DIR}/tools/verify-release.cmake"
            "-DLICENSE_SELECTOR=${CMAKE_CURRENT_SOURCE_DIR}/tools/distribution-license.cmake"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build-info.cmake")
    endif()
    add_library(LauncherResourceFixture MODULE tests/resources/fixture.rc)
    set_target_properties(LauncherResourceFixture PROPERTIES LINKER_LANGUAGE CXX)
    target_include_directories(LauncherResourceFixture PRIVATE tests/resources)
    target_link_options(LauncherResourceFixture PRIVATE /NOENTRY /MANIFEST:NO)
    add_executable(LauncherImageTests tests/icon_images_tests.cpp tests/png_tests.cpp tests/resource_images_tests.cpp tests/item_presentation_tests.cpp tests/pending_overlay_tests.cpp tests/menu_icons_tests.cpp resources/pending_overlay.rc)
    target_include_directories(LauncherImageTests PRIVATE resources)
    target_link_libraries(LauncherImageTests PRIVATE LauncherImages Catch2::Catch2WithMain)
    add_dependencies(LauncherImageTests LauncherResourceFixture)
    target_compile_definitions(LauncherImageTests PRIVATE
        "LAUNCHER_RESOURCE_FIXTURE=L\"$<TARGET_FILE:LauncherResourceFixture>\""
        "LAUNCHER_RESOURCE_ASSETS=L\"${CMAKE_CURRENT_SOURCE_DIR}/tests/resources\"")
    launcher_use_windows_pch(LauncherImageTests)
    launcher_use_std_module(LauncherImageTests)
    catch_discover_tests(LauncherImageTests TEST_PREFIX "Launcher.Images." PROPERTIES FIXTURES_REQUIRED LauncherTestRuntime)
    # Explicit documentation export; no GUI, scans or personal configuration.
    add_executable(LauncherOverlayGallery EXCLUDE_FROM_ALL tools/overlay-gallery/export.cpp resources/pending_overlay.rc)
    target_include_directories(LauncherOverlayGallery PRIVATE resources)
    target_link_libraries(LauncherOverlayGallery PRIVATE LauncherImages d2d1)
    launcher_use_windows_pch(LauncherOverlayGallery)
    launcher_use_std_module(LauncherOverlayGallery)
    add_executable(LauncherResourceProbe tests/resource_probe.cpp)
    target_link_libraries(LauncherResourceProbe PRIVATE LauncherImages LauncherCore)
    launcher_use_windows_pch(LauncherResourceProbe)
    launcher_use_std_module(LauncherResourceProbe)
    add_dependencies(LauncherResourceProbe LauncherResourceFixture)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/resource-icons-$<CONFIG>.toml"
        INPUT "${CMAKE_CURRENT_SOURCE_DIR}/tests/resource-icons.toml.in")
    add_test(NAME Launcher.PeResourcesConfig COMMAND LauncherResourceProbe
        "${CMAKE_CURRENT_BINARY_DIR}/resource-icons-$<CONFIG>.toml"
        "${CMAKE_CURRENT_BINARY_DIR}/resource-payloads/$<CONFIG>")
    add_test(NAME Launcher.PeResourcesSmoke COMMAND Launcher --smoke-test
        --config "${CMAKE_CURRENT_BINARY_DIR}/resource-icons-$<CONFIG>.toml")
    set_tests_properties(Launcher.PeResourcesSmoke PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>" TIMEOUT 15 RESOURCE_LOCK LauncherNativeSmoke)
    add_test(NAME Launcher.IconSourcesSmoke COMMAND Launcher --smoke-test
        --config "${CMAKE_CURRENT_SOURCE_DIR}/tests/icon-sources.toml")
    set_tests_properties(Launcher.IconSourcesSmoke PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>"
        TIMEOUT 15
        RESOURCE_LOCK LauncherNativeSmoke)
    add_test(NAME Launcher.SvgIconsSmoke COMMAND Launcher --smoke-test
        --config "${CMAKE_CURRENT_SOURCE_DIR}/tests/svg-icons.toml")
    set_tests_properties(Launcher.SvgIconsSmoke PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>"
        TIMEOUT 15
        RESOURCE_LOCK LauncherNativeSmoke)
    foreach(probe IN ITEMS LauncherLaunchGuiProbe LauncherLaunchConsoleProbe)
        if(probe STREQUAL "LauncherLaunchGuiProbe")
            add_executable(${probe} WIN32 tests/launch_probe.cpp)
            target_compile_definitions(${probe} PRIVATE LAUNCHER_CONSOLE_PROBE=0)
        else()
            add_executable(${probe} tests/launch_probe.cpp)
            target_compile_definitions(${probe} PRIVATE LAUNCHER_CONSOLE_PROBE=1)
        endif()
        launcher_resources(${probe})
        target_include_directories(${probe} PRIVATE include resources "${LAUNCHER_GENERATED_RESOURCES}")
        target_link_libraries(${probe} PRIVATE LauncherOptions shell32)
        target_link_options(${probe} PRIVATE /MANIFEST:NO)
        launcher_use_windows_pch(${probe})
        launcher_use_std_module(${probe})
    endforeach()
    add_dependencies(LauncherImageTests LauncherLaunchGuiProbe)
    add_executable(LauncherLaunchTests tests/launch_native_tests.cpp tests/restart_tests.cpp tests/window_text_reply_tests.cpp tests/adobe_cc_tests.cpp
        tests/adobe_cc_cache_tests.cpp tests/target_versions_tests.cpp tests/target_version_scan_tests.cpp tests/version_discovery_tests.cpp tests/version_discovery_runtime_tests.cpp tests/item_version_catalog_tests.cpp tests/filesystem_error_log_tests.cpp tests/filesystem_error_notice_tests.cpp tests/app_notification_tests.cpp)
    add_executable(LauncherAppNotificationProbe tests/app-notification-probe.cpp)
    target_link_libraries(LauncherAppNotificationProbe PRIVATE LauncherLaunch windowsapp runtimeobject)
    launcher_use_windows_pch(LauncherAppNotificationProbe)
    launcher_use_std_module(LauncherAppNotificationProbe)
    add_executable(LauncherRestartProbe WIN32 tests/restart_probe.cpp)
    target_link_libraries(LauncherRestartProbe PRIVATE LauncherLaunch shell32 user32)
    launcher_use_windows_pch(LauncherRestartProbe)
    launcher_use_std_module(LauncherRestartProbe)
    add_executable(LauncherWindowTextReplyProbe tests/window_text_reply_probe.cpp)
    target_link_libraries(LauncherWindowTextReplyProbe PRIVATE WindowsMessaging shell32)
    launcher_use_windows_pch(LauncherWindowTextReplyProbe)
    launcher_use_std_module(LauncherWindowTextReplyProbe)
    target_include_directories(LauncherLaunchTests PRIVATE include)
    target_link_libraries(LauncherLaunchTests PRIVATE LauncherLaunch LauncherCore Catch2::Catch2WithMain shell32 ole32 advapi32 uuid)
    launcher_use_windows_pch(LauncherLaunchTests)
    launcher_use_std_module(LauncherLaunchTests)
    add_dependencies(LauncherLaunchTests LauncherLaunchGuiProbe LauncherLaunchConsoleProbe LauncherRestartProbe LauncherWindowTextReplyProbe Launcher)
    add_test(NAME Launcher.WindowTextQuery COMMAND ${LAUNCHER_INSTALLER_CHECK} -NoProfile -STA -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/window-text-query.ps1"
        -Probe "$<TARGET_FILE:LauncherWindowTextReplyProbe>")
    set_tests_properties(Launcher.WindowTextQuery PROPERTIES TIMEOUT 30)
    # Explicit desktop acceptance helper; opens only uniquely owned fixture UI.
    # Keep it outside routine CTest so ordinary tests do not open Explorer.
    add_executable(LauncherExactFileUiProbe tests/exact-file-ui-probe.cpp)
    target_link_libraries(LauncherExactFileUiProbe PRIVATE LauncherLaunch uuid)
    launcher_use_windows_pch(LauncherExactFileUiProbe)
    launcher_use_std_module(LauncherExactFileUiProbe)
    catch_discover_tests(LauncherLaunchTests TEST_PREFIX "Launcher.Launch." TEST_SPEC "~[restart-process]~[version-runtime-child]" PROPERTIES FIXTURES_REQUIRED LauncherTestRuntime)
    add_test(NAME Launcher.RestartHandoffNative COMMAND LauncherLaunchTests "[restart-process]")
    set_tests_properties(Launcher.RestartHandoffNative PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>"
        TIMEOUT 45
        RESOURCE_LOCK LauncherNativeSmoke)
    add_test(NAME Launcher.DrawingSmoke COMMAND Launcher --smoke-test
        --config "${CMAKE_CURRENT_SOURCE_DIR}/Launcher.toml")
    set_tests_properties(Launcher.DrawingSmoke PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>"
        TIMEOUT 10)
    add_test(NAME Launcher.ColorDemoSmoke COMMAND Launcher --smoke-test --color-demo)
    set_tests_properties(Launcher.ColorDemoSmoke PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>"
        TIMEOUT 10
        RESOURCE_LOCK LauncherNativeSmoke)
    add_test(NAME Launcher.HoverSmoke COMMAND Launcher --smoke-test
        --config "${CMAKE_CURRENT_SOURCE_DIR}/tests/hover/Launcher.toml")
    set_tests_properties(Launcher.HoverSmoke PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>"
        TIMEOUT 10
        RESOURCE_LOCK LauncherNativeSmoke)
    add_test(NAME Launcher.TabSelectionSmoke COMMAND Launcher --smoke-test
        --config "${CMAKE_CURRENT_SOURCE_DIR}/tests/tab-selection.toml")
    set_tests_properties(Launcher.TabSelectionSmoke PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>"
        TIMEOUT 10)
    add_test(NAME Launcher.ShiftTargetSmoke COMMAND Launcher --smoke-test
        --config "${CMAKE_CURRENT_SOURCE_DIR}/tests/shift-target.toml")
    set_tests_properties(Launcher.ShiftTargetSmoke PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>"
        TIMEOUT 15)
    add_test(NAME Launcher.OptionalLabelsSmoke COMMAND Launcher --smoke-test
        --config "${CMAKE_CURRENT_SOURCE_DIR}/tests/optional-labels.toml")
    set_tests_properties(Launcher.OptionalLabelsSmoke PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>"
        TIMEOUT 30
        ENVIRONMENT "LAUNCHER_LABEL_SMOKE_DIR=$<TARGET_FILE_DIR:LauncherLaunchGuiProbe>;LAUNCHER_LABEL_SMOKE_EXE=$<TARGET_FILE:LauncherLaunchGuiProbe>")
    add_test(NAME Launcher.VerticalGridSmoke COMMAND Launcher --smoke-test
        --config "${CMAKE_CURRENT_SOURCE_DIR}/tests/vertical-grid-order.toml")
    set_tests_properties(Launcher.VerticalGridSmoke PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>"
        TIMEOUT 10)
    add_test(NAME Launcher.AdobeCCSmoke COMMAND Launcher --smoke-test
        --config "${CMAKE_CURRENT_SOURCE_DIR}/examples/Adobe-CC.toml")
    set_tests_properties(Launcher.AdobeCCSmoke PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>"
        TIMEOUT 30)
    add_test(NAME Launcher.TargetVersionsSmoke COMMAND "${LAUNCHER_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/target-versions-smoke.ps1"
        -Launcher "$<TARGET_FILE:Launcher>" -Probe "$<TARGET_FILE:LauncherLaunchGuiProbe>"
        -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/target-versions-smoke")
    set_tests_properties(Launcher.TargetVersionsSmoke PROPERTIES TIMEOUT 30)
    add_test(NAME Launcher.TabStackingSmoke COMMAND Launcher --smoke-test
        --config "${CMAKE_CURRENT_SOURCE_DIR}/tests/tab-stacking.toml")
    set_tests_properties(Launcher.TabStackingSmoke PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>"
        TIMEOUT 10)
    add_test(NAME Launcher.PrismDescendersSmoke COMMAND Launcher --smoke-test
        --config "${CMAKE_CURRENT_SOURCE_DIR}/tests/prism-descenders.toml")
    set_tests_properties(Launcher.PrismDescendersSmoke PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>"
        TIMEOUT 10)
    add_test(NAME Launcher.ThicknessFittingSmoke COMMAND Launcher --smoke-test
        --config "${CMAKE_CURRENT_SOURCE_DIR}/tests/thickness-fitting.toml")
    set_tests_properties(Launcher.ThicknessFittingSmoke PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>"
        TIMEOUT 10)
    foreach(edge IN ITEMS left right top bottom)
        if(edge STREQUAL "right")
            set(orientation_fixture "prism-reversed-right.toml")
        else()
            set(orientation_fixture "prism-reversed-left.toml")
        endif()
        add_test(NAME Launcher.Reversed-${edge} COMMAND Launcher --smoke-test --preview-edge ${edge}
            --config "${CMAKE_CURRENT_SOURCE_DIR}/tests/${orientation_fixture}")
        set_tests_properties(Launcher.Reversed-${edge} PROPERTIES
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>"
            TIMEOUT 10
            RESOURCE_LOCK LauncherNativeSmoke)
    endforeach()
    add_test(NAME Launcher.InvalidPreviewEdge COMMAND Launcher --smoke-test --preview-edge diagonal)
    set_tests_properties(Launcher.InvalidPreviewEdge PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>"
        WILL_FAIL TRUE
        TIMEOUT 10)
    find_program(LAUNCHER_TEST_POWERSHELL NAMES powershell.exe REQUIRED)
    if(LAUNCHER_VERSION_TEST_HOOKS)
        add_test(NAME Launcher.VersionScenario.Native COMMAND "${LAUNCHER_TEST_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/version-scenarios-native.ps1"
            -Launcher "$<TARGET_FILE:Launcher>"
            -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/version-scenarios-native/$<CONFIG>")
        set_tests_properties(Launcher.VersionScenario.Native PROPERTIES TIMEOUT 120 RESOURCE_LOCK LauncherVersionScenarioDesktop)
        add_test(NAME Launcher.VersionScenario.DefaultVersion COMMAND "${LAUNCHER_TEST_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/version-menus-native.ps1"
            -Launcher "$<TARGET_FILE:Launcher>" -Mode Default
            -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/version-default-native/$<CONFIG>")
        set_tests_properties(Launcher.VersionScenario.DefaultVersion PROPERTIES TIMEOUT 120 RESOURCE_LOCK LauncherVersionScenarioDesktop)
    endif()
    add_test(NAME Launcher.InstallLayout COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath "${LAUNCHER_TEST_POWERSHELL}"
        -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/install-layout.ps1"
        -SourceDirectory "$<TARGET_FILE_DIR:Launcher>"
        -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/install-layout-tests/$<CONFIG>")
    set_tests_properties(Launcher.InstallLayout PROPERTIES TIMEOUT 60 RESOURCE_LOCK LauncherNativeSmoke)
    add_test(NAME Launcher.InstallerRegistration COMMAND ${LAUNCHER_INSTALLER_CHECK}
        -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/installer-registration.ps1"
        -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/installer-registration-tests/$<CONFIG>")
    set_tests_properties(Launcher.InstallerRegistration PROPERTIES TIMEOUT 30)
    add_test(NAME Launcher.InstallerConfiguration COMMAND ${LAUNCHER_INSTALLER_CHECK}
        -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/installer-configuration.ps1"
        -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/installer-configuration-tests/$<CONFIG>")
    set_tests_properties(Launcher.InstallerConfiguration PROPERTIES TIMEOUT 30)
    add_test(NAME Launcher.InstallerStarterLinks COMMAND ${LAUNCHER_INSTALLER_CHECK}
        -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/installer-starter-links.ps1"
        -ProfileScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/installer/PortableProfile.ps1"
        -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/installer-starter-link-tests/$<CONFIG>")
    set_tests_properties(Launcher.InstallerStarterLinks PROPERTIES TIMEOUT 60)
    add_test(NAME Launcher.PackageEntryPoints COMMAND ${LAUNCHER_INSTALLER_CHECK}
        -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/package-entry-points.ps1"
        -EntryPointScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/zip-entry-points.ps1"
        -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/package-entry-point-tests/$<CONFIG>")
    set_tests_properties(Launcher.PackageEntryPoints PROPERTIES TIMEOUT 30)
    add_test(NAME Launcher.UninstallEntryPoints COMMAND ${LAUNCHER_INSTALLER_CHECK}
        -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/uninstall-entry-points.ps1"
        -InstallerDirectory "${CMAKE_CURRENT_SOURCE_DIR}/tools/installer"
        -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/uninstall-entry-point-tests/$<CONFIG>")
    set_tests_properties(Launcher.UninstallEntryPoints PROPERTIES TIMEOUT 45)
    add_test(NAME Launcher.ColorDemo COMMAND ${LAUNCHER_INSTALLER_CHECK}
        -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/color-demo.ps1"
        -Wrapper "${CMAKE_CURRENT_SOURCE_DIR}/examples/demo_many_colors.cmd"
        -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/color-demo-tests/$<CONFIG>")
    set_tests_properties(Launcher.ColorDemo PROPERTIES TIMEOUT 45)
    add_test(NAME Launcher.HtmlGuides COMMAND "${LAUNCHER_HTML_POWERSHELL}"
        -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/html-guides.ps1"
        -SourceDirectory "${CMAKE_CURRENT_SOURCE_DIR}"
        -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/html-guide-tests/$<CONFIG>")
    set_tests_properties(Launcher.HtmlGuides PROPERTIES TIMEOUT 30)
    add_test(NAME Launcher.DisplayEdges COMMAND "${LAUNCHER_TEST_POWERSHELL}"
        -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/display-edges.ps1"
        -Launcher "$<TARGET_FILE:Launcher>"
        -Fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/tab-selection.toml"
        -ExpectedVersion "${LAUNCHER_DISPLAY_VERSION}"
        -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/display-edge-tests/$<CONFIG>")
    set_tests_properties(Launcher.DisplayEdges PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>"
        TIMEOUT 60)
    # Most native smoke tests share the rotating app log. Keep their evidence separate
    # even when CTest runs the rest of the suite in parallel.
    set_tests_properties(Launcher.DrawingSmoke Launcher.TabSelectionSmoke Launcher.VerticalGridSmoke
        Launcher.TabStackingSmoke Launcher.PrismDescendersSmoke Launcher.ThicknessFittingSmoke Launcher.InvalidPreviewEdge Launcher.AdobeCCSmoke Launcher.OptionalLabelsSmoke PROPERTIES
        RESOURCE_LOCK LauncherNativeSmoke)

    add_test(NAME Launcher.TestRuntimeCleanup COMMAND "${CMAKE_COMMAND}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/clean-test-runtime-tests/$<CONFIG>"
        "-DCLEANUP_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tools/clean-test-runtime.cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/clean-test-runtime.cmake")
    # Static tests and discovered Catch cases both require this fixture. CTest
    # also schedules cleanup for filtered runs and after failed or skipped tests.
    get_property(launcher_static_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${launcher_static_tests} PROPERTIES FIXTURES_REQUIRED LauncherTestRuntime)
    add_test(NAME Launcher.CleanTestRuntime COMMAND ${LAUNCHER_CLEAN_TEST_RUNTIME_COMMAND})
    set_tests_properties(Launcher.CleanTestRuntime PROPERTIES FIXTURES_CLEANUP LauncherTestRuntime)

    # A self-closing native window for interactive full-screen integration checks.
    # It is intentionally separate from CTest so normal tests do not cover a display.
    add_executable(LauncherFullscreenProbe WIN32
        tests/fullscreen_probe.cpp)
    launcher_resources(LauncherFullscreenProbe)
    target_link_libraries(LauncherFullscreenProbe PRIVATE
        LauncherOptions user32 gdi32 shell32 dwmapi)
    target_include_directories(LauncherFullscreenProbe PRIVATE resources include "${LAUNCHER_GENERATED_RESOURCES}")
    launcher_use_windows_pch(LauncherFullscreenProbe)
    launcher_use_std_module(LauncherFullscreenProbe)
    target_link_options(LauncherFullscreenProbe PRIVATE /MANIFEST:NO)

    # Explicit live regression probe; never cover a display in a routine CTest run.
    add_executable(LauncherFullscreenEventsProbe tests/fullscreen_events_probe.cpp)
    launcher_resources(LauncherFullscreenEventsProbe)
    target_link_libraries(LauncherFullscreenEventsProbe PRIVATE LauncherOptions user32 gdi32 shell32 dwmapi)
    target_include_directories(LauncherFullscreenEventsProbe PRIVATE resources include "${LAUNCHER_GENERATED_RESOURCES}")
    launcher_use_windows_pch(LauncherFullscreenEventsProbe)
    launcher_use_std_module(LauncherFullscreenEventsProbe)
    target_link_options(LauncherFullscreenEventsProbe PRIVATE /MANIFEST:NO)
endif()

if(LAUNCHER_USE_STD_MODULE)
    get_filename_component(LAUNCHER_MSVC_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(LAUNCHER_MSVC_TOOLSET "${LAUNCHER_MSVC_BIN}/../../.." ABSOLUTE)
    set(LAUNCHER_STL_INCLUDE "")
    foreach(candidate IN ITEMS
            "${LAUNCHER_MSVC_TOOLSET}/include" "$ENV{VCToolsInstallDir}/include")
        if(EXISTS "${candidate}/yvals_core.h")
            get_filename_component(LAUNCHER_STL_INCLUDE "${candidate}" REALPATH)
            break()
        endif()
    endforeach()
    if(NOT LAUNCHER_STL_INCLUDE)
        message(FATAL_ERROR "Cannot find the selected MSVC STL headers for the module adapter.")
    endif()
    set(LAUNCHER_DEPENDENCY_INCLUDE "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
    if(NOT EXISTS "${LAUNCHER_DEPENDENCY_INCLUDE}/toml++/toml.hpp")
        message(FATAL_ERROR "Cannot find the selected vcpkg dependency headers for the module adapter.")
    endif()
    set(LAUNCHER_ADAPTER_COMMAND
        "${LAUNCHER_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-seenthemall.ps1"
        -StlInclude "${LAUNCHER_STL_INCLUDE}"
        -ProjectRoot "${CMAKE_CURRENT_SOURCE_DIR}"
        -DependencyInclude "${LAUNCHER_DEPENDENCY_INCLUDE}"
        -OutputDirectory "${LAUNCHER_GENERATED_INCLUDE}/launcher"
        -IncludeTests)
    # Establish the initial headers during configuration, before CMake asks the
    # compiler to discover module dependencies. Later source edits use the small
    # build-time rule below, without repeating CMake/vcpkg configuration.
    execute_process(
        COMMAND ${LAUNCHER_ADAPTER_COMMAND}
        RESULT_VARIABLE LAUNCHER_ADAPTER_RESULT
        OUTPUT_VARIABLE LAUNCHER_ADAPTER_OUTPUT
        ERROR_VARIABLE LAUNCHER_ADAPTER_ERROR)
    if(NOT LAUNCHER_ADAPTER_RESULT EQUAL 0)
        message(FATAL_ERROR "Generating the standard-header adapter failed:\n${LAUNCHER_ADAPTER_ERROR}")
    endif()
    string(STRIP "${LAUNCHER_ADAPTER_OUTPUT}" LAUNCHER_ADAPTER_OUTPUT)
    message(STATUS "${LAUNCHER_ADAPTER_OUTPUT}")

    # GLOB's verification catches added/deleted input files. Changes to existing
    # file contents regenerate the adapter at build time. Idempotent header
    # writes preserve unrelated translation units when include usage is stable.
    set(LAUNCHER_ADAPTER_INPUTS "")
    foreach(directory IN ITEMS src include tests)
        file(GLOB_RECURSE LAUNCHER_ADAPTER_DIRECTORY_INPUTS CONFIGURE_DEPENDS LIST_DIRECTORIES false
            "${CMAKE_CURRENT_SOURCE_DIR}/${directory}/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/${directory}/*.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/${directory}/*.hh"
            "${CMAKE_CURRENT_SOURCE_DIR}/${directory}/*.hpp")
        list(APPEND LAUNCHER_ADAPTER_INPUTS ${LAUNCHER_ADAPTER_DIRECTORY_INPUTS})
    endforeach()
    file(GLOB_RECURSE LAUNCHER_ADAPTER_DEPENDENCY_INPUTS CONFIGURE_DEPENDS LIST_DIRECTORIES false
        "${LAUNCHER_DEPENDENCY_INCLUDE}/*")
    list(APPEND LAUNCHER_ADAPTER_INPUTS ${LAUNCHER_ADAPTER_DEPENDENCY_INPUTS})
    set(LAUNCHER_ADAPTER_STAMP "${LAUNCHER_GENERATED_INCLUDE}/launcher/adapter.stamp")
    add_custom_command(
        OUTPUT "${LAUNCHER_ADAPTER_STAMP}"
        BYPRODUCTS
            "${LAUNCHER_GENERATED_INCLUDE}/launcher/std_header_macros.hh"
            "${LAUNCHER_GENERATED_INCLUDE}/launcher/seenthemall.hh"
        COMMAND ${LAUNCHER_ADAPTER_COMMAND}
        COMMAND "${CMAKE_COMMAND}" -E touch "${LAUNCHER_ADAPTER_STAMP}"
        DEPENDS ${LAUNCHER_ADAPTER_INPUTS}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-seenthemall.ps1"
            "${LAUNCHER_STL_INCLUDE}/yvals_core.h"
            "${LAUNCHER_MSVC_TOOLSET}/modules/std.ixx"
        COMMENT "Updating the standard-header adapter"
        VERBATIM)
    add_custom_target(LauncherStdHeaderAdapter DEPENDS "${LAUNCHER_ADAPTER_STAMP}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-seenthemall.ps1"
        "${LAUNCHER_STL_INCLUDE}/yvals_core.h"
        "${LAUNCHER_MSVC_TOOLSET}/modules/std.ixx")
endif()

if(LAUNCHER_VERSION_TEST_HOOKS OR NOT LAUNCHER_BUILD_INSTRUMENTATION STREQUAL "")
    install(CODE "message(FATAL_ERROR \"Instrumented builds cannot be installed or packaged.\")")
endif()
if(NOT LAUNCHER_VERSION_TEST_HOOKS)
    add_subdirectory(tools/drop-target)
endif()

if(NOT LAUNCHER_VERSION_TEST_HOOKS AND LAUNCHER_BUILD_INSTRUMENTATION STREQUAL "")
    set(LAUNCHER_INSTALLED_STARTER "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/Launcher.installed.default.toml")
    add_custom_command(OUTPUT "${LAUNCHER_INSTALLED_STARTER}"
        COMMAND "${LAUNCHER_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/build-installed-starter.ps1"
            -SourcePath "${CMAKE_CURRENT_SOURCE_DIR}/Launcher.toml"
            -OutputPath "${LAUNCHER_INSTALLED_STARTER}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Launcher.toml"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/build-installed-starter.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/installer/PortableProfile.ps1"
        COMMENT "Generating the installed starter with personal demo paths" VERBATIM)
    add_custom_target(LauncherInstalledStarter ALL DEPENDS "${LAUNCHER_INSTALLED_STARTER}")
    add_dependencies(Launcher LauncherInstalledStarter)
    set(LAUNCHER_HTML_GUIDE_FILES "")
    foreach(guide_name IN ITEMS Launcher LauncherDrop Installer SvgExtractor
            advanced-icon-catalog FileLocations Configuration LaunchTargets Appearance)
        list(APPEND LAUNCHER_HTML_GUIDE_FILES "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/docs/${guide_name}.html")
    endforeach()
    add_custom_command(OUTPUT ${LAUNCHER_HTML_GUIDE_FILES}
        COMMAND "${LAUNCHER_HTML_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/build-html-guides.ps1"
            -SourceDirectory "${CMAKE_CURRENT_SOURCE_DIR}"
            -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/docs"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/build-html-guides.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/html-guides.css"
            "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/drop-target/README.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/installer/README.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/svg-extractor.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/advanced-icon-catalog.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/file-locations.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/configuration.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/launch-targets.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/appearance.md"
        COMMENT "Rendering offline HTML guides from Markdown" VERBATIM)
    add_custom_target(LauncherGuides ALL DEPENDS ${LAUNCHER_HTML_GUIDE_FILES})
    add_dependencies(Launcher LauncherGuides)
    set(LAUNCHER_EXTRACTOR_RELEASE_ARGUMENTS "")
    if(LAUNCHER_RELEASE_PACKAGE)
        list(APPEND LAUNCHER_EXTRACTOR_RELEASE_ARGUMENTS -ReleasePackage -ReleaseVersion "${PROJECT_VERSION}")
    endif()
    add_custom_target(LauncherSvgExtractor ALL
        COMMAND "${LAUNCHER_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/build-svg-extractor.ps1"
            -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
            -DistributionKind "${LAUNCHER_DISTRIBUTION_KIND}"
            -CMake "${CMAKE_COMMAND}" -Git "${GIT_EXECUTABLE}"
            ${LAUNCHER_EXTRACTOR_RELEASE_ARGUMENTS}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/docs"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
            "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/docs/Launcher.md"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/tools/drop-target/README.md"
            "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/docs/LauncherDrop.md"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/tools/installer/README.md"
            "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/docs/Installer.md"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/docs/svg-extractor.md"
            "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/docs/SvgExtractor.md"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/docs/advanced-icon-catalog.md"
            "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/docs/advanced-icon-catalog.md"
        BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/LauncherSvgExtractor.exe"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/build-svg-extractor.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/advanced-icon-catalog/AdvancedIconCatalog.cs"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/advanced-icon-catalog/SvgExtractor.cs"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/advanced-icon-catalog/SvgGalleryServer.cs"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/advanced-icon-catalog/svg-extractor.manifest"
        COMMENT "Building the independently versioned SVG Extractor" VERBATIM)
    add_dependencies(Launcher LauncherSvgExtractor)
    install(PROGRAMS "$<TARGET_FILE:Launcher>" DESTINATION . RENAME "${LAUNCHER_EXECUTABLE_NAME}")
    install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/LauncherSvgExtractor.exe" DESTINATION .)
    install(FILES Launcher.toml DESTINATION . RENAME Launcher.default.toml)
    install(FILES "${LAUNCHER_INSTALLED_STARTER}" DESTINATION .)
    install(FILES ${LAUNCHER_EXAMPLE_FILES} DESTINATION examples)
    install(FILES "${LAUNCHER_DEMO_ICON}" DESTINATION resources)
    install(FILES "${LAUNCHER_GENERATED_RESOURCES}/LICENSE.txt" THIRD-PARTY-NOTICES.txt DESTINATION .)
    install(FILES "${LAUNCHER_GENERATED_RESOURCES}/README.txt" DESTINATION .)
    install(FILES tools/installer/Install.cmd tools/installer/Install.ps1
        tools/installer/Discovery.ps1 tools/installer/Presentation.ps1
        tools/installer/Deployment.ps1 tools/installer/Lifecycle.ps1 tools/installer/Processes.ps1
        tools/installer/Worker.ps1 tools/installer/PortableProfile.ps1 tools/installer/WindowTextQuery.cs
        tools/installer/Maintenance.ps1 tools/installer/Uninstall.cmd
        tools/installer/Registration.ps1 DESTINATION .)
    if(LAUNCHER_ARCHIVE_BUILD_ID)
        install(FILES "${LAUNCHER_GENERATED_RESOURCES}/ArchiveRecovery.ps1"
            "${LAUNCHER_GENERATED_RESOURCES}/ArchiveSigner.cer"
            "${LAUNCHER_GENERATED_RESOURCES}/Launcher.archive.json"
            tools/installer/ArchiveRestoreJournal.ps1 DESTINATION .)
    endif()
    install(FILES README.md DESTINATION docs RENAME Launcher.md)
    install(FILES tools/drop-target/README.md DESTINATION docs RENAME LauncherDrop.md)
    install(FILES tools/installer/README.md DESTINATION docs RENAME Installer.md)
    install(FILES docs/svg-extractor.md DESTINATION docs RENAME SvgExtractor.md)
    install(FILES docs/advanced-icon-catalog.md DESTINATION docs)
    install(FILES ${LAUNCHER_HTML_GUIDE_FILES} DESTINATION docs)
endif()

set(LAUNCHER_PACKAGE_NAME "Launcher-R${LAUNCHER_DISPLAY_VERSION}-windows-x64")
if(NOT LAUNCHER_VERSION_TEST_HOOKS AND LAUNCHER_BUILD_INSTRUMENTATION STREQUAL "")
    add_subdirectory(tools/installer)
endif()
set(LAUNCHER_SHIPPING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shipping/$<CONFIG>")
set(LAUNCHER_RELEASE_PACKAGE_ARGUMENTS "")
set(LAUNCHER_ARCHIVE_PACKAGE_ARGUMENTS "")
if(LAUNCHER_ARCHIVE_BUILD_ID)
    list(APPEND LAUNCHER_ARCHIVE_PACKAGE_ARGUMENTS -ArchiveBuildId "${LAUNCHER_ARCHIVE_BUILD_ID}")
endif()
if(LAUNCHER_RELEASE_PACKAGE)
    list(APPEND LAUNCHER_RELEASE_PACKAGE_ARGUMENTS -ReleasePackage -Version "${PROJECT_VERSION}" -Git "${GIT_EXECUTABLE}")
endif()

# A package reserves its version before configuring a separate build directory.
# Never invoke Ninja recursively in the tree that is currently running Ninja.
foreach(package_kind IN ITEMS Prerelease Beta Release)
    add_custom_target(LauncherPackage${package_kind}
        COMMAND "${LAUNCHER_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/package-distribution.ps1"
            -Kind "${package_kind}" -SourceDirectory "${CMAKE_CURRENT_SOURCE_DIR}"
            -CallingBuildDirectory "${CMAKE_CURRENT_BINARY_DIR}" -CMake "${CMAKE_COMMAND}"
            -CTest "${CMAKE_CTEST_COMMAND}" -Git "${GIT_EXECUTABLE}"
        COMMENT "Building and checking an isolated ${package_kind} distribution" VERBATIM)
endforeach()
if(LAUNCHER_VERSION_TEST_HOOKS OR NOT LAUNCHER_BUILD_INSTRUMENTATION STREQUAL "")
    add_custom_target(LauncherStage
        COMMAND "${CMAKE_COMMAND}" -E echo "Instrumented builds cannot be staged or packaged."
        COMMAND "${CMAKE_COMMAND}" -E false VERBATIM)
    add_custom_target(LauncherZip DEPENDS LauncherStage)
    add_custom_target(LauncherShippingCheck DEPENDS LauncherZip)
else()
    add_custom_target(LauncherStage
        COMMAND "${LAUNCHER_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/stage-shipping.ps1" -Action Stage
            -BuildDirectory "${CMAKE_CURRENT_BINARY_DIR}" -Configuration "$<CONFIG>"
            -PackageName "${LAUNCHER_PACKAGE_NAME}" -CMake "${CMAKE_COMMAND}"
            ${LAUNCHER_RELEASE_PACKAGE_ARGUMENTS} ${LAUNCHER_ARCHIVE_PACKAGE_ARGUMENTS}
        DEPENDS LauncherSetup
        COMMENT "Staging a clean extracted Launcher download" VERBATIM)
    add_custom_target(LauncherZip
        COMMAND "${LAUNCHER_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/stage-shipping.ps1" -Action Zip
            -BuildDirectory "${CMAKE_CURRENT_BINARY_DIR}" -Configuration "$<CONFIG>"
            -PackageName "${LAUNCHER_PACKAGE_NAME}" -CMake "${CMAKE_COMMAND}"
            ${LAUNCHER_RELEASE_PACKAGE_ARGUMENTS} ${LAUNCHER_ARCHIVE_PACKAGE_ARGUMENTS}
        DEPENDS LauncherStage
        COMMENT "Creating the Launcher download ZIP from its clean stage" VERBATIM)
    add_custom_target(LauncherShippingCheck
        COMMAND "${LAUNCHER_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/shipping-layout.ps1"
            -StageDirectory "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}"
            -Archive "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}.zip"
            -SourceDirectory "${CMAKE_CURRENT_SOURCE_DIR}" -BuildDirectory "${CMAKE_CURRENT_BINARY_DIR}"
            -LicenseFile "${LAUNCHER_LICENSE_FILE}" ${LAUNCHER_ARCHIVE_PACKAGE_ARGUMENTS}
        COMMAND ${LAUNCHER_INSTALLER_CHECK} -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/package-entry-points.ps1"
            -EntryPointScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/zip-entry-points.ps1"
            -SetupExecutable "$<TARGET_FILE:LauncherSetup>"
            -PackageName "${LAUNCHER_PACKAGE_NAME}"
            -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/package-entry-point-tests/$<CONFIG>"
        COMMAND ${LAUNCHER_INSTALLER_CHECK} -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/color-demo.ps1"
            -Wrapper "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}/examples/demo_many_colors.cmd"
            -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/color-demo-tests/$<CONFIG>"
        COMMAND ${LAUNCHER_INSTALLER_CHECK} -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/uninstall-entry-points.ps1"
            -InstallerDirectory "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}"
            -SetupExecutable "$<TARGET_FILE:LauncherSetup>"
            -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/uninstall-entry-point-tests/$<CONFIG>"
        COMMAND ${LAUNCHER_INSTALLER_CHECK} -NoProfile -STA -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/installer-discovery.ps1"
            -DiscoveryScript "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}/Discovery.ps1"
        COMMAND ${LAUNCHER_INSTALLER_CHECK} -NoProfile -STA -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/installer-deployment.ps1"
            -PackageDirectory "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}"
            -DeploymentScript "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}/Deployment.ps1"
        COMMAND ${LAUNCHER_INSTALLER_CHECK} -NoProfile -STA -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/installer-lifecycle.ps1"
            -PackageDirectory "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}"
            -LifecycleScript "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}/Lifecycle.ps1"
        COMMAND ${LAUNCHER_INSTALLER_CHECK} -NoProfile -STA -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/installer-worker.ps1"
            -PackageDirectory "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}"
            -WorkerScript "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}/Worker.ps1"
        COMMAND ${LAUNCHER_INSTALLER_CHECK} -NoProfile -STA -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/installer-portable-profile.ps1"
            -ProfileScript "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}/PortableProfile.ps1"
        COMMAND ${LAUNCHER_INSTALLER_CHECK} -NoProfile -STA -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/installer-configuration.ps1"
            -InstallerDirectory "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}"
            -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/installer-configuration-tests/$<CONFIG>"
        COMMAND ${LAUNCHER_INSTALLER_CHECK} -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/installer-starter-links.ps1"
            -ProfileScript "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}/PortableProfile.ps1"
            -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/installer-starter-link-tests/$<CONFIG>"
        COMMAND ${LAUNCHER_INSTALLER_CHECK} -NoProfile -STA -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/installer-maintenance.ps1"
            -SourceDirectory "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}"
            -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/installer-maintenance-tests/$<CONFIG>"
        COMMAND ${LAUNCHER_INSTALLER_CHECK} -NoProfile -STA -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/installer-registration.ps1"
            -InstallerDirectory "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}"
            -PackageDirectory "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}"
            -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/installer-registration-tests/$<CONFIG>"
        COMMAND ${LAUNCHER_INSTALLER_CHECK} -NoProfile -STA -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/svg-extractor.ps1"
            -ExecutablePath "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}/LauncherSvgExtractor.exe"
        COMMAND ${LAUNCHER_INSTALLER_CHECK} -NoProfile -STA -ExecutionPolicy Bypass
            -File "${LAUNCHER_SHIPPING_DIRECTORY}/${LAUNCHER_PACKAGE_NAME}/Install.ps1" -ValidateUi
        DEPENDS LauncherZip
        COMMENT "Checking the download, ZIP and isolated installer operations" VERBATIM)
endif()
