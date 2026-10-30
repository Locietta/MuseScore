#include <cstdio>
#include <cstdlib>

#include <engrave/error.h>
#include <engrave/output.h>

#include "engrave/src/api/state.h"

namespace engrave {

void check(bool condition, std::string_view message) noexcept {
    if (condition) [[likely]] return;
    std::fprintf(stderr, "engrave: contract violation: %.*s\n", static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
    std::abort();
}

std::string utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::InvalidInput: return "InvalidInput";
    case ErrorCode::UnsupportedFormat: return "UnsupportedFormat";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::StaleHandle: return "StaleHandle";
    case ErrorCode::ResourceLimit: return "ResourceLimit";
    case ErrorCode::Unsupported: return "Unsupported";
    case ErrorCode::Io: return "Io";
    }
    return "Unknown";
}

std::string_view to_string(Format format) noexcept {
    switch (format) {
    case Format::Mscz: return "MSCZ";
    case Format::Mscx: return "MSCX";
    case Format::MusicXml: return "MusicXML";
    case Format::Mxl: return "MXL";
    case Format::Midi: return "MIDI";
    case Format::GuitarPro: return "Guitar Pro";
    }
    return "Unknown";
}

} // namespace engrave
