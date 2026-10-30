#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace engrave {

// Failures the immediate caller can act on. They are returned as Result<T>.
// Failures inside the engine that no caller can repair at the call site
// (an exporter that produces nothing, a broken runtime) are thrown, and
// contract violations (a call from another thread, a second Engine) abort.
enum struct ErrorCode {
    InvalidArgument,   // an option the caller passed is invalid
    InvalidInput,      // the score data is malformed or the importer rejected it
    UnsupportedFormat, // the data matches no format the engine reads
    NotFound,          // a file or template does not exist
    ResourceLimit,     // the input exceeds a size limit
    Unsupported,       // a valid request the engine does not implement
    Io,                // a file exists but cannot be read
};

std::string_view to_string(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::InvalidArgument;
    std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

enum struct Severity {
    Info,
    Warning,
};

// A problem that did not stop the call, e.g. a score that failed the engine's
// sanity check on load.
struct Diagnostic {
    Severity severity = Severity::Info;
    std::string message;
};

} // namespace engrave
