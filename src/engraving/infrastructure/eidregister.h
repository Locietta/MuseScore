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
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "eid.h"
#include "../types/types.h"

namespace mu::engraving {
class EngravingObject;

// EIDs in the input that could not be used as they were: the text was not a valid
// id, or another element already had it. The reader repairs both, so these count
// load-time repairs, not a live inconsistency.
struct EIDRepairCount
{
    size_t malformed = 0;
    size_t duplicate = 0;

    bool any() const { return malformed > 0 || duplicate > 0; }
};

class EIDRegister
{
public:
    EIDRegister() = default;

    EID newEIDForItem(const EngravingObject* item);
    // False, and neither map touched, when the EID already belongs to another
    // element; the caller gives the element a fresh EID instead.
    bool registerItemEID(const EID& eid, const EngravingObject* item);
    void removeItem(const EngravingObject* item);
    void onItemDestroyed(const EngravingObject* item);

    void noteMalformedEID() { ++m_repairs.malformed; }
    EIDRepairCount repairs() const;

    EngravingObject* itemFromEID(const EID& eid) const;
    EID EIDFromItem(const EngravingObject* item) const;
    // Every pair in the register, for the id bijection checks of the tests.
    std::vector<std::pair<EID, const EngravingObject*> > entries() const;
    size_t size() const;

private:
    EIDRegister(const EIDRegister&) = delete;

    std::unordered_map<EID, EngravingObject*> m_eidToItem;
    std::unordered_map<EngravingObject*, EID> m_itemToEid;

    EIDRepairCount m_repairs;

    uint64_t m_maxValTestMode = 0;

    mutable std::shared_mutex m_mutex;
};
}
