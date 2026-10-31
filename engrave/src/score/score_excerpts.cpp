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
// This uses the upstream initialization routine without allocating a detached
// AddExcerpt undo command, and owns each created excerpt until addExcerpt().

#include "score_excerpts.h"

#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

#include "engraving/dom/excerpt.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/part.h"
#include "engraving/dom/score.h"

namespace music {
namespace {
using mu::engraving::Excerpt;
using mu::engraving::MasterScore;
using mu::engraving::Part;
using mu::engraving::Score;

void ensure_layout(Score* score)
{
    // A score gets pages only from layout, so an empty page list is the
    // reliable "not laid out yet" state in the pinned engine.
    if (score->pages().empty() || !score->pages().front()) {
        score->doLayout();
    }
}
} // namespace

bool generate_score_excerpts(MasterScore* score)
{
    if (!score) {
        throw std::invalid_argument("generate_score_excerpts: score is null");
    }
    if (!score->excerpts().empty()) {
        return false;
    }

    // _generateExcerpts() documents that only visible parts get excerpts.
    std::vector<Part*> visible_parts;
    visible_parts.reserve(score->parts().size());
    for (Part* part : score->parts()) {
        if (part && part->isVisible()) {
            visible_parts.push_back(part);
        }
    }
    if (visible_parts.empty()) {
        return false;
    }

    // createExcerptsFromParts() hands back raw, unowned excerpts. Keep them
    // owned here until addExcerpt() transfers each one to the document.
    // Excerpt::createExcerpt() clones measures and links master items with
    // Score::undo(Link): outside of a load that mutates the score without a
    // transaction and logs "called outside of transaction". Upstream wraps the
    // same excerpt/layout construction in a ScoreLoad scope
    // (notation/internal/masternotation.cpp:456, importmusicxml.cpp:223), so the
    // derived excerpt structure is built, not pushed as a user undo step.
    mu::engraving::ScoreLoad scoreLoad;
    std::vector<Excerpt*> pending = Excerpt::createExcerptsFromParts(visible_parts, score);
    std::size_t added_count = 0;
    try {
        for (; added_count < pending.size(); ++added_count) {
            Excerpt* excerpt = pending[added_count];
            if (!excerpt) {
                throw std::runtime_error("createExcerptsFromParts returned a null excerpt");
            }

            // initExcerpt() creates the excerpt score, enables
            // Sid::createMultiMeasureRests before Excerpt::createExcerpt()
            // lays it out, and marks the excerpt inited. addExcerpt() then
            // performs the single ownership transfer into the document.
            score->initExcerpt(excerpt);
            score->addExcerpt(excerpt);
        }
    } catch (...) {
        // Entries before added_count were transferred to MasterScore, so
        // remove them before deleting. removeExcerpt() does not delete.
        // The entry in progress and all later entries are still owned here.
        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (i < added_count) {
                score->removeExcerpt(pending[i]);
            }
            delete pending[i];
        }
        throw;
    }

    return !pending.empty();
}

std::vector<ExcerptInfo> list_score_excerpts(MasterScore* score)
{
    if (!score) {
        throw std::invalid_argument("list_score_excerpts: score is null");
    }

    const std::vector<Excerpt*>& excerpts = score->excerpts();
    std::vector<ExcerptInfo> result;
    result.reserve(excerpts.size());
    for (std::size_t i = 0; i < excerpts.size(); ++i) {
        const Excerpt* excerpt = excerpts[i];
        if (!excerpt) {
            throw std::runtime_error("list_score_excerpts: null excerpt entry");
        }

        ExcerptInfo info;
        info.id = static_cast<int>(i);
        info.title = excerpt->name().toStdString();
        info.part_ids.reserve(excerpt->parts().size());
        for (const Part* part : excerpt->parts()) {
            if (part) {
                info.part_ids.push_back(part->id().toStdString());
            }
        }
        const Score* excerpt_score = excerpt->excerptScore();
        info.pages = excerpt_score ? excerpt_score->npages() : 0;
        result.push_back(std::move(info));
    }
    return result;
}

Score* select_score(MasterScore* score, int excerpt_id)
{
    if (!score) {
        throw std::invalid_argument("select_score: score is null");
    }
    if (excerpt_id < -1) {
        throw std::out_of_range("select_score: invalid excerpt id");
    }
    if (excerpt_id == -1) {
        ensure_layout(score);
        return score;
    }

    const std::vector<Excerpt*>& excerpts = score->excerpts();
    if (static_cast<std::size_t>(excerpt_id) >= excerpts.size()) {
        throw std::out_of_range("select_score: excerpt id is out of range");
    }

    const Excerpt* excerpt = excerpts[static_cast<std::size_t>(excerpt_id)];
    if (!excerpt) {
        throw std::runtime_error("select_score: null excerpt entry");
    }
    if (excerpt->masterScore() != score) {
        throw std::runtime_error("select_score: excerpt is not owned by the score");
    }

    // The 4.7.5 loaders leave Excerpt::inited() false for excerpts read from a
    // file, so a usable excerpt score is the validation here; requiring
    // inited() would reject every existing loaded excerpt.
    Score* selected = excerpt->excerptScore();
    if (!selected || selected->masterScore() != score) {
        throw std::runtime_error("select_score: excerpt has no initialized score");
    }

    ensure_layout(selected);
    return selected;
}
} // namespace music
