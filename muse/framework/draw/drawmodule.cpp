/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
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
#include "drawmodule.h"

#include "global/modularity/ioc.h"

#include "internal/fontprovider.h"
#include "internal/fontsengine.h"

#include "internal/fontsdatabase.h"

#include "muse_framework_config.h"

using namespace muse::draw;
using namespace muse::modularity;

std::string DrawModule::moduleName() const
{
    return "draw";
}

void DrawModule::registerExports()
{

    auto database = std::make_shared<FontsDatabase>();
    m_fontsEngine = std::make_shared<FontsEngine>(*database);
    globalIoc()->registerExport<draw::IFontProvider>(moduleName(), std::make_shared<FontProvider>(*database, *m_fontsEngine));
    globalIoc()->registerExport<draw::IFontsEngine>(moduleName(), m_fontsEngine);
    globalIoc()->registerExport<draw::IFontsDatabase>(moduleName(), database);

}

void DrawModule::onInit(const IApplication::RunMode&)
{
    m_fontsEngine->init();
}
