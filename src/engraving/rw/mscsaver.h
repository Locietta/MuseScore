/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2025 MuseScore Limited and others
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

#pragma once

#include <memory>

#include "draw/iimageprovider.h"

#include "../engravingservices.h"
#include "../iengravingconfiguration.h"
#include "../infrastructure/mscwriter.h"
#include "../rendering/iscorerenderer.h"

namespace muse::draw {
class Pixmap;
}

namespace mu::engraving::write {
class WriteContext;
}

namespace mu::engraving {
class MasterScore;
class Score;
class MscSaver
{
public:
    explicit MscSaver(const EngravingServices& services)
        : m_services(services) {}

    bool writeMscz(MasterScore* score, MscWriter& mscWriter, bool createThumbnail, const write::WriteContext* ctx = nullptr);

    bool exportPart(Score* partScore, MscWriter& mscWriter);

private:
    const std::shared_ptr<muse::draw::IImageProvider>& imageProvider() const { return m_services.imageProvider; }
    const std::shared_ptr<IEngravingConfiguration>& configuration() const { return m_services.configuration; }
    const std::shared_ptr<rendering::IScoreRenderer>& scoreRenderer() const { return m_services.renderer; }

    std::shared_ptr<muse::draw::Pixmap> createThumbnail(Score* score);

    const EngravingServices& m_services;
};
}
