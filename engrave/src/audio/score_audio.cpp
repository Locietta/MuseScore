// Native score audio helpers shared by the host and the worker.
//
// Track identity is the score's InstrumentTrackId; the actual rendering lives
// in MuseScore 4's audio engine (engine_audio.cpp) and only runs in a worker
// process. This file keeps the deterministic track description and the MSCZ
// audiosettings.json default mix.

#include "score_audio.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "global/io/buffer.h"
#include "global/modularity/ioc.h"
#include "global/serialization/json.h"
#include "global/serialization/zipreader.h"

#include "engraving/dom/instrument.h"
#include "engraving/dom/part.h"
#include "engraving/dom/score.h"
#include "engraving/playback/playbackmodel.h"

namespace music {
namespace {

using mu::engraving::Instrument;
using mu::engraving::InstrumentTrackId;
using mu::engraving::Part;
using mu::engraving::Score;

// audiosettings.json is tiny; reject pathological archives before parsing.
constexpr std::size_t max_audio_settings_bytes = 1024 * 1024;
constexpr std::size_t max_audio_settings_tracks = 4096;

std::optional<double> settings_number(const muse::JsonObject& object, const char* key)
{
    if (!object.contains(key)) return std::nullopt;
    const auto value = object.value(key);
    if (!value.isNumber()) return std::nullopt;
    const double number = value.toDouble();
    if (!std::isfinite(number)) return std::nullopt;
    return number;
}

std::optional<bool> settings_bool(const muse::JsonObject& object, const char* key)
{
    if (!object.contains(key)) return std::nullopt;
    const auto value = object.value(key);
    if (!value.isBool()) return std::nullopt;
    return value.toBool();
}

// Unknown fields are ignored by design. Out-of-range known fields are ignored
// individually so one corrupt value cannot discard the rest of the mix.
void apply_settings_output(AudioOutputParams& output, const muse::JsonObject& object)
{
    if (const auto volume = settings_number(object, "volumeDb");
        volume && *volume >= -120.0 && *volume <= 24.0) {
        output.volume_db = *volume;
    }
    if (const auto balance = settings_number(object, "balance");
        balance && *balance >= -1.0 && *balance <= 1.0) {
        output.balance = *balance;
    }
}

} // namespace

std::vector<ScoreAudioTrackInfo> score_audio_track_info(Score* score)
{
    if (!score) {
        throw std::invalid_argument("score_audio: score must not be null");
    }

    // The id helpers on an unloaded model are cheap and need no playback data.
    const mu::engraving::PlaybackModel model(muse::modularity::globalCtx());

    std::vector<ScoreAudioTrackInfo> result;
    std::size_t part_index = 0;
    for (const Part* part : score->parts()) {
        if (!part) {
            ++part_index;
            continue;
        }

        // Part::instrumentTrackIdList() dereferences every instrument, so
        // dedupe here after skipping malformed entries. The order matches the
        // engine: instruments are stored by their first tick in a std::map.
        std::set<muse::String> seen_instruments;
        for (const auto& [tick, instrument] : part->instruments()) {
            (void)tick;
            if (!instrument) {
                continue;
            }
            const InstrumentTrackId id{ part->id(), instrument->id() };
            if (!id.isValid() || !seen_instruments.insert(id.instrumentId).second) {
                continue;
            }

            ScoreAudioTrackInfo info;
            info.id = id;
            info.part_id = part->id().toStdString();
            info.instrument_id = id.instrumentId.toStdString();
            info.part_name = part->partName().toStdString();
            info.instrument_name = instrument->trackName().toStdString();
            info.chord_symbols = false;
            info.part_index = part_index;
            result.push_back(std::move(info));
        }

        if (part->hasChordSymbol()) {
            ScoreAudioTrackInfo info;
            info.id = model.chordSymbolsTrackId(part->id());
            info.part_id = part->id().toStdString();
            info.instrument_id = info.id.instrumentId.toStdString();
            info.part_name = part->partName().toStdString();
            // Mirror PlaybackController's mixer title for the pseudo track.
            info.instrument_name = "Chords." + info.part_name;
            info.chord_symbols = true;
            info.part_index = part_index;
            result.push_back(std::move(info));
        }

        ++part_index;
    }

    return result;
}

bool json_nesting_ok(std::string_view text, std::size_t max_depth = 64)
{
    std::size_t depth = 0;
    bool in_string = false;
    bool escape = false;
    for (char c : text) {
        if (escape) {
            escape = false;
            continue;
        }
        if (in_string) {
            if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            ++depth;
            if (depth > max_depth) return false;
        } else if (c == '}' || c == ']') {
            if (depth > 0) {
                --depth;
            }
        }
    }
    return true;
}

const muse::ZipReader::FileInfo* find_zip_entry(const std::vector<muse::ZipReader::FileInfo>& entries,
                                                std::string_view name)
{
    for (const auto& entry : entries) {
        if (entry.filePath.toStdString() == name) {
            return &entry;
        }
    }
    return nullptr;
}

ScoreAudioMixDefaults score_audio_mix_defaults(const std::string& mscz_bytes)
{
    ScoreAudioMixDefaults result;
    if (mscz_bytes.size() < 4) return result;
    try {
        muse::ByteArray archive(mscz_bytes.data(), mscz_bytes.size());
        muse::io::Buffer buffer(&archive);
        if (!buffer.open(muse::io::IODevice::ReadOnly)) return result;
        muse::ZipReader zip(&buffer);
        if (zip.hasError()) return result;

        const auto file_infos = zip.fileInfoList();
        if (file_infos.empty() || file_infos.size() > 4096) return result;

        // 1. Root audiosettings.json for the full score. Check uncompressed size BEFORE reading.
        const auto* root_entry = find_zip_entry(file_infos, "audiosettings.json");
        if (root_entry && root_entry->size <= max_audio_settings_bytes) {
            const muse::ByteArray json = zip.fileData("audiosettings.json");
            if (!zip.hasError() && !json.empty() && json.size() <= max_audio_settings_bytes) {
                const std::string_view json_view(reinterpret_cast<const char*>(json.constData()), json.size());
                if (json_nesting_ok(json_view, 64)) {
                    std::string error;
                    const muse::JsonDocument document = muse::JsonDocument::fromJson(json, &error);
                    if (error.empty() && document.isObject()) {
                        const muse::JsonObject root = document.rootObject();

                        const auto master = root.value("master");
                        if (master.isObject()) apply_settings_output(result.master, master.toObject());

                        const auto tracks = root.value("tracks");
                        if (tracks.isArray()) {
                            const muse::JsonArray array = tracks.toArray();
                            if (array.size() <= max_audio_settings_tracks) {
                                for (std::size_t index = 0; index < array.size(); ++index) {
                                    const auto entry = array.at(index);
                                    if (!entry.isObject()) continue;
                                    const muse::JsonObject track = entry.toObject();
                                    const auto part_id = track.value("partId");
                                    const auto instrument_id = track.value("instrumentId");
                                    if (!part_id.isString() || !instrument_id.isString()) continue;

                                    AudioOutputParams output;
                                    const auto settings = track.value("out");
                                    if (settings.isObject()) apply_settings_output(output, settings.toObject());
                                    const auto solo_mute = track.value("soloMuteState");
                                    if (solo_mute.isObject()) {
                                        const muse::JsonObject state = solo_mute.toObject();
                                        if (const auto solo = settings_bool(state, "solo")) output.solo = *solo;
                                        if (const auto muted = settings_bool(state, "mute")) output.muted = *muted;
                                    }
                                    result.tracks.emplace(
                                        std::make_pair(part_id.toStdString(), instrument_id.toStdString()), output);
                                }
                            }
                        }
                    }
                }
            }
        }

        // 2. Discover excerpts and parse their notation solo/mute settings.
        std::vector<std::string> excerpt_names;
        for (const auto& entry : file_infos) {
            const std::string path = entry.filePath.toStdString();
            if (path.starts_with("Excerpts/") && (path.ends_with(".mscx") || path.ends_with(".MSCX"))) {
                const std::size_t slash = path.find('/', 9);
                if (slash != std::string::npos) {
                    std::string name = path.substr(9, slash - 9);
                    if (!name.empty() && std::find(excerpt_names.begin(), excerpt_names.end(), name) == excerpt_names.end()) {
                        excerpt_names.push_back(std::move(name));
                    }
                }
            }
        }
        for (const auto& entry : file_infos) {
            const std::string path = entry.filePath.toStdString();
            if (path.starts_with("Excerpts/") && path.ends_with("/audiosettings.json")) {
                std::string name = path.substr(9, path.size() - 9 - 18);
                if (!name.empty() && std::find(excerpt_names.begin(), excerpt_names.end(), name) == excerpt_names.end()) {
                    excerpt_names.push_back(std::move(name));
                }
            }
        }

        result.excerpt_tracks_by_index.resize(excerpt_names.size());
        for (std::size_t i = 0; i < excerpt_names.size(); ++i) {
            const std::string& name = excerpt_names[i];
            const std::string file_path = "Excerpts/" + name + "/audiosettings.json";
            const auto* excerpt_entry = find_zip_entry(file_infos, file_path);
            if (!excerpt_entry || excerpt_entry->size > max_audio_settings_bytes) continue;

            const muse::ByteArray json = zip.fileData(file_path);
            if (zip.hasError() || json.empty() || json.size() > max_audio_settings_bytes) continue;

            const std::string_view json_view(reinterpret_cast<const char*>(json.constData()), json.size());
            if (!json_nesting_ok(json_view, 64)) continue;

            std::string error;
            const muse::JsonDocument document = muse::JsonDocument::fromJson(json, &error);
            if (!error.empty() || !document.isObject()) continue;

            const muse::JsonObject root = document.rootObject();
            const auto tracks = root.value("tracks");
            if (!tracks.isArray()) continue;

            const muse::JsonArray array = tracks.toArray();
            if (array.size() > max_audio_settings_tracks) continue;

            std::map<std::pair<std::string, std::string>, TrackSoloMute> solo_mute_map;
            for (std::size_t index = 0; index < array.size(); ++index) {
                const auto item = array.at(index);
                if (!item.isObject()) continue;
                const muse::JsonObject track = item.toObject();
                const auto part_id = track.value("partId");
                const auto instrument_id = track.value("instrumentId");
                if (!part_id.isString() || !instrument_id.isString()) continue;

                TrackSoloMute sm;
                const auto solo_mute = track.value("soloMuteState");
                if (solo_mute.isObject()) {
                    const muse::JsonObject state = solo_mute.toObject();
                    if (const auto solo = settings_bool(state, "solo")) sm.solo = *solo;
                    if (const auto muted = settings_bool(state, "mute")) sm.muted = *muted;
                }
                solo_mute_map.emplace(
                    std::make_pair(part_id.toStdString(), instrument_id.toStdString()), sm);
            }
            result.excerpt_tracks_by_index[i] = solo_mute_map;
            result.excerpt_tracks_by_name[name] = std::move(solo_mute_map);
        }
    } catch (...) {
        // A damaged archive or unexpected JSON shape is not a score-open error.
        return {};
    }
    return result;
}

AudioMix score_audio_default_mix(const ScoreAudioMixDefaults& defaults,
                                 const std::vector<ScoreAudioTrackInfo>& tracks,
                                 int excerpt_id, const std::string& excerpt_name)
{
    const std::map<std::pair<std::string, std::string>, TrackSoloMute>* excerpt_solo_mute
        = excerpt_id >= 0 ? defaults.get_excerpt_map(excerpt_id, excerpt_name) : nullptr;

    AudioMix mix;
    mix.master = defaults.master;
    mix.tracks.reserve(tracks.size());
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        const ScoreAudioTrackInfo& track = tracks[index];
        AudioOutputParams output;
        const auto key = std::make_pair(track.part_id, track.instrument_id);
        if (const auto found = defaults.tracks.find(key); found != defaults.tracks.end()) {
            output.volume_db = found->second.volume_db;
            output.balance = found->second.balance;
            if (excerpt_id < 0) {
                output.solo = found->second.solo;
                output.muted = found->second.muted;
            }
        }
        if (excerpt_id >= 0) {
            // An excerpt file owns its solo/mute state; without one the track
            // is neither soloed nor muted.
            if (excerpt_solo_mute) {
                const auto found = excerpt_solo_mute->find(key);
                if (found != excerpt_solo_mute->end()) {
                    output.solo = found->second.solo;
                    output.muted = found->second.muted;
                }
            }
        }
        mix.tracks.push_back(AudioTrackParams{ static_cast<std::int64_t>(index), output });
    }

    derive_force_mute(mix);
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        if (tracks[index].chord_symbols) {
            mix.tracks[index].output.muted = true;
            mix.tracks[index].output.force_mute = true;
        }
    }
    return mix;
}

} // namespace music
