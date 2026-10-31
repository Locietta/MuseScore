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

// Qt-free native adaptation of the Score* overload in
// webmscore-4.7.5-1 src/converter/internal/compat/notationmeta.cpp
// (mu::converter::NotationMeta). The JSON field schema and the
// title/composer recognition heuristics are preserved; QJson*/QString are
// replaced by nlohmann::json and muse::String/std::string.

#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace mu::engraving {
class Score;
}

namespace music {
// One audio/instrument track descriptor supplied by the caller. IDs must name
// an existing InstrumentTrackId in the score; this layer never derives names
// or resource metadata from the score's Parts.
struct MetadataTrack {
    std::string part_id;
    std::string instrument_id;
    std::string type;
    std::string name;
    std::optional<std::string> sound_id;
    std::optional<std::string> category;
    std::optional<std::string> vendor;
};

// Audio stem descriptor and mix state for metadata track export.
// Fields match upstream TrackAudioOutputParams; volumeDb is the upstream
// volume_db_t value in decibels [-120..24].
struct ScoreMetadataTrack {
    std::int64_t track_id = 0;
    std::string part_id;
    std::string instrument_id;
    // Part name and instrument/track name; a chord-symbol pseudo track has
    // chord_symbols == true and its mixer title as name.
    std::string part_name;
    std::string name;
    bool chord_symbols = false;
    double volume_db = 0.0;
    double balance = 0.0;
    bool solo = false;
    bool muted = false;
    bool force_mute = false;
};

// Emits score metadata with "tracks" populated from the score's actual tracks
// (via score_audio_track_info) in deterministic score order. The base overload
// has no audiosettings.json context, so it reports neutral output values and
// marks chord-symbol pseudo tracks muted. Returns an empty object when score
// is null.
nlohmann::json score_metadata(mu::engraving::Score* score);

// Emits score metadata with "tracks" populated from explicit track descriptors and
// mix parameters. Returns an empty object when score is null.
nlohmann::json score_metadata(mu::engraving::Score* score, const std::vector<ScoreMetadataTrack>& tracks);

// Same JSON as score_metadata(score), with "tracks" built from the supplied
// descriptors. Every descriptor must reference an existing part in score and
// an instrument ID owned by that part; duplicate (part, instrument) pairs are
// rejected. Tracks are emitted sorted by numeric part ID and then instrument
// ID, using mu::engraving::InstrumentTrackId ordering.
//
// soundId and category are omitted when absent and also when they equal name,
// matching upstream. vendor is emitted only when supplied and type is not
// "fluid_soundfont"; the Fluid type omits it even if one was supplied, as
// upstream does. Values are copied from the descriptors, never synthesized.
//
// Throws std::invalid_argument for a malformed/unknown part ID, an unknown
// instrument ID, a duplicate pair, or non-empty tracks with a null score.
// Excerpts are handled as documented for the base overload.
nlohmann::json score_metadata(mu::engraving::Score* score, const std::vector<MetadataTrack>& tracks);

// Upstream webmscore titleFilenameSafe(), converted to UTF-8 std::string:
//   title.replace(/[\s<>:{}"/\\|?*~.\0\cA-\cZ]+/g, '_')
// Runs of matching characters become one '_'. JS whitespace covers ASCII
// whitespace plus U+00A0, U+1680, U+2000..U+200A, U+2028, U+2029, U+202F,
// U+205F, U+3000 and U+FEFF. Matching is by Unicode code point, not UTF-8
// byte. Returns an empty string when score is null.
std::string score_title_filename_safe(const mu::engraving::Score* score);

// Upstream NotationMeta::title(), converted to std::string. Returns an empty
// string when score is null.
std::string score_title(const mu::engraving::Score* score);
}
