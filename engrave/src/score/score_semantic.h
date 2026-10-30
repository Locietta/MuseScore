// SPDX-License-Identifier: GPL-3.0-only
// Format-independent semantic dump of a score (roadmap-engrave A0).
#pragma once

#include <nlohmann/json.hpp>

namespace mu::engraving {
class MasterScore;
}

namespace music {

// Walks the in-memory DOM, not a serialized file, so the dump stays comparable
// across MSCX format versions (4.70 inline spanners vs 5.00 SpannerMap). It
// records musical content only: no layout, positions, styles or EIDs. Staff
// and track numbers are zero-based and score-global. Elements flagged
// generated (layout-created header clefs, courtesy signatures) are skipped.
// Requires the owner thread; the score is not modified, except that
// repeatList() is rebuilt with repeats expanded.
nlohmann::ordered_json semantic_dump(mu::engraving::MasterScore* score);

} // namespace music
