set(drop_resource "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/drop.generated.rc")
configure_file(drop.manifest "${LAUNCHER_GENERATED_RESOURCES}/drop.generated.manifest" @ONLY)
file(GENERATE OUTPUT "${drop_resource}" CONTENT
    "#include \"launcher_build.h\"\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/drop.rc\"\n")
add_executable(LauncherDrop WIN32 main.cpp "${drop_resource}")
set_target_properties(LauncherDrop PROPERTIES CXX_SCAN_FOR_MODULES ${LAUNCHER_USE_STD_MODULE}
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>")
target_link_libraries(LauncherDrop PRIVATE LauncherCore shell32 ole32 uuid user32 gdi32 comctl32 advapi32 gdiplus dwmapi)
launcher_use_std_module(LauncherDrop)
target_include_directories(LauncherDrop PRIVATE
    "${PROJECT_SOURCE_DIR}/resources" "${LAUNCHER_GENERATED_RESOURCES}"
    "${LAUNCHER_GENERATED_RESOURCES}/$<CONFIG>" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_options(LauncherDrop PRIVATE /MANIFEST:NO)
add_dependencies(LauncherDrop LauncherBuildStamp)
set_property(SOURCE "${drop_resource}" PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/drop.rc;${LAUNCHER_GENERATED_RESOURCES}/drop.generated.manifest;${CMAKE_CURRENT_SOURCE_DIR}/drop.ico;${PROJECT_SOURCE_DIR}/resources/launcher.ico;${LAUNCHER_GENERATED_RESOURCES}/launcher_version.h;${LAUNCHER_GENERATED_RESOURCES}/$<CONFIG>/launcher_build.h")
install(TARGETS LauncherDrop RUNTIME DESTINATION .)

if(BUILD_TESTING)
    add_executable(LauncherDropTests tests.cpp)
    set_target_properties(LauncherDropTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>")
    target_link_libraries(LauncherDropTests PRIVATE LauncherCore Catch2::Catch2WithMain shell32 ole32 uuid advapi32)
    launcher_use_std_module(LauncherDropTests)
    catch_discover_tests(LauncherDropTests PROPERTIES FIXTURES_REQUIRED LauncherTestRuntime)
endif()
