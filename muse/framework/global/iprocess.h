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
#ifndef MUSE_GLOBAL_IPROCESS_H
#define MUSE_GLOBAL_IPROCESS_H

#include <functional>
#include <string>
#include <vector>

#include "modularity/imoduleinterface.h"

namespace muse {
class IProcess : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(muse::IProcess)

public:
    static constexpr int ExecuteCrashCode = -1;
    static constexpr int ExecuteStartFailedCode = -2;
    static constexpr int ExecuteTimeoutCode = -3;
    static constexpr int ExecuteCanceledCode = -4;

    virtual ~IProcess() = default;

    virtual int execute(const std::string& program, const std::vector<std::string>& arguments = {}) = 0;
    virtual int execute(const std::string& program, const std::vector<std::string>& arguments, int timeout_ms) = 0;
    virtual int execute(const std::string& program, const std::vector<std::string>& arguments, int timeout_ms,
                        const std::function<bool()>& shouldCancel) = 0;
    virtual bool startDetached(const std::string& program, const std::vector<std::string>& arguments = {}) = 0;
};
}

#endif // MUSE_GLOBAL_IPROCESS_H
