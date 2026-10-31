// Evidence cases of AddPart, RemovePart, AddStaff, RemoveStaff and
// GenerateExcerpts.

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "engrave/tests/harness/harness.h"

#include "engraving/dom/excerpt.h"
#include "engraving/dom/instrtemplate.h"
#include "engraving/dom/instrument.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/part.h"
#include "engraving/dom/score.h"
#include "engraving/dom/staff.h"

#include "engrave/src/api/handles.h"

namespace engrave::harness {
namespace {

using mu::engraving::EngravingObject;
using mu::engraving::Excerpt;
using mu::engraving::InstrumentTemplate;
using mu::engraving::MasterScore;
using mu::engraving::Part;
using mu::engraving::Score;
using mu::engraving::Staff;

// The n-th element of a type in document order: the master's first, then those of
// its excerpts.
ElementId nth_id(Document& document, std::string_view type, std::size_t index) {
    std::size_t seen = 0;
    return find_id(document, type, [&seen, index](const EngravingObject&) { return seen++ == index; });
}

bool live(Document& document, const ElementId& id) {
    for (const LiveId& entry : live_ids(master_of(document))) {
        if (entry.id == id) return true;
    }
    return false;
}

const InstrumentTemplate* template_of(std::string_view instrument_id) {
    const mu::engraving::InstrumentIndex found =
        mu::engraving::searchTemplateIndexForId(muse::String::fromStdString(std::string(instrument_id)));
    return found.instrTemplate;
}

// What the case saw before the batch: the counts, the operand and where it sat.
struct State {
    std::size_t parts = 0;    // the master's parts
    std::size_t staves = 0;   // the master's staves
    std::size_t excerpts = 0;
    std::size_t excerpt_staves = 0; // of the first excerpt, when there is one
    std::size_t index = 0;    // of the operand
    std::size_t operand_staves = 0;
    ElementId target;
    std::vector<ElementId> targets;
};

void observe(Document& document, State& state) {
    MasterScore& master = master_of(document);
    state.parts = master.parts().size();
    state.staves = master.staves().size();
    state.excerpts = master.excerpts().size();
    if (!master.excerpts().empty()) state.excerpt_staves = master.excerpts().front()->excerptScore()->staves().size();
}

std::string expect_part_added(Document& document, const State& state, std::string_view instrument, bool before) {
    MasterScore& master = master_of(document);
    if (master.parts().size() != state.parts + 1) return std::format("the master has {} part(s)", master.parts().size());
    const InstrumentTemplate* templ = template_of(instrument);
    if (!templ) return "the instrument template is gone";
    const std::size_t index = before ? state.index : state.parts;
    const Part* part = master.parts()[index];
    if (part->nstaves() != templ->staffCount) return "the new part has another number of staves";
    if (part->instrument()->id() != templ->id) return "the new part plays another instrument";
    if (!part->eid().isValid()) return "the new part has no id";
    const std::size_t first = master.staffIdx(part);
    for (std::size_t i = 0; i < part->nstaves(); ++i) {
        const Staff* staff = part->staves()[i];
        if (staff->idx() != first + i) return "a new staff is not where its part says it is";
        if (!staff->eid().isValid()) return "a new staff has no id";
    }
    return {};
}

OpCase add_part(std::string name, std::string fixture, std::string instrument, std::optional<std::size_t> before) {
    struct Shared : State {
        std::string instrument;
    };
    auto shared = std::make_shared<Shared>();
    shared->instrument = instrument;
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared, before](Document& document) {
            observe(document, *shared);
            std::optional<Ref> at;
            if (before) {
                shared->index = *before;
                shared->target = nth_id(document, "Part", shared->index);
                at = shared->target;
            }
            return Batch{{AddPart{shared->instrument, at}}};
        },
        .expect = [shared, before](Document& document, const Fingerprint&, const Fingerprint&) -> std::string {
            return expect_part_added(document, *shared, shared->instrument, before.has_value());
        },
    };
}

OpCase remove_part(std::string name, std::string fixture, std::size_t index) {
    struct Shared : State {
        bool excerpt_part = false;
    };
    auto shared = std::make_shared<Shared>();
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared, index](Document& document) {
            observe(document, *shared);
            shared->index = index;
            shared->target = nth_id(document, "Part", index);
            const EngravingObject& object = element(document, shared->target);
            shared->operand_staves = object.isPart() ? mu::engraving::toPart(&object)->nstaves() : 0;
            shared->excerpt_part = object.isPart() && !mu::engraving::toPart(&object)->score()->isMaster();
            return Batch{{RemovePart{shared->target}}};
        },
        .expect = [shared](Document& document, const Fingerprint&, const Fingerprint&) -> std::string {
            MasterScore& master = master_of(document);
            if (live(document, shared->target)) return "the removed part still resolves";
            if (shared->excerpt_part) {
                // An excerpt that loses its last part goes with it; the master keeps its own.
                if (master.excerpts().size() != shared->excerpts - 1) return "the excerpt stayed with no part";
                if (master.parts().size() != shared->parts) return "the master lost a part";
                return std::string{};
            }
            if (master.parts().size() != shared->parts - 1)
                return std::format("the master has {} part(s)", master.parts().size());
            if (master.staves().size() != shared->staves - shared->operand_staves) return "the staves did not go with it";
            if (master.excerpts().size() != shared->excerpts) return "an excerpt came or went";
            return std::string{};
        },
    };
}

OpCase add_staff(std::string name, std::string fixture) {
    struct Shared : State {
    };
    auto shared = std::make_shared<Shared>();
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared](Document& document) {
            observe(document, *shared);
            shared->target = nth_id(document, "Part", 0);
            const EngravingObject& object = element(document, shared->target);
            shared->operand_staves = mu::engraving::toPart(&object)->nstaves();
            return Batch{{AddStaff{shared->target}}};
        },
        .expect = [shared](Document& document, const Fingerprint&, const Fingerprint&) -> std::string {
            MasterScore& master = master_of(document);
            if (master.staves().size() != shared->staves + 1) return "the staff was not added";
            const auto& parts = master.parts();
            if (parts.empty() || parts.front()->nstaves() != shared->operand_staves + 1)
                return "the part did not get another staff";
            const Staff* staff = parts.front()->staves().back();
            if (staff->idx() != shared->operand_staves) return "the new staff is not the part's last";
            if (!staff->eid().isValid()) return "the new staff has no id";
            if (master.excerpts().size() != shared->excerpts) return "an excerpt came or went";
            if (!master.excerpts().empty()) {
                const Score* excerpt = master.excerpts().front()->excerptScore();
                // The excerpt links a copy of the new staff to the new staff.
                if (excerpt->staves().size() != shared->excerpt_staves + 1) return "the excerpt did not get a staff";
                if (!excerpt->staff(shared->excerpt_staves)->isLinked()) return "the excerpt's new staff is not linked";
            }
            return std::string{};
        },
    };
}

OpCase remove_staff(std::string name, std::string fixture, std::size_t index) {
    struct Shared : State {
    };
    auto shared = std::make_shared<Shared>();
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared, index](Document& document) {
            observe(document, *shared);
            shared->index = index;
            shared->target = nth_id(document, "Staff", index);
            return Batch{{RemoveStaff{shared->target}}};
        },
        .expect = [shared](Document& document, const Fingerprint&, const Fingerprint&) -> std::string {
            MasterScore& master = master_of(document);
            if (master.staves().size() != shared->staves - 1) return "the staff was not removed";
            if (live(document, shared->target)) return "the removed staff still resolves";
            if (master.excerpts().size() != shared->excerpts) return "an excerpt came or went";
            if (!master.excerpts().empty()) {
                const Score* excerpt = master.excerpts().front()->excerptScore();
                if (excerpt->staves().size() != shared->excerpt_staves - 1) return "the excerpt kept the linked staff";
            }
            return std::string{};
        },
    };
}

OpCase excerpts(std::string name, std::string fixture, std::size_t parts) {
    struct Shared : State {
        std::size_t made = 0;
    };
    auto shared = std::make_shared<Shared>();
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared, parts](Document& document) {
            observe(document, *shared);
            shared->made = parts;
            std::vector<Ref> refs;
            for (std::size_t i = 0; i < parts; ++i) refs.push_back(nth_id(document, "Part", i));
            return Batch{{GenerateExcerpts{refs}}};
        },
        .expect = [shared](Document& document, const Fingerprint&, const Fingerprint&) -> std::string {
            MasterScore& master = master_of(document);
            if (master.excerpts().size() != shared->excerpts + shared->made) return "an excerpt is missing";
            for (const Excerpt* excerpt : master.excerpts()) {
                const Score* score = excerpt->excerptScore();
                if (!score) return "an excerpt has no score";
                if (!score->eid().isValid()) return "an excerpt score has no id";
                if (score->parts().empty()) return "an excerpt has no part";
                if (score->staves().empty()) return "an excerpt has no staff";
                if (score->parts().front()->nstaves() != score->staves().size()) return "an excerpt's part and staves disagree";
            }
            return std::string{};
        },
    };
}

std::vector<OpCase> all_cases() {
    std::vector<OpCase> cases = {
        add_part("AddPart flute appended", "ghost_note.mscx", "flute", std::nullopt),
        add_part("AddPart piano before the first part", "lyrics.mscx", "piano", 0),
        remove_part("RemovePart of the second of two parts", "chordline_path.mscx", 1),
        remove_part("RemovePart of the first of two parts", "chordline_path.mscx", 0),
        add_staff("AddStaff to a part", "lyrics.mscx"),
        add_staff("AddStaff to a one-staff part", "hairpins_and_repeats.mscx"),
        remove_staff("RemoveStaff of a two-staff part", "lyrics.mscx", 1),
        remove_staff("RemoveStaff from a linked part", "linked_parts.mscx", 1),
        excerpts("GenerateExcerpts for one part", "custom_part_name.mscx", 1),
        excerpts("GenerateExcerpts of a two-staff part", "ghost_note.mscx", 1),
    };
    return cases;
}

const bool registered = register_cases(all_cases());

} // namespace
} // namespace engrave::harness
