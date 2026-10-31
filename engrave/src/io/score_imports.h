#pragma once

#include <string>

namespace mu::engraving {
class MasterScore;
}

namespace music {

// Imports Guitar Pro 3/4/5/6/7 (.gp3/.gp4/.gp5/.gpx/.gp) and Power Tab (.ptb)
// bytes into an already-created master score. The caller owns layout setup.
void import_guitarpro_score(mu::engraving::MasterScore* score, const std::string& bytes);

} // namespace music
