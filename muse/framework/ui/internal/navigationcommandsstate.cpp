/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) MuseScore Limited and others
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

#include "navigationcommandsstate.h"

using namespace muse;
using namespace muse::rcommand;
using namespace muse::ui;

std::string NavigationCommandsState::moduleName() const
{
    return "navigation";
}

void NavigationCommandsState::init()
{
}

void NavigationCommandsState::deinit()
{
}

CommandState NavigationCommandsState::commandState(const Command&) const
{
    return muse::rcommand::CommandState(true, false);
}

async::Channel<Command, CommandState> NavigationCommandsState::commandStateChanged() const
{
    static async::Channel<Command, CommandState> channel;
    return channel;
}
