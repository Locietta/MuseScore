// The audio half of the output API: audio_tracks() and render_audio().  Every
// render goes through the public API only, with the SoundFonts main() loaded
// (the first is the default; a second one, when given, is the per-track
// alternative).
//
// The A2 audio comparison (features within the recorded tolerances) is done by
// the superproject's scripts/test_a4_output.py on this binary's probe; here the
// checks are the ones a golden cannot make: each encoder writes its container,
// the sink receives the stream in order, the SoundFont list and the per-track
// choice reach the renderer, the reverb switch changes the mix, options are
// validated, and D4 holds: a second render in this process equals a fresh one.

#include "output_fixture.h"

#include <engrave/engrave.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace music::test::detail {

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(first, first + text.size());
}

std::string text_of(std::span<const std::byte> bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::uint32_t little32(const std::byte* bytes) {
    return std::uint32_t(std::to_integer<std::uint8_t>(bytes[0]))
           | (std::uint32_t(std::to_integer<std::uint8_t>(bytes[1])) << 8)
           | (std::uint32_t(std::to_integer<std::uint8_t>(bytes[2])) << 16)
           | (std::uint32_t(std::to_integer<std::uint8_t>(bytes[3])) << 24);
}

std::uint16_t little16(const std::byte* bytes) {
    return std::uint16_t(std::to_integer<std::uint8_t>(bytes[0])
                         | (std::uint16_t(std::to_integer<std::uint8_t>(bytes[1])) << 8));
}

struct Wav {
    std::uint16_t format = 0; // 3 = IEEE float
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t bits = 0;
    std::vector<float> samples; // interleaved, float32 form only

    double rms() const {
        if (samples.empty()) return 0.0;
        double sum = 0.0;
        for (const float sample : samples) sum += double(sample) * sample;
        return std::sqrt(sum / double(samples.size()));
    }
};

std::optional<Wav> decode_wav(std::span<const std::byte> bytes, std::string& error) {
    if (bytes.size() < 12 || text_of(bytes.first(4)) != "RIFF" || text_of(bytes.subspan(8, 4)) != "WAVE") {
        error = "not a RIFF/WAVE file";
        return std::nullopt;
    }
    Wav wav;
    std::span<const std::byte> data;
    for (std::size_t offset = 12; offset + 8 <= bytes.size();) {
        const std::string type = text_of(bytes.subspan(offset, 4));
        const std::uint32_t size = little32(bytes.data() + offset + 4);
        if (offset + 8 + size > bytes.size()) {
            error = "a truncated WAV chunk";
            return std::nullopt;
        }
        if (type == "fmt ") {
            wav.format = little16(bytes.data() + offset + 8);
            wav.channels = little16(bytes.data() + offset + 10);
            wav.sample_rate = little32(bytes.data() + offset + 12);
            wav.bits = little16(bytes.data() + offset + 22);
        } else if (type == "data") {
            data = bytes.subspan(offset + 8, size);
        }
        offset += 8 + size + (size & 1);
    }
    if (wav.channels == 0 || wav.sample_rate == 0 || data.empty()) {
        error = "a WAV without fmt or data";
        return std::nullopt;
    }
    if (wav.format != 3 || wav.bits != 32) {
        // Only the float32 form is decoded; the others are checked by header.
        return wav;
    }
    wav.samples.resize(data.size() / 4);
    std::memcpy(wav.samples.data(), data.data(), wav.samples.size() * 4);
    return wav;
}

struct Rendered {
    std::vector<std::byte> bytes;
    std::vector<std::size_t> chunks;
    engrave::AudioRenderResult result;
};

Rendered render_audio(engrave::Document& document, const engrave::AudioOptions& options) {
    Rendered rendered;
    const auto result = document.render_audio(options, [&rendered](std::span<const std::byte> chunk) {
        rendered.chunks.push_back(chunk.size());
        rendered.bytes.insert(rendered.bytes.end(), chunk.begin(), chunk.end());
    });
    if (result) rendered.result = *result;
    return rendered;
}

std::unique_ptr<engrave::Document> open_score(const std::string& name) {
    const std::filesystem::path path = corpus_file(name);
    if (path.empty()) return nullptr;
    auto document = g_engine->open(path);
    if (!document) return nullptr;
    return std::move(*document);
}

std::filesystem::path temp_file(const std::string& name) {
    static int counter = 0;
    return std::filesystem::temp_directory_path() / ("engrave-audio-test-" + std::to_string(++counter) + "-" + name);
}

const std::string& audio_score() {
    static const std::string name = "ghost_note.mscx";
    return name;
}

} // namespace music::test::detail

namespace {

namespace detail = music::test::detail;
using music::test::g_soundfonts;

TEST(Audio, EachEncoderWritesItsContainer) {
    auto document = detail::open_score(detail::audio_score());
    ASSERT_TRUE(document) << "the corpus has no " << detail::audio_score();

    struct Case {
        engrave::AudioFormat format;
        bool (*magic)(std::span<const std::byte>);
        const char* name;
    };
    const Case cases[] = {
        { engrave::AudioFormat::Wav,
          [](std::span<const std::byte> b) { return b.size() > 12 && detail::text_of(b.first(4)) == "RIFF"; }, "WAV" },
        { engrave::AudioFormat::Flac,
          [](std::span<const std::byte> b) { return b.size() > 4 && detail::text_of(b.first(4)) == "fLaC"; }, "FLAC" },
        { engrave::AudioFormat::Opus,
          [](std::span<const std::byte> b) { return b.size() > 4 && detail::text_of(b.first(4)) == "OggS"; }, "Opus in Ogg" },
        { engrave::AudioFormat::Mp3,
          [](std::span<const std::byte> b) {
              return b.size() > 4
                     && (detail::text_of(b.first(3)) == "ID3"
                         || (std::to_integer<std::uint8_t>(b[0]) == 0xff
                             && (std::to_integer<std::uint8_t>(b[1]) & 0xe0) == 0xe0));
          },
          "MP3" },
        { engrave::AudioFormat::Aac,
          [](std::span<const std::byte> b) {
              return b.size() > 4 && std::to_integer<std::uint8_t>(b[0]) == 0xff
                     && (std::to_integer<std::uint8_t>(b[1]) & 0xf0) == 0xf0;
          },
          "AAC" },
    };

    for (const Case& entry : cases) {
        engrave::AudioOptions options;
        options.format = entry.format;
        const detail::Rendered rendered = detail::render_audio(*document, options);
        ASSERT_GT(rendered.bytes.size(), 0u) << entry.name << " produced nothing";
        EXPECT_TRUE(entry.magic(rendered.bytes)) << entry.name << " does not start with its container";
        EXPECT_GT(rendered.result.frames, 0u) << entry.name;
        EXPECT_GT(rendered.result.bytes, 0u) << entry.name;
        EXPECT_NEAR(rendered.result.seconds, double(rendered.result.frames) / 44100.0, 0.01) << entry.name;
        if (entry.format == engrave::AudioFormat::Wav) {
            std::string error;
            const auto wav = detail::decode_wav(rendered.bytes, error);
            ASSERT_TRUE(wav) << error;
            EXPECT_EQ(wav->channels, 2);
            EXPECT_EQ(wav->sample_rate, 44100u);
            EXPECT_EQ(wav->samples.size(), rendered.result.frames * 2);
            EXPECT_GT(wav->rms(), 1e-4) << "the render is silent";
        }
    }
}

TEST(Audio, TheSinkReceivesTheStreamInChunks) {
    auto document = detail::open_score(detail::audio_score());
    ASSERT_TRUE(document);
    engrave::AudioOptions options;
    options.format = engrave::AudioFormat::Wav;
    const detail::Rendered rendered = detail::render_audio(*document, options);
    ASSERT_GT(rendered.bytes.size(), 0u);
    // A 4.8 s float32 stereo WAV is about 1.7 MiB, so the 1 MiB chunk size shows.
    EXPECT_GE(rendered.chunks.size(), 2u) << "the sink saw one chunk for " << rendered.bytes.size() << " bytes";
    std::size_t total = 0;
    for (const std::size_t chunk : rendered.chunks) total += chunk;
    EXPECT_EQ(total, rendered.bytes.size());
    EXPECT_EQ(rendered.result.bytes, rendered.bytes.size());
}

// D4: the engine keeps state between renders (tracks, synthesizer instances,
// reverb tails). render_audio drops it before every render, so the second
// render in this process must equal a render in a fresh process.
TEST(Audio, SecondRenderEqualsAFreshProcess) {
    auto document = detail::open_score(detail::audio_score());
    ASSERT_TRUE(document) << "the corpus has no " << detail::audio_score();
    engrave::AudioOptions options;
    options.format = engrave::AudioFormat::Wav;

    const detail::Rendered first = detail::render_audio(*document, options);
    const detail::Rendered second = detail::render_audio(*document, options);
    ASSERT_FALSE(first.bytes.empty());
    ASSERT_EQ(second.bytes.size(), first.bytes.size());
    EXPECT_EQ(second.bytes, first.bytes) << "the second render in one process differs from the first";

    ASSERT_TRUE(std::filesystem::is_regular_file(music::test::probe_binary()))
        << "no probe at " << music::test::probe_binary().string();
    const auto out = detail::temp_file("fresh.wav");
    std::vector<std::string> arguments = { "--assets=" + music::test::g_assets.string() };
    for (const auto& soundfont : g_soundfonts) arguments.push_back("--soundfont=" + soundfont.string());
    arguments.push_back("--audio=" + music::test::corpus_file(detail::audio_score()).string());
    arguments.push_back("--format=wav");
    arguments.push_back("--out=" + out.string());
    ASSERT_EQ(music::test::run_probe(arguments), 0) << "the probe failed";
    const std::vector<std::byte> fresh = detail::read_file(out);
    std::filesystem::remove(out);
    ASSERT_FALSE(fresh.empty());
    EXPECT_EQ(fresh, second.bytes) << "the second in-process render differs from a fresh-process render";
}

TEST(Audio, TheSoundFontListSelectsTheDefaultAndPerTrack) {
    auto document = detail::open_score(detail::audio_score());
    ASSERT_TRUE(document);
    ASSERT_GE(g_soundfonts.size(), 1u);
    const auto track_list = document->audio_tracks();
    ASSERT_TRUE(track_list) << track_list.error().message;
    ASSERT_FALSE(track_list->tracks.empty());
    const std::int32_t track = track_list->tracks.front().track;

    engrave::AudioOptions options;
    options.format = engrave::AudioFormat::Wav;
    const detail::Rendered defaults = detail::render_audio(*document, options);
    ASSERT_FALSE(defaults.bytes.empty());

    if (g_soundfonts.size() < 2) {
        GTEST_SKIP() << "pass --soundfont= twice to compare two SoundFonts";
    }
    engrave::AudioOptions per_track = options;
    engrave::AudioTrackOptions choice;
    choice.track = track;
    choice.soundfont = 1;
    per_track.tracks.push_back(choice);
    const detail::Rendered alternate = detail::render_audio(*document, per_track);
    ASSERT_FALSE(alternate.bytes.empty());
    EXPECT_NE(alternate.bytes, defaults.bytes) << "the per-track SoundFont did not reach the render";

    engrave::AudioTrackOptions by_name = choice;
    by_name.soundfont = g_soundfonts[1].stem().string();
    per_track.tracks.front() = by_name;
    const detail::Rendered named = detail::render_audio(*document, per_track);
    ASSERT_FALSE(named.bytes.empty());
    EXPECT_EQ(named.bytes, alternate.bytes) << "selecting the SoundFont by name differs from its index";

    choice.soundfont = "no such font";
    per_track.tracks.front() = choice;
    const auto missing = document->render_audio(per_track, [](std::span<const std::byte>) {});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, engrave::ErrorCode::NotFound);

    choice.soundfont = 7;
    per_track.tracks.front() = choice;
    const auto out_of_range = document->render_audio(per_track, [](std::span<const std::byte>) {});
    ASSERT_FALSE(out_of_range);
    EXPECT_EQ(out_of_range.error().code, engrave::ErrorCode::InvalidArgument);
}

TEST(Audio, TheReverbSwitchChangesTheMix) {
    auto document = detail::open_score(detail::audio_score());
    ASSERT_TRUE(document);
    engrave::AudioOptions wet;
    wet.format = engrave::AudioFormat::Wav;
    engrave::AudioOptions dry = wet;
    dry.reverb = false;
    const detail::Rendered reverb = detail::render_audio(*document, wet);
    const detail::Rendered direct = detail::render_audio(*document, dry);
    ASSERT_FALSE(reverb.bytes.empty());
    ASSERT_EQ(direct.bytes.size(), reverb.bytes.size());
    EXPECT_NE(direct.bytes, reverb.bytes) << "the reverb switch did not change the render";
    std::string error;
    const auto wav = detail::decode_wav(direct.bytes, error);
    ASSERT_TRUE(wav) << error;
    EXPECT_GT(wav->rms(), 1e-4) << "the dry render is silent";
}

TEST(Audio, EncoderSettingsReachTheEncoder) {
    auto document = detail::open_score(detail::audio_score());
    ASSERT_TRUE(document);
    engrave::AudioOptions options;
    options.format = engrave::AudioFormat::Wav;
    const detail::Rendered float32 = detail::render_audio(*document, options);

    options.bit_depth = 16;
    const detail::Rendered int16 = detail::render_audio(*document, options);
    ASSERT_FALSE(int16.bytes.empty());
    std::string error;
    const auto wav = detail::decode_wav(int16.bytes, error);
    ASSERT_TRUE(wav) << error;
    EXPECT_EQ(wav->bits, 16);
    EXPECT_EQ(wav->format, 1) << "16-bit WAV is not integer PCM";
    EXPECT_LT(int16.bytes.size(), float32.bytes.size());

    engrave::AudioOptions lossy;
    lossy.format = engrave::AudioFormat::Mp3;
    lossy.bitrate_kbps = 64;
    const detail::Rendered small = detail::render_audio(*document, lossy);
    lossy.bitrate_kbps = 320;
    const detail::Rendered large = detail::render_audio(*document, lossy);
    ASSERT_FALSE(small.bytes.empty());
    ASSERT_FALSE(large.bytes.empty());
    EXPECT_LT(small.bytes.size(), large.bytes.size()) << "a higher bitrate did not make a larger MP3";
}

TEST(Audio, OptionsAreValidated) {
    auto document = detail::open_score(detail::audio_score());
    ASSERT_TRUE(document);
    const auto sink = [](std::span<const std::byte>) {};
    const auto code_of = [&document, &sink](engrave::AudioOptions options) -> std::optional<engrave::ErrorCode> {
        const auto result = document->render_audio(options, sink);
        return result ? std::nullopt : std::optional<engrave::ErrorCode>(result.error().code);
    };

    engrave::AudioOptions options;
    options.sample_rate = 48000;
    EXPECT_EQ(code_of(options), engrave::ErrorCode::Unsupported) << "a sample rate other than the engine's";
    options.sample_rate = 44100;
    EXPECT_EQ(code_of(options), std::nullopt) << "the engine's own sample rate";

    options = {};
    engrave::AudioTrackOptions track;
    track.preset = "Grand Piano";
    options.tracks.push_back(track);
    EXPECT_EQ(code_of(options), engrave::ErrorCode::Unsupported) << "a preset";

    options = {};
    options.tracks.push_back(engrave::AudioTrackOptions{ .track = 99 });
    EXPECT_EQ(code_of(options), engrave::ErrorCode::InvalidArgument) << "a track that does not exist";
    options = {};
    options.tracks.push_back(engrave::AudioTrackOptions{ .track = 0 });
    options.tracks.push_back(engrave::AudioTrackOptions{ .track = 0 });
    EXPECT_EQ(code_of(options), engrave::ErrorCode::InvalidArgument) << "a track named twice";

    options = {};
    options.bit_depth = 16;
    options.format = engrave::AudioFormat::Flac;
    EXPECT_EQ(code_of(options), std::nullopt) << "FLAC takes 16 bits";
    options.bit_depth = 32;
    EXPECT_EQ(code_of(options), engrave::ErrorCode::InvalidArgument) << "FLAC takes no float samples";
    options = {};
    options.bit_depth = 16;
    options.format = engrave::AudioFormat::Mp3;
    EXPECT_EQ(code_of(options), engrave::ErrorCode::InvalidArgument) << "a bit depth for a lossy format";
    options = {};
    options.bitrate_kbps = 192;
    options.format = engrave::AudioFormat::Wav;
    EXPECT_EQ(code_of(options), engrave::ErrorCode::InvalidArgument) << "a bitrate for a lossless format";
    options = {};
    options.bitrate_kbps = 1000;
    options.format = engrave::AudioFormat::Aac;
    EXPECT_EQ(code_of(options), engrave::ErrorCode::InvalidArgument) << "a bitrate above the range";

    options = {};
    options.reverb_send = 1.5;
    EXPECT_EQ(code_of(options), engrave::ErrorCode::InvalidArgument) << "a reverb send above 1";
    options = {};
    options.master_volume_db = 25.0;
    EXPECT_EQ(code_of(options), engrave::ErrorCode::InvalidArgument) << "a master volume above 24 dB";
    options = {};
    options.tracks.push_back(engrave::AudioTrackOptions{ .track = 0, .balance = 2.0 });
    EXPECT_EQ(code_of(options), engrave::ErrorCode::InvalidArgument) << "a balance outside [-1, 1]";
}

TEST(Audio, AudioTracksReportTheDefaultsAndTheExcerpt) {
    auto document = detail::open_score("custom_part_name.mscx");
    ASSERT_TRUE(document) << "the corpus has no custom_part_name.mscx";
    const auto tracks = document->audio_tracks();
    ASSERT_TRUE(tracks) << tracks.error().message;
    ASSERT_FALSE(tracks->tracks.empty());
    for (std::size_t index = 0; index < tracks->tracks.size(); ++index) {
        EXPECT_EQ(tracks->tracks[index].track, static_cast<std::int32_t>(index));
        EXPECT_FALSE(tracks->tracks[index].part_id.empty());
        EXPECT_GE(tracks->tracks[index].mix.volume_db, -120.0);
        EXPECT_LE(tracks->tracks[index].mix.volume_db, 24.0);
    }

    // fixtures/audio/mix_parts.mscz's audiosettings.json mutes its Violin, and
    // its Violin excerpt's own file plays it; the default mix must say both
    // (fixtures/audio is part of the corpus).
    auto mixed = detail::open_score("mix_parts.mscz");
    if (!mixed) {
        std::fprintf(stderr, "note: fixtures/audio/mix_parts.mscz is not in the corpus; "
                             "the default mix and the excerpt mix were not checked\n");
        return;
    }
    const auto mix = mixed->audio_tracks();
    ASSERT_TRUE(mix) << mix.error().message;
    const auto violin = std::find_if(mix->tracks.begin(), mix->tracks.end(), [](const engrave::AudioTrackInfo& track) {
        return track.instrument_id == "violin";
    });
    ASSERT_NE(violin, mix->tracks.end()) << "mix_parts.mscz has no violin track";
    EXPECT_TRUE(violin->mix.mute) << "mix_parts.mscz's muted Violin is not in the default mix";

    const auto outline = mixed->outline();
    ASSERT_TRUE(outline);
    ASSERT_GE(outline->excerpts.size(), 2u);
    const auto excerpt_tracks = mixed->audio_tracks(engrave::ScoreSel{ outline->excerpts.front().id });
    ASSERT_TRUE(excerpt_tracks) << excerpt_tracks.error().message;
    EXPECT_LT(excerpt_tracks->tracks.size(), mix->tracks.size()) << "an excerpt lists every part of the master";
    ASSERT_FALSE(excerpt_tracks->tracks.empty());
    EXPECT_FALSE(excerpt_tracks->tracks.front().mix.mute)
        << "the Violin excerpt kept the master's mute instead of its own audiosettings";
}

// EngineOptions::audio_sample_rate is the engine's fixed rate: the probe in a
// fresh process renders at 48000 Hz and the stream says so, with the same
// duration as the 44100 Hz render here.
TEST(Audio, TheEngineSampleRateReachesTheEncoder) {
    const auto out = detail::temp_file("48k.wav");
    std::vector<std::string> arguments = { "--assets=" + music::test::g_assets.string() };
    for (const auto& soundfont : g_soundfonts) arguments.push_back("--soundfont=" + soundfont.string());
    arguments.push_back("--audio=" + music::test::corpus_file(detail::audio_score()).string());
    arguments.push_back("--format=wav");
    arguments.push_back("--sample-rate=48000");
    arguments.push_back("--out=" + out.string());
    ASSERT_EQ(music::test::run_probe(arguments), 0) << "the 48 kHz probe failed";
    const auto bytes = detail::read_file(out);
    std::filesystem::remove(out);
    std::string error;
    const auto wav = detail::decode_wav(bytes, error);
    ASSERT_TRUE(wav) << error;
    EXPECT_EQ(wav->sample_rate, 48000u);

    auto document = detail::open_score(detail::audio_score());
    ASSERT_TRUE(document);
    engrave::AudioOptions options;
    const detail::Rendered base = detail::render_audio(*document, options);
    ASSERT_GT(base.result.frames, 0u);
    EXPECT_NEAR(double(wav->samples.size() / 2) / 48000.0, double(base.result.frames) / 44100.0, 0.002)
        << "the 48 kHz render has a different duration";
}

// An excerpt renders as the master with the parts the excerpt does not have
// muted (masternotation.cpp:748-770): the same length, the excerpt's parts only.
TEST(Audio, AnExcerptRendersAsTheMasterWithItsOwnParts) {
    auto document = detail::open_score("mix_parts.mscz");
    if (!document) GTEST_SKIP() << "fixtures/audio/mix_parts.mscz is not in the corpus";
    const auto outline = document->outline();
    ASSERT_TRUE(outline);
    ASSERT_GE(outline->excerpts.size(), 2u);

    engrave::AudioOptions master;
    master.format = engrave::AudioFormat::Wav;
    engrave::AudioOptions excerpt = master;
    excerpt.score = engrave::ScoreSel{ outline->excerpts.front().id };

    const detail::Rendered full = detail::render_audio(*document, master);
    const detail::Rendered part = detail::render_audio(*document, excerpt);
    ASSERT_FALSE(full.bytes.empty());
    ASSERT_FALSE(part.bytes.empty());
    EXPECT_EQ(part.result.frames, full.result.frames) << "the excerpt has a different length";
    EXPECT_NE(part.bytes, full.bytes) << "the excerpt rendered as the full score";
}

TEST(Audio, AnEngineWithoutSoundFontsCannotRender) {
    const auto out = detail::temp_file("nosf.wav");
    std::vector<std::string> arguments = { "--assets=" + music::test::g_assets.string(),
                                           "--audio=" + music::test::corpus_file(detail::audio_score()).string(),
                                           "--format=wav", "--out=" + out.string() };
    EXPECT_EQ(music::test::run_probe(arguments), 2) << "a SoundFont-less Engine rendered audio";
    std::filesystem::remove(out);
}

} // namespace
