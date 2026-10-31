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

#ifndef MUSE_MPE_ARTICULATIONPROFILESREPOSITORY_H
#define MUSE_MPE_ARTICULATIONPROFILESREPOSITORY_H

#include "io/ifilesystem.h"
#include "serialization/json.h"

#include "mpe/iarticulationprofilesrepository.h"

namespace muse::mpe {
class ArticulationProfilesRepository : public IArticulationProfilesRepository
{
public:
    explicit ArticulationProfilesRepository(io::IFileSystem& fileSystem)
        : m_fileSystem(fileSystem) {}

    ArticulationsProfilePtr createNew() const override;
    ArticulationsProfilePtr defaultProfile(const ArticulationFamily family) const override;
    ArticulationsProfilePtr loadProfile(const io::path_t& path) const override;
    void saveProfile(const io::path_t& path, const ArticulationsProfilePtr profilePtr) override;

private:
    io::IFileSystem* fileSystem() const { return &m_fileSystem; }

    std::vector<ArticulationFamily> supportedFamiliesFromJson(const JsonArray& array) const;
    JsonArray supportedFamiliesToJson(const std::vector<ArticulationFamily>& families) const;

    ArticulationPattern patternsScopeFromJson(const JsonArray& array) const;
    JsonArray patternsScopeToJson(const ArticulationPattern& scope) const;

    ArrangementPattern arrangementPatternFromJson(const JsonObject& obj) const;
    JsonObject arrangementPatternToJson(const ArrangementPattern& pattern) const;

    PitchPattern pitchPatternFromJson(const JsonObject& obj) const;
    JsonObject pitchPatternToJson(const PitchPattern& pattern) const;

    ExpressionPattern expressionPatternFromJson(const JsonObject& obj) const;
    JsonObject expressionPatternToJson(const ExpressionPattern& pattern) const;

    mutable std::unordered_map<ArticulationFamily, ArticulationsProfilePtr> m_defaultProfiles;

    io::IFileSystem& m_fileSystem;
};
}

#endif // MUSE_MPE_ARTICULATIONPROFILESREPOSITORY_H
