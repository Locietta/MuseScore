// Tests of the describe catalogue, of the value codec and of the pid support table.
//
//   engrave_describe_test --assets=<resources> [--python=<interpreter>] <score file or directory>...
//
// The score files are the corpus: they supply one live instance per element type,
// so the table can be checked against the engine itself.  A type the corpus does
// not contain is skipped and counted.  --python enables the generator check
// (tools/gen_pid_support.py --check); without it that test is skipped.
//
// The corpus is read with the library's own loader (music::load_native_score), not
// through a Document, because the engine's objects are what the table is about.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <engrave/engrave.h>

#include "engraving/dom/excerpt.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/part.h"
#include "engraving/dom/property.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/staff.h"
#include "engraving/engravingproject.h"
#include "engraving/types/groupnode.h"
#include "engraving/types/pitchvalue.h"
#include "engraving/types/typesconv.h"

#include "engrave/src/api/pid_support.h"
#include "engrave/src/api/sniff.h"
#include "engrave/src/api/value_codec.h"
#include "engrave/src/runtime/noqt_runtime.h"

namespace {

using engrave::Enum;
using engrave::ErrorCode;
using engrave::Opaque;
using engrave::Point;
using engrave::Size;
using engrave::Unit;
using engrave::Value;
using engrave::ValueKind;
using mu::engraving::ElementType;
using mu::engraving::EngravingObject;
using mu::engraving::P_TYPE;
using mu::engraving::Pid;
using mu::engraving::PropertyValue;

std::filesystem::path g_assets;
std::filesystem::path g_python;
std::vector<std::filesystem::path> g_corpus_files;
std::unique_ptr<engrave::Engine> g_engine;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

const char* backend_name(engrave::Format format) {
    switch (format) {
    case engrave::Format::Mscz: return "mscz";
    case engrave::Format::Mscx: return "mscx";
    case engrave::Format::MusicXml: return "musicxml";
    case engrave::Format::Mxl: return "mxl";
    case engrave::Format::Midi: return "midi";
    case engrave::Format::GuitarPro: return "gp";
    }
    return "";
}

// One loaded corpus score per file, with the first live instance of each type.
struct Corpus {
    std::vector<music::ScoreProject> projects;
    std::map<ElementType, EngravingObject*> instances;
    std::vector<std::filesystem::path> unreadable;

    void add(EngravingObject* object) {
        instances.try_emplace(object->type(), object);
    }

    // The engine's scanElements visits an item's children, not the item, so the
    // walk adds every level itself: the score's parts and staves, then each
    // measure's segments and their elements.
    void add_score(mu::engraving::Score* score) {
        add(score);
        for (mu::engraving::Part* part : score->parts()) {
            add(part);
            for (mu::engraving::Staff* staff : part->staves()) add(staff);
        }
        const auto scan = [this](mu::engraving::EngravingItem* item) {
            add(item);
            item->scanElements([this](mu::engraving::EngravingItem* child) { add(child); });
        };
        for (mu::engraving::MeasureBase* base = score->first(); base; base = base->next()) {
            add(base);
            if (!base->isMeasure()) continue;
            auto* measure = mu::engraving::toMeasure(base);
            add(measure);
            for (mu::engraving::Segment* segment = measure->first(); segment; segment = segment->next()) {
                add(segment);
                for (mu::engraving::track_idx_t track = 0; track < score->nstaves() * mu::engraving::VOICES; ++track)
                    if (mu::engraving::EngravingItem* item = segment->element(track)) scan(item);
                for (mu::engraving::EngravingItem* item : segment->annotations()) scan(item);
            }
        }
        // The engine's own walk reaches the system-level items (pages, systems,
        // instrument names, brackets) that the measure walk above does not.
        score->scanElements([this](mu::engraving::EngravingItem* item) { add(item); });
    }

    void load(const std::filesystem::path& file) {
        const std::string bytes = read_file(file);
        const auto format = engrave::sniff_format(bytes);
        if (!format) {
            unreadable.push_back(file);
            return;
        }
        try {
            projects.push_back(music::load_native_score(bytes, backend_name(*format)));
        } catch (const std::exception&) {
            unreadable.push_back(file);
            return;
        }
        mu::engraving::MasterScore* master = projects.back()->masterScore();
        add_score(master);
        for (mu::engraving::Excerpt* excerpt : master->excerpts()) add_score(excerpt->excerptScore());
    }
};

bool is_enum_without_tokens(P_TYPE type) {
    for (const P_TYPE declared :
         {P_TYPE::SYMID, P_TYPE::ALIGN, P_TYPE::ORNAMENT_INTERVAL, P_TYPE::ACCIDENTAL_ROLE, P_TYPE::HOOK_TYPE})
        if (declared == type) return true;
    return false;
}

std::vector<P_TYPE> all_types() {
    std::vector<P_TYPE> types;
    for (int type = 0; type <= int(P_TYPE::GROUPS); ++type) types.push_back(P_TYPE(type));
    return types;
}

// A value of each kind, and an enum token for each enum type.
Value sample(P_TYPE type) {
    switch (engrave::api::kind_of(type)) {
    case ValueKind::Bool: return true;
    case ValueKind::Int: return std::int64_t(42);
    case ValueKind::Real: return 1.5;
    case ValueKind::String: return std::string("text");
    case ValueKind::Point: return Point{1.5, -2.5};
    case ValueKind::Size: return Size{3.5, 4.5};
    case ValueKind::Color: return engrave::Color{1, 2, 3, 4};
    case ValueKind::Fraction: return engrave::Fraction{3, 4};
    case ValueKind::IntList: return std::vector<std::int64_t>{1, 2, 3};
    case ValueKind::Enum: {
        const auto tokens = engrave::api::enum_tokens(type);
        if (!tokens.empty()) return Enum{std::string(tokens.front())};
        switch (type) {
        case P_TYPE::SYMID: return Enum{"noteheadBlack"};
        case P_TYPE::ALIGN: return Enum{"left,top"};
        case P_TYPE::ORNAMENT_INTERVAL: return Enum{"second,auto"};
        case P_TYPE::ACCIDENTAL_ROLE: return Enum{"0"};
        case P_TYPE::HOOK_TYPE: return Enum{"0"};
        default: return Enum{"unspecified"};
        }
    }
    case ValueKind::Opaque: return Opaque{"read-only"};
    }
    return {};
}

// A property value each Opaque kind maps from.
PropertyValue opaque_sample(P_TYPE type) {
    switch (type) {
    case P_TYPE::DRAW_PATH: return PropertyValue(muse::draw::PainterPath());
    case P_TYPE::GROUPS: return PropertyValue(mu::engraving::GroupNodes());
    case P_TYPE::PITCH_VALUES: return PropertyValue(mu::engraving::PitchValues());
    case P_TYPE::DURATION_TYPE_WITH_DOTS: return PropertyValue(mu::engraving::DurationTypeWithDots());
    default: return PropertyValue();
    }
}

ElementType element_type(std::string_view name) {
    return mu::engraving::TConv::fromXml(muse::AsciiStringView(name.data(), name.size()), ElementType::INVALID);
}

P_TYPE value_type(std::string_view name) {
    return mu::engraving::propertyType(engrave::api::pid_of(name));
}

bool contains(const std::vector<std::string>& list, std::string_view text) {
    return std::ranges::find(list, text) != list.end();
}

} // namespace

TEST(ValueCodec, RoundTripsEveryType) {
    int opaque = 0;
    for (const P_TYPE type : all_types()) {
        if (type == P_TYPE::UNDEFINED) {
            EXPECT_TRUE(std::holds_alternative<std::monostate>(engrave::api::to_value(type, PropertyValue())));
            continue;
        }
        if (engrave::api::kind_of(type) == ValueKind::Opaque) {
            EXPECT_TRUE(std::holds_alternative<Opaque>(engrave::api::to_value(type, opaque_sample(type))))
                << int(type);
            ++opaque;
            continue;
        }
        const Value value = sample(type);
        const auto property = engrave::api::from_value(type, value);
        ASSERT_TRUE(property) << int(type) << ": " << property.error().message;
        EXPECT_EQ(engrave::api::to_value(type, *property), value) << int(type);
    }
    EXPECT_EQ(opaque, 4) << "DRAW_PATH, GROUPS, PITCH_VALUES and DURATION_TYPE_WITH_DOTS";
}

TEST(ValueCodec, RoundTripsEveryEnumToken) {
    int tokens = 0;
    for (const P_TYPE type : all_types()) {
        if (engrave::api::kind_of(type) != ValueKind::Enum) continue;
        const auto listed = engrave::api::enum_tokens(type);
        EXPECT_TRUE(!listed.empty() || is_enum_without_tokens(type)) << int(type) << " lists no tokens";
        for (const std::string_view token : listed) {
            const auto property = engrave::api::from_value(type, Enum{std::string(token)});
            ASSERT_TRUE(property) << int(type) << " token '" << token << "': " << property.error().message;
            EXPECT_EQ(engrave::api::to_value(type, *property), Value(Enum{std::string(token)})) << int(type);
            ++tokens;
        }
    }
    EXPECT_GT(tokens, 400) << "the enum token tables";
}

TEST(ValueCodec, RejectsWhatDoesNotFitTheType) {
    const auto unknown = engrave::api::from_value(P_TYPE::ALIGN_H, Enum{"no-such-token"});
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, ErrorCode::InvalidArgument);

    const auto wrong_kind = engrave::api::from_value(P_TYPE::BOOL, std::int64_t(1));
    ASSERT_FALSE(wrong_kind);
    EXPECT_EQ(wrong_kind.error().code, ErrorCode::InvalidArgument);

    const auto wrong_size = engrave::api::from_value(P_TYPE::SIZE, Point{1, 2});
    ASSERT_FALSE(wrong_size);
    EXPECT_EQ(wrong_size.error().code, ErrorCode::InvalidArgument);

    for (const P_TYPE type :
         {P_TYPE::DRAW_PATH, P_TYPE::GROUPS, P_TYPE::PITCH_VALUES, P_TYPE::DURATION_TYPE_WITH_DOTS}) {
        const auto read_only = engrave::api::from_value(type, Opaque{"draw_path"});
        ASSERT_FALSE(read_only);
        EXPECT_EQ(read_only.error().code, ErrorCode::Unsupported) << int(type);
    }
}

TEST(Describe, ListsEveryImplementedTypeWithSortedProps) {
    const auto types = g_engine->describe();
    EXPECT_EQ(types.size(), 150u) << "the TConv element types that a class declares";
    std::set<std::string> names;
    for (const auto& type : types) {
        EXPECT_TRUE(names.insert(type.type).second) << "duplicate type " << type.type;
        EXPECT_NE(element_type(type.type), ElementType::INVALID) << type.type;
        EXPECT_TRUE(std::ranges::is_sorted(type.props, {}, &engrave::PropInfo::name)) << type.type;
        // Score's getProperty answers nothing: its properties are structural.
        EXPECT_TRUE(!type.props.empty() || type.type == "Score") << type.type;
        for (const auto& prop : type.props) {
            EXPECT_NE(engrave::api::pid_of(prop.name), Pid::END) << type.type << " / " << prop.name;
            EXPECT_FALSE(prop.group.empty()) << prop.name;
            if (prop.kind == ValueKind::Enum)
                EXPECT_TRUE(!prop.enum_tokens.empty() || is_enum_without_tokens(value_type(prop.name)))
                    << type.type << " / " << prop.name;
        }
    }
    EXPECT_TRUE(names.contains("Note"));
    EXPECT_TRUE(names.contains("Accidental"));
    EXPECT_FALSE(names.contains("Selection")) << "no class declares it";
}

TEST(Describe, NotesTheKindsAndUnits) {
    const auto note = g_engine->describe("Note");
    ASSERT_TRUE(note) << note.error().message;
    EXPECT_EQ(note->type, "Note");
    const auto find = [&note](std::string_view name) -> const engrave::PropInfo* {
        const auto it = std::ranges::find(note->props, name, &engrave::PropInfo::name);
        return it == note->props.end() ? nullptr : &*it;
    };
    const auto* pitch = find("pitch");
    ASSERT_TRUE(pitch);
    EXPECT_EQ(pitch->kind, ValueKind::Int);
    EXPECT_FALSE(pitch->settable) << "pitch goes through the engine's pitch edit";
    EXPECT_EQ(pitch->group, "none");
    const auto* min_distance = find("minDistance");
    ASSERT_TRUE(min_distance);
    EXPECT_EQ(min_distance->kind, ValueKind::Real);
    EXPECT_EQ(min_distance->unit, Unit::Spatium);
    EXPECT_EQ(min_distance->group, "position");
    const auto* color = find("color");
    ASSERT_TRUE(color);
    EXPECT_EQ(color->kind, ValueKind::Color);
    EXPECT_EQ(color->unit, Unit::None);
    const auto* autoplace = find("autoplace");
    ASSERT_TRUE(autoplace);
    EXPECT_TRUE(autoplace->settable);
}

TEST(Describe, UnknownTypeIsNotFound) {
    const auto missing = g_engine->describe("NoSuchElement");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
    EXPECT_FALSE(missing.error().message.empty());
}

TEST(Describe, StylesCoverEveryStyleId) {
    const auto styles = g_engine->describe_styles();
    ASSERT_EQ(styles.size(), size_t(mu::engraving::Sid::STYLES));
    std::set<std::string> names;
    int with_value = 0;
    for (size_t i = 0; i < styles.size(); ++i) {
        const auto& style = styles[i];
        EXPECT_TRUE(names.insert(style.name).second) << "duplicate style " << style.name;
        EXPECT_FALSE(style.name.empty());
        if (std::holds_alternative<std::monostate>(style.default_value)) continue;
        ++with_value;
        EXPECT_EQ(style.kind, engrave::api::kind_of(mu::engraving::StyleDef::styleValues[i].valueType()))
            << style.name;
        EXPECT_EQ(style.unit, engrave::api::unit_of(mu::engraving::StyleDef::styleValues[i].valueType()))
            << style.name;
    }
    EXPECT_GT(with_value, 200) << "styles with a default value";
    EXPECT_TRUE(names.contains("spatium"));
    EXPECT_TRUE(names.contains("pageWidth"));
}

TEST(Corpus, EveryPropertyOfEveryCorpusTypeReads) {
    ASSERT_FALSE(g_corpus_files.empty()) << "pass score files or directories on the command line";
    Corpus corpus;
    for (const auto& file : g_corpus_files) {
        ASSERT_NO_FATAL_FAILURE(corpus.load(file)) << file;
    }
    ASSERT_FALSE(corpus.instances.empty());

    std::vector<std::string> skipped;
    std::vector<std::string> without_value;
    int checked = 0;
    int with_corpus = 0;
    for (const auto& type : g_engine->describe()) {
        const auto instance = corpus.instances.find(element_type(type.type));
        if (instance == corpus.instances.end()) {
            skipped.push_back(type.type);
            continue;
        }
        ++with_corpus;
        for (const auto& prop : type.props) {
            const Pid pid = engrave::api::pid_of(prop.name);
            ASSERT_NE(pid, Pid::END) << type.type << " / " << prop.name;
            const PropertyValue value = instance->second->getProperty(pid);
            if (!value.isValid()) {
                without_value.push_back(type.type + " / " + prop.name);
                continue;
            }
            const Value mapped = engrave::api::to_value(mu::engraving::propertyType(pid), value);
            EXPECT_FALSE(std::holds_alternative<std::monostate>(mapped)) << type.type << " / " << prop.name;
            if (const auto* token = std::get_if<Enum>(&mapped); token && !prop.enum_tokens.empty()) {
                EXPECT_TRUE(contains(prop.enum_tokens, token->token))
                    << type.type << " / " << prop.name << ": token '" << token->token << "' is not in the table";
            }
            ++checked;
        }
    }
    std::printf("corpus: %zu file(s), %zu unreadable, %zu type(s) with an instance, %d checked (type, pid) pair(s), "
                "%zu type(s) skipped, %zu pair(s) without a value\n",
                g_corpus_files.size(), corpus.unreadable.size(), corpus.instances.size(), checked, skipped.size(),
                without_value.size());
    for (const auto& file : corpus.unreadable) std::printf("  unreadable: %s\n", file.string().c_str());
    for (const auto& name : skipped) std::printf("  skipped type: %s\n", name.c_str());
    for (const auto& name : without_value) std::printf("  no value: %s\n", name.c_str());
    EXPECT_GT(checked, 1000);
    EXPECT_GT(with_corpus, 60);
    EXPECT_LT(without_value.size(), 10u) << "the table should not claim properties that answer nothing";
}

TEST(Generator, CheckedInTableIsCurrent) {
    if (g_python.empty()) GTEST_SKIP() << "pass --python=<interpreter> to run the generator check";
    const std::string command = std::format(
        "\"{}\" \"{}\" --check --root \"{}\"", g_python.make_preferred().string(),
        (std::filesystem::path(ENGRAVE_TEST_SOURCE_ROOT) / "tools" / "gen_pid_support.py").make_preferred().string(),
        std::filesystem::path(ENGRAVE_TEST_SOURCE_ROOT).make_preferred().string());
#ifdef _WIN32
    // cmd.exe drops the first and the last quote of a command line that starts with
    // one; an extra pair keeps the inner quotes intact.
    const std::string quoted = std::format("\"{}\"", command);
#else
    const std::string quoted = command;
#endif
    EXPECT_EQ(std::system(quoted.c_str()), 0) << quoted;
}

namespace {

void collect(const std::filesystem::path& path) {
    static const std::set<std::string> score_extensions = {".mscx", ".mscz", ".musicxml", ".mxl", ".mid", ".midi",
                                                            ".gp",   ".gp3",  ".gp4",      ".gp5", ".gpx", ".ptb"};
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path, ec))
            if (entry.is_regular_file(ec) && score_extensions.contains(entry.path().extension().string()))
                files.push_back(entry.path());
        std::ranges::sort(files);
        for (const auto& file : files) g_corpus_files.push_back(file);
        return;
    }
    g_corpus_files.push_back(path);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--assets=")) g_assets = std::filesystem::path(arg.substr(9));
        else if (arg.starts_with("--python=")) g_python = std::filesystem::path(arg.substr(9));
        else if (!arg.starts_with("--gtest")) collect(arg);
    }
    auto engine = engrave::Engine::create({.resources = g_assets});
    if (!engine) {
        std::fprintf(stderr, "Engine::create: %s (pass --assets=<resources>)\n", engine.error().message.c_str());
        return 1;
    }
    g_engine = std::move(*engine);
    const int result = RUN_ALL_TESTS();
    g_engine.reset();
    return result;
}
