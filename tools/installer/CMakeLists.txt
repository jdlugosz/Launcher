# Keep CRT-dependent runtime checks enabled for the rest of Launcher.
string(REGEX REPLACE "(^| )[-/]RTC[1csu]+( |$)" " " CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")

configure_file(LauncherSetup.rc.in LauncherSetup.rc @ONLY)
set(setup_inputs)
foreach(name IN ITEMS Worker.ps1 Maintenance.ps1 Discovery.ps1 Deployment.ps1 Lifecycle.ps1
                      Registration.ps1 Processes.ps1 WindowTextQuery.cs)
    list(APPEND setup_inputs "${CMAKE_CURRENT_SOURCE_DIR}/${name}")
endforeach()
if(LAUNCHER_ARCHIVE_BUILD_ID)
    list(APPEND setup_inputs "${LAUNCHER_GENERATED_RESOURCES}/ArchiveRecovery.ps1"
                             "${LAUNCHER_GENERATED_RESOURCES}/ArchiveSigner.cer"
                             "${LAUNCHER_GENERATED_RESOURCES}/Launcher.archive.json"
                             "${CMAKE_CURRENT_SOURCE_DIR}/ArchiveRestoreJournal.ps1")
endif()
set(extractor "${PROJECT_BINARY_DIR}/$<CONFIG>/LauncherSvgExtractor.exe")
set(setup_hashes "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/setup_hashes.h")
add_custom_command(OUTPUT "${setup_hashes}"
    COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${setup_hashes}" "-DINPUTS=${setup_inputs}"
        "-DBINARIES=$<TARGET_FILE:Launcher>;$<TARGET_FILE:LauncherDrop>;${extractor}"
        "-DLAUNCHER_NAME=${LAUNCHER_EXECUTABLE_NAME}" -P "${CMAKE_CURRENT_SOURCE_DIR}/setup-hashes.cmake"
    DEPENDS ${setup_inputs} Launcher LauncherDrop LauncherSvgExtractor "${extractor}" setup-hashes.cmake
    VERBATIM)

add_executable(LauncherSetup WIN32 LauncherSetup.cpp "${CMAKE_CURRENT_BINARY_DIR}/LauncherSetup.rc" "${setup_hashes}")
set_target_properties(LauncherSetup PROPERTIES OUTPUT_NAME "Launcher Setup" CXX_SCAN_FOR_MODULES OFF
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>" MSVC_RUNTIME_CHECKS "")
target_include_directories(LauncherSetup PRIVATE "${PROJECT_SOURCE_DIR}/resources" "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
target_compile_definitions(LauncherSetup PRIVATE "LAUNCHER_PACKAGE_NAME=L\"${LAUNCHER_PACKAGE_NAME}\"")
if(LAUNCHER_ARCHIVE_BUILD_ID)
    target_compile_definitions(LauncherSetup PRIVATE LAUNCHER_SETUP_ARCHIVE)
endif()
target_compile_options(LauncherSetup PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/GS-;/GR-;/Zl>")
target_link_options(LauncherSetup PRIVATE /ENTRY:launcher_setup_entry /NODEFAULTLIB /INCREMENTAL:NO)
target_link_libraries(LauncherSetup PRIVATE kernel32 user32 shell32)
install(PROGRAMS "$<TARGET_FILE:LauncherSetup>" DESTINATION .)
