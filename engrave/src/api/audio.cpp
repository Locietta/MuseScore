// The audio API: audio_tracks() and render_audio().  The tracks and the default
// mix come from the A2 score code in engrave/src/audio/ (score_audio), the mix
// and the render from the A2 engine driver there (engine_audio).
//
// The driver encodes to a file; the public call hands the encoded stream to a
// sink. The WAV and FLAC encoders patch their headers when the stream ends (the
// WAV chunk sizes, FLAC's STREAMINFO), so a truly streaming destination cannot
// work: the render goes to one temporary file in the engine's cache directory,
// which is then read back in chunks and removed.

#include <engrave/document.h>

#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "engrave/src/api/handles.h"
#include "engrave/src/api/state.h"
#include "engrave/src/audio/engine_audio.h"
#include "engrave/src/audio/score_audio.h"
#include "engrave/src/runtime/noqt_runtime.h"

#include "engraving/dom/excerpt.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/score.h"
#include "engraving/engravingproject.h"

namespace engrave {
namespace {

using mu::engraving::MasterScore;

constexpr std::size_t audio_chunk_bytes = 1 << 20;

std::unexpected<Error> fail(ErrorCode code, std::string message) {
    return std::unexpected(Error{code, std::move(message)});
}

music::EngineAudioFormat engine_format(AudioFormat format) {
    switch (format) {
    case AudioFormat::Wav: return music::EngineAudioFormat::Wav;
    case AudioFormat::Flac: return music::EngineAudioFormat::Flac;
    case AudioFormat::Opus: return music::EngineAudioFormat::Ogg;
    case AudioFormat::Mp3: return music::EngineAudioFormat::Mp3;
    case AudioFormat::Aac: return music::EngineAudioFormat::Aac;
    }
    return music::EngineAudioFormat::Wav;
}

const char* file_extension(AudioFormat format) {
    switch (format) {
    case AudioFormat::Wav: return ".wav";
    case AudioFormat::Flac: return ".flac";
    case AudioFormat::Opus: return ".ogg";
    case AudioFormat::Mp3: return ".mp3";
    case AudioFormat::Aac: return ".aac";
    }
    return ".audio";
}

bool is_lossy(AudioFormat format) {
    return format == AudioFormat::Opus || format == AudioFormat::Mp3 || format == AudioFormat::Aac;
}

AudioMix to_mix(const music::AudioOutputParams& output) {
    return { output.volume_db, output.balance, output.muted, output.solo };
}

// A mix value the engine accepts; the ranges are the ones the A2 host enforced.
std::optional<Error> mix_error(std::optional<double> volume_db, std::optional<double> balance,
                               std::string_view label) {
    if (volume_db && (!std::isfinite(*volume_db) || *volume_db < -120.0 || *volume_db > 24.0))
        return Error{ErrorCode::InvalidArgument, std::format("{} volume_db must be finite and in [-120, 24]", label)};
    if (balance && (!std::isfinite(*balance) || *balance < -1.0 || *balance > 1.0))
        return Error{ErrorCode::InvalidArgument, std::format("{} balance must be finite and in [-1, 1]", label)};
    return std::nullopt;
}

// The index of the SoundFont a track plays: the default (the first) unless the
// track names one by index or by name.
Result<int> soundfont_index(const SoundFontSelection& selection, const std::vector<std::filesystem::path>& fonts) {
    if (std::holds_alternative<std::monostate>(selection)) return 0;
    if (const auto* index = std::get_if<std::int32_t>(&selection)) {
        if (*index < 0 || static_cast<std::size_t>(*index) >= fonts.size())
            return fail(ErrorCode::InvalidArgument, std::format("no SoundFont {} of {}", *index, fonts.size()));
        return *index;
    }
    const std::string& name = std::get<std::string>(selection);
    for (std::size_t index = 0; index < fonts.size(); ++index) {
        if (utf8(fonts[index].stem()) == name) return static_cast<int>(index);
    }
    std::string available;
    for (const auto& font : fonts) available += (available.empty() ? "" : ", ") + utf8(font.stem());
    return fail(ErrorCode::NotFound, std::format("no SoundFont named '{}' (available: {})", name, available));
}

struct TempFile {
    std::filesystem::path path;
    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
};

// The tracks of the selected score and its default mix, from the audiosettings
// the document was opened with (Document::Impl::audio_defaults).
struct MixContext {
    std::vector<music::ScoreAudioTrackInfo> tracks;
    music::AudioMix defaults;
};

Result<MixContext> resolve_mix(const music::ScoreAudioMixDefaults& audio_defaults, int excerpt_id,
                               mu::engraving::Score& selected, const std::string& excerpt_name) {
    MixContext context;
    try {
        context.tracks = music::score_audio_track_info(&selected);
    } catch (const std::invalid_argument& error) {
        return fail(ErrorCode::InvalidArgument, error.what());
    }
    context.defaults = music::score_audio_default_mix(audio_defaults, context.tracks, excerpt_id, excerpt_name);
    return context;
}

std::string excerpt_name_of(const music::ScoreProject& project, int excerpt_id) {
    if (excerpt_id < 0) return {};
    const auto& excerpts = project->masterScore()->excerpts();
    if (static_cast<std::size_t>(excerpt_id) < excerpts.size() && excerpts[excerpt_id])
        return excerpts[excerpt_id]->name().toStdString();
    return {};
}

} // namespace

Result<AudioTracks> Document::audio_tracks(const ScoreSel& selection) {
    impl_->engine->check_thread();
    MasterScore& master = *impl_->project->masterScore();
    const Result<int> excerpt_id = excerpt_id_of(master, selection);
    if (!excerpt_id) return std::unexpected(excerpt_id.error());
    const Result<mu::engraving::Score*> selected = select_score(master, selection);
    if (!selected) return std::unexpected(selected.error());
    const Result<MixContext> context = resolve_mix(impl_->audio_defaults, *excerpt_id, **selected,
                                                   excerpt_name_of(impl_->project, *excerpt_id));
    if (!context) return std::unexpected(context.error());

    AudioTracks result;
    result.master = to_mix(context->defaults.master);
    result.tracks.reserve(context->tracks.size());
    for (std::size_t index = 0; index < context->tracks.size(); ++index) {
        const music::ScoreAudioTrackInfo& track = context->tracks[index];
        AudioTrackInfo info;
        info.track = static_cast<std::int32_t>(index);
        info.part_id = track.part_id;
        info.instrument_id = track.instrument_id;
        info.part_name = track.part_name;
        info.instrument_name = track.instrument_name;
        info.chord_symbols = track.chord_symbols;
        info.mix = to_mix(context->defaults.tracks[index].output);
        result.tracks.push_back(std::move(info));
    }
    return result;
}

Result<AudioRenderResult> Document::render_audio(const AudioOptions& options, const ByteSink& sink) {
    impl_->engine->check_thread();
    EngineState& engine = *impl_->engine;
    MasterScore& master = *impl_->project->masterScore();
    const Result<int> excerpt_id = excerpt_id_of(master, options.score);
    if (!excerpt_id) return std::unexpected(excerpt_id.error());
    const Result<mu::engraving::Score*> selected = select_score(master, options.score);
    if (!selected) return std::unexpected(selected.error());

    if (engine.soundfonts.empty())
        return fail(ErrorCode::Unsupported, "the Engine was created without a SoundFont");
    if (options.sample_rate != 0 && options.sample_rate != engine.audio_sample_rate) {
        return fail(ErrorCode::Unsupported,
                    std::format("the Engine renders at {} Hz, not {}", engine.audio_sample_rate, options.sample_rate));
    }
    if (options.bit_depth != 0) {
        if (is_lossy(options.format))
            return fail(ErrorCode::InvalidArgument, "bit_depth applies to WAV and FLAC, not to a lossy format");
        const bool allowed = options.format == AudioFormat::Wav
                             ? options.bit_depth == 16 || options.bit_depth == 24 || options.bit_depth == 32
                             : options.bit_depth == 16 || options.bit_depth == 24;
        if (!allowed)
            return fail(ErrorCode::InvalidArgument, std::format("{} does not take a bit depth of {}", to_string(options.format),
                                                                options.bit_depth));
    }
    if (options.bitrate_kbps != 0) {
        if (!is_lossy(options.format))
            return fail(ErrorCode::InvalidArgument, "bitrate_kbps applies to Opus, MP3 and AAC, not to a lossless format");
        if (options.bitrate_kbps < 32 || options.bitrate_kbps > 512)
            return fail(ErrorCode::InvalidArgument,
                        std::format("bitrate_kbps must be between 32 and 512, not {}", options.bitrate_kbps));
    }
    if (!std::isfinite(options.reverb_send) || options.reverb_send < 0.0 || options.reverb_send > 1.0)
        return fail(ErrorCode::InvalidArgument, "reverb_send must be in [0, 1]");
    if (const auto error = mix_error(options.master_volume_db, options.master_balance, "master"))
        return std::unexpected(*error);

    const Result<MixContext> context = resolve_mix(impl_->audio_defaults, *excerpt_id, **selected,
                                                   excerpt_name_of(impl_->project, *excerpt_id));
    if (!context) return std::unexpected(context.error());

    std::vector<const AudioTrackOptions*> by_track(context->tracks.size(), nullptr);
    for (const AudioTrackOptions& track : options.tracks) {
        if (track.track < 0 || static_cast<std::size_t>(track.track) >= context->tracks.size())
            return fail(ErrorCode::InvalidArgument,
                        std::format("no track {} of {}", track.track, context->tracks.size()));
        if (by_track[static_cast<std::size_t>(track.track)])
            return fail(ErrorCode::InvalidArgument, std::format("track {} is named twice", track.track));
        if (!track.preset.empty())
            return fail(ErrorCode::Unsupported, "choosing a preset is not supported");
        if (const auto error = mix_error(track.volume_db, track.balance,
                                         std::format("track {}", track.track))) {
            return std::unexpected(*error);
        }
        by_track[static_cast<std::size_t>(track.track)] = &track;
    }

    music::AudioRenderRequest request;
    request.format = engine_format(options.format);
    request.excerpt_id = *excerpt_id;
    request.use_repeats = options.use_repeats;
    request.master_volume_db = options.master_volume_db.value_or(context->defaults.master.volume_db);
    request.master_balance = options.master_balance.value_or(context->defaults.master.balance);
    request.bit_rate = options.bitrate_kbps;
    request.bit_depth = options.bit_depth;
    request.reverb = options.reverb;
    request.reverb_send = options.reverb_send;
    request.tracks.resize(context->tracks.size());
    for (std::size_t index = 0; index < context->tracks.size(); ++index) {
        const music::AudioOutputParams& defaults = context->defaults.tracks[index].output;
        music::EngineAudioTrackParams& track = request.tracks[index];
        track.volume_db = defaults.volume_db;
        track.balance = defaults.balance;
        track.muted = defaults.muted;
        track.solo = defaults.solo;
        if (const AudioTrackOptions* override = by_track[index]) {
            track.volume_db = override->volume_db.value_or(track.volume_db);
            track.balance = override->balance.value_or(track.balance);
            track.muted = override->mute.value_or(track.muted);
            track.solo = override->solo.value_or(track.solo);
            const Result<int> font = soundfont_index(override->soundfont, engine.soundfonts);
            if (!font) return std::unexpected(font.error());
            track.soundfont = *font;
        }
    }

    // The audio engine is process-global and starts on first use, never at
    // Engine::create: a caller that only reads or draws pages pays nothing.
    if (!music::engine_audio_initialized(engine.context->audio())) {
        music::EngineAudioSetup setup;
        setup.sample_rate = engine.audio_sample_rate;
        for (const auto& soundfont : engine.soundfonts) setup.soundfonts.push_back(utf8(soundfont));
        std::string error;
        if (!music::initialize_engine_audio(engine.context->audio(), setup, &error)) return fail(ErrorCode::InvalidInput, error);
    }

    std::error_code ec;
    std::filesystem::create_directories(engine.cache_dir, ec);
    if (!std::filesystem::is_directory(engine.cache_dir, ec))
        return fail(ErrorCode::Io, "cannot use the cache directory '" + utf8(engine.cache_dir) + "' for the render");
#ifdef _WIN32
    const int process = _getpid();
#else
    const int process = getpid();
#endif
    TempFile file{ engine.cache_dir
                   / std::format("engrave-audio-{}-{}{}", process, ++engine.audio_render_index,
                                 file_extension(options.format)) };
    request.output_path = utf8(file.path);

    const music::AudioRenderResult rendered = music::export_score_audio(engine.context->audio(), &master, request);
    if (!rendered.ok) {
        switch (rendered.failure) {
        case music::AudioRenderResult::Failure::ResourceLimit: return fail(ErrorCode::ResourceLimit, rendered.error);
        case music::AudioRenderResult::Failure::InvalidScore: return fail(ErrorCode::InvalidArgument, rendered.error);
        case music::AudioRenderResult::Failure::EngineFailure:
        case music::AudioRenderResult::Failure::None: throw std::runtime_error(rendered.error);
        }
    }

    std::ifstream in(file.path, std::ios::binary);
    if (!in) return fail(ErrorCode::Io, "cannot read the rendered audio '" + utf8(file.path) + "'");
    std::uint64_t bytes = 0;
    std::vector<std::byte> chunk(audio_chunk_bytes);
    while (in) {
        in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        const auto count = in.gcount();
        if (count <= 0) break;
        sink(std::span<const std::byte>(chunk.data(), static_cast<std::size_t>(count)));
        bytes += static_cast<std::uint64_t>(count);
    }
    if (in.bad()) return fail(ErrorCode::Io, "cannot read the rendered audio '" + utf8(file.path) + "'");
    return AudioRenderResult{ bytes, rendered.frames, rendered.duration_secs };
}

} // namespace engrave
