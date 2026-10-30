#include "engrave/src/api/value_codec.h"

#include <format>
#include <string>

#include "engraving/types/bps.h"
#include "engraving/types/fraction.h"
#include "engraving/types/propertyvalue.h"
#include "engraving/types/spatium.h"
#include "engraving/types/types.h"
#include "engraving/types/typesconv.h"

using namespace mu::engraving;

namespace engrave::api {
namespace {

std::string token_of(const muse::AsciiStringView& text) {
    return text.ascii();
}

std::string token_of(const muse::String& text) {
    return text.toStdString();
}

// The name of a property value type whose value is opaque to the API.
std::string_view opaque_name(P_TYPE type) {
    switch (type) {
    case P_TYPE::DRAW_PATH: return "draw_path";
    case P_TYPE::GROUPS: return "groups";
    case P_TYPE::PITCH_VALUES: return "pitch_values";
    case P_TYPE::DURATION_TYPE_WITH_DOTS: return "duration_type_with_dots";
    default: return "undefined";
    }
}

std::string_view kind_name(ValueKind kind) {
    switch (kind) {
    case ValueKind::Bool: return "bool";
    case ValueKind::Int: return "int";
    case ValueKind::Real: return "real";
    case ValueKind::String: return "string";
    case ValueKind::Point: return "point";
    case ValueKind::Size: return "size";
    case ValueKind::Color: return "color";
    case ValueKind::Fraction: return "fraction";
    case ValueKind::Enum: return "enum token";
    case ValueKind::IntList: return "int list";
    case ValueKind::Opaque: return "opaque";
    }
    return "?";
}

Result<PropertyValue> fail(ValueKind expected) {
    return std::unexpected(Error{ErrorCode::InvalidArgument, std::format("expected a {} value", kind_name(expected))});
}

// A stored value of the type the pid declares, including the engine's own
// int-to-enum and enum-to-int tolerances (PropertyValue::value).
template <typename T>
Result<PropertyValue> enum_value(std::string_view token, T fallback) {
    const T parsed = TConv::fromXml(muse::AsciiStringView(token.data(), token.size()), fallback);
    if (token_of(TConv::toXml(parsed)) != token) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, std::format("unknown token '{}'", token)});
    }
    return PropertyValue(parsed);
}

// Align and OrnamentInterval take their token as a String.
template <typename T>
Result<PropertyValue> enum_value_text(std::string_view token, T fallback) {
    const T parsed = TConv::fromXml(muse::String::fromUtf8(token), fallback);
    if (token_of(TConv::toXml(parsed)) != token) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, std::format("unknown token '{}'", token)});
    }
    return PropertyValue(parsed);
}

double real_of(const PropertyValue& value) {
    // A tempo the engine read from a score is a double; one it built is a
    // BeatsPerSecond.
    if (value.type() == P_TYPE::TEMPO) return value.value<BeatsPerSecond>().val;
    return value.toDouble();
}

std::vector<std::int64_t> int_list(const std::vector<int>& values) {
    return {values.begin(), values.end()};
}

} // namespace

ValueKind kind_of(P_TYPE type) noexcept {
    switch (type) {
    case P_TYPE::BOOL: return ValueKind::Bool;
    case P_TYPE::INT: return ValueKind::Int;
    case P_TYPE::SIZE_T: return ValueKind::Int;
    case P_TYPE::INT_VEC: return ValueKind::IntList;
    case P_TYPE::REAL: return ValueKind::Real;
    case P_TYPE::SPATIUM: return ValueKind::Real;
    case P_TYPE::ABSOLUTE: return ValueKind::Real;
    case P_TYPE::TEMPO: return ValueKind::Real;
    case P_TYPE::STRING: return ValueKind::String;
    case P_TYPE::POINT: return ValueKind::Point;
    case P_TYPE::PAIR_REAL: return ValueKind::Point;
    case P_TYPE::SIZE: return ValueKind::Size;
    case P_TYPE::SCALE: return ValueKind::Size;
    case P_TYPE::COLOR: return ValueKind::Color;
    case P_TYPE::FRACTION: return ValueKind::Fraction;
    case P_TYPE::SYMID: return ValueKind::Enum;
    case P_TYPE::ORNAMENT_STYLE: return ValueKind::Enum;
    case P_TYPE::ORNAMENT_INTERVAL: return ValueKind::Enum;
    case P_TYPE::GLISS_STYLE: return ValueKind::Enum;
    case P_TYPE::GLISS_TYPE: return ValueKind::Enum;
    case P_TYPE::ALIGN: return ValueKind::Enum;
    case P_TYPE::ALIGN_H: return ValueKind::Enum;
    case P_TYPE::PLACEMENT_V: return ValueKind::Enum;
    case P_TYPE::PLACEMENT_H: return ValueKind::Enum;
    case P_TYPE::TEXT_PLACE: return ValueKind::Enum;
    case P_TYPE::DIRECTION_V: return ValueKind::Enum;
    case P_TYPE::DIRECTION_H: return ValueKind::Enum;
    case P_TYPE::ORIENTATION: return ValueKind::Enum;
    case P_TYPE::SHARED_LABEL_ORIENTATION: return ValueKind::Enum;
    case P_TYPE::BEAM_MODE: return ValueKind::Enum;
    case P_TYPE::ACCIDENTAL_ROLE: return ValueKind::Enum;
    case P_TYPE::TIE_PLACEMENT: return ValueKind::Enum;
    case P_TYPE::TIE_DOTS_PLACEMENT: return ValueKind::Enum;
    case P_TYPE::TIMESIG_PLACEMENT: return ValueKind::Enum;
    case P_TYPE::TIMESIG_STYLE: return ValueKind::Enum;
    case P_TYPE::TIMESIG_MARGIN: return ValueKind::Enum;
    case P_TYPE::NOTE_SPELLING_TYPE: return ValueKind::Enum;
    case P_TYPE::CHORD_PRESET_TYPE: return ValueKind::Enum;
    case P_TYPE::PARENTHESES_MODE: return ValueKind::Enum;
    case P_TYPE::PLAY_COUNT_PRESET: return ValueKind::Enum;
    case P_TYPE::CHANGE_METHOD: return ValueKind::Enum;
    case P_TYPE::LAYOUTBREAK_TYPE: return ValueKind::Enum;
    case P_TYPE::VELO_TYPE: return ValueKind::Enum;
    case P_TYPE::BARLINE_TYPE: return ValueKind::Enum;
    case P_TYPE::NOTEHEAD_TYPE: return ValueKind::Enum;
    case P_TYPE::NOTEHEAD_SCHEME: return ValueKind::Enum;
    case P_TYPE::NOTEHEAD_GROUP: return ValueKind::Enum;
    case P_TYPE::CLEF_TYPE: return ValueKind::Enum;
    case P_TYPE::DYNAMIC_TYPE: return ValueKind::Enum;
    case P_TYPE::DYNAMIC_SPEED: return ValueKind::Enum;
    case P_TYPE::LINE_TYPE: return ValueKind::Enum;
    case P_TYPE::HOOK_TYPE: return ValueKind::Enum;
    case P_TYPE::KEY_MODE: return ValueKind::Enum;
    case P_TYPE::TEXT_STYLE: return ValueKind::Enum;
    case P_TYPE::PLAYTECH_TYPE: return ValueKind::Enum;
    case P_TYPE::TEMPOCHANGE_TYPE: return ValueKind::Enum;
    case P_TYPE::LYRICS_DASH_SYSTEM_START_TYPE: return ValueKind::Enum;
    case P_TYPE::PARTIAL_SPANNER_DIRECTION: return ValueKind::Enum;
    case P_TYPE::MARKER_TYPE: return ValueKind::Enum;
    case P_TYPE::ACTION_TYPE: return ValueKind::Enum;
    case P_TYPE::LH_TAPPING_SYMBOL: return ValueKind::Enum;
    case P_TYPE::RH_TAPPING_SYMBOL: return ValueKind::Enum;
    case P_TYPE::VIBRATO_LINE_TYPE: return ValueKind::Enum;
    case P_TYPE::VOICE_ASSIGNMENT: return ValueKind::Enum;
    case P_TYPE::AUTO_ON_OFF: return ValueKind::Enum;
    case P_TYPE::AUTO_CUSTOM_HIDE: return ValueKind::Enum;
    case P_TYPE::MEASURE_NUMBER_PLACEMENT: return ValueKind::Enum;
    case P_TYPE::CAPO_TRANSPOSE_MODE: return ValueKind::Enum;
    case P_TYPE::INSTRUMENT_NAMES_ALIGN: return ValueKind::Enum;
    case P_TYPE::INSTRUMENT_NAMES_FORMAT: return ValueKind::Enum;
    case P_TYPE::INSTRUMENT_LABEL_VISIBILITY: return ValueKind::Enum;
    // The enum types the engine stores and writes as plain ints.
    case P_TYPE::ORNAMENT_SHOW_ACCIDENTAL: return ValueKind::Int;
    case P_TYPE::CLEF_TO_BARLINE_POS: return ValueKind::Int;
    case P_TYPE::SLUR_STYLE_TYPE: return ValueKind::Int;
    case P_TYPE::NOTELINE_PLACEMENT_TYPE: return ValueKind::Int;
    case P_TYPE::UNDEFINED: return ValueKind::Opaque;
    case P_TYPE::DRAW_PATH: return ValueKind::Opaque;
    case P_TYPE::GROUPS: return ValueKind::Opaque;
    case P_TYPE::PITCH_VALUES: return ValueKind::Opaque;
    case P_TYPE::DURATION_TYPE_WITH_DOTS: return ValueKind::Opaque;
    }
    return ValueKind::Opaque;
}

Unit unit_of(P_TYPE type) noexcept {
    switch (type) {
    case P_TYPE::SPATIUM: return Unit::Spatium;
    case P_TYPE::ABSOLUTE: return Unit::Absolute;
    default: return Unit::None;
    }
}

std::span<const std::string_view> enum_tokens(P_TYPE type) noexcept {
    switch (type) {
    case P_TYPE::ALIGN_H: {
        static constexpr std::string_view tokens[] = {"left", "right", "center", "justify"};
        return tokens;
    }
    case P_TYPE::AUTO_CUSTOM_HIDE: {
        static constexpr std::string_view tokens[] = {"auto", "custom", "hide"};
        return tokens;
    }
    case P_TYPE::AUTO_ON_OFF: {
        static constexpr std::string_view tokens[] = {"auto", "on", "off"};
        return tokens;
    }
    case P_TYPE::BARLINE_TYPE: {
        static constexpr std::string_view tokens[] = {"normal", "double", "start-repeat", "end-repeat", "dashed", "end", "end-start-repeat", "dotted", "reverse-end", "heavy", "double-heavy"};
        return tokens;
    }
    case P_TYPE::BEAM_MODE: {
        static constexpr std::string_view tokens[] = {"auto", "begin", "mid", "end", "no", "begin16", "begin32", "invalid"};
        return tokens;
    }
    case P_TYPE::CHANGE_METHOD: {
        static constexpr std::string_view tokens[] = {"normal", "ease-in", "ease-out", "ease-in-out", "exponential"};
        return tokens;
    }
    case P_TYPE::CHORD_PRESET_TYPE: {
        static constexpr std::string_view tokens[] = {"std", "jazz", "legacy", "custom"};
        return tokens;
    }
    case P_TYPE::CLEF_TYPE: {
        static constexpr std::string_view tokens[] = {"G", "G15mb", "G8vb", "G8va", "G15ma", "G8vbo", "G8vbp", "G1", "C1", "C2", "C3", "C4", "C5", "C_19C", "C1_F18C", "C3_F18C", "C4_F18C", "C1_F20C", "C3_F20C", "C4_F20C", "F", "F15mb", "F8vb", "F8va", "F15ma", "F3", "F5", "F_F18C", "F_19C", "PERC", "PERC2", "TAB", "TAB4", "TAB2", "TAB4_SERIF", "C4_8VB", "G8vbc"};
        return tokens;
    }
    case P_TYPE::DIRECTION_H: {
        static constexpr std::string_view tokens[] = {"auto", "right", "left"};
        return tokens;
    }
    case P_TYPE::DIRECTION_V: {
        static constexpr std::string_view tokens[] = {"auto", "up", "down"};
        return tokens;
    }
    case P_TYPE::DYNAMIC_SPEED: {
        static constexpr std::string_view tokens[] = {"normal", "slow", "fast"};
        return tokens;
    }
    case P_TYPE::DYNAMIC_TYPE: {
        static constexpr std::string_view tokens[] = {"other-dynamics", "pppppp", "ppppp", "pppp", "ppp", "pp", "p", "mp", "mf", "f", "ff", "fff", "ffff", "fffff", "ffffff", "fp", "pf", "sf", "sfz", "sff", "sffz", "sfff", "sfffz", "sfp", "sfpp", "rfz", "rf", "fz", "m", "r", "s", "z", "n"};
        return tokens;
    }
    case P_TYPE::GLISS_STYLE: {
        static constexpr std::string_view tokens[] = {"blackkeys", "whitekeys", "diatonic", "portamento", "chromatic"};
        return tokens;
    }
    case P_TYPE::GLISS_TYPE: {
        static constexpr std::string_view tokens[] = {"0", "1"};
        return tokens;
    }
    case P_TYPE::INSTRUMENT_LABEL_VISIBILITY: {
        static constexpr std::string_view tokens[] = {"auto", "long", "short", "hide"};
        return tokens;
    }
    case P_TYPE::INSTRUMENT_NAMES_ALIGN: {
        static constexpr std::string_view tokens[] = {"right-right", "center-right", "center-center", "left-right"};
        return tokens;
    }
    case P_TYPE::INSTRUMENT_NAMES_FORMAT: {
        static constexpr std::string_view tokens[] = {"name-in-transp-num", "name-num-in-transp", "transp-name-num", "custom"};
        return tokens;
    }
    case P_TYPE::KEY_MODE: {
        static constexpr std::string_view tokens[] = {"unknown", "none", "major", "minor", "dorian", "phrygian", "lydian", "mixolydian", "aeolian", "ionian", "locrian"};
        return tokens;
    }
    case P_TYPE::LAYOUTBREAK_TYPE: {
        static constexpr std::string_view tokens[] = {"line", "page", "section", "nobreak"};
        return tokens;
    }
    case P_TYPE::LINE_TYPE: {
        static constexpr std::string_view tokens[] = {"solid", "dashed", "dotted"};
        return tokens;
    }
    case P_TYPE::LH_TAPPING_SYMBOL: {
        static constexpr std::string_view tokens[] = {"dot", "circledT"};
        return tokens;
    }
    case P_TYPE::LYRICS_DASH_SYSTEM_START_TYPE: {
        static constexpr std::string_view tokens[] = {"standard", "underHeader", "underFirstNote"};
        return tokens;
    }
    case P_TYPE::MARKER_TYPE: {
        static constexpr std::string_view tokens[] = {"segno", "varsegno", "codab", "varcoda", "codetta", "fine", "coda", "codasym", "dacoda", "dadblcoda", "user"};
        return tokens;
    }
    case P_TYPE::MEASURE_NUMBER_PLACEMENT: {
        static constexpr std::string_view tokens[] = {"above-system", "below-system", "on-so-staves", "on-all-staves"};
        return tokens;
    }
    case P_TYPE::NOTEHEAD_GROUP: {
        static constexpr std::string_view tokens[] = {"normal", "cross", "plus", "xcircle", "withx", "triangle-up", "triangle-down", "slashed1", "slashed2", "diamond", "diamond-old", "circled", "circled-large", "large-arrow", "altbrevis", "slash", "large-diamond", "heavy-cross", "heavy-cross-hat", "sol", "la", "fa", "mi", "do", "re", "ti", "do-walker", "re-walker", "ti-walker", "do-funk", "re-funk", "ti-funk", "do-name", "di-name", "ra-name", "re-name", "ri-name", "me-name", "mi-name", "fa-name", "fi-name", "se-name", "sol-name", "le-name", "la-name", "li-name", "te-name", "ti-name", "si-name", "a-sharp-name", "a-name", "a-flat-name", "b-sharp-name", "b-name", "b-flat-name", "c-sharp-name", "c-name", "c-flat-name", "d-sharp-name", "d-name", "d-flat-name", "e-sharp-name", "e-name", "e-flat-name", "f-sharp-name", "f-name", "f-flat-name", "g-sharp-name", "g-name", "g-flat-name", "h-name", "h-sharp-name", "swiss-rudiments-flam", "swiss-rudiments-double", "custom"};
        return tokens;
    }
    case P_TYPE::NOTEHEAD_SCHEME: {
        static constexpr std::string_view tokens[] = {"auto", "normal", "name-pitch", "name-pitch-no-acc", "name-pitch-german", "name-pitch-german-no-acc", "solfege-movable", "solfege-fixed", "shape-4", "shape-7-aikin", "shape-7-funk", "shape-7-walker"};
        return tokens;
    }
    case P_TYPE::NOTEHEAD_TYPE: {
        static constexpr std::string_view tokens[] = {"auto", "whole", "half", "quarter", "breve"};
        return tokens;
    }
    case P_TYPE::NOTE_SPELLING_TYPE: {
        static constexpr std::string_view tokens[] = {"standard", "german", "germanPure", "solfeggio", "french"};
        return tokens;
    }
    case P_TYPE::ORIENTATION: {
        static constexpr std::string_view tokens[] = {"vertical", "horizontal"};
        return tokens;
    }
    case P_TYPE::ORNAMENT_STYLE: {
        static constexpr std::string_view tokens[] = {"baroque", "default"};
        return tokens;
    }
    case P_TYPE::PARENTHESES_MODE: {
        static constexpr std::string_view tokens[] = {"none", "both", "left", "right"};
        return tokens;
    }
    case P_TYPE::PARTIAL_SPANNER_DIRECTION: {
        static constexpr std::string_view tokens[] = {"none", "outgoing", "incoming", "both"};
        return tokens;
    }
    case P_TYPE::PLACEMENT_H: {
        static constexpr std::string_view tokens[] = {"left", "center", "right"};
        return tokens;
    }
    case P_TYPE::PLACEMENT_V: {
        static constexpr std::string_view tokens[] = {"above", "below"};
        return tokens;
    }
    case P_TYPE::PLAYTECH_TYPE: {
        static constexpr std::string_view tokens[] = {"undefined", "natural", "pizzicato", "open", "mute", "tremolo", "detache", "martele", "col_legno", "sul_ponticello", "sul_tasto", "vibrato", "legato", "distortion", "overdrive", "harmonics", "jazz_tone", "handbells_swing", "handbells_swing_up", "handbells_swing_down", "handbells_echo_1", "handbells_echo_2", "handbells_damp", "handbells_lv", "handbells_r"};
        return tokens;
    }
    case P_TYPE::PLAY_COUNT_PRESET: {
        static constexpr std::string_view tokens[] = {"xn", "nx", "playntimes", "nrepeats"};
        return tokens;
    }
    case P_TYPE::RH_TAPPING_SYMBOL: {
        static constexpr std::string_view tokens[] = {"T", "plus"};
        return tokens;
    }
    case P_TYPE::SHARED_LABEL_ORIENTATION: {
        static constexpr std::string_view tokens[] = {"vertical", "horizontal", "voice"};
        return tokens;
    }
    case P_TYPE::TEMPOCHANGE_TYPE: {
        static constexpr std::string_view tokens[] = {"undefined", "accelerando", "allargando", "calando", "lentando", "morendo", "precipitando", "rallentando", "ritardando", "smorzando", "sostenuto", "stringendo"};
        return tokens;
    }
    case P_TYPE::TEXT_PLACE: {
        static constexpr std::string_view tokens[] = {"auto", "above", "below", "left"};
        return tokens;
    }
    case P_TYPE::TEXT_STYLE: {
        static constexpr std::string_view tokens[] = {"default", "title", "subtitle", "composer", "poet", "translator", "frame", "instrument_excerpt", "instrument_long", "instrument_short", "instrument_change", "group_bracket", "header", "footer", "copyright", "page_number", "measure_number", "measure_number_alternate", "mmrest_range", "tempo", "tempo change", "metronome", "repeat_play_count", "repeat_left", "repeat_right", "rehearsal_mark", "system", "staff", "stave_sharing", "expression", "dynamics", "hairpin", "lyrics_odd", "lyrics_even", "harmony_a", "harmony_b", "harmony_roman", "harmony_nashville", "tuplet", "sticking", "fingering", "tab_fret_number", "guitar_fingering_lh", "guitar_fingering_rh", "hammer_on_pull_off", "string_number", "string_tunings", "fret_diagram_fingering", "fret_diagram_fret_number", "harp_pedal_diagram", "harp_pedal_text_diagram", "articulation", "textline", "system_textline", "noteline", "volta", "ottava", "glissando", "pedal", "bend", "let_ring", "whammy_bar", "palm_mute", "user_1", "user_2", "user_3", "user_4", "user_5", "user_6", "user_7", "user_8", "user_9", "user_10", "user_11", "user_12"};
        return tokens;
    }
    case P_TYPE::TIE_DOTS_PLACEMENT: {
        static constexpr std::string_view tokens[] = {"auto", "before", "after"};
        return tokens;
    }
    case P_TYPE::TIE_PLACEMENT: {
        static constexpr std::string_view tokens[] = {"auto", "inside", "outside"};
        return tokens;
    }
    case P_TYPE::TIMESIG_MARGIN: {
        static constexpr std::string_view tokens[] = {"hangIntoMargin", "rightAlignToBarline", "createSpace"};
        return tokens;
    }
    case P_TYPE::TIMESIG_PLACEMENT: {
        static constexpr std::string_view tokens[] = {"normal", "aboveStaves", "acrossStaves"};
        return tokens;
    }
    case P_TYPE::TIMESIG_STYLE: {
        static constexpr std::string_view tokens[] = {"normal", "narrow", "large"};
        return tokens;
    }
    case P_TYPE::VELO_TYPE: {
        static constexpr std::string_view tokens[] = {"offset", "user"};
        return tokens;
    }
    case P_TYPE::VIBRATO_LINE_TYPE: {
        static constexpr std::string_view tokens[] = {"none", "guitarVibrato", "guitarVibratoWide", "vibratoSawtooth", "vibratoSawtoothWide"};
        return tokens;
    }
    case P_TYPE::VOICE_ASSIGNMENT: {
        static constexpr std::string_view tokens[] = {"allInInstrument", "allInStaff", "currentVoiceOnly"};
        return tokens;
    }
    case P_TYPE::ACTION_TYPE: {
        static constexpr std::string_view tokens[] = {"undefined", "acciaccatura", "appoggiatura", "grace4", "grace16", "grace32", "grace8after", "grace16after", "grace32after", "beam-auto", "beam-none", "beam-break-left", "beam-join", "beam-break-inner-8th", "beam-break-inner-16th", "beam-feathered-accelerate", "beam-feathered-decelerate", "add-parentheses", "add-brackets", "insert-vbox", "insert-hbox", "insert-textframe", "insert-fretframe", "insert-staff-type-change", "insert-measure", "standard-bend", "pre-bend", "grace-note-bend", "slight-bend", "dive", "pre-dive", "dip", "scoop", "add-noteline", "toggle-system-lock", "toggle-page-lock"};
        return tokens;
    }
    case P_TYPE::CAPO_TRANSPOSE_MODE: {
        static constexpr std::string_view tokens[] = {"playback", "standard", "tab"};
        return tokens;
    }

    default: return {};
    }
}

Value to_value(P_TYPE type, const PropertyValue& value) {
    switch (type) {
    case P_TYPE::UNDEFINED: return {};
    case P_TYPE::BOOL: return value.toBool();
    case P_TYPE::INT: return std::int64_t(value.toInt());
    case P_TYPE::SIZE_T: return std::int64_t(value.value<size_t>());
    case P_TYPE::INT_VEC: return int_list(value.value<std::vector<int>>());
    case P_TYPE::REAL: return value.toDouble();
    case P_TYPE::SPATIUM: return value.value<Spatium>().val();
    case P_TYPE::ABSOLUTE: return value.toDouble();
    case P_TYPE::TEMPO: return real_of(value);
    case P_TYPE::STRING: return value.value<String>().toStdString();
    case P_TYPE::POINT: {
        const PointF point = value.value<PointF>();
        return Point{point.x(), point.y()};
    }
    case P_TYPE::PAIR_REAL: {
        const PairF pair = value.value<PairF>();
        return Point{pair.first, pair.second};
    }
    case P_TYPE::SIZE: {
        const SizeF size = value.value<SizeF>();
        return Size{size.width(), size.height()};
    }
    case P_TYPE::SCALE: {
        const ScaleF scale = value.value<ScaleF>();
        return Size{scale.width(), scale.height()};
    }
    case P_TYPE::COLOR: {
        const mu::engraving::Color color = value.value<mu::engraving::Color>();
        return Color{std::uint8_t(color.red()), std::uint8_t(color.green()), std::uint8_t(color.blue()),
                     std::uint8_t(color.alpha())};
    }
    case P_TYPE::FRACTION: {
        const mu::engraving::Fraction fraction = value.value<mu::engraving::Fraction>();
        return engrave::Fraction{fraction.numerator(), fraction.denominator()};
    }
    case P_TYPE::ALIGN_H: return Enum{token_of(TConv::toXml(value.value<AlignH>()))};
    case P_TYPE::AUTO_CUSTOM_HIDE: return Enum{token_of(TConv::toXml(value.value<AutoCustomHide>()))};
    case P_TYPE::AUTO_ON_OFF: return Enum{token_of(TConv::toXml(value.value<AutoOnOff>()))};
    case P_TYPE::BARLINE_TYPE: return Enum{token_of(TConv::toXml(value.value<BarLineType>()))};
    case P_TYPE::BEAM_MODE: return Enum{token_of(TConv::toXml(value.value<BeamMode>()))};
    case P_TYPE::CHANGE_METHOD: return Enum{token_of(TConv::toXml(value.value<ChangeMethod>()))};
    case P_TYPE::CHORD_PRESET_TYPE: return Enum{token_of(TConv::toXml(value.value<ChordStylePreset>()))};
    case P_TYPE::CLEF_TYPE: return Enum{token_of(TConv::toXml(value.value<ClefType>()))};
    case P_TYPE::DIRECTION_H: return Enum{token_of(TConv::toXml(value.value<DirectionH>()))};
    case P_TYPE::DIRECTION_V: return Enum{token_of(TConv::toXml(value.value<DirectionV>()))};
    case P_TYPE::DYNAMIC_SPEED: return Enum{token_of(TConv::toXml(value.value<DynamicSpeed>()))};
    case P_TYPE::DYNAMIC_TYPE: return Enum{token_of(TConv::toXml(value.value<DynamicType>()))};
    case P_TYPE::GLISS_STYLE: return Enum{token_of(TConv::toXml(value.value<GlissandoStyle>()))};
    case P_TYPE::GLISS_TYPE: return Enum{token_of(TConv::toXml(value.value<GlissandoType>()))};
    case P_TYPE::INSTRUMENT_LABEL_VISIBILITY: return Enum{token_of(TConv::toXml(value.value<InstrumentLabelVisibility>()))};
    case P_TYPE::INSTRUMENT_NAMES_ALIGN: return Enum{token_of(TConv::toXml(value.value<InstrumentNamesAlign>()))};
    case P_TYPE::INSTRUMENT_NAMES_FORMAT: return Enum{token_of(TConv::toXml(value.value<InstrumentNamesFormat>()))};
    case P_TYPE::KEY_MODE: return Enum{token_of(TConv::toXml(value.value<KeyMode>()))};
    case P_TYPE::LAYOUTBREAK_TYPE: return Enum{token_of(TConv::toXml(value.value<LayoutBreakType>()))};
    case P_TYPE::LINE_TYPE: return Enum{token_of(TConv::toXml(value.value<LineType>()))};
    case P_TYPE::LH_TAPPING_SYMBOL: return Enum{token_of(TConv::toXml(value.value<LHTappingSymbol>()))};
    case P_TYPE::LYRICS_DASH_SYSTEM_START_TYPE: return Enum{token_of(TConv::toXml(value.value<LyricsDashSystemStart>()))};
    case P_TYPE::MARKER_TYPE: return Enum{token_of(TConv::toXml(value.value<MarkerType>()))};
    case P_TYPE::MEASURE_NUMBER_PLACEMENT: return Enum{token_of(TConv::toXml(value.value<MeasureNumberPlacement>()))};
    case P_TYPE::NOTEHEAD_GROUP: return Enum{token_of(TConv::toXml(value.value<NoteHeadGroup>()))};
    case P_TYPE::NOTEHEAD_SCHEME: return Enum{token_of(TConv::toXml(value.value<NoteHeadScheme>()))};
    case P_TYPE::NOTEHEAD_TYPE: return Enum{token_of(TConv::toXml(value.value<NoteHeadType>()))};
    case P_TYPE::NOTE_SPELLING_TYPE: return Enum{token_of(TConv::toXml(value.value<NoteSpellingType>()))};
    case P_TYPE::ORIENTATION: return Enum{token_of(TConv::toXml(value.value<Orientation>()))};
    case P_TYPE::ORNAMENT_STYLE: return Enum{token_of(TConv::toXml(value.value<OrnamentStyle>()))};
    case P_TYPE::PARENTHESES_MODE: return Enum{token_of(TConv::toXml(value.value<ParenthesesMode>()))};
    case P_TYPE::PARTIAL_SPANNER_DIRECTION: return Enum{token_of(TConv::toXml(value.value<PartialSpannerDirection>()))};
    case P_TYPE::PLACEMENT_H: return Enum{token_of(TConv::toXml(value.value<PlacementH>()))};
    case P_TYPE::PLACEMENT_V: return Enum{token_of(TConv::toXml(value.value<PlacementV>()))};
    case P_TYPE::PLAYTECH_TYPE: return Enum{token_of(TConv::toXml(value.value<PlayingTechniqueType>()))};
    case P_TYPE::PLAY_COUNT_PRESET: return Enum{token_of(TConv::toXml(value.value<RepeatPlayCountPreset>()))};
    case P_TYPE::RH_TAPPING_SYMBOL: return Enum{token_of(TConv::toXml(value.value<RHTappingSymbol>()))};
    case P_TYPE::SHARED_LABEL_ORIENTATION: return Enum{token_of(TConv::toXml(value.value<SharedLabelOrientation>()))};
    case P_TYPE::TEMPOCHANGE_TYPE: return Enum{token_of(TConv::toXml(value.value<GradualTempoChangeType>()))};
    case P_TYPE::TEXT_PLACE: return Enum{token_of(TConv::toXml(value.value<TextPlace>()))};
    case P_TYPE::TEXT_STYLE: return Enum{token_of(TConv::toXml(value.value<TextStyleType>()))};
    case P_TYPE::TIE_DOTS_PLACEMENT: return Enum{token_of(TConv::toXml(value.value<TieDotsPlacement>()))};
    case P_TYPE::TIE_PLACEMENT: return Enum{token_of(TConv::toXml(value.value<TiePlacement>()))};
    case P_TYPE::TIMESIG_MARGIN: return Enum{token_of(TConv::toXml(value.value<TimeSigVSMargin>()))};
    case P_TYPE::TIMESIG_PLACEMENT: return Enum{token_of(TConv::toXml(value.value<TimeSigPlacement>()))};
    case P_TYPE::TIMESIG_STYLE: return Enum{token_of(TConv::toXml(value.value<TimeSigStyle>()))};
    case P_TYPE::VELO_TYPE: return Enum{token_of(TConv::toXml(value.value<VeloType>()))};
    case P_TYPE::VIBRATO_LINE_TYPE: return Enum{token_of(TConv::toXml(value.value<VibratoType>()))};
    case P_TYPE::VOICE_ASSIGNMENT: return Enum{token_of(TConv::toXml(value.value<VoiceAssignment>()))};
    case P_TYPE::ACTION_TYPE: return Enum{token_of(TConv::toXml(value.value<ActionIconType>()))};
    case P_TYPE::CAPO_TRANSPOSE_MODE: return Enum{token_of(TConv::toXml(value.value<CapoParams::TransposeMode>()))};

    case P_TYPE::SYMID: return Enum{token_of(TConv::toXml(value.value<SymId>()))};
    case P_TYPE::ORNAMENT_INTERVAL:
        return Enum{token_of(TConv::toXml(value.value<OrnamentInterval>()))};
    case P_TYPE::ALIGN: return Enum{token_of(TConv::toXml(value.value<Align>()))};
    case P_TYPE::ACCIDENTAL_ROLE: return Enum{token_of(TConv::toXml(value.value<AccidentalRole>()))};
    case P_TYPE::HOOK_TYPE: return Enum{token_of(TConv::toXml(value.value<HookType>()))};
    case P_TYPE::ORNAMENT_SHOW_ACCIDENTAL: return std::int64_t(value.value<int>());
    case P_TYPE::CLEF_TO_BARLINE_POS: return std::int64_t(value.value<int>());
    case P_TYPE::SLUR_STYLE_TYPE: return std::int64_t(value.value<int>());
    case P_TYPE::NOTELINE_PLACEMENT_TYPE: return std::int64_t(value.value<int>());
    case P_TYPE::DRAW_PATH: case P_TYPE::GROUPS: case P_TYPE::PITCH_VALUES: case P_TYPE::DURATION_TYPE_WITH_DOTS:
        return Opaque{std::string(opaque_name(type))};
    }
    return {};
}

Result<PropertyValue> from_value(P_TYPE type, const Value& value) {
    switch (type) {
    case P_TYPE::BOOL:
        if (const auto* v = std::get_if<bool>(&value)) return PropertyValue(*v);
        return fail(ValueKind::Bool);
    case P_TYPE::INT:
        if (const auto* v = std::get_if<std::int64_t>(&value)) return PropertyValue(int(*v));
        return fail(ValueKind::Int);
    case P_TYPE::SIZE_T:
        if (const auto* v = std::get_if<std::int64_t>(&value)) return PropertyValue(size_t(*v));
        return fail(ValueKind::Int);
    case P_TYPE::INT_VEC:
        if (const auto* v = std::get_if<std::vector<std::int64_t>>(&value))
            return PropertyValue(std::vector<int>(v->begin(), v->end()));
        return fail(ValueKind::IntList);
    case P_TYPE::REAL:
        if (const auto* v = std::get_if<double>(&value)) return PropertyValue(*v);
        return fail(ValueKind::Real);
    case P_TYPE::SPATIUM:
        if (const auto* v = std::get_if<double>(&value)) return PropertyValue(Spatium(*v));
        return fail(ValueKind::Real);
    case P_TYPE::ABSOLUTE:
        if (const auto* v = std::get_if<double>(&value)) return PropertyValue(*v);
        return fail(ValueKind::Real);
    case P_TYPE::TEMPO:
        if (const auto* v = std::get_if<double>(&value)) return PropertyValue(BeatsPerSecond(*v));
        return fail(ValueKind::Real);
    case P_TYPE::STRING:
        if (const auto* v = std::get_if<std::string>(&value)) return PropertyValue(muse::String::fromUtf8(*v));
        return fail(ValueKind::String);
    case P_TYPE::POINT:
        if (const auto* v = std::get_if<Point>(&value)) return PropertyValue(PointF(v->x, v->y));
        return fail(ValueKind::Point);
    case P_TYPE::PAIR_REAL:
        if (const auto* v = std::get_if<Point>(&value)) return PropertyValue(PairF(v->x, v->y));
        return fail(ValueKind::Point);
    case P_TYPE::SIZE:
        if (const auto* v = std::get_if<Size>(&value)) return PropertyValue(SizeF(v->w, v->h));
        return fail(ValueKind::Size);
    case P_TYPE::SCALE:
        if (const auto* v = std::get_if<Size>(&value)) return PropertyValue(ScaleF(v->w, v->h));
        return fail(ValueKind::Size);
    case P_TYPE::COLOR:
        if (const auto* v = std::get_if<engrave::Color>(&value))
            return PropertyValue(mu::engraving::Color(v->r, v->g, v->b, v->a));
        return fail(ValueKind::Color);
    case P_TYPE::FRACTION:
        if (const auto* v = std::get_if<engrave::Fraction>(&value))
            return PropertyValue(mu::engraving::Fraction(int(v->num), int(v->den)));
        return fail(ValueKind::Fraction);
    case P_TYPE::ALIGN_H:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<AlignH>(token->token, AlignH::HCENTER);
        return fail(ValueKind::Enum);
    case P_TYPE::AUTO_CUSTOM_HIDE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<AutoCustomHide>(token->token, AutoCustomHide::AUTO);
        return fail(ValueKind::Enum);
    case P_TYPE::AUTO_ON_OFF:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<AutoOnOff>(token->token, AutoOnOff::AUTO);
        return fail(ValueKind::Enum);
    case P_TYPE::BARLINE_TYPE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<BarLineType>(token->token, BarLineType::NORMAL);
        return fail(ValueKind::Enum);
    case P_TYPE::BEAM_MODE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<BeamMode>(token->token, BeamMode::AUTO);
        return fail(ValueKind::Enum);
    case P_TYPE::CHANGE_METHOD:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<ChangeMethod>(token->token, ChangeMethod::NORMAL);
        return fail(ValueKind::Enum);
    case P_TYPE::CHORD_PRESET_TYPE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<ChordStylePreset>(token->token, ChordStylePreset::STANDARD);
        return fail(ValueKind::Enum);
    case P_TYPE::CLEF_TYPE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<ClefType>(token->token, ClefType::G);
        return fail(ValueKind::Enum);
    case P_TYPE::DIRECTION_H:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<DirectionH>(token->token, DirectionH::AUTO);
        return fail(ValueKind::Enum);
    case P_TYPE::DIRECTION_V:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<DirectionV>(token->token, DirectionV::AUTO);
        return fail(ValueKind::Enum);
    case P_TYPE::DYNAMIC_SPEED:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<DynamicSpeed>(token->token, DynamicSpeed::NORMAL);
        return fail(ValueKind::Enum);
    case P_TYPE::DYNAMIC_TYPE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<DynamicType>(token->token, DynamicType::OTHER);
        return fail(ValueKind::Enum);
    case P_TYPE::GLISS_STYLE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<GlissandoStyle>(token->token, GlissandoStyle::CHROMATIC);
        return fail(ValueKind::Enum);
    case P_TYPE::GLISS_TYPE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<GlissandoType>(token->token, GlissandoType::STRAIGHT);
        return fail(ValueKind::Enum);
    case P_TYPE::INSTRUMENT_LABEL_VISIBILITY:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<InstrumentLabelVisibility>(token->token, InstrumentLabelVisibility::AUTO);
        return fail(ValueKind::Enum);
    case P_TYPE::INSTRUMENT_NAMES_ALIGN:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<InstrumentNamesAlign>(token->token, InstrumentNamesAlign::RIGHT_RIGHT);
        return fail(ValueKind::Enum);
    case P_TYPE::INSTRUMENT_NAMES_FORMAT:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<InstrumentNamesFormat>(token->token, InstrumentNamesFormat::NAME_IN_TRANSP_NUM);
        return fail(ValueKind::Enum);
    case P_TYPE::KEY_MODE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<KeyMode>(token->token, KeyMode::NONE);
        return fail(ValueKind::Enum);
    case P_TYPE::LAYOUTBREAK_TYPE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<LayoutBreakType>(token->token, LayoutBreakType::NOBREAK);
        return fail(ValueKind::Enum);
    case P_TYPE::LINE_TYPE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<LineType>(token->token, LineType::SOLID);
        return fail(ValueKind::Enum);
    case P_TYPE::LH_TAPPING_SYMBOL:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<LHTappingSymbol>(token->token, LHTappingSymbol::DOT);
        return fail(ValueKind::Enum);
    case P_TYPE::LYRICS_DASH_SYSTEM_START_TYPE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<LyricsDashSystemStart>(token->token, LyricsDashSystemStart::STANDARD);
        return fail(ValueKind::Enum);
    case P_TYPE::MARKER_TYPE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<MarkerType>(token->token, MarkerType::USER);
        return fail(ValueKind::Enum);
    case P_TYPE::MEASURE_NUMBER_PLACEMENT:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<MeasureNumberPlacement>(token->token, MeasureNumberPlacement::ABOVE_SYSTEM);
        return fail(ValueKind::Enum);
    case P_TYPE::NOTEHEAD_GROUP:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<NoteHeadGroup>(token->token, NoteHeadGroup::HEAD_NORMAL);
        return fail(ValueKind::Enum);
    case P_TYPE::NOTEHEAD_SCHEME:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<NoteHeadScheme>(token->token, NoteHeadScheme::HEAD_AUTO);
        return fail(ValueKind::Enum);
    case P_TYPE::NOTEHEAD_TYPE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<NoteHeadType>(token->token, NoteHeadType::HEAD_AUTO);
        return fail(ValueKind::Enum);
    case P_TYPE::NOTE_SPELLING_TYPE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<NoteSpellingType>(token->token, NoteSpellingType::STANDARD);
        return fail(ValueKind::Enum);
    case P_TYPE::ORIENTATION:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<Orientation>(token->token, Orientation::VERTICAL);
        return fail(ValueKind::Enum);
    case P_TYPE::ORNAMENT_STYLE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<OrnamentStyle>(token->token, OrnamentStyle::DEFAULT);
        return fail(ValueKind::Enum);
    case P_TYPE::PARENTHESES_MODE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<ParenthesesMode>(token->token, ParenthesesMode::NONE);
        return fail(ValueKind::Enum);
    case P_TYPE::PARTIAL_SPANNER_DIRECTION:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<PartialSpannerDirection>(token->token, PartialSpannerDirection::OUTGOING);
        return fail(ValueKind::Enum);
    case P_TYPE::PLACEMENT_H:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<PlacementH>(token->token, PlacementH::LEFT);
        return fail(ValueKind::Enum);
    case P_TYPE::PLACEMENT_V:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<PlacementV>(token->token, PlacementV::ABOVE);
        return fail(ValueKind::Enum);
    case P_TYPE::PLAYTECH_TYPE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<PlayingTechniqueType>(token->token, PlayingTechniqueType::Natural);
        return fail(ValueKind::Enum);
    case P_TYPE::PLAY_COUNT_PRESET:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<RepeatPlayCountPreset>(token->token, RepeatPlayCountPreset::X_N);
        return fail(ValueKind::Enum);
    case P_TYPE::RH_TAPPING_SYMBOL:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<RHTappingSymbol>(token->token, RHTappingSymbol::T);
        return fail(ValueKind::Enum);
    case P_TYPE::SHARED_LABEL_ORIENTATION:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<SharedLabelOrientation>(token->token, SharedLabelOrientation::HORIZONTAL);
        return fail(ValueKind::Enum);
    case P_TYPE::TEMPOCHANGE_TYPE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<GradualTempoChangeType>(token->token, GradualTempoChangeType::Undefined);
        return fail(ValueKind::Enum);
    case P_TYPE::TEXT_PLACE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<TextPlace>(token->token, TextPlace::AUTO);
        return fail(ValueKind::Enum);
    case P_TYPE::TEXT_STYLE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<TextStyleType>(token->token, TextStyleType::DEFAULT);
        return fail(ValueKind::Enum);
    case P_TYPE::TIE_DOTS_PLACEMENT:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<TieDotsPlacement>(token->token, TieDotsPlacement::AUTO);
        return fail(ValueKind::Enum);
    case P_TYPE::TIE_PLACEMENT:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<TiePlacement>(token->token, TiePlacement::AUTO);
        return fail(ValueKind::Enum);
    case P_TYPE::TIMESIG_MARGIN:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<TimeSigVSMargin>(token->token, TimeSigVSMargin::HANG_INTO_MARGIN);
        return fail(ValueKind::Enum);
    case P_TYPE::TIMESIG_PLACEMENT:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<TimeSigPlacement>(token->token, TimeSigPlacement::NORMAL);
        return fail(ValueKind::Enum);
    case P_TYPE::TIMESIG_STYLE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<TimeSigStyle>(token->token, TimeSigStyle::NORMAL);
        return fail(ValueKind::Enum);
    case P_TYPE::VELO_TYPE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<VeloType>(token->token, VeloType::OFFSET_VAL);
        return fail(ValueKind::Enum);
    case P_TYPE::VIBRATO_LINE_TYPE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<VibratoType>(token->token, VibratoType::NONE);
        return fail(ValueKind::Enum);
    case P_TYPE::VOICE_ASSIGNMENT:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<VoiceAssignment>(token->token, VoiceAssignment::ALL_VOICE_IN_INSTRUMENT);
        return fail(ValueKind::Enum);
    case P_TYPE::ACTION_TYPE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<ActionIconType>(token->token, ActionIconType::UNDEFINED);
        return fail(ValueKind::Enum);
    case P_TYPE::CAPO_TRANSPOSE_MODE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<CapoParams::TransposeMode>(token->token, CapoParams::TransposeMode::PLAYBACK_ONLY);
        return fail(ValueKind::Enum);
    case P_TYPE::SYMID:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<SymId>(token->token, SymId::noSym);
        return fail(ValueKind::Enum);
    case P_TYPE::ORNAMENT_INTERVAL:
        if (const auto* token = std::get_if<Enum>(&value))
            return enum_value_text<OrnamentInterval>(token->token, DEFAULT_ORNAMENT_INTERVAL);
        return fail(ValueKind::Enum);
    case P_TYPE::ALIGN:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value_text<Align>(token->token, Align());
        return fail(ValueKind::Enum);
    case P_TYPE::ACCIDENTAL_ROLE:
        if (const auto* token = std::get_if<Enum>(&value))
            return enum_value<AccidentalRole>(token->token, AccidentalRole::AUTO);
        return fail(ValueKind::Enum);
    case P_TYPE::HOOK_TYPE:
        if (const auto* token = std::get_if<Enum>(&value)) return enum_value<HookType>(token->token, HookType::NONE);
        return fail(ValueKind::Enum);
    case P_TYPE::ORNAMENT_SHOW_ACCIDENTAL:
        if (const auto* v = std::get_if<std::int64_t>(&value)) return PropertyValue(OrnamentShowAccidental(*v));
        return fail(ValueKind::Int);
    case P_TYPE::CLEF_TO_BARLINE_POS:
        if (const auto* v = std::get_if<std::int64_t>(&value)) return PropertyValue(ClefToBarlinePosition(*v));
        return fail(ValueKind::Int);
    case P_TYPE::SLUR_STYLE_TYPE:
        if (const auto* v = std::get_if<std::int64_t>(&value)) return PropertyValue(SlurStyleType(*v));
        return fail(ValueKind::Int);
    case P_TYPE::NOTELINE_PLACEMENT_TYPE:
        if (const auto* v = std::get_if<std::int64_t>(&value)) return PropertyValue(NoteLineEndPlacement(*v));
        return fail(ValueKind::Int);
    case P_TYPE::DRAW_PATH: case P_TYPE::GROUPS: case P_TYPE::PITCH_VALUES: case P_TYPE::DURATION_TYPE_WITH_DOTS:
    case P_TYPE::UNDEFINED:
        return std::unexpected(Error{ErrorCode::Unsupported,
                                     std::format("a {} value is read-only", opaque_name(type))});
    }
    return std::unexpected(Error{ErrorCode::InvalidArgument, "unknown property value type"});
}

} // namespace engrave::api
