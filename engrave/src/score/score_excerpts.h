// SPDX-License-Identifier: GPL-3.0-only
//
// Native excerpt helpers for the Qt-free host, adapted from
// partitioncloud/webmscore (tag webmscore-4.7.5-1, commit
// 8ec6401a60deac5582403e6914595d12c384d9ae):
//   - web/main.cpp, _generateExcerpts()
//   - src/engraving/dom/excerpt.cpp, Excerpt::createExcerptsFromParts()
//     and Excerpt::createExcerpt()
//   - src/engraving/dom/excerpt.cpp, MasterScore::initExcerpt()
//   - src/engraving/dom/masterscore.cpp, addExcerpt() and removeExcerpt()
//   - web/score.cpp, MainScore::maybeUseExcerpt()
// The pinned engine revision is the vendor/musescore submodule (docs/dev/fork.md).
//
// These are private core helpers, not a public API. They take raw pointers to
// document-owned objects and must be called on the MasterScore's owner thread.
// A returned Score* is non-owning and is valid only while the MasterScore that
// owns it is alive.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace mu::engraving {
class MasterScore;
class Score;
}

namespace music {
struct ExcerptInfo {
    int id = 0;
    std::string title;
    std::vector<std::string> part_ids;
    std::size_t pages = 0;
};

// Adds one excerpt per visible part when the score has no excerpts yet.
// Returns true only when excerpts were added. Existing excerpts, including
// excerpts loaded from a file, are left untouched, so repeated calls are
// idempotent. Throws std::invalid_argument when score is null.
bool generate_score_excerpts(mu::engraving::MasterScore* score);

// Lists excerpts in MasterScore::excerpts() order. "pages" is the excerpt
// score's current layout page count (0 when that score has no layout). This
// never generates excerpts. Throws std::invalid_argument when score is null.
std::vector<ExcerptInfo> list_score_excerpts(mu::engraving::MasterScore* score);

// Returns the full score for excerpt_id == -1, otherwise the excerpt score at
// that index. The selected score is laid out first when it has no pages.
// Throws std::invalid_argument when score is null, std::out_of_range for an
// invalid id, and std::runtime_error when the excerpt has no usable,
// document-owned score.
mu::engraving::Score* select_score(mu::engraving::MasterScore* score, int excerpt_id = -1);
}
