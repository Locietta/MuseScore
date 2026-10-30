// SPDX-License-Identifier: GPL-3.0-only
//
// CLI probe for the headless MuseScore audio engine driver (roadmap step 5).
//
//   engine_audio_probe ASSETS SOUNDFONT SCORE OUT.EXT [--no-repeats] [--mute-part N] [--excerpt N]
//
// ASSETS is the native runtime asset root (.runtime/assets), SOUNDFONT an
// .sf2/.sf3 file, SCORE a MusicXML/MSCZ/MSCX/MIDI/GuitarPro file and OUT.EXT
// the target audio file. --mute-part N mutes every track of the zero-based
// part index N (repeatable; mix check); --excerpt N selects the zero-based
// excerpt to render (one excerpt per visible part is generated first when the
// score has none). --midi-events records the events FluidSynth handles together
// with their render-frame (sample) offset; at most the first 1,000,000 are
// printed and midiEventsTruncated marks a longer stream. The probe prints one
// JSON object on stdout; engine and engraving logs go to stderr.

#include "audio/engine_audio.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engraving/dom/masterscore.h"
#include "engraving/engravingproject.h"
#include "global/types/uri.h"
#include "runtime/noqt_runtime.h"
#include "score/score_excerpts.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::size_t kMaxScoreBytes = 8 * 1024 * 1024;

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string to_utf8(const fs::path& path)
{
    const std::u8string utf8 = path.u8string();
    std::string result;
    result.reserve(utf8.size());
    for (const char8_t c : utf8) {
        result.push_back(static_cast<char>(c));
    }
    return result;
}

std::string score_format_from_path(const fs::path& path)
{
    const std::string ext = lowercase(to_utf8(path.extension()));
    if (ext == ".musicxml" || ext == ".xml") {
        return "musicxml";
    }
    if (ext == ".mxl") {
        return "mxl";
    }
    if (ext == ".mscx") {
        return "mscx";
    }
    if (ext == ".mscz") {
        return "mscz";
    }
    if (ext == ".mid" || ext == ".midi" || ext == ".kar") {
        return "mid";
    }
    if (ext == ".gp" || ext == ".gp3" || ext == ".gp4" || ext == ".gp5" || ext == ".gp6"
        || ext == ".gp7" || ext == ".gpx" || ext == ".gtp" || ext == ".ptb") {
        return ext.substr(1);
    }
    throw std::invalid_argument("unsupported score extension: " + ext);
}

std::string audio_format_from_path(const fs::path& path)
{
    const std::string ext = lowercase(to_utf8(path.extension()));
    if (ext == ".wav") {
        return "wav";
    }
    if (ext == ".flac") {
        return "flac";
    }
    if (ext == ".ogg") {
        return "ogg";
    }
    if (ext == ".mp3") {
        return "mp3";
    }
    if (ext == ".aac") {
        return "aac";
    }
    throw std::invalid_argument("unsupported audio extension: " + ext);
}

music::EngineAudioFormat parse_audio_format(const std::string& name)
{
    if (name == "wav") {
        return music::EngineAudioFormat::Wav;
    }
    if (name == "flac") {
        return music::EngineAudioFormat::Flac;
    }
    if (name == "ogg") {
        return music::EngineAudioFormat::Ogg;
    }
    if (name == "mp3") {
        return music::EngineAudioFormat::Mp3;
    }
    return music::EngineAudioFormat::Aac;
}

std::string read_file(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        throw std::runtime_error("cannot open score file: " + to_utf8(path));
    }

    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        throw std::runtime_error("cannot stat score file: " + to_utf8(path));
    }
    if (size == 0 || size > kMaxScoreBytes) {
        throw std::runtime_error("score file must contain 1..8388608 bytes");
    }

    std::string data(static_cast<std::size_t>(size), '\0');
    stream.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (!stream || static_cast<std::size_t>(stream.gcount()) != data.size()) {
        throw std::runtime_error("short read from score file: " + to_utf8(path));
    }
    return data;
}

std::uint64_t peak_working_set_bytes()
{
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) {
        return 0;
    }
    return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
#else
    // VmHWM is the peak resident set size in KiB on Linux.
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            return std::strtoull(line.c_str() + 6, nullptr, 10) * 1024ULL;
        }
    }
    return 0;
#endif
}

double seconds_since(const std::chrono::steady_clock::time_point& start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

Json track_json(const music::EngineAudioTrackInfo& track)
{
    return Json {
        { "partId", track.part_id },
        { "instrumentId", track.instrument_id },
        { "partName", track.part_name },
        { "instrumentName", track.instrument_name },
        { "chordSymbols", track.chord_symbols },
        { "partIndex", track.part_index },
    };
}

Json midi_event_json(const muse::midi::Event& event)
{
    Json json = { { "opcode", event.opcodeString() } };
    if (!event.isChannelVoice()) {
        return json;
    }

    json["channel"] = static_cast<int>(event.channel());
    switch (event.opcode()) {
    case muse::midi::Event::Opcode::NoteOn:
    case muse::midi::Event::Opcode::NoteOff:
        json["note"] = static_cast<int>(event.note());
        json["velocity"] = static_cast<int>(event.velocity7());
        break;
    case muse::midi::Event::Opcode::ControlChange:
        json["index"] = static_cast<int>(event.index());
        json["data"] = static_cast<int>(event.data7());
        break;
    case muse::midi::Event::Opcode::PitchBend:
        json["pitchBend"] = static_cast<int>(event.pitchBend14());
        break;
    case muse::midi::Event::Opcode::ProgramChange:
        json["program"] = static_cast<int>(event.program());
        break;
    default:
        break;
    }

    return json;
}
} // namespace

int main(int argc, char** argv)
{
    Json out = Json::object();
    out["ok"] = false;

    if (argc < 5) {
        std::cerr << "Usage: engine_audio_probe ASSETS SOUNDFONT SCORE OUT.EXT"
                     " [--no-repeats] [--mute-part N]\n";
        return 2;
    }

    const std::string assets = argv[1];
    const std::string soundfont = argv[2];
    const fs::path scorePath = fs::u8path(argv[3]);
    const fs::path outputPath = fs::u8path(argv[4]);

    bool useRepeats = true;
    bool captureMidi = false;
    std::vector<int> muteParts;
    int excerptId = -1;
    try {
        for (int i = 5; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--no-repeats") {
                useRepeats = false;
            } else if (arg == "--midi-events") {
                captureMidi = true;
            } else if (arg == "--mute-part" && i + 1 < argc) {
                muteParts.push_back(std::stoi(argv[++i]));
            } else if (arg == "--excerpt" && i + 1 < argc) {
                excerptId = std::stoi(argv[++i]);
            } else {
                std::cerr << "Unknown argument: " << arg << '\n';
                return 2;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Invalid argument: " << e.what() << '\n';
        return 2;
    }

    try {
        out["assets"] = assets;
        out["score"] = to_utf8(scorePath);
        out["output"] = to_utf8(outputPath);
        out["repeats"] = useRepeats;
        out["excerptId"] = excerptId;
        out["muteParts"] = muteParts;
        out["format"] = audio_format_from_path(outputPath);

        const std::string scoreFormat = score_format_from_path(scorePath);
        const std::string scoreBytes = read_file(scorePath);
        out["scoreFormat"] = scoreFormat;

        auto runtimeStart = std::chrono::steady_clock::now();
        music::initialize_native_runtime(assets);
        auto score = music::load_native_score(scoreBytes, scoreFormat);
        mu::engraving::MasterScore* master = score->masterScore();
        out["loadSeconds"] = seconds_since(runtimeStart);

        auto engineStart = std::chrono::steady_clock::now();
        std::string bootstrapError;
        const bool engineReady = music::initialize_engine_audio(soundfont, &bootstrapError);
        out["engineSeconds"] = seconds_since(engineStart);
        out["soundfont"] = {
            { "path", soundfont },
            { "resourceId", music::engine_soundfont_resource_id() },
            { "uri", music::engine_soundfont_uri() },
            { "uriRoundTrip", false },
        };
        if (!engineReady) {
            throw std::runtime_error(bootstrapError.empty() ? "engine initialisation failed" : bootstrapError);
        }

        {
            // Windows drive letters and non-ASCII names must survive
            // Uri::fromLocalFile()/toLocalFile() (fromLocalFile percent-encodes
            // ':' and non-safe bytes, uri.cpp:117-154).
            const muse::Uri uri(music::engine_soundfont_uri());
            const std::string roundTripped = uri.toLocalFile().toStdString();
            std::error_code ec;
            const fs::path expected = fs::absolute(fs::u8path(soundfont), ec).lexically_normal();
            out["soundfont"]["uriRoundTrip"] = !ec && fs::u8path(roundTripped).lexically_normal() == expected;
        }

        if (excerptId >= 0) {
            // One excerpt per visible part when the file has none; existing
            // excerpts are left untouched.
            music::generate_score_excerpts(master);
        }
        out["excerptCount"] = master->excerpts().size();
        const std::vector<music::EngineAudioTrackInfo> tracks =
            music::engine_audio_tracks(master, excerptId);
        out["trackCount"] = tracks.size();

        music::AudioRenderRequest request;
        request.format = parse_audio_format(out["format"].get<std::string>());
        request.output_path = to_utf8(outputPath);
        request.excerpt_id = excerptId;
        request.use_repeats = useRepeats;
        request.tracks.assign(tracks.size(), music::EngineAudioTrackParams{});
        for (const int mutePart : muteParts) {
            if (mutePart < 0 || static_cast<std::size_t>(mutePart) >= master->parts().size()) {
                throw std::invalid_argument("--mute-part index is out of range");
            }
            for (std::size_t index = 0; index < tracks.size(); ++index) {
                if (tracks[index].part_index == static_cast<std::size_t>(mutePart)) {
                    request.tracks[index].muted = true;
                }
            }
        }

        auto exportStart = std::chrono::steady_clock::now();
        std::size_t midiEventCount = 0;
        std::vector<Json> midiEvents;
        if (captureMidi) {
            constexpr std::size_t kMaxMidiEvents = 1000000;
            midiEvents.reserve(4096);
            music::set_engine_audio_midi_event_hook(
                [&midiEventCount, &midiEvents](const muse::midi::Event& event, std::uint64_t frame) {
                    ++midiEventCount;
                    if (midiEvents.size() < kMaxMidiEvents) {
                        Json json = midi_event_json(event);
                        json["frame"] = frame;
                        midiEvents.push_back(std::move(json));
                    }
                });
        }
        const music::AudioRenderResult result = music::export_score_audio(master, request);
        if (captureMidi) {
            music::set_engine_audio_midi_event_hook(nullptr);
        }
        out["exportSeconds"] = seconds_since(exportStart);

        out["playTime"] = result.play_time_secs;
        out["duration"] = result.duration_secs;
        out["frames"] = result.frames;
        out["bytes"] = result.bytes;
        out["ok"] = result.ok;
        out["error"] = result.error;
        out["tracks"] = Json::array();
        for (const music::EngineAudioTrackInfo& track : tracks) {
            out["tracks"].push_back(track_json(track));
        }
        out["midiEventTotal"] = midiEventCount;
        out["midiEventsTruncated"] = midiEventCount > midiEvents.size();
        if (captureMidi) {
            out["midiEvents"] = std::move(midiEvents);
        }
        out["peakWorkingSetBytes"] = peak_working_set_bytes();
        out["peakWorkingSetMiB"] = peak_working_set_bytes() / (1024.0 * 1024.0);
    } catch (const std::exception& e) {
        out["ok"] = false;
        out["error"] = e.what();
        out["peakWorkingSetBytes"] = peak_working_set_bytes();
        out["peakWorkingSetMiB"] = peak_working_set_bytes() / (1024.0 * 1024.0);
    }

    std::cout << out.dump(2) << '\n';
    return out["ok"].get<bool>() ? 0 : 1;
}
