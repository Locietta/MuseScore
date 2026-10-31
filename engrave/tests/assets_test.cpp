// SPDX-License-Identifier: GPL-3.0-only
#include "engrave/src/runtime/engine_context.h"
#include "engrave/src/runtime/runtime_assets.h"
#include "draw/internal/ifontsengine.h"
#include "draw/types/font.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>

namespace {
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string read(const std::filesystem::path& p) {
    std::ifstream f(p,std::ios::binary);
    expect(bool(f), "Cannot read font test input");
    return {std::istreambuf_iterator<char>(f), {}};
}
std::string utf8(const std::filesystem::path& p) {
    auto v=p.u8string(); return {v.begin(),v.end()};
}
struct FontFixture {
    std::string bytes;
    std::string family;
    bool bold=false;
    bool italic=false;
};
FontFixture inspectFont(FT_Library lib,const std::filesystem::path& p) {
    FontFixture result;
    result.bytes=read(p);
    FT_Face face=nullptr;
    expect(FT_New_Memory_Face(lib,reinterpret_cast<const FT_Byte*>(result.bytes.data()),
                              static_cast<FT_Long>(result.bytes.size()),0,&face)==0,
           "Invalid font fixture");
    struct FaceGuard { FT_Face p; ~FaceGuard(){ if(p) FT_Done_Face(p); } } guard{face};
    expect(face->num_faces==1 && face->family_name,"Use a single-face font fixture");
    result.family=face->family_name;
    result.bold=(face->style_flags&FT_STYLE_FLAG_BOLD)!=0;
    result.italic=(face->style_flags&FT_STYLE_FLAG_ITALIC)!=0;
    return result;
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 4 && argc != 5) throw std::invalid_argument("Usage: assets_test ASSETS UNREGISTERED_FONT OUTPUT_DIRECTORY [SOUNDFONT]");
        const auto out=std::filesystem::absolute(std::filesystem::u8path(argv[3]));
        std::filesystem::create_directories(out);
        FT_Library lib=nullptr;
        expect(FT_Init_FreeType(&lib)==0,"Cannot initialize FreeType for test");
        struct LibGuard { FT_Library p; ~LibGuard(){ FT_Done_FreeType(p); } } lg{lib};
        const auto fixture=inspectFont(lib,std::filesystem::u8path(argv[2]));
        const auto& bytes=fixture.bytes;
        const std::string family=fixture.family;
        const auto input=out/"mutable-input.ttf";
        { std::ofstream f(input,std::ios::binary); f.write(bytes.data(),bytes.size()); expect(bool(f),"Cannot write test font"); }
        music::EngineContext context(argv[1]);
        music::RuntimeAssets& assets=context.assets();
        muse::draw::IFontsEngine* engine=&assets.fontsEngine();
        muse::draw::Font font(muse::draw::Font::FontFamily(family),muse::draw::Font::Type::Text);
        font.setPointSizeF(12);
        const std::u32string sample=U"MMMWWWiii012345";
        const auto before=engine->horizontalAdvance(font,sample); // Cache its missing-family fallback.
        const auto initial_count=assets.fonts().size();
        const auto info=assets.addFont(utf8(input));
        expect(info.family==family && info.path!=utf8(input),"Font was not copied with correct family");
        expect(assets.fonts().size()==initial_count+1,"Font registry count mismatch");
        const auto after=engine->horizontalAdvance(font,sample);
        expect(before>0 && after>0 && std::abs(after-before)>0.01,"Cached fallback was not invalidated; choose an unregistered font");
        // A family that only ships a Medium/Bold face (FreeType flags it bold)
        // must still satisfy the regular lookup with that registered face.
        const bool boldOnlyFamily=info.bold && !info.italic;
        double boldAdvance=0;
        if (boldOnlyFamily) {
            muse::draw::Font boldFont(font);
            boldFont.setBold(true);
            boldAdvance=engine->horizontalAdvance(boldFont,sample);
            expect(std::abs(after-boldAdvance)<0.0001,"Regular lookup of a bold-only family did not use the registered face");
        }
        const auto again=assets.addFont(utf8(input));
        expect(again.id==info.id && again.path==info.path,"Identical font was not deduplicated");
        expect(assets.fonts().size()==initial_count+1,"Deduplication added a font");
        muse::draw::Font folded(muse::draw::Font::FontFamily(muse::String::fromUtf8(family).toLower()),muse::draw::Font::Type::Text);
        folded.setPointSizeF(12);
        expect(std::abs(engine->horizontalAdvance(folded,sample)-after)<0.0001,"Family lookup lost case-insensitivity");
        { std::ofstream f(input,std::ios::binary|std::ios::trunc); f<<"invalid-font"; }
        bool bad_rejected=false;
        try { assets.addFont(utf8(input)); } catch (const std::exception&) { bad_rejected=true; }
        expect(bad_rejected && assets.fonts().size()==initial_count+1,"Bad font changed registry");
        expect(read(std::filesystem::u8path(info.path))==bytes,"Cached font changed with source file");
        expect(std::abs(engine->horizontalAdvance(font,sample)-after)<0.0001,"Bad input changed font engine state");
        bool thread_rejected=false,engine_thread_rejected=false;
        std::thread other([&]{
            try { assets.fonts(); } catch (const std::exception&) { thread_rejected=true; }
            try { engine->height(font); } catch (const std::exception&) { engine_thread_rejected=true; }
        });
        other.join();
        expect(thread_rejected && engine_thread_rejected,"Font service accepted wrong thread");
        const auto active=assets.fonts();
        expect(active.back().content_hash==info.content_hash,"Font fingerprint changed");
        nlohmann::json result={{"status","passed"},{"family",family},{"id",info.id},
            {"contentHash",info.content_hash},{"cachedPath",info.path},{"before",before},{"after",after},
            {"bold",info.bold},{"boldOnlyFamily",boldOnlyFamily},{"boldAdvance",boldAdvance},
            {"sourceBytes",bytes.size()},{"immutableCopy",true},{"caseInsensitive",true},{"wrongThreadRejected",true}};
        if (argc == 5) {
            const auto sound_bytes=read(std::filesystem::u8path(argv[4]));
            const auto sound_input=out/"mutable-input.sf2";
            { std::ofstream f(sound_input,std::ios::binary); f.write(sound_bytes.data(),sound_bytes.size());
              expect(bool(f),"Cannot write SoundFont test fixture"); }
            const auto sound=assets.freezeSoundFont(utf8(sound_input));
            const auto repeated=assets.freezeSoundFont(utf8(sound_input));
            expect(sound.path!=utf8(sound_input) && repeated.path==sound.path,"SoundFont immutable copy/dedup failed");
            expect(read(std::filesystem::u8path(sound.path))==sound_bytes,"SoundFont copy changed bytes");
            const auto cache_dir=std::filesystem::u8path(sound.path).parent_path();
            const auto file_count=std::distance(std::filesystem::directory_iterator(cache_dir),std::filesystem::directory_iterator{});
            { std::ofstream f(sound_input,std::ios::binary|std::ios::trunc); f<<"invalid-soundfont"; }
            bool rejected=false;
            try { assets.freezeSoundFont(utf8(sound_input)); } catch(const std::exception&) { rejected=true; }
            expect(rejected,"Invalid SoundFont was published");
            expect(std::distance(std::filesystem::directory_iterator(cache_dir),std::filesystem::directory_iterator{})==file_count,
                   "Rejected SoundFont leaked a cache file");
            expect(read(std::filesystem::u8path(sound.path))==sound_bytes,"Source mutation altered retained SoundFont");
            bool wrong_thread=false;
            std::thread wrong([&]{ try { assets.freezeSoundFont(sound.path); }
                                  catch(const std::exception&) { wrong_thread=true; } });
            wrong.join();
            expect(wrong_thread,"SoundFont accepted wrong owner thread");
            result["soundfont"]={{"cachedPath",sound.path},{"contentHash",sound.content_hash},
                {"sourceBytes",sound_bytes.size()},{"invalidRejected",true},{"immutableCopy",true}};
        }
        auto lease=assets.retain();
        assets.shutdown();
        expect(std::filesystem::exists(std::filesystem::u8path(info.path)),"Outstanding lease lost its font file");
        if (result.contains("soundfont"))
            expect(std::filesystem::exists(std::filesystem::u8path(result["soundfont"]["cachedPath"].get<std::string>())),
                   "Outstanding lease lost its SoundFont file");
        lease.reset();
        expect(!std::filesystem::exists(std::filesystem::u8path(info.path)),"Font cache survived runtime shutdown");
        if (result.contains("soundfont"))
            expect(!std::filesystem::exists(std::filesystem::u8path(result["soundfont"]["cachedPath"].get<std::string>())),
                   "SoundFont cache survived runtime shutdown");
        result["shutdownCleanup"] = true;
        result["leaseDeferredCleanup"] = true;
        std::ofstream report(out/"evidence.json"); report<<result.dump(2); report.close();
        expect(bool(report),"Cannot write evidence");
        std::cout<<result.dump()<<'\n';
    } catch(const std::exception& e){ std::cerr<<e.what()<<'\n'; return 1; }
}
