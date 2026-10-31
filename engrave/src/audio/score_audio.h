#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "audio_mix.h"
#include "engraving/types/types.h"

namespace mu::engraving {
class Score;
}

namespace music {

// One MuseScore playback track (an InstrumentTrackId), in deterministic score
// order: score parts in Score::parts() order, each part's instruments in
// Part::instrumentTrackIdList() order, then the part's chord-symbol pseudo
// track when the part has chord symbols.
//
// A track's public trackId is its zero-based index in this list, not a score
// structure: it only has to be stable for one listing, and it is identical in
// score_audio_tracks, metadata.tracks and the worker snapshot because all of
// them derive the list from the same score.
struct ScoreAudioTrackInfo {
    mu::engraving::InstrumentTrackId id;
    std::string part_id;
    std::string instrument_id;
    std::string part_name;
    std::string instrument_name;
    bool chord_symbols = false;
    // Zero-based index in Score::parts().
    std::size_t part_index = 0;
};

// Describes every instrument track of the selected score, including one entry
// per instrument of a multi-instrument part and instrument change, and the
// chord-symbol pseudo track. Throws std::invalid_argument for a null score.
std::vector<ScoreAudioTrackInfo> score_audio_track_info(mu::engraving::Score* score);

struct TrackSoloMute {
    bool solo = false;
    bool muted = false;
    bool operator==(const TrackSoloMute&) const = default;
};

// Per-document default mix read from an MSCZ archive's audiosettings.json.
// Tracks are keyed by the same (partId, instrumentId) identities that
// score_audio_track_info() reports. Volume and balance are project-wide (from
// root audiosettings.json). For the full score (excerptId == -1), solo and
// muted come from the root file. For an excerpt (excerptId >= 0), solo and
// muted come from Excerpts/<name>/audiosettings.json when present, else neutral.
// Unknown entries and fields are ignored; missing, malformed or non-MSCZ input
// yields neutral defaults.
struct ScoreAudioMixDefaults {
    AudioOutputParams master;
    std::map<std::pair<std::string, std::string>, AudioOutputParams> tracks;
    std::map<std::string, std::map<std::pair<std::string, std::string>, TrackSoloMute>> excerpt_tracks_by_name;
    std::vector<std::map<std::pair<std::string, std::string>, TrackSoloMute>> excerpt_tracks_by_index;

    const std::map<std::pair<std::string, std::string>, TrackSoloMute>* get_excerpt_map(
        int excerpt_id, const std::string& excerpt_name = "") const noexcept
    {
        if (!excerpt_name.empty()) {
            auto it = excerpt_tracks_by_name.find(excerpt_name);
            if (it != excerpt_tracks_by_name.end()) return &it->second;
        }
        if (excerpt_id >= 0 && static_cast<std::size_t>(excerpt_id) < excerpt_tracks_by_index.size()) {
            return &excerpt_tracks_by_index[static_cast<std::size_t>(excerpt_id)];
        }
        return nullptr;
    }

    bool is_neutral(int excerpt_id = -1, const std::string& excerpt_name = "") const noexcept
    {
        if (!(master == AudioOutputParams{})) return false;
        if (excerpt_id < 0) {
            for (const auto& [identity, output] : tracks) {
                (void)identity;
                if (!(output == AudioOutputParams{})) return false;
            }
            return true;
        }
        const auto* excerpt_map = get_excerpt_map(excerpt_id, excerpt_name);
        for (const auto& [identity, output] : tracks) {
            if (output.volume_db != 0.0 || output.balance != 0.0) return false;
            if (excerpt_map) {
                auto it = excerpt_map->find(identity);
                if (it != excerpt_map->end()) {
                    if (it->second.solo || it->second.muted) return false;
                }
            }
        }
        return true;
    }
};

// Reads audiosettings.json from MSCZ bytes. Never throws: invalid archives,
// absent files and malformed JSON all produce neutral defaults. Values outside
// the supported range are ignored field by field.
ScoreAudioMixDefaults score_audio_mix_defaults(const std::string& mscz_bytes);

// The document's default mix for the listed tracks: master volume/balance from
// the archive's root audiosettings.json, each track's volume/balance by
// (partId, instrumentId), and solo/mute from the root file for the master score
// or from the excerpt's own file otherwise. forceMute is derived as playback
// derives it and chord-symbol pseudo tracks are muted, so this is the mix a
// render uses when the caller overrides nothing. `tracks` must be the list
// score_audio_track_info() gave for the same excerpt, and `defaults` the values
// score_audio_mix_defaults() read from the document's source.
AudioMix score_audio_default_mix(const ScoreAudioMixDefaults& defaults,
                                 const std::vector<ScoreAudioTrackInfo>& tracks,
                                 int excerpt_id = -1, const std::string& excerpt_name = {});

} // namespace music
