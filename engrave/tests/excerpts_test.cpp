// SPDX-License-Identifier: GPL-3.0-only
#include "runtime/noqt_runtime.h"
#include "score/score_excerpts.h"
#include "score/score_metadata.h"
#include "render/score_render.h"
#include "engraving/engravingproject.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/part.h"
#include "engraving/dom/text.h"
#include "global/io/buffer.h"
#include "importexport/musicxml/internal/export/exportmusicxml.h"
#include "importexport/midi/internal/midiexport/exportmidi.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void save(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), bytes.size());
    stream.close();
    expect(bool(stream), "Cannot write test output");
}
std::string export_selection(mu::engraving::Score* score, bool midi) {
    muse::ByteArray bytes;
    muse::io::Buffer buffer(&bytes);
    buffer.open(muse::io::IODevice::WriteOnly);
    if (midi) {
        mu::iex::midi::ExportMidi writer(score);
        expect(writer.write(&buffer, true, true), "Excerpt MIDI export failed");
    } else expect(mu::iex::musicxml::saveXml(score, &buffer), "Excerpt XML export failed");
    return {reinterpret_cast<const char*>(bytes.constData()), bytes.size()};
}
const char* fixture = R"(<score-partwise version="4.0"><part-list>
<score-part id="P1"><part-name>Violin</part-name><midi-instrument id="I1"><midi-channel>1</midi-channel><midi-program>41</midi-program></midi-instrument></score-part>
<score-part id="P2"><part-name>Cello</part-name><midi-instrument id="I2"><midi-channel>2</midi-channel><midi-program>43</midi-program></midi-instrument></score-part>
</part-list><part id="P1"><measure number="1"><attributes><divisions>1</divisions><time><beats>4</beats><beat-type>4</beat-type></time><clef><sign>G</sign><line>2</line></clef></attributes><note><pitch><step>C</step><octave>4</octave></pitch><duration>4</duration><type>whole</type></note></measure></part>
<part id="P2"><measure number="1"><attributes><divisions>1</divisions><time><beats>4</beats><beat-type>4</beat-type></time><clef><sign>F</sign><line>4</line></clef></attributes><note><pitch><step>E</step><octave>3</octave></pitch><duration>4</duration><type>whole</type></note></measure></part></score-partwise>)";
}
int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::invalid_argument("Usage: excerpts_test ASSETS OUTPUT_DIRECTORY");
        const auto out = std::filesystem::absolute(std::filesystem::u8path(argv[2]));
        std::filesystem::create_directories(out);
        music::initialize_native_runtime(argv[1]);
        auto project = music::load_native_score(fixture, "musicxml");
        auto* master = project->masterScore();
        expect(music::list_score_excerpts(master).empty(), "Unexpected initial excerpts");
        expect(music::generate_score_excerpts(master), "No excerpts generated");
        auto records = music::list_score_excerpts(master);
        expect(records.size() == 2, "Expected one excerpt per part");
        auto* first = music::select_score(master, 0);
        expect(!music::generate_score_excerpts(master), "Repeated generation changed score");
        expect(music::select_score(master, 0) == first && music::list_score_excerpts(master).size() == 2,
               "Repeated generation changed excerpt identity");
        for (int i = 0; i < 2; ++i) {
            auto* selected = music::select_score(master, i);
            expect(selected->parts().size() == 1 && selected->nstaves() == 1 && selected->npages() > 0,
                   "Excerpt staff/part/layout filter failed");
            expect(records[i].part_ids.at(0) == master->parts()[i]->id().toStdString(), "Excerpt part ID mismatch");
            save(out / ("part"+std::to_string(i)+".musicxml"), export_selection(selected, false));
            save(out / ("part"+std::to_string(i)+".mid"), export_selection(selected, true));
            save(out / ("part"+std::to_string(i)+".svg"), music::render_score(selected, "svg", 0));
        }
        for (int invalid : {-2, 2}) {
            bool rejected = false;
            try { music::select_score(master, invalid); } catch (const std::out_of_range&) { rejected = true; }
            expect(rejected, "Invalid excerpt ID accepted");
        }
        const auto archive = music::export_native_score(project, "mscz", 0, "");
        save(out / "with-excerpts.mscz", archive);
        auto reopened = music::load_native_score(archive, "mscz");
        expect(music::list_score_excerpts(reopened->masterScore()).size() == 2, "Saved excerpts lost on reopen");
        expect(!music::generate_score_excerpts(reopened->masterScore()), "Loaded excerpts were regenerated");
        for (int i = 0; i < 2; ++i) {
            auto* selected = music::select_score(reopened->masterScore(), i);
            expect(selected->parts().size() == 1 && selected->npages() > 0, "Reopened excerpt invalid");
            save(out / ("reopened-part"+std::to_string(i)+".musicxml"), export_selection(selected, false));
        }
        auto hidden = music::load_native_score(fixture, "musicxml");
        hidden->masterScore()->parts()[1]->setShow(false);
        expect(music::generate_score_excerpts(hidden->masterScore()), "Visible excerpt missing");
        expect(music::list_score_excerpts(hidden->masterScore()).size() == 1, "Hidden part generated excerpt");
        // No title Text exists in this fixture, so workTitle is the selected title source.
        master->setMetaTag(u"workTitle", muse::String::fromUtf8("\xE4\xB8\xAD\xE6\x96\x87 /\xC2\xA0\xE3\x80\x80" "A.B\xEF\xBB\xBF" "C"));
        expect(music::score_title_filename_safe(master) == "\xE4\xB8\xAD\xE6\x96\x87" "_A_B_C", "Unicode filename-safe title mismatch");
        music::MetadataTrack binding;
        binding.part_id = master->parts()[0]->id().toStdString();
        binding.instrument_id = master->parts()[0]->instrumentId().toStdString();
        binding.type = "fluid_soundfont";
        binding.name = "Test preset";
        binding.sound_id = "test-resource-id";
        binding.vendor = "must be omitted for Fluid";
        auto metadata = music::score_metadata(master, {binding});
        expect(metadata["tracks"].size() == 1 && !metadata["tracks"][0].contains("vendor"), "Track metadata incorrect");
        bool duplicate_rejected = false;
        try { music::score_metadata(master, {binding,binding}); } catch (const std::invalid_argument&) { duplicate_rejected = true; }
        expect(duplicate_rejected, "Duplicate metadata track accepted");
        save(out / "metadata.json", metadata.dump(2));
        auto track_project = music::load_native_score(fixture, "musicxml");
        auto* tracks_score = track_project->masterScore();
        tracks_score->parts()[0]->setId(muse::ID(uint64_t(10)));
        tracks_score->parts()[1]->setId(muse::ID(uint64_t(2)));
        music::MetadataTrack upper = binding, lower = binding;
        upper.part_id = "10";
        upper.instrument_id = tracks_score->parts()[0]->instrumentId().toStdString();
        lower.part_id = "2";
        lower.instrument_id = tracks_score->parts()[1]->instrumentId().toStdString();
        lower.type = "vst_plugin";
        lower.vendor = "Actual test vendor";
        lower.sound_id = lower.name;
        lower.category = lower.name;
        const auto sorted = music::score_metadata(tracks_score, {upper, lower})["tracks"];
        expect(sorted[0]["partId"] == "2" && sorted[1]["partId"] == "10", "Metadata track order is not numeric");
        expect(sorted[0]["vendor"] == "Actual test vendor" && !sorted[0].contains("soundId")
               && !sorted[0].contains("category"), "Metadata optional resource fields mismatch");
        for (const auto& bad_part : {"99999", "18446744073709551616", "not-an-id"}) {
            auto bad = lower;
            bad.part_id = bad_part;
            bool rejected = false;
            try { music::score_metadata(tracks_score, {bad}); } catch (const std::invalid_argument&) { rejected = true; }
            expect(rejected, "Invalid metadata part accepted");
        }
        lower.instrument_id = "not-an-instrument";
        bool instrument_rejected = false;
        try { music::score_metadata(tracks_score, {lower}); } catch (const std::invalid_argument&) { instrument_rejected = true; }
        expect(instrument_rejected, "Invalid metadata instrument accepted");
        tracks_score->setMetaTag(u"workTitle", muse::String::fromUtf8("A\xC2\x85\xE1\xA0\x8E" "B\xE2\x80\xA8" "C"));
        expect(music::score_title_filename_safe(tracks_score) == "A\xC2\x85\xE1\xA0\x8E" "B_C", "Non-JS whitespace was replaced");
        std::cout << "Excerpt generation/select/idempotence/visibility/save-reopen and Unicode title/track metadata passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
