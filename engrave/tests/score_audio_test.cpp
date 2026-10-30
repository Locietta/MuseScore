#include "runtime/noqt_runtime.h"
#include "audio/score_audio.h"

#include "engraving/engravingproject.h"
#include "engraving/dom/masterscore.h"
#include "global/io/buffer.h"
#include "global/serialization/zipwriter.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
void expect(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

constexpr const char* fixture = R"(<score-partwise version="4.0">
<part-list>
<score-part id="P1"><part-name>Grand Piano</part-name><score-instrument id="P1-I1"><instrument-name>Piano</instrument-name></score-instrument><midi-instrument id="P1-I1"><midi-channel>1</midi-channel><midi-program>1</midi-program></midi-instrument></score-part>
<score-part id="P2"><part-name>Violin</part-name><score-instrument id="P2-I1"><instrument-name>Violin</instrument-name></score-instrument><midi-instrument id="P2-I1"><midi-channel>2</midi-channel><midi-program>41</midi-program></midi-instrument></score-part>
</part-list>
<part id="P1"><measure number="1"><attributes><divisions>1</divisions><key><fifths>0</fifths></key><time><beats>4</beats><beat-type>4</beat-type></time><staves>2</staves><clef number="1"><sign>G</sign><line>2</line></clef><clef number="2"><sign>F</sign><line>4</line></clef></attributes>
<note><pitch><step>C</step><octave>4</octave></pitch><duration>4</duration><voice>1</voice><type>whole</type><staff>1</staff></note><backup><duration>4</duration></backup>
<note><pitch><step>C</step><octave>3</octave></pitch><duration>4</duration><voice>2</voice><type>whole</type><staff>2</staff></note></measure></part>
<part id="P2"><measure number="1"><attributes><divisions>1</divisions><key><fifths>2</fifths></key><time><beats>4</beats><beat-type>4</beat-type></time><clef><sign>G</sign><line>2</line></clef></attributes>
<note><pitch><step>G</step><octave>4</octave></pitch><duration>4</duration><type>whole</type></note></measure></part>
</score-partwise>)";
}

int main(int argc, char** argv)
{
    try {
        if (argc != 3) throw std::invalid_argument("Usage: score_audio_test ASSETS UPSTREAM_ROOT");
        music::initialize_native_runtime(argv[1]);

        // Default mix: a document opened from MSCZ carries the archive's
        // audiosettings.json values, matched by (partId, instrumentId).
        expect(music::score_audio_mix_defaults("").is_neutral(), "Empty input must give neutral defaults");
        expect(music::score_audio_mix_defaults("not a zip archive").is_neutral(),
               "Invalid archive must give neutral defaults");
        std::ifstream mix_stream("fixtures/audio/mix.mscz", std::ios::binary);
        expect(bool(mix_stream), "Missing fixtures/audio/mix.mscz");
        const std::string mix_bytes{std::istreambuf_iterator<char>(mix_stream),
                                    std::istreambuf_iterator<char>()};
        const auto defaults = music::score_audio_mix_defaults(mix_bytes);
        expect(!defaults.is_neutral(), "MSCZ audiosettings.json was not read");
        expect(defaults.master.volume_db == 0.0 && defaults.master.balance == 0.0,
               "Unexpected fixture master defaults");
        auto mix_project = music::load_native_score(mix_bytes, "mscz");
        const auto mix_tracks = music::score_audio_track_info(mix_project->masterScore());
        expect(mix_tracks.size() == 3, "Mix fixture must expose three tracks");
        std::size_t muted_tracks = 0;
        for (const auto& track : mix_tracks) {
            const auto found = defaults.tracks.find({ track.part_id, track.instrument_id });
            expect(found != defaults.tracks.end(), "audiosettings entry did not match a score track");
            if (found->second.muted) ++muted_tracks;
        }
        expect(muted_tracks == 1, "MSCZ soloMuteState.mute was not applied");

        music::AudioMix forced;
        forced.tracks.push_back({0, music::AudioOutputParams{}});
        forced.tracks.push_back({1, music::AudioOutputParams{}});
        forced.tracks[0].output.solo = true;
        music::derive_force_mute(forced);
        expect(!forced.tracks[0].output.force_mute && forced.tracks[1].output.force_mute,
               "forceMute must silence non-solo tracks");
        expect(!music::is_neutral_mix(forced), "Soloed mix must not be neutral");

        // Hostile deeply nested JSON (>64 nesting) and oversized entry (>1 MiB)
        {
            // 1) Hostile nested JSON: [[[[... 100 deep
            std::string nested = "";
            for (int i = 0; i < 100; ++i) nested += "[";
            for (int i = 0; i < 100; ++i) nested += "]";
            muse::ByteArray zip_bytes;
            {
                muse::io::Buffer buf(&zip_bytes);
                expect(buf.open(muse::io::IODevice::WriteOnly), "Cannot open zip buffer");
                muse::ZipWriter writer(&buf);
                writer.addFile("audiosettings.json", muse::ByteArray(nested.data(), nested.size()));
                writer.close();
            }
            std::string nested_archive(reinterpret_cast<const char*>(zip_bytes.constData()), zip_bytes.size());
            const auto nested_defaults = music::score_audio_mix_defaults(nested_archive);
            expect(nested_defaults.is_neutral(), "Deeply nested hostile document must yield neutral defaults");

            // 2) Oversized entry: > 1 MiB uncompressed size
            {
                std::string huge(1024 * 1024 + 16, ' ');
                muse::ByteArray huge_zip;
                muse::io::Buffer huge_buf(&huge_zip);
                expect(huge_buf.open(muse::io::IODevice::WriteOnly), "Cannot open huge zip buffer");
                muse::ZipWriter huge_writer(&huge_buf);
                huge_writer.addFile("audiosettings.json", muse::ByteArray(huge.data(), huge.size()));
                huge_writer.close();
                std::string huge_archive(reinterpret_cast<const char*>(huge_zip.constData()), huge_zip.size());
                const auto huge_defaults = music::score_audio_mix_defaults(huge_archive);
                expect(huge_defaults.is_neutral(), "Oversized audiosettings entry must yield neutral defaults");
            }
        }

        // Excerpts audiosettings.json fixture (mix_parts.mscz)
        {
            std::ifstream parts_stream("fixtures/audio/mix_parts.mscz", std::ios::binary);
            expect(bool(parts_stream), "Missing fixtures/audio/mix_parts.mscz");
            const std::string parts_bytes{std::istreambuf_iterator<char>(parts_stream),
                                          std::istreambuf_iterator<char>()};
            const auto parts_defaults = music::score_audio_mix_defaults(parts_bytes);
            // Root defaults: Violin is muted
            auto violin_it = parts_defaults.tracks.find({"1", "violin"});
            expect(violin_it != parts_defaults.tracks.end() && violin_it->second.muted,
                   "mix_parts root must report Violin muted");

            // Excerpt 0 (Violin): Violin is NOT muted in excerpt (ignores root mute)
            const auto* ex0_map = parts_defaults.get_excerpt_map(0, "0_Violin");
            expect(ex0_map != nullptr, "mix_parts must find excerpt 0 audiosettings");
            auto ex0_violin = ex0_map->find({"1", "violin"});
            expect(ex0_violin != ex0_map->end() && !ex0_violin->second.muted,
                   "Excerpt 0 must have Violin unmuted");

            // Excerpt 2 (Cello): Cello IS muted in excerpt
            const auto* ex2_map = parts_defaults.get_excerpt_map(2, "2_Cello");
            expect(ex2_map != nullptr, "mix_parts must find excerpt 2 audiosettings");
            auto ex2_cello = ex2_map->find({"3", "baryton"});
            expect(ex2_cello != ex2_map->end() && ex2_cello->second.muted,
                   "Excerpt 2 must have Cello muted");
        }

        // Every InstrumentTrackId is listed once, in deterministic score order.
        auto project = music::load_native_score(fixture, "musicxml");
        auto* score = project->masterScore();
        const auto info = music::score_audio_track_info(score);
        expect(info.size() == 2, "Two parts must expose one track each");
        expect(info[0].part_id != info[1].part_id, "Distinct parts share descriptor identity");
        expect(!info[0].instrument_id.empty() && !info[1].instrument_id.empty(), "Missing instrument identity");
        expect(!info[0].part_name.empty() && !info[0].instrument_name.empty(), "Missing track names");
        expect(info[0].part_index == 0 && info[1].part_index == 1, "Part indexes are not score order");

        // Instrument changes are no longer rejected: the part exposes one entry
        // per instrument and all entries share the part identity.
        const std::string path = std::string(argv[2]) + "/src/engraving/tests/instrumentchange_data/change-ref.mscx";
        std::ifstream stream(path, std::ios::binary);
        expect(bool(stream), "Missing pinned instrument-change fixture");
        const std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        auto changed = music::load_native_score(bytes, "mscx");
        const auto changed_tracks = music::score_audio_track_info(changed->masterScore());
        expect(changed_tracks.size() == 3,
               "Flute part with an Oboe change plus the Oboe part must expose three instrument tracks");
        expect(changed_tracks[0].part_id == changed_tracks[1].part_id,
               "Instrument tracks lost their part identity");
        expect(changed_tracks[0].instrument_id != changed_tracks[1].instrument_id,
               "Instrument change did not produce distinct instrument identities");

        std::cout << "{\"status\":\"passed\",\"logicalTracks\":2,\"instrumentChangeTracks\":3,"
                     "\"msczMixDefaults\":{\"tracks\":3,\"mutedTracks\":"
                  << muted_tracks << "},\"forceMuteDerived\":true}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
