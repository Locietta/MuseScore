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
#include "importgtp.h"

#include <cstdint>

#include "serialization/zipreader.h"

#include "gtp/gp7dombuilder.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/bracketItem.h"
#include "engraving/dom/instrtemplate.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/part.h"
#include "engraving/dom/staff.h"

#include "log.h"

using namespace muse;
using namespace muse::io;
using namespace mu::engraving;

namespace mu::iex::guitarpro {
namespace {
constexpr uint64_t GP7_MAX_ENTRY_BYTES = 64ull * 1024 * 1024;
constexpr uint64_t GP7_MAX_TOTAL_BYTES = 128ull * 1024 * 1024;
constexpr size_t GP7_MAX_ENTRIES = 4096;
}

//---------------------------------------------------------
//   read
//---------------------------------------------------------

bool GuitarPro7::read(IODevice* io)
{
    f = io;
    previousTempo = -1;
    m_gpifRead = false;

    ZipReader zip(io);
    const auto entries = zip.fileInfoList();
    if (zip.hasError() || entries.empty() || entries.size() > GP7_MAX_ENTRIES) {
        LOGE() << "Invalid or oversized Guitar Pro 7 archive";
        return false;
    }

    uint64_t total = 0;
    for (const auto& entry : entries) {
        if (entry.size > GP7_MAX_ENTRY_BYTES || total > GP7_MAX_TOTAL_BYTES - entry.size) {
            LOGE() << "Guitar Pro 7 archive declares an unsafe uncompressed size";
            return false;
        }
        total += entry.size;
    }

    if (!zip.fileExists("Content/score.gpif")) {
        LOGE() << "Guitar Pro 7 archive has no Content/score.gpif";
        return false;
    }

    ByteArray fileData = zip.fileData("Content/score.gpif");
    if (zip.hasError() || fileData.empty() || fileData.size() > GP7_MAX_ENTRY_BYTES) {
        LOGE() << "Invalid Content/score.gpif in Guitar Pro 7 archive";
        return false;
    }

    ByteArray partsData;
    if (zip.fileExists("Content/PartConfiguration")) {
        partsData = zip.fileData("Content/PartConfiguration");
        if (partsData.size() > GP7_MAX_ENTRY_BYTES) {
            LOGE() << "Oversized Content/PartConfiguration in Guitar Pro 7 archive";
            return false;
        }
    }
    zip.close();

    m_properties = readProperties(&partsData);

    readGpif(&fileData);
    return m_gpifRead;
}

//---------------------------------------------------------
//   readProperties
//---------------------------------------------------------

GuitarPro::GPProperties GuitarPro7::readProperties(ByteArray* data)
{
    GPProperties properties;
    size_t partsInfoSize = data->size();
    const size_t numInstrOffset = 8;
    if (partsInfoSize <= numInstrOffset) {
        LOGE() << "failed to read gp properties";
        return properties;
    }

    size_t numberOfInstruments = static_cast<size_t>(data->at(numInstrOffset));
    if (partsInfoSize <= numInstrOffset + numberOfInstruments) {
        LOGE() << "failed to read gp properties";
        return properties;
    }

    std::vector<TabImportOption> partsImportOpts = properties.partsImportOptions;

    for (size_t i = numInstrOffset + 1; i <= numInstrOffset + numberOfInstruments; i++) {
        partsImportOpts.push_back(static_cast<TabImportOption>(data->at(static_cast<int>(i))));
    }

    return properties;
}

std::unique_ptr<IGPDomBuilder> GuitarPro7::createGPDomBuilder() const
{
    return std::make_unique<GP7DomBuilder>();
}
} // namespace mu::iex::guitarpro
