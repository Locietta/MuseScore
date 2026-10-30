/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Adapted for the Qt-free native host from
// webmscore-4.7.5-1 src/converter/internal/compat/notationmeta.cpp
// (mu::converter::NotationMeta, Score* overload and helpers).
//
// The JSON field schema and the title/composer recognition heuristics are kept
// identical to the upstream implementation. QJsonArray/QJsonObject are replaced
// by nlohmann::json arrays/objects and QString by muse::String/std::string. The
// notation/audio-settings dependency: track descriptions come from
// score_audio_track_info(), which lists every InstrumentTrackId and the
// chord-symbol pseudo track.

#include "score_metadata.h"
#include "engrave/src/audio/score_audio.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "global/realfn.h"
#include "global/types/string.h"

#include "engraving/dom/engravingitem.h"
#include "engraving/dom/excerpt.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measurebase.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/part.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/tempotext.h"
#include "engraving/dom/text.h"
#include "engraving/dom/textbase.h"
#include "engraving/dom/timesig.h"
#include "engraving/style/style.h"

namespace music {
namespace {
using Json = nlohmann::json;
using namespace mu::engraving;

std::string boolToString(bool b)
{
    return b ? "true" : "false";
}

bool shouldTryRecognizeText(const Text* text)
{
    const TextStyleType type = text->textStyleType();
    if (type == TextStyleType::DEFAULT || type == TextStyleType::FRAME) {
        return true;
    }

    return static_cast<int>(type) >= static_cast<int>(TextStyleType::USER1)
           && static_cast<int>(type) <= static_cast<int>(TextStyleType::USER12);
}

muse::String recognizeTitle(const Score* score)
{
    const MeasureBase* mb = score->first();
    if (!mb || !mb->isVBox()) {
        return muse::String();
    }

    const Text* titleText = nullptr;
    double maxFontSize = DBL_MIN;
    double minY = DBL_MAX;

    for (const EngravingItem* item : mb->el()) {
        if (!item || !item->isText()) {
            continue;
        }

        const Text* text = toText(item);
        if (!shouldTryRecognizeText(text)) {
            continue;
        }

        if (text->size() < maxFontSize) {
            continue;
        }

        if (muse::RealIsEqual(text->size(), maxFontSize) && text->y() > minY) {
            continue;
        }

        titleText = text;
        maxFontSize = text->size();
        minY = text->y();
    }

    return titleText ? titleText->plainText() : muse::String();
}

muse::String recognizeComposer(const Score* score)
{
    const MeasureBase* mb = score->first();
    if (!mb || !mb->isVBox()) {
        return muse::String();
    }

    const Text* rightmostText = nullptr;
    double rightmostTextX = mb->ldata()->bbox().center().x();

    for (const EngravingItem* item : mb->el()) {
        if (!item || !item->isText()) {
            continue;
        }

        const Text* text = toText(item);
        if (!shouldTryRecognizeText(text)) {
            continue;
        }

        if (text->x() > rightmostTextX) {
            rightmostText = text;
            rightmostTextX = text->x();
        }
    }

    return rightmostText ? rightmostText->plainText() : muse::String();
}

muse::String titleString(const Score* score)
{
    muse::String title;
    const Text* text = score->getText(TextStyleType::TITLE);
    if (text) {
        title = text->plainText();
    }

    if (title.isEmpty()) {
        title = score->metaTag(u"workTitle");
    }

    if (title.isEmpty()) {
        title = recognizeTitle(score);
    }

    if (title.isEmpty()) {
        title = score->name();
    }

    return title;
}

std::string title(const Score* score)
{
    return titleString(score).toStdString();
}

// Exactly the character set of the JS regex
// /[\s<>:{}"/\\|?*~.\0\cA-\cZ]+/g. The JS class includes NUL, the control range
// U+0001..U+001A (which also covers TAB/LF/VT/FF/CR), ASCII space, and the
// Unicode WhiteSpace/LineTerminator additions below. U+0085 and U+180E are not
// JS whitespace and are deliberately not matched.
bool isFilenameUnsafe(char32_t c)
{
    if (c <= 0x1A) {
        return true; // U+0000, U+0001..U+001A
    }

    switch (c) {
    case U' ': case U'<': case U'>': case U':': case U'{': case U'}':
    case U'"': case U'/': case U'\\': case U'|': case U'?': case U'*':
    case U'~': case U'.':
        return true;
    default:
        break;
    }

    if (c < 0x7F) {
        return false;
    }

    return c == 0x00A0
           || c == 0x1680
           || (c >= 0x2000 && c <= 0x200A)
           || c == 0x2028
           || c == 0x2029
           || c == 0x202F
           || c == 0x205F
           || c == 0x3000
           || c == 0xFEFF;
}

std::string subtitle(const Score* score)
{
    muse::String subtitle;
    const Text* text = score->getText(TextStyleType::SUBTITLE);
    if (text) {
        subtitle = text->plainText();
    }

    return subtitle.toStdString();
}

std::string composer(const Score* score)
{
    muse::String composer;
    const Text* text = score->getText(TextStyleType::COMPOSER);
    if (text) {
        composer = text->plainText();
    }

    if (composer.isEmpty()) {
        composer = score->metaTag(u"composer");
    }

    if (composer.isEmpty()) {
        composer = recognizeComposer(score);
    }

    return composer.toStdString();
}

std::string poet(const Score* score)
{
    muse::String poet;
    const Text* text = score->getText(TextStyleType::LYRICIST);
    if (text) {
        poet = text->plainText();
    }

    if (poet.isEmpty()) {
        poet = score->metaTag(u"lyricist");
    }

    return poet.toStdString();
}

std::string timesig(const Score* score)
{
    size_t staves = score->nstaves();
    size_t tracks = staves * VOICES;
    const Segment* timeSigSegment = score->firstSegmentMM(SegmentType::TimeSig);
    if (!timeSigSegment) {
        return std::string();
    }

    std::string timeSig;
    const EngravingItem* element = nullptr;
    for (size_t track = 0; track < tracks; ++track) {
        element = timeSigSegment->element(track);
        if (element) {
            break;
        }
    }

    if (element && element->isTimeSig()) {
        const TimeSig* ts = toTimeSig(element);
        timeSig = std::to_string(ts->numerator()) + "/" + std::to_string(ts->denominator());
    }

    return timeSig;
}

std::pair<int, std::string> tempo(const Score* score)
{
    int tempo = 0;
    muse::String tempoText;
    for (const Segment* segment = score->firstSegmentMM(SegmentType::All); segment;
         segment = segment->next1MM()) {
        auto annotations = segment->annotations();
        for (const EngravingItem* annotation : annotations) {
            if (annotation && annotation->isTempoText()) {
                const TempoText* tt = toTempoText(annotation);
                tempo = static_cast<int>(std::round(tt->tempo().toBPM().val));
                tempoText = tt->xmlText();
            }
        }
    }

    return { tempo, tempoText.toStdString() };
}

bool parsePartId(const std::string& text, std::uint64_t& value)
{
    if (text.empty()) {
        return false;
    }

    std::uint64_t parsed = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }

        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }

    value = parsed;
    return true;
}

Json trackJson(const InstrumentTrackId& id, const MetadataTrack& track)
{
    Json jsonTrack = Json::object();
    jsonTrack["instrumentId"] = id.instrumentId.toStdString();
    jsonTrack["partId"] = id.partId.toStdString();
    jsonTrack["type"] = track.type;

    // Upstream inserts vendor only for non-Fluid sources. The descriptor
    // carries the actual vendor value; the type check preserves the upstream
    // omission without inventing a vendor for Fluid.
    if (track.vendor && track.type != "fluid_soundfont") {
        jsonTrack["vendor"] = *track.vendor;
    }

    jsonTrack["name"] = track.name;

    // Upstream only adds these keys when they differ from the display name.
    if (track.sound_id && *track.sound_id != track.name) {
        jsonTrack["soundId"] = *track.sound_id;
    }
    if (track.category && *track.category != track.name) {
        jsonTrack["category"] = *track.category;
    }

    return jsonTrack;
}

Json tracksJsonArray(const Score* score, const std::vector<MetadataTrack>& tracks)
{
    Json jsonTracksArray = Json::array();
    std::map<InstrumentTrackId, Json> sortedJsonTracks;

    for (const MetadataTrack& track : tracks) {
        std::uint64_t partIdValue = 0;
        if (!parsePartId(track.part_id, partIdValue) || partIdValue == 0) {
            throw std::invalid_argument("score_metadata: invalid part id '" + track.part_id + "'");
        }

        const Part* part = nullptr;
        for (const Part* candidate : score->parts()) {
            if (candidate && candidate->id().toUint64() == partIdValue) {
                part = candidate;
                break;
            }
        }
        if (!part) {
            throw std::invalid_argument("score_metadata: unknown part id '" + track.part_id + "'");
        }

        if (track.instrument_id.empty()) {
            throw std::invalid_argument("score_metadata: empty instrument id for part '" + track.part_id + "'");
        }

        const InstrumentTrackIdSet ids = part->instrumentTrackIdSet();
        const auto match = std::find_if(ids.begin(), ids.end(), [&track](const InstrumentTrackId& id) {
            return id.instrumentId.toStdString() == track.instrument_id;
        });
        if (match == ids.end()) {
            throw std::invalid_argument("score_metadata: unknown instrument id '" + track.instrument_id
                                        + "' for part '" + track.part_id + "'");
        }

        const auto inserted = sortedJsonTracks.emplace(*match, Json::object());
        if (!inserted.second) {
            throw std::invalid_argument("score_metadata: duplicate track for part '" + track.part_id
                                        + "' and instrument '" + track.instrument_id + "'");
        }
        inserted.first->second = trackJson(*match, track);
    }

    for (const auto& entry : sortedJsonTracks) {
        jsonTracksArray.push_back(entry.second);
    }

    return jsonTracksArray;
}

Json partsJsonArray(const Score* score)
{
    Json jsonPartsArray = Json::array();
    for (const Part* part : score->parts()) {
        Json jsonPart = Json::object();
        jsonPart["id"] = part->id().toStdString();
        muse::String name = part->longName();
        jsonPart["name"] = name.replace(u"\n", u"").toStdString();
        int midiProgram = part->midiProgram();
        jsonPart["program"] = midiProgram;
        jsonPart["instrumentId"] = part->instrumentId().toStdString();
        jsonPart["instrumentName"] = part->instrumentName().toStdString();
        jsonPart["lyricCount"] = part->lyricCount();
        jsonPart["harmonyCount"] = part->harmonyCount();
        jsonPart["hasPitchedStaff"] = boolToString(part->hasPitchedStaff());
        jsonPart["hasTabStaff"] = boolToString(part->hasTabStaff());
        jsonPart["hasDrumStaff"] = boolToString(part->hasDrumStaff());
        jsonPart["isVisible"] = boolToString(part->show());
        jsonPartsArray.push_back(jsonPart);
    }

    return jsonPartsArray;
}

Json pageFormatJson(const MStyle& style)
{
    Json format = Json::object();
    // Upstream stores round(...) as a QJson double; QJsonDocument serializes a
    // whole value without a fractional part, so int keeps the existing output.
    format["height"] = static_cast<int>(std::round(style.styleD(Sid::pageHeight) * INCH));
    format["width"] = static_cast<int>(std::round(style.styleD(Sid::pageWidth) * INCH));
    format["twosided"] = boolToString(style.styleB(Sid::pageTwosided));

    return format;
}

void findTextByType(TextStyleType textStyleType, std::vector<muse::String>& strings,
                    EngravingItem* element)
{
    if (!element || !element->isTextBase()) {
        return;
    }

    const TextBase* text = toTextBase(element);
    if (text->textStyleType() == textStyleType) {
        strings.push_back(text->plainText());
    }
}

Json typeDataJson(Score* score)
{
    Json typesData = Json::object();
    static const std::vector<std::pair<std::string, TextStyleType> > namesTypesList {
        { "titles", TextStyleType::TITLE },
        { "subtitles", TextStyleType::SUBTITLE },
        { "composers", TextStyleType::COMPOSER },
        { "poets", TextStyleType::LYRICIST }
    };

    for (const auto& nameType : namesTypesList) {
        Json typeData = Json::array();
        std::vector<muse::String> typeTextStrings;
        score->scanElements([&](EngravingItem* item) { findTextByType(nameType.second, typeTextStrings, item); });
        for (const auto& typeStr : typeTextStrings) {
            typeData.push_back(typeStr.toStdString());
        }
        typesData[nameType.first] = typeData;
    }

    return typesData;
}

Json excerptsJsonArray(const Score* score)
{
    Json jsonExcerptsArray = Json::array();

    const MasterScore* master = score->masterScore();
    if (!master) {
        return jsonExcerptsArray;
    }

    const std::vector<Excerpt*>& excerpts = master->excerpts();
    for (size_t i = 0; i < excerpts.size(); ++i) {
        const Excerpt* e = excerpts[i];
        if (!e) {
            continue;
        }

        // Loaders can leave an excerpt entry without an excerpt score. Skip it
        // instead of dereferencing null; the remaining entries keep their
        // original master-list index as "id".
        Score* part = e->excerptScore();
        if (!part) {
            continue;
        }

        Json jsonExcerpt = Json::object();
        jsonExcerpt["id"] = static_cast<int>(i);
        jsonExcerpt["title"] = e->name().toStdString();
        jsonExcerpt["parts"] = partsJsonArray(part);

        jsonExcerptsArray.push_back(jsonExcerpt);
    }

    return jsonExcerptsArray;
}
Json metadataTracksJsonArray(const std::vector<ScoreMetadataTrack>& tracks)
{
    Json jsonTracksArray = Json::array();
    for (const ScoreMetadataTrack& track : tracks) {
        Json jsonTrack = Json::object();
        jsonTrack["trackId"] = track.track_id;
        jsonTrack["partId"] = track.part_id;
        jsonTrack["instrumentId"] = track.instrument_id;
        jsonTrack["partName"] = track.part_name;
        jsonTrack["name"] = track.name;
        jsonTrack["chordSymbols"] = track.chord_symbols;
        jsonTrack["volumeDb"] = track.volume_db;
        jsonTrack["volume"] = track.volume_db;
        jsonTrack["balance"] = track.balance;
        jsonTrack["solo"] = track.solo;
        jsonTrack["muted"] = track.muted;
        jsonTrack["forceMute"] = track.force_mute;
        jsonTracksArray.push_back(std::move(jsonTrack));
    }
    return jsonTracksArray;
}

Json make_score_metadata_json(Score* score, Json jsonTracks)
{
    Json json = Json::object();

    json["title"] = title(score);
    json["subtitle"] = subtitle(score);
    json["composer"] = composer(score);
    json["poet"] = poet(score);
    json["mscoreVersion"] = score->mscoreVersion().toStdString();
    json["fileVersion"] = score->mscVersion();
    json["pages"] = static_cast<int>(score->npages());       // no = operator for size_t
    json["measures"] = static_cast<int>(score->nmeasures()); // no = operator for size_t
    json["hasLyrics"] = boolToString(score->hasLyrics());
    json["hasHarmonies"] = boolToString(score->hasHarmonies());
    json["keysig"] = score->keysig();
    json["previousSource"] = score->metaTag(u"source").toStdString();
    json["timesig"] = timesig(score);
    json["duration"] = score->duration();
    json["lyrics"] = score->extractLyrics().toStdString();

    auto _tempo = tempo(score);
    json["tempo"] = _tempo.first;
    json["tempoText"] = _tempo.second;

    json["parts"] = partsJsonArray(score);
    json["pageFormat"] = pageFormatJson(score->style());
    json["textFramesData"] = typeDataJson(score);
    json["tracks"] = std::move(jsonTracks);
    json["excerpts"] = excerptsJsonArray(score);

    return json;
}
} // namespace

nlohmann::json score_metadata(mu::engraving::Score* score, const std::vector<ScoreMetadataTrack>& tracks)
{
    if (!score) {
        return nlohmann::json::object();
    }
    return make_score_metadata_json(score, metadataTracksJsonArray(tracks));
}

nlohmann::json score_metadata(mu::engraving::Score* score, const std::vector<MetadataTrack>& tracks)
{
    if (!score) {
        if (!tracks.empty()) {
            throw std::invalid_argument("score_metadata: cannot validate tracks without a score");
        }
        return nlohmann::json::object();
    }

    // Validate and build the tracks before doing any other work, so an invalid
    // descriptor cannot produce a partially populated result.
    Json jsonTracks = tracksJsonArray(score, tracks);
    return make_score_metadata_json(score, std::move(jsonTracks));
}

nlohmann::json score_metadata(mu::engraving::Score* score)
{
    if (!score) {
        return nlohmann::json::object();
    }
    std::vector<ScoreMetadataTrack> tracks;
    const auto infos = score_audio_track_info(score);
    tracks.reserve(infos.size());
    for (std::size_t index = 0; index < infos.size(); ++index) {
        const auto& info = infos[index];
        ScoreMetadataTrack t;
        t.track_id = static_cast<std::int64_t>(index);
        t.part_id = info.part_id;
        t.instrument_id = info.instrument_id;
        t.part_name = info.part_name;
        t.name = info.instrument_name;
        t.chord_symbols = info.chord_symbols;
        // No export mix is available here; chord playback is still off.
        t.muted = info.chord_symbols;
        t.force_mute = info.chord_symbols;
        tracks.push_back(std::move(t));
    }
    return score_metadata(score, tracks);
}

std::string score_title_filename_safe(const mu::engraving::Score* score)
{
    if (!score) {
        return std::string();
    }

    // Work on Unicode code points. UTF-8 is not a fixed-width encoding, so a
    // byte-wise character class would corrupt multibyte text (and would also
    // be unable to match U+FEFF, U+2028, etc. correctly).
    const std::u32string input = titleString(score).toStdU32String();
    std::u32string output;
    output.reserve(input.size());

    bool inReplacementRun = false;
    for (const char32_t ch : input) {
        if (isFilenameUnsafe(ch)) {
            if (!inReplacementRun) {
                output.push_back(U'_');
                inReplacementRun = true;
            }
        } else {
            output.push_back(ch);
            inReplacementRun = false;
        }
    }

    if (output.empty()) {
        return std::string();
    }

    return muse::String::fromUcs4(output.data(), output.size()).toStdString();
}

std::string score_title(const mu::engraving::Score* score)
{
    if (!score) {
        return std::string();
    }

    return title(score);
}
} // namespace music
