#include "noqt_filesystem.h"
#include "noqt_application.h"
#include "global/log.h"
#include "global/io/buffer.h"
#include "draw/internal/fontprovider.h"
#include "draw/internal/fontsengine.h"
#include "draw/internal/fontsdatabase.h"
#include "engraving/engravingmodule.h"
#include "engraving/engravingproject.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/instrtemplate.h"
#include "mpe/mpemodule.h"
#include "importexport/musicxml/internal/import/importmusicxml.h"
#include "importexport/musicxml/internal/export/exportmusicxml.h"
#include "importexport/musicxml/internal/musicxmlconfiguration.h"
#include "importexport/midi/internal/midiexport/exportmidi.h"
#include <iostream>

// The existing FreeType provider uses 360 DPI; engraving coordinates use 1200.
class EngravingFontProvider final : public muse::draw::FontProvider {
    static muse::draw::Font scaled(muse::draw::Font font) {
        font.setPointSizeF(font.pointSizeF() * (1200.0 / 360.0));
        return font;
    }
public:
    EngravingFontProvider() : FontProvider(nullptr) {}
    double lineSpacing(const muse::draw::Font& f) const override { return FontProvider::lineSpacing(scaled(f)); }
    double xHeight(const muse::draw::Font& f) const override { return FontProvider::xHeight(scaled(f)); }
    double height(const muse::draw::Font& f) const override { return FontProvider::height(scaled(f)); }
    double capHeight(const muse::draw::Font& f) const override { return FontProvider::capHeight(scaled(f)); }
    double ascent(const muse::draw::Font& f) const override { return FontProvider::ascent(scaled(f)); }
    double descent(const muse::draw::Font& f) const override { return FontProvider::descent(scaled(f)); }
    double horizontalAdvance(const muse::draw::Font& f, const muse::String& s) const override { return FontProvider::horizontalAdvance(scaled(f), s); }
    double horizontalAdvance(const muse::draw::Font& f, char32_t c) const override { return FontProvider::horizontalAdvance(scaled(f), c); }
    muse::RectF boundingRect(const muse::draw::Font& f, const muse::String& s) const override { return FontProvider::boundingRect(scaled(f), s); }
    muse::RectF boundingRect(const muse::draw::Font& f, char32_t c) const override { return FontProvider::boundingRect(scaled(f), c); }
    muse::RectF tightBoundingRect(const muse::draw::Font& f, const muse::String& s) const override { return FontProvider::tightBoundingRect(scaled(f), s); }
};

int main(int argc, char** argv) {
    if (argc != 4) { std::cerr << "Usage: noqt_probe ASSETS INPUT.musicxml OUTPUT.musicxml\n"; return 2; }
    try {
        using namespace muse;
        using namespace mu;
        auto fs = std::make_shared<music::NativeFileSystem>(std::filesystem::u8path(argv[1]));
        modularity::globalIoc()->registerExport<io::IFileSystem>("native", fs);
        modularity::globalIoc()->registerExport<draw::IFontsDatabase>("native", new draw::FontsDatabase());
        auto fonts = std::make_shared<draw::FontsEngine>(modularity::globalCtx());
        modularity::globalIoc()->registerExport<draw::IFontsEngine>("native", fonts);
        modularity::globalIoc()->registerExport<draw::IFontProvider>("native", new EngravingFontProvider());
        modularity::globalIoc()->registerExport<IApplication>("native", std::make_shared<music::NoQtApplication>());
        fonts->init();
        engraving::EngravingModule engraving;
        engraving.registerExports();
        engraving.onInit(IApplication::RunMode::ConsoleApp);
        mpe::MpeModule mpe;
        mpe.registerExports();
        auto xml = std::make_shared<iex::musicxml::MusicXmlConfiguration>();
        modularity::globalIoc()->registerExport<iex::musicxml::IMusicXmlConfiguration>("native", xml);
        xml->init();
        engraving::loadInstrumentTemplates(":/engraving/instruments/instruments.xml");
        auto project = engraving::EngravingProject::create(modularity::globalCtx());
        auto input = fs->readFile(argv[2]);
        if (!input.ret) throw std::runtime_error(input.ret.toString());
        auto score = project->masterScore();
        if (iex::musicxml::importMusicXmlfromBuffer(score, String::fromUtf8(argv[2]), input.val) != engraving::Err::NoError)
            throw std::runtime_error("MusicXML import failed");
        score->connectTies();
        if (!project->setupMasterScore(true)) throw std::runtime_error("Score setup failed");
        score->setLayoutAll();
        score->update();
        ByteArray output;
        io::Buffer buffer(&output);
        buffer.open(io::IODevice::WriteOnly);
        if (!iex::musicxml::saveXml(score, &buffer)) throw std::runtime_error("MusicXML export failed");
        buffer.close();
        if (!fs->writeFile(argv[3], output)) throw std::runtime_error("Cannot write output");
        ByteArray midi;
        io::Buffer midiBuffer(&midi);
        midiBuffer.open(io::IODevice::WriteOnly);
        iex::midi::ExportMidi midiWriter(score);
        if (!midiWriter.write(&midiBuffer, true, true)) throw std::runtime_error("MIDI export failed");
        midiBuffer.close();
        if (!fs->writeFile(std::string(argv[3]) + ".mid", midi)) throw std::runtime_error("Cannot write MIDI");
        std::cout << "Qt-free import/layout/export: parts=" << score->parts().size() << " pages=" << score->npages() << " bytes=" << output.size() << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
