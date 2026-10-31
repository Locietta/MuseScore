#pragma once
#include "engrave/src/score/export_options.h"
#include <stdexcept>
#include <string>

namespace mu::engraving { class Score; }
namespace music {

// The page is larger than the renderer draws: 36..1200 dpi, 32767 pixels per
// side and 64e6 pixels in all. A caller maps this to a resource-limit error,
// not to an engine failure.
struct RenderLimitExceeded : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Throws std::invalid_argument for a format, page or dpi the renderer does not
// take, RenderLimitExceeded for a page beyond the size limit, and
// std::runtime_error for an engine failure.
std::string render_score(mu::engraving::Score* score, const std::string& format, int page);
std::string render_score(mu::engraving::Score* score, const std::string& format, const ExportOptions& options);
}
