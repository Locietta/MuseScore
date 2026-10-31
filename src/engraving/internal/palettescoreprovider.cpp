/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited
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
 #include "palettescoreprovider.h"


 #include "../compat/scoreaccess.h"
 #include "../infrastructure/localfileinfoprovider.h"
 #include "../style/defaultstyle.h"

using namespace muse;
using namespace mu::engraving;

PaletteScoreProvider::PaletteScoreProvider(const EngravingServices& services)
    : m_services(services)
{
}

PaletteScoreProvider::~PaletteScoreProvider()
{
}

void PaletteScoreProvider::init()
{
    m_paletteScore = compat::ScoreAccess::createMasterScore(m_services);
    m_paletteScore->setFileInfoProvider(std::make_shared<LocalFileInfoProvider>(""));

    m_paletteScore->setStyle(DefaultStyle::baseStyle());
    m_paletteScore->style().set(Sid::musicalTextFont, String(u"Leland Text"));
    IEngravingFontPtr scoreFont = m_services.engravingFonts->fontByName("Leland");
    m_paletteScore->setEngravingFont(scoreFont);
    m_paletteScore->setNoteHeadWidth(scoreFont->width(SymId::noteheadBlack,
                                                      m_paletteScore->style().spatium()) / m_paletteScore->style().defaultSpatium());
}

void PaletteScoreProvider::deinit()
{
    delete m_paletteScore;
    m_paletteScore = nullptr;
}

MasterScore* PaletteScoreProvider::paletteScore() const
{
    return m_paletteScore;
}
