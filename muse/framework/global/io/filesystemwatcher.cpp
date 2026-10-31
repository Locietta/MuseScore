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

#include "filesystemwatcher.h"


namespace muse::io {

FileSystemWatcher::FileSystemWatcher() = default;
FileSystemWatcher::~FileSystemWatcher() = default;

void FileSystemWatcher::startWatching(const std::string&) {}
void FileSystemWatcher::stopWatching(const std::string&) {}


muse::async::Channel<std::string> FileSystemWatcher::fileChanged() const
{
    return m_fileChannel;
}

muse::async::Channel<std::string> FileSystemWatcher::directoryChanged() const
{
    return m_directoryChannel;
}
}
