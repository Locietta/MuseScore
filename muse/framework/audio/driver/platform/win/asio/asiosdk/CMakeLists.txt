
muse_create_thirdparty_module(asiosdk)

get_property(_asio_src GLOBAL PROPERTY asiosdk_SOURCE_DIR)
if (NOT _asio_src)
    message(FATAL_ERROR "[asiosdk] not populated; ExtDepsManifest.cmake must run before this module")
endif()
set(ASIO_SDK_PATH ${_asio_src}/asiosdk)

target_sources(asiosdk PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/asiosdk_asio.cpp
    ${CMAKE_CURRENT_LIST_DIR}/asiosdk_asiolist.cpp
    ${CMAKE_CURRENT_LIST_DIR}/asiosdk_asiodrivers.cpp
)

target_include_directories(asiosdk PUBLIC
    ${ASIO_SDK_PATH}
    ${ASIO_SDK_PATH}/common
    ${ASIO_SDK_PATH}/host
    ${ASIO_SDK_PATH}/host/pc
)

# Silence warnings from SDK
target_compile_options(asiosdk PRIVATE /wd4267)
