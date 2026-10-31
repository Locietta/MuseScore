#pragma once

#include <string_view>

namespace engrave {

// Version of the engrave API. The engine version that is written into saved
// scores is Capabilities::engine_version.
inline constexpr int version_major = 0;
inline constexpr int version_minor = 1;
inline constexpr int version_patch = 0;
inline constexpr std::string_view version_string = "0.1.0";

} // namespace engrave
