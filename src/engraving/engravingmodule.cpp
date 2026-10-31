/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "engravingmodule.h"

#include "modularity/ioc.h"
#include "global/allocator.h"

#include "draw/ifontprovider.h"
#include "draw/internal/ifontsdatabase.h"

#include "infrastructure/smufl.h"

#include "internal/engravingconfiguration.h"
#include "internal/engravingcontextconfiguration.h"
#include "internal/engravingfontsprovider.h"
#include "internal/palettescoreprovider.h"

#include "engraving/style/defaultstyle.h"

#include "engraving/dom/stafftype.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/drumset.h"
#include "engraving/dom/figuredbass.h"

#include "rendering/score/scorerenderer.h"
#include "rendering/single/singlerenderer.h"
#include "rendering/editmode/editmoderenderer.h"


#ifdef MUE_BUILD_ENGRAVING_DEVTOOLS
#include "interactive/iinteractiveuriregister.h"
#include "devtools/engravingelementsprovider.h"
#include "devtools/drawdata/diagnosticdrawprovider.h"
#endif

#include "muse_framework_config.h"

#include "log.h"

using namespace mu::engraving;
using namespace muse;
using namespace muse::modularity;
using namespace muse::draw;

static const std::string mname("engraving");

static void engraving_init_qrc()
{
}

std::string EngravingModule::moduleName() const
{
    return mname;
}

void EngravingModule::registerExports()
{

    m_configuration = std::make_shared<EngravingConfiguration>();
    m_engravingfonts = std::make_shared<EngravingFontsProvider>();

    globalIoc()->registerExport<IEngravingConfiguration>(mname, m_configuration);
    globalIoc()->registerExport<IEngravingFontsProvider>(mname, m_engravingfonts);

    // internal
    globalIoc()->registerExport<rendering::IScoreRenderer>(mname, new rendering::score::ScoreRenderer());
    globalIoc()->registerExport<rendering::ISingleRenderer>(mname, new rendering::single::SingleRenderer());
    globalIoc()->registerExport<rendering::IEditModeRenderer>(mname, new rendering::editmode::EditModeRenderer());

#ifdef MUE_BUILD_ENGRAVING_DEVTOOLS
    globalIoc()->registerExport<IDiagnosticDrawProvider>(mname, new DiagnosticDrawProvider());
#endif
}

void EngravingModule::resolveImports()
{
#ifdef MUE_BUILD_ENGRAVING_DEVTOOLS
    auto ir = globalIoc()->resolve<muse::interactive::IInteractiveUriRegister>(mname);
    if (ir) {
        ir->registerQmlUri(Uri("musescore://diagnostics/engraving/elements"), "MuseScore.Engraving", "EngravingElementsDialog");
        ir->registerQmlUri(Uri("musescore://diagnostics/engraving/undostack"), "MuseScore.Engraving", "EngravingUndoStackDialog");
        ir->registerQmlUri(Uri("musescore://diagnostics/engraving/style"), "MuseScore.Engraving", "EngravingStyleDialog");
    }
#endif
}

void EngravingModule::registerApi()
{
}

void EngravingModule::registerResources()
{
    engraving_init_qrc();
}

void EngravingModule::registerUiTypes()
{
    MScore::registerUiTypes();
}

void EngravingModule::onInit(const IApplication::RunMode&)
{
    // Init fonts
    {
        using namespace muse::draw;

        std::shared_ptr<IFontsDatabase> fdb = globalIoc()->resolve<IFontsDatabase>(mname);

        // Text
        fdb->addFont(FontDataKey(u"Edwin", false, false), ":/fonts/edwin/Edwin-Roman.otf");
        fdb->addFont(FontDataKey(u"Edwin", false, true), ":/fonts/edwin/Edwin-Italic.otf");
        fdb->addFont(FontDataKey(u"Edwin", true, false), ":/fonts/edwin/Edwin-Bold.otf");
        fdb->addFont(FontDataKey(u"Edwin", true, true), ":/fonts/edwin/Edwin-BdIta.otf");

        // MusicSymbol[Text]
        auto addMusicFont = [this, fdb](const std::string& name, const FontDataKey& fontDataKey, const muse::io::path_t& filePath){
            fdb->addFont(FontDataKey(fontDataKey), filePath);
            m_engravingfonts->addInternalFont(name, fontDataKey.family().id().toStdString(), filePath);
        };

        addMusicFont("Bravura", FontDataKey(u"Bravura"), ":/fonts/bravura/Bravura.otf");
        fdb->addFont(FontDataKey(u"Bravura Text"), ":/fonts/bravura/BravuraText.otf");
        addMusicFont("Leland", FontDataKey(u"Leland"), ":/fonts/leland/Leland.otf");
        fdb->addFont(FontDataKey(u"Leland Text"), ":/fonts/leland/LelandText.otf");
        addMusicFont("Emmentaler", FontDataKey(u"MScore"), ":/fonts/mscore/MScore.otf");
        fdb->addFont(FontDataKey(u"MScore Text"), ":/fonts/mscore/MScoreText.otf");
        addMusicFont("Gonville", FontDataKey(u"Gootville"), ":/fonts/gootville/Gootville.otf");
        fdb->addFont(FontDataKey(u"Gootville Text"), ":/fonts/gootville/GootvilleText.otf");
        addMusicFont("MuseJazz", FontDataKey(u"MuseJazz"), ":/fonts/musejazz/MuseJazz.otf");
        fdb->addFont(FontDataKey(u"MuseJazz Text"), ":/fonts/musejazz/MuseJazzText.otf");
        addMusicFont("Petaluma", FontDataKey(u"Petaluma"),    ":/fonts/petaluma/Petaluma.otf");
        fdb->addFont(FontDataKey(u"Petaluma Text"), ":/fonts/petaluma/PetalumaText.otf");
        addMusicFont("Finale Maestro", FontDataKey(u"Finale Maestro"), ":/fonts/finalemaestro/FinaleMaestro.otf");
        fdb->addFont(FontDataKey(u"Finale Maestro Text"), ":/fonts/finalemaestro/FinaleMaestroText.otf");
        addMusicFont("Finale Broadway", FontDataKey(u"Finale Broadway"), ":/fonts/finalebroadway/FinaleBroadway.otf");
        fdb->addFont(FontDataKey(u"Finale Broadway Text"), ":/fonts/finalebroadway/FinaleBroadwayText.otf");

        // Tabulature
        fdb->addFont(FontDataKey(u"FreeSerif"), ":/fonts/FreeSerif.ttf");
        fdb->addFont(FontDataKey(u"FreeSerif", true, false), ":/fonts/FreeSerifBold.ttf");
        fdb->addFont(FontDataKey(u"FreeSerif", false, true), ":/fonts/FreeSerifItalic.ttf");
        fdb->addFont(FontDataKey(u"FreeSerif", true, true), ":/fonts/FreeSerifBoldItalic.ttf");
        fdb->addFont(FontDataKey(u"FreeSans"), ":/fonts/FreeSans.ttf");
        fdb->addFont(FontDataKey(u"MuseScoreTabRenaiss"), ":/fonts/MuseScoreTabRenaiss.ttf");
        fdb->addFont(FontDataKey(u"MuseScoreTabPhalese"), ":/fonts/MuseScoreTabPhalese.ttf");
        fdb->addFont(FontDataKey(u"MuseScoreTabBonneuilDeVisee"), ":/fonts/MuseScoreTabBonneuilDeVisee.ttf");
        fdb->addFont(FontDataKey(u"MuseScoreTabBonneuilGaultier"), ":/fonts/MuseScoreTabBonneuilGaultier.ttf");
        fdb->addFont(FontDataKey(u"MuseScoreTabDowland"), ":/fonts/MuseScoreTabDowland.ttf");
        fdb->addFont(FontDataKey(u"MuseScoreTabLuteDidactic"), ":/fonts/MuseScoreTabLuteDidactic.ttf");
        fdb->addFont(FontDataKey(u"MuseScoreTabModern"), ":/fonts/MuseScoreTabModern.ttf");
        fdb->addFont(FontDataKey(u"MuseScoreTabItalian"), ":/fonts/MuseScoreTabItalian.ttf");
        fdb->addFont(FontDataKey(u"MuseScoreTabFrench"), ":/fonts/MuseScoreTabFrench.ttf");
        fdb->addFont(FontDataKey(u"MuseScoreTabFrenchBaroqueHeadless"), ":/fonts/MuseScoreTabFrenchBaroqueHeadless.ttf");
        fdb->addFont(FontDataKey(u"MuseScoreTabFrenchBaroque"), ":/fonts/MuseScoreTabFrenchBaroque.ttf");

        // Figured Bass
        fdb->addFont(FontDataKey(u"MscoreBC"), ":/fonts/mscore-BC.ttf");

        // Roman Numeral Analysis
        fdb->addFont(FontDataKey(u"Campania"), ":/fonts/campania/Campania.otf");

        // Defaults
        fdb->setDefaultFont(Font::Type::Unknown, FontDataKey(u"Edwin"));
        fdb->setDefaultFont(Font::Type::Text, FontDataKey(u"Edwin"));
        fdb->setDefaultFont(Font::Type::Tablature, FontDataKey(u"FreeSerif"));
        fdb->setDefaultFont(Font::Type::MusicSymbolText, FontDataKey(u"Bravura Text"));
        fdb->setDefaultFont(Font::Type::MusicSymbol, FontDataKey(u"Bravura"));
        m_engravingfonts->setFallbackFont("Bravura");

        //! NOTE Used for Qt font provider
        fdb->insertSubstitution(u"Leland Text",    u"Bravura Text");
        fdb->insertSubstitution(u"Bravura Text",   u"Leland Text");
        fdb->insertSubstitution(u"MScore Text",    u"Leland Text");
        fdb->insertSubstitution(u"Gootville Text", u"Leland Text");
        fdb->insertSubstitution(u"MuseJazz Text",  u"Leland Text");
        fdb->insertSubstitution(u"Petaluma Text",  u"MuseJazz Text");
        fdb->insertSubstitution(u"Finale Maestro Text", u"Leland Text");
        fdb->insertSubstitution(u"Finale Broadway Text", u"MuseJazz Text");
        fdb->insertSubstitution(u"ScoreFont",      u"Leland Text");// alias for current Musical Text Font

        // Symbols
        Smufl::init();

        //! NOTE It may be necessary to draw something with these fonts without requesting the fonts themselves
        //! (for example, simply specifying the family name for painter).
        //! But if they are not loaded, then they are not added to the font database and,
        //! accordingly, they are drawn incorrectly
        m_engravingfonts->loadAllFonts();
    }

    m_configuration->init();

    DefaultStyle::instance()->init(m_configuration->defaultStyleFilePath(),
                                   m_configuration->partStyleFilePath(),
                                   m_configuration->defaultPageSize());

    StaffType::initStaffTypes(m_configuration->defaultColor());

    // initialize dom

    MScore::warnPitchRange      = true;
    MScore::warnGuitarBends     = true;
    MScore::pedalEventsMinTicks = 1;

    Drumset::initDrumset();
    FiguredBass::readConfigFile(String());

    MScore::setNudgeStep(0.1);     // cursor key (default 0.1)
    MScore::setNudgeStep10(1.0);     // Ctrl + cursor key (default 1.0)
    MScore::setNudgeStep50(0.01);     // Alt  + cursor key (default 0.01)

    //! NOTE And some initialization in the `Notation::init()`
}

void EngravingModule::onDeinit()
{
    m_engravingfonts->deinit();
}

IContextSetup* EngravingModule::newContext(const muse::modularity::ContextPtr& ctx) const
{
    return new EngravingContext(ctx);
}

void EngravingContext::registerExports()
{
    m_paletteScoreProvider = std::make_shared<PaletteScoreProvider>(iocContext());
    ioc()->registerExport<IPaletteScoreProvider>(mname, m_paletteScoreProvider);
#ifdef MUE_BUILD_ENGRAVING_DEVTOOLS
    ioc()->registerExport<IEngravingElementsProvider>(mname, new EngravingElementsProvider());
#endif
    ioc()->registerExport<IEngravingContextConfiguration>(mname, new EngravingContextConfiguration(iocContext()));
}

void EngravingContext::onInit(const muse::IApplication::RunMode&)
{
    m_paletteScoreProvider->init();
}

void EngravingContext::onDestroy()
{
    m_paletteScoreProvider->deinit();
}
