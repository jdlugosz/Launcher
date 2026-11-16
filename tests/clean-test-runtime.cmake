cmake_minimum_required(VERSION 3.25)
if(NOT DEFINED TEST_ROOT OR NOT IS_ABSOLUTE "${TEST_ROOT}" OR NOT DEFINED CLEANUP_SCRIPT)
    message(FATAL_ERROR "TEST_ROOT and CLEANUP_SCRIPT are required.")
endif()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef case_name)
set(case_directory "${TEST_ROOT}/case-${case_name}")
set(source "${case_directory}/source")
set(binary "${case_directory}/build")
set(removed
    "${source}/Launcher.runtime.toml"
    "${source}/examples/nested/preview.runtime.toml"
    "${source}/tests/fixtures/fixture.runtime.toml"
    "${source}/tests/fixtures/CAPS.RUNTIME.TOML"
    "${binary}/generated.runtime.toml"
    "${binary}/examples/preview.runtime.toml"
    "${binary}/Release/preview.runtime.toml"
    "${binary}/Release/examples/nested/preview.runtime.toml"
    "${binary}/install-layout-tests/Release/run/AppData/Launcher.runtime.toml"
    "${binary}/display-edge-tests/Release/run/display.runtime.toml")
set(preserved
    "${source}/Launcher.toml"
    "${source}/examples/nested/preview.toml"
    "${source}/tests/fixtures/fixture.toml"
    "${case_directory}/personal/Launcher.runtime.toml"
    "${case_directory}/personal/examples/nested.runtime.toml"
    "${binary}/vcpkg_installed/dependency.runtime.toml"
    "${binary}/unrelated-tool/fixture.runtime.toml")
foreach(path IN LISTS removed preserved)
    get_filename_component(parent "${path}" DIRECTORY)
    file(MAKE_DIRECTORY "${parent}")
    file(WRITE "${path}" "preserve this fixture content\n")
endforeach()
# Directory links must not let a source-tree cleanup reach personal settings.
# Developer Mode is optional, so lack of symlink permission does not fail this test.
file(CREATE_LINK "${case_directory}/personal" "${source}/examples/personal-link" SYMBOLIC RESULT link_result)
file(CREATE_LINK "${case_directory}/personal" "${binary}/Debug" SYMBOLIC RESULT configuration_link_result)
execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${source}" "-DBINARY_DIR=${binary}"
    -P "${CLEANUP_SCRIPT}" RESULT_VARIABLE cleanup_result OUTPUT_VARIABLE cleanup_output ERROR_VARIABLE cleanup_error)
if(NOT cleanup_result STREQUAL "0")
    message(FATAL_ERROR "Cleanup failed: ${cleanup_output}${cleanup_error}")
endif()
foreach(path IN LISTS removed)
    if(EXISTS "${path}")
        message(FATAL_ERROR "Generated runtime file was not removed: ${path}")
    endif()
endforeach()
foreach(path IN LISTS preserved)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Cleanup removed a preserved file: ${path}")
    endif()
    file(READ "${path}" content)
    if(NOT content STREQUAL "preserve this fixture content\n")
        message(FATAL_ERROR "Cleanup changed a preserved file: ${path}")
    endif()
endforeach()
if(link_result STREQUAL "0" AND NOT IS_SYMLINK "${source}/examples/personal-link")
    message(FATAL_ERROR "Cleanup removed the directory symlink.")
endif()
# The entire generated case is owned by this test. Verify its resolved location
# before deleting it, including the simulated personal directory.
file(REAL_PATH "${case_directory}" resolved_case)
file(REAL_PATH "${TEST_ROOT}" resolved_root)
get_filename_component(case_parent "${resolved_case}" DIRECTORY)
if(NOT resolved_case STREQUAL case_directory OR NOT case_parent STREQUAL resolved_root)
    message(FATAL_ERROR "Refusing to remove an unexpected test directory: ${resolved_case}")
endif()
if(link_result STREQUAL "0")
    file(REMOVE "${source}/examples/personal-link")
endif()
if(configuration_link_result STREQUAL "0")
    file(REMOVE "${binary}/Debug")
endif()
file(REMOVE_RECURSE "${resolved_case}")
message(STATUS "Runtime cleanup removed generated files and preserved sources and outside settings.")
