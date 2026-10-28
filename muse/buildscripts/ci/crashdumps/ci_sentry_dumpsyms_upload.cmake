# Uploads the generated breakpad symbols to sentry. sentry-cli from extdeps.

set(HERE ${CMAKE_CURRENT_LIST_DIR})

set(ARTIFACTS_DIR "build.artifacts")
set(SYMBOLS_PATH "${ARTIFACTS_DIR}/symbols")

set(SENTRY_URL "" CACHE STRING "Sentry URL")
set(SENTRY_AUTH_TOKEN "" CACHE STRING "Sentry Auth Token")
set(SENTRY_ORG "" CACHE STRING "Sentry Organization")
set(SENTRY_PROJECT "" CACHE STRING "Sentry Project")

# Check
if(NOT SENTRY_URL)
    message(FATAL_ERROR "error: not set SENTRY_URL")
endif()
if(NOT SENTRY_AUTH_TOKEN)
    message(FATAL_ERROR "error: not set SENTRY_AUTH_TOKEN")
endif()
if(NOT SENTRY_ORG)
    message(FATAL_ERROR "error: not set SENTRY_ORG")
endif()
if(NOT SENTRY_PROJECT)
    message(FATAL_ERROR "error: not set SENTRY_PROJECT")
endif()

message(STATUS "SYMBOLS_PATH: ${SYMBOLS_PATH}")
message(STATUS "SENTRY_URL: ${SENTRY_URL}")
message(STATUS "SENTRY_ORG: ${SENTRY_ORG}")
message(STATUS "SENTRY_PROJECT: ${SENTRY_PROJECT}")

set(LOCAL_ROOT_PATH "${HERE}/_deps")
set(EXTDEPS_DIR "${CMAKE_SOURCE_DIR}/muse_deps" CACHE PATH "muse_deps checkout")
include("${EXTDEPS_DIR}/buildtools/manifest.cmake")
require_tool(sentry-cli)
get_property(_bin_dir GLOBAL PROPERTY sentry-cli_BIN_DIR)
if(WIN32)
    set(SENTRY_CLI "${_bin_dir}/sentry-cli.exe")
else()
    set(SENTRY_CLI "${_bin_dir}/sentry-cli")
endif()

# Upload symbols
set(ENV{SENTRY_URL} ${SENTRY_URL})
set(ENV{SENTRY_AUTH_TOKEN} ${SENTRY_AUTH_TOKEN})

execute_process(
    COMMAND ${SENTRY_CLI} upload-dif -o ${SENTRY_ORG} -p ${SENTRY_PROJECT} ${SYMBOLS_PATH}
    RESULT_VARIABLE result
)

if(result EQUAL 0)
    message(STATUS "Success symbols uploaded")
else()
    message(FATAL_ERROR "Failed symbols uploaded, code: ${result}")
endif()
