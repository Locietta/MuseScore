if(NOT DEFINED CMAKE_SCRIPT_MODE_FILE)
    message(FATAL_ERROR "This file is a script")
endif()

# Find muse_deps:
# When building as part of the app, muse_deps will be in a sibling directory
# For standaline builds, clone using muse_deps.ref
if(NOT DEFINED EXTDEPS_DIR)
    get_filename_component(_app_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
    set(EXTDEPS_DIR "${_app_root}/muse_deps")
    if(NOT EXISTS "${EXTDEPS_DIR}/buildtools/manifest.cmake")
        file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../../buildscripts/muse_deps.ref" _extdeps_ref LIMIT_COUNT 1)
        find_package(Git REQUIRED)
        execute_process(COMMAND ${GIT_EXECUTABLE} clone --quiet
                        https://github.com/musescore/muse_deps.git "${EXTDEPS_DIR}"
                        RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "muse_deps clone failed; set EXTDEPS_DIR to an existing checkout")
        endif()
        execute_process(COMMAND ${GIT_EXECUTABLE} -C "${EXTDEPS_DIR}" fetch --quiet origin "${_extdeps_ref}")
        execute_process(COMMAND ${GIT_EXECUTABLE} -C "${EXTDEPS_DIR}" checkout --quiet "${_extdeps_ref}"
                        RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "muse_deps checkout ${_extdeps_ref} failed")
        endif()
    endif()
endif()

include("${EXTDEPS_DIR}/buildtools/manifest.cmake")
require_tool(uncrustify)
find_program(UNCRUSTIFY_BIN uncrustify REQUIRED)
