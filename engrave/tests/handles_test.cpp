// Element handles: the inventory (H1), the ids a score has after open and create
// (H2), what survives a save and a replay (H3, H4), linked parts (H5), deletion and
// replacement (H7, H8), hostile ids in the input (H9, H10), stale handles (H11),
// addresses (H12) and determinism (H13).  Every test ends with INV: the register is
// a bijection onto the elements that are part of the score, and every element that
// should have an id has one.
//
//   engrave_handles_test --assets=<resources> [--corpus=<file or dir>]... [options]
//   --deterministic       create the engine with sequential ids (H13's child)
//   --update-inventory    rewrite handle_inventory.json from the corpus
//
// The public edit API does not exist yet, so a test that needs an edit, a rollback
// or a restore drives the engine's own primitives: startCmd/endCmd (a transaction),
// the undo history, and an MSCZ save followed by a reopen; H7 runs its removal as an
// edit batch (api::BatchGuard).  H6 (copy and paste) is deferred; the report says why.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <engrave/engrave.h>

#include "engrave/src/api/batch.h"
#include "engrave/src/api/handles.h"
#include "engrave/src/api/state.h"
#include "engrave/src/runtime/noqt_runtime.h"
#include "engrave/src/score/export_options.h"
#include "engrave/src/score/score_excerpts.h"

#include "engraving/dom/chord.h"
#include "engraving/dom/clef.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/excerpt.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/instrtemplate.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/measurebase.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/part.h"
#include "engraving/dom/property.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/textbase.h"
#include "engraving/editing/addremoveelement.h"
#include "engraving/editing/transaction/undostack.h"
#include "engraving/infrastructure/eidregister.h"

namespace {

using engrave::ElementId;
using engrave::ErrorCode;
using engrave::IdClass;
using mu::engraving::ElementType;
using mu::engraving::EngravingItem;
using mu::engraving::EngravingObject;
using mu::engraving::MasterScore;
using mu::engraving::Measure;
using mu::engraving::MeasureBase;
using mu::engraving::Note;
using mu::engraving::Part;
using mu::engraving::Score;
using mu::engraving::Segment;
using mu::engraving::SegmentType;
using mu::engraving::Staff;

std::filesystem::path g_assets;
std::filesystem::path g_inventory = ENGRAVE_TEST_SOURCE_ROOT "/engrave/tests/handle_inventory.json";
std::filesystem::path g_source_root = ENGRAVE_TEST_SOURCE_ROOT;
std::vector<std::filesystem::path> g_corpus;
std::vector<std::string> g_engine_args; // --assets and --corpus again, for the child
std::string g_program;
bool g_deterministic = false;
bool g_update_inventory = false;
std::unique_ptr<engrave::Engine> g_engine;

// --- text and bytes -------------------------------------------------------

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::byte> as_bytes(std::string_view text) {
    const auto* data = reinterpret_cast<const std::byte*>(text.data());
    return {data, data + text.size()};
}

std::string text_of(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// --- ids ------------------------------------------------------------------

const char* class_name(IdClass value) {
    switch (value) {
    case IdClass::persisted: return "persisted";
    case IdClass::layout: return "layout";
    case IdClass::address: return "address";
    case IdClass::session: return "session";
    }
    return "unknown";
}

MasterScore& master_of(engrave::Document& document) {
    MasterScore* master = engrave::DocumentAccess::master_score(document);
    EXPECT_NE(master, nullptr);
    return *master;
}

/// Whether an id still names an element that is part of the score.
bool live(MasterScore& master, const ElementId& id) {
    return engrave::resolve(master, id).has_value();
}

ElementId engine_id(const EngravingObject& object) {
    return engrave::from_engine(object.eid());
}

std::string id_text(const EngravingObject& object) {
    return engrave::to_string(engine_id(object));
}

std::string object_name(const EngravingObject& object) {
    return std::string(object.typeName());
}

/// Where an element is, for the inventory's evidence.
std::string detail(const EngravingObject& object) {
    if (!object.isEngravingItem()) return object_name(object);
    const auto& item = static_cast<const EngravingItem&>(object);
    return std::format("tick={} track={} parent={}", item.tick().ticks(), static_cast<long long>(item.track()),
                       item.parent() ? item.parent()->typeName() : "null");
}

// --- score elements -------------------------------------------------------

Note* first_note(Score& score) {
    Note* found = nullptr;
    score.scanElements([&found](EngravingItem* item) {
        if (!found && item->isNote()) found = mu::engraving::toNote(item);
    });
    return found;
}

Segment* first_segment(Score& score, SegmentType kind) {
    for (MeasureBase* mb = score.first(); mb; mb = mb->next()) {
        if (!mb->isMeasure()) continue;
        if (mu::engraving::toMeasure(mb)->isMMRest()) continue;
        if (Segment* segment = mu::engraving::toMeasure(mb)->first(kind)) return segment;
    }
    return nullptr;
}

mu::engraving::ChordRest* first_chord_rest(Score& score) {
    for (MeasureBase* mb = score.first(); mb; mb = mb->next()) {
        if (!mb->isMeasure()) continue;
        if (mu::engraving::toMeasure(mb)->isMMRest()) continue;
        for (Segment* segment = mu::engraving::toMeasure(mb)->first(SegmentType::ChordRest); segment; segment = segment->next()) {
            for (mu::engraving::track_idx_t track = 0; track < score.nstaves() * mu::engraving::VOICES; ++track) {
                EngravingItem* element = segment->element(track);
                if (element && element->isChordRest()) return mu::engraving::toChordRest(element);
            }
        }
    }
    return nullptr;
}

/// The first element of a type that the walk hands an id out for.
EngravingObject* first_live(MasterScore& master, ElementType type) {
    for (const engrave::LiveId& entry : engrave::live_ids(master)) {
        if (entry.object->type() == type) return entry.object;
    }
    return nullptr;
}

// --- transactions ---------------------------------------------------------

/// One transaction on the score, so that the engine's undo commands may run.
class Command {
public:
    explicit Command(MasterScore& score) : m_score(score) {
        mu::engraving::MScore::setError(mu::engraving::MsError::MS_NO_ERROR);
        m_score.startCmd(muse::TranslatableString::untranslatable("engrave handles test"));
    }
    ~Command() {
        if (m_open) m_score.endCmd(true, true);
    }
    Command(const Command&) = delete;
    Command& operator=(const Command&) = delete;

    /// Commits, and then does what a batch does: give the elements the commands made
    /// their ids again, and drop the history so that a removal destroys its elements.
    void commit(bool drop_history) {
        m_score.endCmd(false, true);
        m_open = false;
        engrave::assign_missing_ids(m_score);
        if (drop_history) m_score.undoStack()->clearHistory();
    }

    void rollback() {
        m_score.endCmd(true, true);
        m_open = false;
    }

private:
    MasterScore& m_score;
    bool m_open = true;
};

template <typename Function>
void commit(MasterScore& master, const Function& function) {
    Command command(master);
    function(master);
    command.commit(true);
}

// --- documents ------------------------------------------------------------

std::unique_ptr<engrave::Document> open_path(const std::filesystem::path& path) {
    auto document = g_engine->open(path);
    EXPECT_TRUE(document) << path.string() << ": " << document.error().message;
    return document ? std::move(*document) : nullptr;
}

std::unique_ptr<engrave::Document> open_text(std::string_view text, engrave::OpenOptions options = {}) {
    auto document = g_engine->open(as_bytes(text), options);
    EXPECT_TRUE(document) << document.error().message;
    return document ? std::move(*document) : nullptr;
}

std::string export_mscz(engrave::Document& document) {
    auto bytes = document.export_score({.format = engrave::Format::Mscz});
    EXPECT_TRUE(bytes) << bytes.error().message;
    return bytes ? text_of(*bytes) : std::string{};
}

/// The engine's own MSCX of the score (or of one of its excerpts). The version it
/// carries is the current one, so the reader that repairs ids is the one under test.
std::string export_mscx(engrave::Document& document, int excerpt_id = -1) {
    music::ExportOptions options;
    options.excerpt_id = excerpt_id;
    return music::export_native_score(engrave::DocumentAccess::project(document), "mscx", options, {});
}

/// The snapshot path: write the score and read it back into a new document.
std::unique_ptr<engrave::Document> reopen(engrave::Document& document) {
    const std::string mscz = export_mscz(document);
    if (mscz.empty()) return nullptr;
    auto copy = g_engine->open(as_bytes(mscz));
    EXPECT_TRUE(copy) << copy.error().message;
    return copy ? std::move(*copy) : nullptr;
}

// --- corpus choice --------------------------------------------------------

/// A corpus file with one of these extensions (any, when none is given).
std::filesystem::path corpus_file(std::initializer_list<std::string_view> extensions) {
    for (const auto& path : g_corpus) {
        if (extensions.size() == 0) return path;
        for (std::string_view extension : extensions) {
            if (path.extension() == extension) return path;
        }
    }
    return {};
}

/// Whether the corpus file is one the engine writes itself, rather than an import that
/// brings its own layout quirks.
bool written_by_engine(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    return extension == ".mscx" || extension == ".mscz" || extension == ".musicxml" || extension == ".mxl";
}

// --- descriptors ----------------------------------------------------------

/// What an element is beyond its id: its type, where it sits, and a payload that a
/// save and reload has to preserve.
std::string payload(const EngravingObject& object) {
    if (object.type() == ElementType::NOTE) {
        const auto& note = static_cast<const Note&>(object);
        return std::format("pitch={} tpc={}", note.pitch(), note.tpc1());
    }
    if (object.type() == ElementType::MEASURE) {
        return std::format("ticks={}", static_cast<const Measure&>(object).ticks().ticks());
    }
    if (object.isChordRest()) {
        const auto& element = static_cast<const mu::engraving::ChordRest&>(object);
        return std::format("ticks={}", element.durationType().ticks().ticks());
    }
    if (object.isTextBase()) {
        return "text=" + static_cast<const mu::engraving::TextBase&>(object).plainText().toStdString();
    }
    return {};
}

std::string describe(const engrave::LiveId& entry) {
    const EngravingObject& object = *entry.object;
    return std::format("{} score={} tick={} track={} {}", object.typeName(), entry.placement.score, entry.placement.tick,
                       entry.placement.track, payload(object));
}

/// Every id of the document and what it names, keyed by the id's text so that a
/// failure prints ids rather than bytes.
std::map<std::string, std::string> element_map(engrave::Document& document) {
    std::map<std::string, std::string> elements;
    for (const engrave::LiveId& entry : engrave::live_ids(master_of(document))) {
        elements.emplace(engrave::to_string(entry.id), describe(entry));
    }
    return elements;
}

/// Compares two element maps, reporting the ids that differ rather than the whole map.
void expect_same_ids(const std::map<std::string, std::string>& before, const std::map<std::string, std::string>& after,
                     std::string_view what) {
    for (const auto& [id, description] : before) {
        const auto found = after.find(id);
        if (found == after.end()) {
            ADD_FAILURE() << what << ": the reload dropped " << id << " (" << description << ")";
            continue;
        }
        EXPECT_EQ(found->second, description) << what << ": " << id << " names something else";
    }
    for (const auto& [id, description] : after) {
        if (!before.contains(id)) ADD_FAILURE() << what << ": the reload added " << id << " (" << description << ")";
    }
}

/// The first corpus score that has an element of this type with an id.
std::unique_ptr<engrave::Document> open_with(ElementType type) {
    for (const auto& path : g_corpus) {
        if (!written_by_engine(path)) continue;
        auto document = g_engine->open(path);
        if (!document) continue;
        if (first_live(*engrave::DocumentAccess::master_score(**document), type)) return std::move(*document);
    }
    return nullptr;
}

// --- INV ------------------------------------------------------------------

void expect_inv(engrave::Document& document) {
    MasterScore& master = master_of(document);
    std::set<const EngravingObject*> objects;
    for (EngravingObject* object : engrave::document_objects(master)) objects.insert(object);

    // The register is a bijection: an entry's element reports that id, and the id of a
    // persisted element names an element of the document.
    std::set<std::string> registered;
    for (const auto& [eid, object] : master.eidRegister()->entries()) {
        const std::string text = eid.toStdString();
        ASSERT_TRUE(eid.isValid()) << "an invalid id is registered";
        EXPECT_EQ(object->eid(), eid) << text << ": the reverse entry disagrees";
        EXPECT_TRUE(registered.insert(text).second) << text << " is registered twice";
        if (engrave::id_class(*object) != IdClass::persisted) continue; // an id that layout
        // or a read may drop is not handed out, so it is the engine's own entry.
        EXPECT_TRUE(objects.contains(object) || engrave::in_score(master, object))
            << text << " (" << object->typeName() << ") left the document";
    }

    // Every element that should have an id has one, and the register maps it back to it.
    for (const engrave::LiveId& entry : engrave::live_ids(master)) {
        EXPECT_TRUE(registered.contains(engrave::to_string(entry.id)))
            << object_name(*entry.object) << " " << engrave::to_string(entry.id) << " is not registered";
        EXPECT_EQ(master.eidRegister()->itemFromEID(engrave::to_engine(entry.id)), entry.object)
            << engrave::to_string(entry.id) << " names another element";
    }
    for (EngravingObject* object : engrave::document_objects(master)) {
        if (!engrave::takes_id(*object)) continue;
        EXPECT_TRUE(object->eid().isValid()) << object_name(*object) << " has no id";
    }
    EXPECT_LE(registered.size(), master.eidRegister()->size());
}

// --- H1 -------------------------------------------------------------------

struct Evidence {
    IdClass declared = IdClass::persisted;
    int assigned = 0;    // instances the walk gives an id
    int kept = 0;        // of those, how many kept that id through a reload
    int others = 0;      // other instances (not generated)
    int other_kept = 0;  // of those, how many kept the id the probe gave them
};

TEST(Handles, H1Inventory) {
    ASSERT_FALSE(g_corpus.empty());
    std::map<std::string, Evidence> evidence;

    for (const auto& path : g_corpus) {
        auto document = open_path(path);
        if (!document) continue;
        SCOPED_TRACE(path.string());
        MasterScore& master = master_of(*document);

        // The id of every object, and whether the walk is what gave it: the objects
        // the walk leaves alone get one here, so a save can show what it is worth. A
        // generated element stays out of it: it never gets one and its ids do not name
        // anything a caller can hold.
        struct Probed {
            std::string type;
            bool assigned = false;
            std::string detail;
        };
        std::map<ElementId, Probed> probed;
        for (EngravingObject* object : engrave::document_objects(master)) {
            Evidence& row = evidence[object_name(*object)];
            row.declared = engrave::id_class(*object);
            if (engrave::takes_id(*object)) {
                ++row.assigned;
                EXPECT_TRUE(object->eid().isValid()) << object_name(*object) << " has no id";
                probed.emplace(engine_id(*object), Probed{object_name(*object), true, detail(*object)});
            } else if (!object->isEngravingItem() || !static_cast<const EngravingItem&>(*object).generated()) {
                ++row.others;
                if (!object->eid().isValid()) object->assignNewEID();
                probed.emplace(engine_id(*object), Probed{object_name(*object), false, detail(*object)});
            }
        }

        auto reloaded = reopen(*document);
        if (!reloaded) continue;
        std::map<ElementId, std::string> after;
        for (EngravingObject* object : engrave::document_objects(master_of(*reloaded))) {
            if (object->eid().isValid()) after.emplace(engine_id(*object), object_name(*object));
        }
        for (const auto& [id, entry] : probed) {
            const auto found = after.find(id);
            if (found == after.end() || found->second != entry.type) {
                if (entry.assigned)
                    std::fprintf(stderr, "LOST %s %s %s %s\n", path.filename().string().c_str(), entry.type.c_str(),
                                 engrave::to_string(id).c_str(), entry.detail.c_str());
                continue;
            }
            Evidence& row = evidence[entry.type];
            ++(entry.assigned ? row.kept : row.other_kept);
        }
    }

    ASSERT_FALSE(evidence.empty()) << "no corpus score opens";

    std::map<std::string, std::string> observed;
    for (const auto& [type, row] : evidence) {
        if (type == "Segment") {
            observed[type] = class_name(IdClass::address);
        } else if (row.assigned > 0) {
            // The walk hands this type out, so every id it gave must survive a save.
            if (row.kept != row.assigned) {
                ADD_FAILURE() << type << ": only " << row.kept << " of " << row.assigned << " ids survived a save";
                continue;
            }
            observed[type] = class_name(IdClass::persisted);
        } else {
            // The walk hands nothing out for this type, so only its declared class is left
            // to pin: an element layout builds, one an address reaches, or one whose id
            // only the file carries.
            observed[type] = class_name(row.declared);
        }
        EXPECT_EQ(class_name(row.declared), observed[type])
            << type << " is declared " << class_name(row.declared) << ", the corpus shows " << observed[type];
    }

    if (g_update_inventory) {
        nlohmann::json types = nlohmann::json::object();
        for (const auto& [type, value] : observed) types[type] = value;
        nlohmann::json document = {
            {"note", "The id class of every element type the corpus shows. persisted: the writer stores its id and a "
                     "reload restores it; layout: created by layout, so the next pass hands out a new id; address: "
                     "reached through an Address instead; session: alive in memory but not written. This test fails "
                     "when a type is missing here or its class changed."},
            {"types", types},
        };
        std::ofstream out(g_inventory, std::ios::binary);
        out << document.dump(2) << '\n';
        std::printf("wrote %zu types to %s\n", observed.size(), g_inventory.string().c_str());
        return;
    }

    ASSERT_TRUE(std::filesystem::is_regular_file(g_inventory)) << "no " << g_inventory.string() << "; run --update-inventory";
    const auto declared = nlohmann::json::parse(read_text(g_inventory)).at("types");
    for (const auto& [type, value] : observed) {
        ASSERT_TRUE(declared.contains(type)) << type << " is unclassified (" << value << "); add it to " << g_inventory.string();
        EXPECT_EQ(declared.at(type).get<std::string>(), value) << "the class of " << type << " changed";
    }
    for (const auto& [type, value] : declared.items()) {
        if (!observed.contains(type)) std::printf("note: %s is no longer in the corpus\n", type.c_str());
    }
}

// --- H2 -------------------------------------------------------------------

void expect_first_ids(engrave::Document& document) {
    MasterScore& master = master_of(document);
    std::map<std::string, std::string> by_text;
    int exposed = 0;
    for (EngravingObject* object : engrave::document_objects(master)) {
        if (!engrave::takes_id(*object)) continue;
        const mu::engraving::EID eid = object->eid();
        ASSERT_TRUE(eid.isValid()) << object_name(*object) << " has no id";
        // The public text form is the engine's, byte for byte.
        EXPECT_EQ(engrave::to_string(engrave::from_engine(eid)), eid.toStdString());
        const auto [entry, inserted] = by_text.emplace(eid.toStdString(), object_name(*object));
        EXPECT_TRUE(inserted) << "id " << entry->first << " names both " << entry->second << " and " << object_name(*object);
        ++exposed;
    }
    EXPECT_GT(exposed, 0) << "the score exposes no element at all";
    expect_inv(document);
}

TEST(Handles, H2IdsBeforeTheFirstSave) {
    ASSERT_FALSE(g_corpus.empty());
    for (const auto& path : g_corpus) {
        auto document = open_path(path);
        if (!document) continue;
        SCOPED_TRACE(path.string());
        expect_first_ids(*document);
    }

    const auto templates = g_engine->templates();
    ASSERT_GE(templates.size(), 40u);
    for (const auto& info : templates) {
        auto document = g_engine->create_document({.template_path = info.path});
        ASSERT_TRUE(document) << info.id << ": " << document.error().message;
        SCOPED_TRACE("template " + info.id);
        expect_first_ids(**document);
    }
}

// --- H3 -------------------------------------------------------------------

TEST(Handles, H3IdsSurviveSaveAndRestore) {
    ASSERT_FALSE(g_corpus.empty());
    for (const auto& path : g_corpus) {
        auto document = open_path(path);
        if (!document) continue;
        SCOPED_TRACE(path.string());
        const auto before = element_map(*document);
        EXPECT_FALSE(before.empty());

        auto reloaded = reopen(*document);
        if (!reloaded) continue;
        expect_same_ids(before, element_map(*reloaded), "a save and reload");

        // A snapshot restore is the same write and read again.
        auto restored = reopen(*reloaded);
        if (!restored) continue;
        expect_same_ids(before, element_map(*restored), "the second round trip");
        expect_inv(*reloaded);
        expect_inv(*restored);
    }
}

// --- H4 -------------------------------------------------------------------

TEST(Handles, H4ExportLeavesIdsAndRevisionAlone) {
    ASSERT_FALSE(g_corpus.empty());
    for (const auto& path : g_corpus) {
        auto document = open_path(path);
        if (!document) continue;
        SCOPED_TRACE(path.string());
        const auto before = element_map(*document);
        const std::string first = export_mscz(*document);
        const std::string second = export_mscz(*document);
        EXPECT_FALSE(first.empty());
        EXPECT_EQ(first, second) << "two exports of one score differ";
        EXPECT_EQ(element_map(*document), before) << "an export changed an id";
        EXPECT_EQ(document->revision(), 1u);
        expect_inv(*document);
    }
}

// --- H5 -------------------------------------------------------------------

TEST(Handles, H5aLinksAcrossExcerpts) {
    // Excerpt generation links a part's elements as clones in the excerpt score.  It needs
    // notes to link, which a template-built score has none of, so the corpus is the source.
    int checked = 0;
    for (const auto& path : g_corpus) {
        if (path.extension() != ".mscx") continue;
        auto document = open_path(path);
        if (!document) continue;
        SCOPED_TRACE(path.string());
        MasterScore& master = master_of(*document);
        if (master.excerpts().empty()) music::generate_score_excerpts(&master);
        if (master.excerpts().empty()) continue;
        engrave::assign_missing_ids(master);

        // A note of the master and its clone in an excerpt: any note whose link group holds
        // a note of another score.
        Note* master_note = nullptr;
        Note* excerpt_note = nullptr;
        for (const engrave::LiveId& entry : engrave::live_ids(master)) {
            if (master_note || entry.object->type() != ElementType::NOTE) continue;
            if (entry.object->score() != &master) continue;   // the master's own note, not a clone
            for (EngravingObject* linked : entry.object->linkList()) {
                if (!linked->isNote() || linked->score() == &master) continue;
                master_note = mu::engraving::toNote(entry.object);
                excerpt_note = mu::engraving::toNote(linked);
                break;
            }
        }
        if (!master_note || !excerpt_note) continue;
        ++checked;

        const ElementId master_id = engine_id(*master_note);
        const ElementId excerpt_id = engine_id(*excerpt_note);
        EXPECT_NE(master_note->eid(), excerpt_note->eid()) << "the excerpt clone shares the master's id";
        EXPECT_TRUE(live(master, master_id));
        EXPECT_TRUE(live(master, excerpt_id));

        // A property set on the master propagates to the excerpt clone.
        const muse::draw::Color red(255, 0, 0);
        commit(master, [&](MasterScore&) {
            master_note->undoChangeProperty(mu::engraving::Pid::COLOR, red, mu::engraving::PropertyFlags::NOSTYLE);
        });
        EXPECT_EQ(excerpt_note->color(), red) << "the property did not reach the linked element";
        EXPECT_TRUE(live(master, master_id)) << "a property edit lost the element's id";

        // Deleting the master element takes the linked one with it.  The engine's deletion
        // command is used, not a bare element removal: deleting the last note of a chord
        // leaves a rest in both scores, because a chord without notes is not a valid model
        // element (layout reads its first note).
        commit(master, [&](MasterScore&) {
            master.select(master_note);
            master.cmdDeleteSelection();
        });
        EXPECT_FALSE(live(master, master_id)) << "the removed element still resolves";
        EXPECT_FALSE(live(master, excerpt_id)) << "the linked clone survived the removal";
        expect_inv(*document);
        break;   // one linked score is what this test needs; the first with notes is enough
    }
    EXPECT_GT(checked, 0) << "no corpus score has a linked note";
}

TEST(Handles, H5bRemovingAPartLeavesNoId) {
    auto document = open_path(corpus_file({".mscx"}));
    ASSERT_TRUE(document);
    MasterScore& master = master_of(*document);
    // A save gives the parts and their staves their ids.
    EXPECT_FALSE(export_mscz(*document).empty());
    expect_inv(*document);

    std::set<std::string> seen;
    for (const engrave::LiveId& entry : engrave::live_ids(master)) seen.insert(engrave::to_string(entry.id));

    const mu::engraving::InstrumentIndex index = mu::engraving::searchTemplateIndexForId(muse::String(u"piano"));
    ASSERT_NE(index.instrTemplate, nullptr);
    for (int round = 0; round < 3; ++round) {
        master.appendPart(index.instrTemplate);
        master.undoStack()->clearHistory();
        ASSERT_GT(master.parts().size(), 1u);
        Part* part = master.parts().back();
        engrave::assign_missing_ids(master);
        const ElementId part_id = engine_id(*part);
        EXPECT_TRUE(seen.insert(engrave::to_string(part_id)).second) << "a new part inherited an id";
        for (Staff* staff : part->staves()) {
            if (!staff->eid().isValid()) continue;
            EXPECT_TRUE(seen.insert(id_text(*staff)).second) << "a new staff inherited an id";
        }
        expect_inv(*document);

        master.cmdRemovePart(part);
        master.undoStack()->clearHistory();
        engrave::assign_missing_ids(master);
        expect_inv(*document);
        EXPECT_FALSE(live(master, part_id)) << "the removed part still resolves";
    }

    // An excerpt is a score, not an element, and leaves no entry either.
    if (master.excerpts().empty()) music::generate_score_excerpts(&master);
    ASSERT_FALSE(master.excerpts().empty());
    engrave::assign_missing_ids(master);
    mu::engraving::Excerpt* excerpt = master.excerpts().front();
    const ElementId excerpt_id = engine_id(*excerpt->excerptScore());
    expect_inv(*document);
    master.removeExcerpt(excerpt);
    delete excerpt;
    master.undoStack()->clearHistory();
    engrave::assign_missing_ids(master);
    expect_inv(*document);
    EXPECT_FALSE(live(master, excerpt_id)) << "the removed excerpt still resolves";

    // The same round through the edit API: a part and its staves get fresh ids and
    // leave none behind.
    for (int round = 0; round < 3; ++round) {
        auto added = document->edit(engrave::Batch{{engrave::AddPart{"piano", std::nullopt}}});
        ASSERT_TRUE(added) << added.error().message;
        ASSERT_FALSE(added->ops.empty());
        ASSERT_FALSE(added->ops.front().created.empty());
        const ElementId part_id = *added->ops.front().created.front();
        for (const auto& id : added->ops.front().created) {
            ASSERT_TRUE(id) << "a created part or staff has no id";
            EXPECT_TRUE(seen.insert(engrave::to_string(*id)).second) << "a new part or staff inherited an id";
        }
        expect_inv(*document);

        auto removed = document->edit(engrave::Batch{{engrave::RemovePart{part_id}}});
        ASSERT_TRUE(removed) << removed.error().message;
        expect_inv(*document);
        EXPECT_FALSE(live(master, part_id)) << "the removed part still resolves";
    }
}

// --- H7 -------------------------------------------------------------------

TEST(Handles, H7RemovedElementsGoStale) {
    // The removal runs as an edit batch, through the engine's own deletion: deleteItem
    // turns a chord whose only note goes into a rest. (A bare undoRemoveElement of such
    // a note leaves an empty chord, which layout cannot handle.)
    auto document = open_with(ElementType::NOTE);
    ASSERT_TRUE(document) << "no corpus score has a note with an id";
    MasterScore& master = master_of(*document);
    const auto before = element_map(*document);
    const ElementId id = engine_id(*first_live(master, ElementType::NOTE));
    const auto remove = [&master, id](bool roll_back) {
        return engrave::api::Step([&master, id, roll_back](engrave::api::EditContext&) -> engrave::Result<void> {
            master.deleteItem(mu::engraving::toEngravingItem(engrave::resolve(master, id)->object));
            // Inside the batch the element is out of the score but not yet destroyed.
            const auto during = engrave::resolve(master, id);
            EXPECT_FALSE(during) << "a removed element resolves inside its own batch";
            if (!during) EXPECT_EQ(during.error().code, ErrorCode::StaleHandle);
            if (roll_back) return std::unexpected(engrave::Error{ErrorCode::EditRejected, "rolled back by the test"});
            return {};
        });
    };

    // The rollback brings the element back with its id.
    const auto rolled_back = engrave::api::BatchGuard(*document).run({remove(true)}, engrave::api::Policy::Transaction, {});
    ASSERT_FALSE(rolled_back);
    EXPECT_EQ(rolled_back.error().op_index, 0u);
    EXPECT_EQ(document->revision(), 1u);
    EXPECT_TRUE(live(master, id));
    expect_same_ids(before, element_map(*document), "a rollback");
    expect_inv(*document);

    // Committed, the history cleanup destroys the element, and the id names nothing.
    const auto committed = engrave::api::BatchGuard(*document).run({remove(false)}, engrave::api::Policy::Transaction, {});
    ASSERT_TRUE(committed) << committed.error().message;
    EXPECT_EQ(document->revision(), 2u);
    EXPECT_TRUE(std::ranges::contains(committed->removed, id) || std::ranges::contains(committed->replaced, id))
        << "the batch does not report the removed id";
    const auto gone = engrave::resolve(master, id);
    ASSERT_FALSE(gone) << "the removed element still resolves after the commit";
    EXPECT_EQ(gone.error().code, ErrorCode::NotFound);
    expect_inv(*document);
}

// --- H8 -------------------------------------------------------------------

TEST(Handles, H8ReplacedElementsLoseTheirId) {
    auto document = open_with(ElementType::CLEF);
    ASSERT_TRUE(document) << "no corpus score has a clef with an id";
    MasterScore& master = master_of(*document);

    // A ChangeElement swaps the element for a new one: the old id goes, and a new one
    // takes its place at the same position.
    EngravingItem* clef = mu::engraving::toEngravingItem(first_live(master, ElementType::CLEF));
    ASSERT_NE(clef, nullptr);
    Measure* measure = master.tick2measure(clef->tick());
    ASSERT_NE(measure, nullptr);
    Segment* clef_segment = nullptr;
    for (Segment* segment = measure->first(); segment && !clef_segment; segment = segment->next()) {
        if (segment->element(clef->track()) == clef) clef_segment = segment;
    }
    ASSERT_NE(clef_segment, nullptr) << "no segment holds the clef";
    const ElementId clef_id = engine_id(*clef);
    const auto before = engrave::live_ids(master);
    commit(master, [&](MasterScore&) {
        auto* replacement = mu::engraving::Factory::createClef(clef_segment);
        replacement->setClefType(mu::engraving::ClefType::F);
        replacement->setTrack(clef->track());
        master.undo(new mu::engraving::ChangeElement(clef, replacement));
    });
    const engrave::IdDiff diff = engrave::diff_ids(before, engrave::live_ids(master));
    EXPECT_FALSE(diff.replaced.empty()) << "the replaced clef kept its id";
    EXPECT_FALSE(diff.created.empty());
    for (const ElementId& id : diff.replaced) {
        EXPECT_FALSE(live(master, id)) << "a replaced id still resolves";
    }
    EXPECT_FALSE(live(master, clef_id)) << "the replaced clef still resolves";
    expect_inv(*document);

    // A duration change keeps a chord whose new length fits where it is (the length is
    // a property then) and rebuilds it otherwise; either way the diff and the resolver
    // agree on whether the old id still names the chord.
    mu::engraving::ChordRest* chord_rest = mu::engraving::toChordRest(first_live(master, ElementType::CHORD));
    ASSERT_NE(chord_rest, nullptr) << "the walk hands out no chord";
    const ElementId chord_id = engine_id(*chord_rest);
    const auto before_change = engrave::live_ids(master);
    commit(master, [&](MasterScore&) { master.changeCRlen(chord_rest, mu::engraving::Fraction(1, 2)); });
    const engrave::IdDiff change = engrave::diff_ids(before_change, engrave::live_ids(master));
    const bool dropped = std::ranges::contains(change.replaced, chord_id) || std::ranges::contains(change.removed, chord_id);
    EXPECT_NE(live(master, chord_id), dropped) << "the diff and the resolver disagree about the chord";
    expect_inv(*document);
}

// --- crafted input --------------------------------------------------------

/// A current-version MSCX of an imported corpus score, plus one of its excerpts:
/// the files the 5.00 reader reads, which is the reader that repairs ids.
struct CraftedInput {
    std::string master;
    std::string excerpt;
};

CraftedInput crafted_input() {
    CraftedInput input;
    for (const auto& path : g_corpus) {
        const std::string extension = path.extension().string();
        if (extension != ".musicxml" && extension != ".mxl" && extension != ".mid") continue;
        auto document = open_path(path);
        if (!document) continue;
        MasterScore& master = master_of(*document);
        if (master.excerpts().empty()) music::generate_score_excerpts(&master);
        engrave::assign_missing_ids(master);
        std::string written = export_mscx(*document);
        if (written.find("version=\"5.00\"") == std::string::npos) continue;
        input.master = std::move(written);
        if (!master.excerpts().empty()) input.excerpt = export_mscx(*document, 0);
        return input;
    }
    return input;
}

/// The spans of the text inside every <eid> element.
std::vector<std::pair<std::size_t, std::size_t>> eid_spans(const std::string& text) {
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    const std::string open = "<eid>";
    const std::string close = "</eid>";
    for (std::size_t at = text.find(open); at != std::string::npos; at = text.find(open, at)) {
        const std::size_t start = at + open.size();
        const std::size_t end = text.find(close, start);
        if (end == std::string::npos) break;
        spans.emplace_back(start, end);
        at = end;
    }
    return spans;
}

/// The text with the n-th id replaced.
std::string with_eid(std::string text, std::size_t index, std::string_view value) {
    const auto spans = eid_spans(text);
    if (index >= spans.size()) return text;
    text.replace(spans[index].first, spans[index].second - spans[index].first, value);
    return text;
}

/// The text with every id `from` renamed to `to`, which gives two elements one id.
std::string with_copied_id(std::string text, std::string_view from, std::string_view to) {
    const std::string needle = ">" + std::string(from) + "<";
    const std::string replacement = ">" + std::string(to) + "<";
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + replacement.size())) {
        text.replace(at, needle.size(), replacement);
    }
    return text;
}

/// A corpus MSCX file that has ids of its own and, if asked, a tag.
std::filesystem::path corpus_mscx(std::string_view tag) {
    for (const auto& path : g_corpus) {
        if (path.extension() != ".mscx") continue;
        const std::string text = read_text(path);
        if (text.find("<eid>") == std::string::npos) continue;
        if (!tag.empty() && text.find(tag) == std::string::npos) continue;
        return path;
    }
    return {};
}

/// The id texts of a corpus file, with the type of the element each names.
std::map<std::string, std::string> id_types(const std::string& text) {
    std::map<std::string, std::string> types;
    auto document = open_text(text);
    if (!document) return types;
    for (const engrave::LiveId& entry : engrave::live_ids(master_of(*document))) {
        types.emplace(engrave::to_string(entry.id), object_name(*entry.object));
    }
    return types;
}

bool warned_about(engrave::Document& document, std::string_view text) {
    for (const auto& diagnostic : document.diagnostics()) {
        if (diagnostic.severity == engrave::Severity::Warning && diagnostic.message.find(text) != std::string::npos) return true;
    }
    return false;
}

// --- H9 -------------------------------------------------------------------

/// Whether the crafted text gives the reader a duplicate to repair.
bool reader_sees_duplicate(const std::string& text) {
    auto document = open_text(text);
    if (!document) return false;
    return master_of(*document).eidRegister()->repairs().duplicate > 0;
}

void expect_repaired(const std::string& text) {
    auto document = open_text(text);
    ASSERT_TRUE(document);
    MasterScore& master = master_of(*document);
    // If the reader keeps both elements it repairs the duplicate; if it rebuilds one of
    // them, there is nothing left to repair and the register is consistent either way.
    const bool repaired = master.eidRegister()->repairs().any();
    if (repaired) EXPECT_TRUE(warned_about(*document, "shared an id"));
    EXPECT_FALSE(engrave::live_ids(master).empty());
    expect_inv(*document);

    // The repaired score is usable: it saves and comes back with the same ids.
    const auto before = element_map(*document);
    auto reloaded = reopen(*document);
    if (reloaded) {
        EXPECT_EQ(element_map(*reloaded), before);
        expect_inv(*reloaded);
    }

    // A caller that cannot accept the repair asks for strict ids instead.
    auto strict = g_engine->open(as_bytes(text), {.strict_ids = true});
    if (repaired) {
        ASSERT_FALSE(strict);
        EXPECT_EQ(strict.error().code, ErrorCode::InvalidInput);
    } else {
        EXPECT_TRUE(strict) << "a dropped duplicate does not need strict ids to reject it";
    }
}

TEST(Handles, H9DuplicateIdsInTheInput) {
    const CraftedInput input = crafted_input();
    if (input.master.empty()) GTEST_SKIP() << "no imported corpus score to write a current MSCX from";
    const auto types = id_types(input.master);
    ASSERT_GE(types.size(), 4u);

    // (a) two elements of the same type claim one id, (b) two types share one id.  Only
    // ids the file itself carries can be duplicated.
    const auto spans = eid_spans(input.master);
    std::set<std::string> in_file;
    for (const auto& [start, end] : spans) in_file.insert(input.master.substr(start, end - start));
    std::map<std::string, std::vector<std::string>> by_type;
    for (const auto& [id, type] : types) {
        if (in_file.contains(id)) by_type[type].push_back(id);
    }
    std::string same_a;
    std::string same_b;
    std::string other;
    std::string same_type;
    for (const auto& [type, ids] : by_type) {
        if (!same_a.empty() || ids.size() < 2) continue;
        same_type = type;
        same_a = ids[0];
        same_b = ids[1];
    }
    for (const auto& [type, ids] : by_type) {
        if (ids.empty() || type == same_type) continue;
        other = ids.front();
        break;
    }
    ASSERT_FALSE(same_b.empty()) << "no two elements share a type";
    ASSERT_FALSE(other.empty()) << "the score has only one element type";

    // (a) two elements of the same type claim one id, (b) two types share one id.  Only
    // ids the file itself carries can be duplicated, and only where the reader keeps both
    // elements: it replaces some of what it read (a measure it rebuilt, for one).
    std::string duplicate;
    for (const auto& [type, ids] : by_type) {
        if (ids.size() < 2) continue;
        std::string candidate = with_copied_id(input.master, ids[1], ids[0]);
        if (candidate == input.master || !reader_sees_duplicate(candidate)) continue;
        duplicate = std::move(candidate);
        break;
    }
    ASSERT_FALSE(duplicate.empty()) << "no two elements of one type that the reader keeps share an id";
    expect_repaired(duplicate);

    bool crossed = false;
    for (const auto& [from_type, from_ids] : by_type) {
        for (const auto& [to_type, to_ids] : by_type) {
            if (from_type == to_type || from_ids.empty() || to_ids.empty()) continue;
            std::string candidate = with_copied_id(input.master, from_ids.front(), to_ids.front());
            if (candidate == input.master || !reader_sees_duplicate(candidate)) continue;
            expect_repaired(candidate);
            crossed = true;
            break;
        }
        if (crossed) break;
    }
    EXPECT_TRUE(crossed) << "two types cannot be given one id";

    // (c) an element a <linkedTo> names appears twice: the link still has one target.
    if (!input.excerpt.empty()) {
        const std::size_t at = input.excerpt.find("<linkedTo>");
        if (at != std::string::npos) {
            const std::size_t start = at + std::string("<linkedTo>").size();
            const std::string target = input.excerpt.substr(start, input.excerpt.find("</linkedTo>", start) - start);
            const auto excerpt_types = id_types(input.excerpt);
            const auto candidate = std::ranges::find_if(excerpt_types, [&target](const auto& entry) {
                return entry.first != target;
            });
            ASSERT_TRUE(candidate != excerpt_types.end());
            expect_repaired(with_copied_id(input.excerpt, candidate->first, target));
        }
    }
}

// --- H10 ------------------------------------------------------------------

TEST(Handles, H10MalformedIdsInTheInput) {
    const CraftedInput input = crafted_input();
    if (input.master.empty()) GTEST_SKIP() << "no imported corpus score to write a current MSCX from";
    const std::string& source = input.master;
    const auto spans = eid_spans(source);
    ASSERT_GE(spans.size(), 8u);
    const std::size_t victim = 5;

    const std::vector<std::string> malformed = {
        "",                          // empty
        "A",                         // no separator
        "A_B_C",                     // three halves
        "!_A",                       // a character outside the alphabet
        "AAAAAAAAAAAA_AAAAAAAAAAAA", // halves longer than 11 characters
        "_",                         // two empty halves
    };
    for (const std::string& value : malformed) {
        SCOPED_TRACE("eid '" + value + "'");
        const std::string text = with_eid(source, victim, value);
        ASSERT_NE(text, source);
        auto document = open_text(text);
        ASSERT_TRUE(document);
        MasterScore& master = master_of(*document);
        EXPECT_EQ(master.eidRegister()->repairs().malformed, 1u);
        EXPECT_TRUE(warned_about(*document, "malformed"));
        EXPECT_FALSE(engrave::live_ids(master).empty());
        expect_inv(*document);
        // The element got a fresh id, so a caller can still name it.
        EXPECT_TRUE(live(master, engrave::live_ids(master).front().id));

        auto strict = g_engine->open(as_bytes(text), {.strict_ids = true});
        ASSERT_FALSE(strict) << "strict ids accepted '" << value << "'";
        EXPECT_EQ(strict.error().code, ErrorCode::InvalidInput);
    }

    // Two texts that differ but decode to one id: a half keeps its low 64 bits, so
    // the top two bits of an 11-character half do not count.
    const std::string colliding = with_eid(with_eid(source, victim, "AAAAAAAAAAP_AAAAAAAAAAP"), victim + 1, "AAAAAAAAAAf_AAAAAAAAAAf");
    auto document = open_text(colliding);
    ASSERT_TRUE(document);
    EXPECT_EQ(master_of(*document).eidRegister()->repairs().duplicate, 1u);
    expect_inv(*document);
}

// --- H11 ------------------------------------------------------------------

TEST(Handles, H11StaleHandles) {
    ASSERT_GE(g_corpus.size(), 2u);
    const auto path = corpus_file({".mscx"});
    ASSERT_FALSE(path.empty());
    ElementId id;
    {
        auto document = open_path(path);
        ASSERT_TRUE(document);
        Note* note = first_note(master_of(*document));
        ASSERT_NE(note, nullptr);
        id = engine_id(*note);
        EXPECT_TRUE(live(master_of(*document), id));
        expect_inv(*document);
    }

    // Ids are document-scoped: the same file opened twice hands out the same ids, so
    // an id of the closed document is a documented false match in the new one.
    auto same = open_path(path);
    ASSERT_TRUE(same);
    MasterScore& master = master_of(*same);
    const auto same_ids = engrave::live_ids(master);
    const bool present = std::ranges::any_of(same_ids, [&id](const engrave::LiveId& entry) { return entry.id == id; });
    EXPECT_EQ(present, live(master, id)) << "the same file gave a different id set, or the id resolves differently";

    // An id of another document is not found.
    auto other = open_path(corpus_file({".musicxml"}));
    ASSERT_TRUE(other);
    MasterScore& other_master = master_of(*other);
    std::set<std::string> other_texts;
    for (const engrave::LiveId& entry : engrave::live_ids(other_master)) other_texts.insert(engrave::to_string(entry.id));
    int checked = 0;
    for (const engrave::LiveId& entry : same_ids) {
        if (other_texts.contains(engrave::to_string(entry.id))) continue;
        const auto resolved = engrave::resolve(other_master, entry.id);
        ASSERT_FALSE(resolved) << "another document's id resolves";
        EXPECT_EQ(resolved.error().code, ErrorCode::NotFound);
        ++checked;
        break;
    }
    EXPECT_EQ(checked, 1);

    // An id the snapshot path preserved still works.
    auto restored = reopen(*same);
    ASSERT_TRUE(restored);
    EXPECT_TRUE(live(master_of(*restored), id));

    // Generated and layout elements never carry an id, so no caller can hold one.
    for (EngravingObject* object : engrave::document_objects(master)) {
        if (engrave::id_class(*object) == IdClass::persisted) continue;
        if (engrave::id_class(*object) == IdClass::session) continue; // the file stores one
        EXPECT_FALSE(object->eid().isValid()) << object_name(*object) << " has an id";
    }
    for (EngravingObject* object : engrave::document_objects(master)) {
        if (!object->isEngravingItem() || !static_cast<EngravingItem&>(*object).generated()) continue;
        EXPECT_FALSE(engrave::takes_id(*object)) << object_name(*object) << " is handed out";
    }
    expect_inv(*same);
}

// --- H12 ------------------------------------------------------------------

TEST(Handles, H12Addresses) {
    auto document = open_path(corpus_file({".mscx"}));
    ASSERT_TRUE(document);
    MasterScore& master = master_of(*document);

    Segment* segment = first_segment(master, SegmentType::ChordRest);
    ASSERT_NE(segment, nullptr);
    const auto address = engrave::address_of(*segment);
    ASSERT_TRUE(address) << "a chord-rest segment has no address";
    const auto first = engrave::resolve(master, *address);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ(first->object, segment);
    EXPECT_TRUE(first->writable);

    // Unrelated edits leave the address alone.
    Note* note = first_note(master);
    ASSERT_NE(note, nullptr);
    commit(master, [&](MasterScore&) {
        note->undoChangeProperty(mu::engraving::Pid::COLOR, muse::draw::Color(0, 0, 255), mu::engraving::PropertyFlags::NOSTYLE);
    });
    const auto second = engrave::resolve(master, *address);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(second->object, segment);

    // The address fails once its measure is gone.
    Measure* measure = segment->measure()->nextMeasure();
    if (!measure) measure = segment->measure();
    Segment* doomed = measure->first(SegmentType::ChordRest);
    ASSERT_NE(doomed, nullptr);
    const auto doomed_address = engrave::address_of(*doomed);
    ASSERT_TRUE(doomed_address);
    commit(master, [&](MasterScore&) { master.deleteMeasures(measure, measure); });
    const auto gone = engrave::resolve(master, *doomed_address);
    EXPECT_FALSE(gone) << "an address of a removed measure still resolves";
    expect_inv(*document);

    // Layout addresses are read-only and name the layout object.
    const auto page = engrave::resolve(master, engrave::PageAddress{.score = {}, .index = 0});
    ASSERT_TRUE(page) << page.error().message;
    EXPECT_FALSE(page->writable);
    ASSERT_EQ(page->object->type(), ElementType::PAGE);
    const auto page_address = engrave::address_of(*page->object);
    ASSERT_TRUE(page_address);
    const auto page_again = engrave::resolve(master, *page_address);
    ASSERT_TRUE(page_again);
    EXPECT_EQ(page_again->object, page->object);

    const auto system = engrave::resolve(master, engrave::SystemAddress{.score = {}, .page = 0, .index = 0});
    ASSERT_TRUE(system) << system.error().message;
    EXPECT_FALSE(system->writable);
    ASSERT_EQ(system->object->type(), ElementType::SYSTEM);
    const auto system_address = engrave::address_of(*system->object);
    ASSERT_TRUE(system_address);
    const auto system_again = engrave::resolve(master, *system_address);
    ASSERT_TRUE(system_again);
    EXPECT_EQ(system_again->object, system->object);

    // A missing position is a stale handle, an unknown kind a caller error.
    const auto missing = engrave::resolve(master, engrave::PageAddress{.score = {}, .index = 9999});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::StaleHandle);
    auto bad_kind = std::get<engrave::SegmentAddress>(*address);
    bad_kind.kind = "NoSuchSegment";
    const auto bad = engrave::resolve(master, engrave::Address{bad_kind});
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, ErrorCode::InvalidArgument);
}

// --- H13 ------------------------------------------------------------------

TEST(Handles, H13DeterministicIds) {
    // The engine is one per process and deterministic ids are a process-global
    // switch, so the check runs in a child process of this same binary.
    ASSERT_FALSE(g_deterministic) << "already the deterministic child";
    std::string command = "\"" + g_program + "\" --deterministic --gtest_filter=Handles.H13IdsWithDeterministicEngine";
#ifdef _WIN32
    command = "call " + command; // cmd strips the first quote of a command that starts with one
#endif
    for (const auto& argument : g_engine_args) command += " \"" + argument + "\"";
    const std::filesystem::path log = std::filesystem::temp_directory_path() / "engrave-handles-h13.log";
    command += " > \"" + log.string() + "\" 2>&1";
    const int status = std::system(command.c_str());
    if (status != 0) {
        std::fprintf(stderr, "the deterministic child failed: %s\n%s\n", command.c_str(), read_text(log).c_str());
    }
    EXPECT_EQ(status, 0) << "the deterministic child failed";
}

TEST(Handles, H13IdsWithDeterministicEngine) {
    if (!g_deterministic) GTEST_SKIP() << "only meaningful in the --deterministic child";
    auto first = open_path(g_corpus.front());
    ASSERT_TRUE(first);
    auto second = open_path(g_corpus.front());
    ASSERT_TRUE(second);
    EXPECT_EQ(element_map(*second), element_map(*first)) << "two opens of one file disagree";
    expect_inv(*first);
    expect_inv(*second);
}

// --- corpus discovery -----------------------------------------------------

bool looks_like_score(const std::filesystem::path& path) {    static const std::set<std::string> extensions = {
        ".mscx", ".mscz", ".musicxml", ".mxl", ".mid", ".midi", ".gp", ".gp3", ".gp4", ".gp5", ".gpx", ".ptb"
    };
    return extensions.contains(path.extension().string());
}

void collect(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        std::vector<std::filesystem::path> entries;
        for (const auto& entry : std::filesystem::directory_iterator(path, ec)) entries.push_back(entry.path());
        std::ranges::sort(entries);
        for (const auto& entry : entries) collect(entry);
        return;
    }
    if (std::filesystem::is_regular_file(path, ec) && looks_like_score(path)) g_corpus.push_back(path);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    g_program = argv[0];
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument.starts_with("--assets=")) {
            g_assets = std::filesystem::path(argument.substr(9));
            g_engine_args.emplace_back(argument);
        } else if (argument.starts_with("--corpus=")) {
            collect(std::filesystem::path(argument.substr(9)));
            g_engine_args.emplace_back(argument);
        } else if (argument == "--deterministic") {
            g_deterministic = true;
        } else if (argument == "--update-inventory") {
            g_update_inventory = true;
        }
    }
    if (g_corpus.empty()) {
        for (const auto& candidate : {std::filesystem::path("fixtures/golden"), std::filesystem::path("../fixtures/golden"),
                                      std::filesystem::path("../../fixtures/golden")}) {
            std::error_code ec;
            if (std::filesystem::is_directory(candidate, ec)) collect(candidate);
        }
    }
    if (g_corpus.empty()) {
        std::fprintf(stderr, "no corpus: pass --corpus=<file or directory>\n");
        return 1;
    }

    auto engine = engrave::Engine::create({.resources = g_assets, .deterministic_ids = g_deterministic});
    if (!engine) {
        std::fprintf(stderr, "Engine::create: %s (pass --assets=<resources>)\n", engine.error().message.c_str());
        return 1;
    }
    g_engine = std::move(*engine);
    const int result = RUN_ALL_TESTS();
    g_engine.reset();
    return result;
}
