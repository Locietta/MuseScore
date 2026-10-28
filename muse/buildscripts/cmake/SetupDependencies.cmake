# SPDX-License-Identifier: GPL-3.0-only
# MuseScore-Studio-CLA-applies
#
# MuseScore Studio
# Music Composition & Notation
#
# Copyright (C) 2024 MuseScore Limited
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 3 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

message(STATUS "Setup dependencies")

# Standalone framework build clones the ref in buildscripts/muse_deps.ref
# Apps provide their own muse_deps checkout and include ExtDepsManifest.cmake themselves
if (NOT DEFINED EXTDEPS_DIR OR EXTDEPS_DIR STREQUAL "")
    set(EXTDEPS_DIR "${PROJECT_BINARY_DIR}/muse_deps")
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../muse_deps.ref" _extdeps_ref LIMIT_COUNT 1)
    if (NOT EXISTS "${EXTDEPS_DIR}/buildtools/manifest.cmake")
        find_package(Git REQUIRED)
        execute_process(COMMAND ${GIT_EXECUTABLE} clone --quiet
                        https://github.com/musescore/muse_deps.git "${EXTDEPS_DIR}"
                        RESULT_VARIABLE _rc)
        if (NOT _rc EQUAL 0)
            message(FATAL_ERROR "muse_deps clone failed; set EXTDEPS_DIR to an existing checkout")
        endif()
    endif()
    execute_process(COMMAND ${GIT_EXECUTABLE} -C "${EXTDEPS_DIR}" fetch --quiet origin "${_extdeps_ref}")
    execute_process(COMMAND ${GIT_EXECUTABLE} -C "${EXTDEPS_DIR}" checkout --quiet "${_extdeps_ref}"
                    RESULT_VARIABLE _rc)
    if (NOT _rc EQUAL 0)
        message(FATAL_ERROR "muse_deps checkout ${_extdeps_ref} failed")
    endif()
endif()

include(${EXTDEPS_DIR}/buildtools/manifest.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/ExtDepsManifest.cmake)
