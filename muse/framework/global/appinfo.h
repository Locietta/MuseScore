/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include "types/string.h"
#include "types/version.h"

namespace muse::appinfo {
// The engine's build metadata (generated engrave_version.h, read by one
// translation unit): the version as written into scores and the short Git
// revision of the build, empty when unknown.
const char* versionString();
Version version();
String revision();
}