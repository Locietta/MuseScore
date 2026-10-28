/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited and others
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

#include "global/modularity/imoduleinterface.h"
#include "global/functional/inplace_function.h"
#include <memory>
#include <vector>

namespace muse::audio {
class IAudioTaskScheduler : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IAudioTaskScheduler)
public:
    using Task = muse::functional::inplace_function<void (), 64>;
    virtual void submitRealtimeTasksAndWait(const std::vector<Task>& tasks) = 0;
};

using IAudioTaskSchedulerPtr = std::shared_ptr<IAudioTaskScheduler>;
}
