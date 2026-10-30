#pragma once
#include "engrave/src/score/export_options.h"
#include <string>

namespace mu::engraving { class Score; }
namespace music {
std::string render_score(mu::engraving::Score* score, const std::string& format, int page);
std::string render_score(mu::engraving::Score* score, const std::string& format, const ExportOptions& options);
}
