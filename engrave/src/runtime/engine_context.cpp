#include "engine_context.h"
#include "engrave/src/audio/engine_audio.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>

#include "noqt_filesystem.h"
#include "noqt_runtime.h"
#include "runtime_assets.h"

#include "draw/fontmetrics.h"
#include "draw/internal/ifontsdatabase.h"
#include "mpe/internal/articulationprofilesrepository.h"

#include "engraving/dom/drumset.h"
#include "engraving/dom/figuredbass.h"
#include "engraving/dom/instrtemplate.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/stafftype.h"
#include "engraving/engravingservices.h"
#include "engraving/infrastructure/smufl.h"
#include "engraving/internal/engravingconfiguration.h"
#include "engraving/internal/engravingcontextconfiguration.h"
#include "engraving/internal/engravingfontsprovider.h"
#include "engraving/rendering/score/scorerenderer.h"
#include "engraving/style/defaultstyle.h"
#include "importexport/musicxml/internal/musicxmlconfiguration.h"

namespace music {
using namespace muse;
using namespace muse::draw;
using namespace mu::engraving;

namespace {
// The built-in text, music and tablature faces and their defaults (upstream's
// EngravingModule::onInit).
void register_builtin_fonts(IFontsDatabase& fdb, EngravingFontsProvider& engravingFonts)
{
    // Text
    fdb.addFont(FontDataKey(u"Edwin", false, false), ":/fonts/edwin/Edwin-Roman.otf");
    fdb.addFont(FontDataKey(u"Edwin", false, true), ":/fonts/edwin/Edwin-Italic.otf");
    fdb.addFont(FontDataKey(u"Edwin", true, false), ":/fonts/edwin/Edwin-Bold.otf");
    fdb.addFont(FontDataKey(u"Edwin", true, true), ":/fonts/edwin/Edwin-BdIta.otf");

    // MusicSymbol[Text]
    auto addMusicFont = [&](const std::string& name, const FontDataKey& fontDataKey, const io::path_t& filePath) {
        fdb.addFont(FontDataKey(fontDataKey), filePath);
        engravingFonts.addInternalFont(name, fontDataKey.family().id().toStdString(), filePath);
    };

    addMusicFont("Bravura", FontDataKey(u"Bravura"), ":/fonts/bravura/Bravura.otf");
    fdb.addFont(FontDataKey(u"Bravura Text"), ":/fonts/bravura/BravuraText.otf");
    addMusicFont("Leland", FontDataKey(u"Leland"), ":/fonts/leland/Leland.otf");
    fdb.addFont(FontDataKey(u"Leland Text"), ":/fonts/leland/LelandText.otf");
    addMusicFont("Emmentaler", FontDataKey(u"MScore"), ":/fonts/mscore/MScore.otf");
    fdb.addFont(FontDataKey(u"MScore Text"), ":/fonts/mscore/MScoreText.otf");
    addMusicFont("Gonville", FontDataKey(u"Gootville"), ":/fonts/gootville/Gootville.otf");
    fdb.addFont(FontDataKey(u"Gootville Text"), ":/fonts/gootville/GootvilleText.otf");
    addMusicFont("MuseJazz", FontDataKey(u"MuseJazz"), ":/fonts/musejazz/MuseJazz.otf");
    fdb.addFont(FontDataKey(u"MuseJazz Text"), ":/fonts/musejazz/MuseJazzText.otf");
    addMusicFont("Petaluma", FontDataKey(u"Petaluma"),    ":/fonts/petaluma/Petaluma.otf");
    fdb.addFont(FontDataKey(u"Petaluma Text"), ":/fonts/petaluma/PetalumaText.otf");
    addMusicFont("Finale Maestro", FontDataKey(u"Finale Maestro"), ":/fonts/finalemaestro/FinaleMaestro.otf");
    fdb.addFont(FontDataKey(u"Finale Maestro Text"), ":/fonts/finalemaestro/FinaleMaestroText.otf");
    addMusicFont("Finale Broadway", FontDataKey(u"Finale Broadway"), ":/fonts/finalebroadway/FinaleBroadway.otf");
    fdb.addFont(FontDataKey(u"Finale Broadway Text"), ":/fonts/finalebroadway/FinaleBroadwayText.otf");

    // Tabulature
    fdb.addFont(FontDataKey(u"FreeSerif"), ":/fonts/FreeSerif.ttf");
    fdb.addFont(FontDataKey(u"FreeSerif", true, false), ":/fonts/FreeSerifBold.ttf");
    fdb.addFont(FontDataKey(u"FreeSerif", false, true), ":/fonts/FreeSerifItalic.ttf");
    fdb.addFont(FontDataKey(u"FreeSerif", true, true), ":/fonts/FreeSerifBoldItalic.ttf");
    fdb.addFont(FontDataKey(u"FreeSans"), ":/fonts/FreeSans.ttf");
    fdb.addFont(FontDataKey(u"MuseScoreTabRenaiss"), ":/fonts/MuseScoreTabRenaiss.ttf");
    fdb.addFont(FontDataKey(u"MuseScoreTabPhalese"), ":/fonts/MuseScoreTabPhalese.ttf");
    fdb.addFont(FontDataKey(u"MuseScoreTabBonneuilDeVisee"), ":/fonts/MuseScoreTabBonneuilDeVisee.ttf");
    fdb.addFont(FontDataKey(u"MuseScoreTabBonneuilGaultier"), ":/fonts/MuseScoreTabBonneuilGaultier.ttf");
    fdb.addFont(FontDataKey(u"MuseScoreTabDowland"), ":/fonts/MuseScoreTabDowland.ttf");
    fdb.addFont(FontDataKey(u"MuseScoreTabLuteDidactic"), ":/fonts/MuseScoreTabLuteDidactic.ttf");
    fdb.addFont(FontDataKey(u"MuseScoreTabModern"), ":/fonts/MuseScoreTabModern.ttf");
    fdb.addFont(FontDataKey(u"MuseScoreTabItalian"), ":/fonts/MuseScoreTabItalian.ttf");
    fdb.addFont(FontDataKey(u"MuseScoreTabFrench"), ":/fonts/MuseScoreTabFrench.ttf");
    fdb.addFont(FontDataKey(u"MuseScoreTabFrenchBaroqueHeadless"), ":/fonts/MuseScoreTabFrenchBaroqueHeadless.ttf");
    fdb.addFont(FontDataKey(u"MuseScoreTabFrenchBaroque"), ":/fonts/MuseScoreTabFrenchBaroque.ttf");

    // Figured Bass
    fdb.addFont(FontDataKey(u"MscoreBC"), ":/fonts/mscore-BC.ttf");

    // Roman Numeral Analysis
    fdb.addFont(FontDataKey(u"Campania"), ":/fonts/campania/Campania.otf");

    // Defaults
    fdb.setDefaultFont(Font::Type::Unknown, FontDataKey(u"Edwin"));
    fdb.setDefaultFont(Font::Type::Text, FontDataKey(u"Edwin"));
    fdb.setDefaultFont(Font::Type::Tablature, FontDataKey(u"FreeSerif"));
    fdb.setDefaultFont(Font::Type::MusicSymbolText, FontDataKey(u"Bravura Text"));
    fdb.setDefaultFont(Font::Type::MusicSymbol, FontDataKey(u"Bravura"));
    engravingFonts.setFallbackFont("Bravura");

    fdb.insertSubstitution(u"Leland Text",    u"Bravura Text");
    fdb.insertSubstitution(u"Bravura Text",   u"Leland Text");
    fdb.insertSubstitution(u"MScore Text",    u"Leland Text");
    fdb.insertSubstitution(u"Gootville Text", u"Leland Text");
    fdb.insertSubstitution(u"MuseJazz Text",  u"Leland Text");
    fdb.insertSubstitution(u"Petaluma Text",  u"MuseJazz Text");
    fdb.insertSubstitution(u"Finale Maestro Text", u"Leland Text");
    fdb.insertSubstitution(u"Finale Broadway Text", u"MuseJazz Text");
    fdb.insertSubstitution(u"ScoreFont",      u"Leland Text");// alias for current Musical Text Font

    // Symbols
    Smufl::init();

    // A painter may name one of these families without requesting the font
    // first; unloaded faces would be missing from the database and drawn wrong.
    engravingFonts.loadAllFonts();
}
}

struct EngineContext::Impl {
    std::unique_ptr<NativeFileSystem> fileSystem;
    std::unique_ptr<RuntimeAssets> assets;
    std::shared_ptr<EngravingFontsProvider> engravingFonts;
    std::shared_ptr<EngravingConfiguration> configuration;
    std::shared_ptr<rendering::score::ScoreRenderer> renderer;
    std::unique_ptr<mpe::ArticulationProfilesRepository> articulationProfiles;
    std::unique_ptr<mu::iex::musicxml::MusicXmlConfiguration> musicXml;
    std::shared_ptr<EngravingContextConfiguration> contextConfiguration;
    EngravingServices engraving;

    std::unique_ptr<EngineAudio> audio;
    void teardown() noexcept;
};

// Reverse of the constructor; tolerates a partially built context.
void EngineContext::Impl::teardown() noexcept
{
    try {
        audio.reset();
        engraving = EngravingServices();
        contextConfiguration.reset();
        musicXml.reset();
        articulationProfiles.reset();
        renderer.reset();
        configuration.reset();
        if (engravingFonts) engravingFonts->deinit();
        engravingFonts.reset();
        if (assets) {
            FontMetrics::setProvider(nullptr);
            assets->shutdown();
            assets.reset();
        }
        if (fileSystem) {
            io::setFileSystem(nullptr);
            fileSystem.reset();
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "engrave: engine teardown failed: %s\n", error.what());
    }
}

EngineContext::EngineContext(const std::string& assets, const std::string& cache_root)
    : m_impl(std::make_unique<Impl>())
{
    // Validates the resources and claims the process; diagnostics go to stderr.
    initialize_native_runtime(assets);
    Impl& d = *m_impl;
    try {
        d.fileSystem = std::make_unique<NativeFileSystem>(std::filesystem::u8path(assets));
        io::setFileSystem(d.fileSystem.get());

        d.assets = std::make_unique<RuntimeAssets>(std::filesystem::u8path(cache_root));
        FontMetrics::setProvider(&d.assets->fontProvider());

        d.engravingFonts = std::make_shared<EngravingFontsProvider>(d.assets->fontProvider());
        d.configuration = std::make_shared<EngravingConfiguration>();
        d.renderer = std::make_shared<rendering::score::ScoreRenderer>();
        d.articulationProfiles = std::make_unique<mpe::ArticulationProfilesRepository>(*d.fileSystem);
        d.audio = std::make_unique<EngineAudio>(*d.articulationProfiles);
        d.musicXml = std::make_unique<mu::iex::musicxml::MusicXmlConfiguration>();
        d.contextConfiguration = std::make_shared<EngravingContextConfiguration>();
        d.engraving.configuration = d.configuration;
        d.engraving.engravingFonts = d.engravingFonts;
        d.engraving.contextConfiguration = d.contextConfiguration;
        d.engraving.renderer = d.renderer;

        register_builtin_fonts(d.assets->fontsDatabase(), *d.engravingFonts);

        d.configuration->init();
        DefaultStyle::instance()->init(d.configuration->defaultStyleFilePath(),
                                       d.configuration->partStyleFilePath(),
                                       d.configuration->defaultPageSize());
        StaffType::initStaffTypes(d.configuration->defaultColor());

        MScore::warnPitchRange      = true;
        MScore::warnGuitarBends     = true;
        MScore::pedalEventsMinTicks = 1;
        Drumset::initDrumset();
        FiguredBass::readConfigFile(String());
        MScore::setNudgeStep(0.1);     // cursor key (default 0.1)
        MScore::setNudgeStep10(1.0);     // Ctrl + cursor key (default 1.0)
        MScore::setNudgeStep50(0.01);     // Alt  + cursor key (default 0.01)

        d.musicXml->init();

        loadInstrumentTemplates(":/engraving/instruments/instruments.xml");
    } catch (...) {
        d.teardown();
        throw;
    }
}

EngineContext::~EngineContext()
{
    m_impl->teardown();
}

io::IFileSystem& EngineContext::fileSystem() const { return *m_impl->fileSystem; }
EngineAudio& EngineContext::audio() const { return *m_impl->audio; }
RuntimeAssets& EngineContext::assets() const { return *m_impl->assets; }
IFontsDatabase& EngineContext::fontsDatabase() const { return m_impl->assets->fontsDatabase(); }
EngravingServices& EngineContext::engraving() const { return m_impl->engraving; }
const mpe::IArticulationProfilesRepository& EngineContext::articulationProfiles() const
{
    return *m_impl->articulationProfiles;
}
const mu::iex::musicxml::IMusicXmlConfiguration& EngineContext::musicXml() const { return *m_impl->musicXml; }
}
