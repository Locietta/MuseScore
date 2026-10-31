// AddText and Add: text at a position through the engine's own Score::addText, and
// the generic Add that Factory::createItem and undoAddElement make of one
// (table-driven; a pair the table does not list is not an operation).

#include <algorithm>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engraving/dom/chord.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/engravingobject.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/property.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/tempotext.h"
#include "engraving/dom/textbase.h"
#include "engraving/types/constants.h"
#include "engraving/types/propertyvalue.h"
#include "engraving/types/types.h"
#include "engraving/types/typesconv.h"

#include "engrave/src/api/batch.h"
#include "engrave/src/api/pid_support.h"
#include "engrave/src/api/value_codec.h"

namespace engrave::api {
namespace {

using mu::engraving::Chord;
using mu::engraving::ElementType;
using mu::engraving::EngravingItem;
using mu::engraving::EngravingObject;
using mu::engraving::Pid;
using mu::engraving::PropertyFlags;
using mu::engraving::PropertyValue;
using mu::engraving::Score;
using mu::engraving::Segment;
using mu::engraving::TextBase;
using mu::engraving::TextStyleType;

std::unexpected<Error> invalid(std::string message) {
    return std::unexpected(Error{ErrorCode::InvalidArgument, std::move(message)});
}

std::unexpected<Error> rejected(std::string message) {
    return std::unexpected(Error{ErrorCode::EditRejected, std::move(message)});
}

// --- where an operation writes ----------------------------------------------

// The segment address of an operation, checked as far as its own fields go. Its
// staff is checked against the score once the address is resolved.
Result<SegmentAddress> chord_rest_address(const Address& address) {
    const auto* at = std::get_if<SegmentAddress>(&address);
    if (!at) return invalid("text is added at a segment address, not at a page or a spanner segment");
    if (at->kind != "ChordRest") {
        return invalid(std::format("text attaches to a ChordRest segment, not to a '{}' one", at->kind));
    }
    if (at->staff < 0 || at->voice < 0 || static_cast<std::size_t>(at->voice) >= mu::engraving::VOICES) {
        return invalid(std::format("a staff has no voice {}", at->voice));
    }
    return *at;
}

// The track an address names, once its score is known.
Result<mu::engraving::track_idx_t> address_track(const SegmentAddress& at, const Score& score) {
    if (static_cast<std::size_t>(at.staff) >= score.nstaves()) {
        return invalid(std::format("the score has no staff {}", at.staff));
    }
    return mu::engraving::staff2track(at.staff, at.voice);
}

// --- AddText ------------------------------------------------------------------

// The text styles AddText writes at a chord or rest: the ones Score::addText
// (editing/edit.cpp:718-1040) handles for one. The page styles need a frame, the
// harmony styles change the part's channels, an instrument change rebuilds the MIDI
// mapping, and a dynamic carries a DynamicType apart from its text; each is left to
// its own operation or slice.
constexpr TextStyleType add_text_styles[] = {
    TextStyleType::STAFF, TextStyleType::SYSTEM, TextStyleType::TEMPO, TextStyleType::LYRICS_ODD,
};

std::string add_text_style_names() {
    std::string names;
    for (const TextStyleType style : add_text_styles) {
        names += names.empty() ? "" : ", ";
        names += mu::engraving::TConv::toXml(style).ascii();
    }
    return names;
}

// The style an AddText names: the engine's own style name, as a saved score writes
// it ("staff", "tempo", "lyrics_odd").
Result<TextStyleType> add_text_style(const std::string& name) {
    const TextStyleType style = mu::engraving::TConv::fromXml(muse::AsciiStringView(name), TextStyleType::TEXT_TYPES);
    if (std::ranges::find(add_text_styles, style) == std::end(add_text_styles)) {
        return invalid(std::format("AddText writes the text styles {}; not '{}'", add_text_style_names(), name));
    }
    return style;
}

// --- Add ----------------------------------------------------------------------

// The (anchor type, child type) pairs Add creates, each one the way the engine
// itself makes the pair.
struct Pair {
    std::string_view anchor;
    std::string_view child;
};

constexpr Pair add_pairs[] = {
    // EditChord::toggleArticulation (editing/editchord.cpp:97-101) sets the symbol,
    // the parent and the track, then adds the articulation to the chord.
    {"Chord", "Articulation"},
    // The reader and the paste code attach a fermata to its segment
    // (rw/read500/read500.cpp:705-720 and :966-971: track, ownership parent, add).
    {"Segment", "Fermata"},
    // Score::addText's DYNAMICS case (editing/edit.cpp:783-791), and rw/read500's
    // paste path, which attaches a Dynamic to its segment.
    {"Segment", "Dynamic"},
    // Score::addText's STAFF case (editing/edit.cpp:763-771), and the reader's
    // StaffText (rw/read500/measureread.cpp:487-...).
    {"Segment", "StaffText"},
    // Score::addText's TEMPO case (editing/edit.cpp:941-1005) and the reader's
    // TempoText (rw/read500/measureread.cpp:477-486).
    {"Segment", "Tempo"},
};

const Pair* add_pair(std::string_view anchor, std::string_view child) {
    const auto found = std::ranges::find_if(add_pairs, [anchor, child](const Pair& pair) {
        return pair.anchor == anchor && pair.child == child;
    });
    return found == std::end(add_pairs) ? nullptr : &*found;
}

bool addable(std::string_view child) {
    return std::ranges::find_if(add_pairs, [child](const Pair& pair) { return pair.child == child; }) != std::end(add_pairs);
}

std::string add_pairs_text() {
    std::string pairs;
    for (const Pair& pair : add_pairs) {
        pairs += std::format("{}{} on a {}", pairs.empty() ? "" : ", ", pair.child, pair.anchor);
    }
    return pairs;
}

Result<void> check_pair(const EngravingObject& anchor, std::string_view child) {
    if (add_pair(anchor.typeName(), child)) return {};
    return invalid(std::format("Add creates {}; not a {} on a {}", add_pairs_text(), child, anchor.typeName()));
}

// The track the new item belongs to. A segment address names it, since a segment
// has no staff of its own; a chord or a rest has one.
Result<mu::engraving::track_idx_t> anchor_track(const EngravingObject& anchor, const Ref& ref) {
    if (anchor.type() == ElementType::SEGMENT) {
        const auto* address = std::get_if<Address>(&ref);
        const auto* at = address ? std::get_if<SegmentAddress>(address) : nullptr;
        if (!at) return invalid("an item added to a segment is named by a segment address");
        return address_track(*at, *mu::engraving::toSegment(&anchor)->score());
    }
    if (!anchor.isEngravingItem()) return invalid(std::format("{} is not an element to add to", anchor.typeName()));
    return static_cast<const EngravingItem&>(anchor).track();
}

// --- the properties of an Add ------------------------------------------------

struct Setting {
    Pid pid;
    PropertyValue value;
};

// The properties of an operation, resolved against the type the new item will have
// and checked against the catalogue, as SetProperty checks them.
Result<std::vector<Setting>> settings(const std::vector<std::pair<std::string, Value>>& props, std::string_view type) {
    std::vector<Setting> settings;
    settings.reserve(props.size());
    for (const auto& [name, value] : props) {
        const Pid pid = pid_of(name);
        if (pid == Pid::END) return invalid(std::format("no property '{}'", name));
        const std::optional<bool> listed = settable(type, pid);
        if (!listed) return invalid(std::format("a {} has no property '{}'", type, name_of(pid)));
        if (!*listed) {
            return invalid(std::format("'{}' of a {} is changed by structural operations only", name_of(pid), type));
        }
        auto checked = from_value(mu::engraving::propertyType(pid), value);
        if (!checked) return std::unexpected(checked.error());
        settings.push_back({pid, *checked});
    }
    return settings;
}

// A property of the item being made, the way the engine's own creation paths set
// one (ChangeProperty::flip, editing/editproperty.cpp:44-47: the value, then the
// flags). A styled property that is given a value of its own stops following the
// style. The item is not in the score yet, so this needs no command of its own: the
// AddElement that follows owns it.
void set_setting(EngravingItem& item, const Setting& setting) {
    const PropertyFlags flags = item.propertyFlags(setting.pid);
    item.setProperty(setting.pid, setting.value);
    item.setPropertyFlags(setting.pid, flags == PropertyFlags::STYLED ? PropertyFlags::UNSTYLED : flags);
}

// The tempo text Score::addText's TEMPO branch makes (editing/edit.cpp:978-1003): a
// system object of the top staff, its text and follow-text flag set before the add,
// and the tempo derived from the text after it. The derivation is an undoable
// property change, and a change made before the AddElement would outlive the element
// a fault at that add deletes.
mu::engraving::TempoText* add_tempo_text(Score& score, Segment& segment, const std::string& text) {
    mu::engraving::TempoText* tempo = mu::engraving::Factory::createTempoText(&segment);
    tempo->setTrack(0);
    tempo->setFollowText(true);
    tempo->setXmlText(muse::String::fromUtf8(text));
    score.undoAddElement(tempo);
    tempo->updateTempo();
    return tempo;
}

} // namespace

Result<Step> prepare(EditContext& context, const AddText& op) {
    auto at = chord_rest_address(op.at);
    if (!at) return std::unexpected(at.error());
    auto style = add_text_style(op.style);
    if (!style) return std::unexpected(style.error());
    // The writer stores no id for an empty text, so one is not an item a caller can
    // address afterwards.
    if (op.text.empty()) return invalid("a text has at least one character");
    auto target = context.target(op.at);
    if (!target) return std::unexpected(target.error());
    if (auto track = address_track(*at, *mu::engraving::toSegment(target->object)->score()); !track) {
        return std::unexpected(track.error());
    }
    return [target = *target, at = *at, style = *style, text = op.text,
            name = op.style](EditContext& context) -> Result<void> {
        auto object = context.object(target);
        if (!object) return std::unexpected(object.error());
        Segment* segment = mu::engraving::toSegment(*object);
        Score* score = segment->score();
        auto track = address_track(at, *score);
        if (!track) return std::unexpected(track.error());
        EngravingItem* anchor = segment->element(*track);
        if (!anchor || !anchor->isChordRest()) {
            return invalid(std::format("there is no chord or rest at staff {} voice {} to write on", at.staff, at.voice));
        }
        // Score::addText for every style but the tempo. Its tempo branch derives the
        // tempo, an undoable property change, before it adds the text; a fault at that
        // add would then leave the change pointing at the element the add deleted.
        TextBase* added = nullptr;
        if (style == TextStyleType::TEMPO) {
            added = add_tempo_text(*score, *segment, text);
        } else {
            // addText's lyrics branch (editing/edit.cpp:880-910) writes under a note or
            // a rest, not under the chord.
            EngravingItem* destination = anchor;
            if (style == TextStyleType::LYRICS_ODD && anchor->isChord()) {
                Chord* chord = mu::engraving::toChord(anchor);
                if (chord->notes().empty()) return rejected("the chord has no note to write the lyrics under");
                destination = chord->notes().front();
            }
            added = score->addText(style, destination);
            if (!added) return rejected(std::format("the engine did not add a '{}' text here", name));
            // addText creates the item but writes no text; the engine's text edit changes
            // it as a property, which also reaches its linked copies (editing/textedit.cpp:237).
            added->undoChangeProperty(Pid::TEXT, PropertyValue(muse::String::fromUtf8(text)));
        }
        context.created(added);
        return {};
    };
}

Result<Step> prepare(EditContext& context, const Add& op) {
    const ElementType type = mu::engraving::TConv::fromXml(muse::AsciiStringView(op.type), ElementType::INVALID);
    if (type == ElementType::INVALID) return invalid(std::format("no element type '{}'", op.type));
    const std::string name = mu::engraving::TConv::toXml(type).ascii();
    if (!addable(name)) return invalid(std::format("Add creates {}; not a {}", add_pairs_text(), name));
    auto props = settings(op.props, name);
    if (!props) return std::unexpected(props.error());
    auto anchor = context.target(op.anchor);
    if (!anchor) return std::unexpected(anchor.error());
    // An OpRef's element exists only when the step runs; the step checks the pair
    // for it as well, since an earlier operation decides its type.
    if (anchor->object) {
        if (auto pair = check_pair(*anchor->object, name); !pair) return std::unexpected(pair.error());
        if (auto track = anchor_track(*anchor->object, op.anchor); !track) return std::unexpected(track.error());
    }
    return [anchor = *anchor, type, name, props = std::move(*props)](EditContext& context) -> Result<void> {
        auto object = context.object(anchor);
        if (!object) return std::unexpected(object.error());
        if (auto pair = check_pair(**object, name); !pair) return std::unexpected(pair.error());
        auto track = anchor_track(**object, anchor.ref);
        if (!track) return std::unexpected(track.error());
        if (!(*object)->isEngravingItem()) return invalid("an item is added to an element");
        std::unique_ptr<EngravingItem> created(
            mu::engraving::Factory::createItem(type, mu::engraving::toEngravingItem(*object)));
        if (!created) return invalid(std::format("the engine does not create a {}", name));
        created->setTrack(*track);
        for (const Setting& setting : props) set_setting(*created, setting);
        Score* score = created->score();
        EngravingItem* added = created.release();
        score->undoAddElement(added);
        context.created(added);
        return {};
    };
}

} // namespace engrave::api
