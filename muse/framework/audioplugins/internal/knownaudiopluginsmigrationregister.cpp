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

#include "knownaudiopluginsmigrationregister.h"

#include "../audiopluginstypes.h"

#include "log.h"

using namespace muse;
using namespace muse::audioplugins;

KnownAudioPluginsMigrationRegister::KnownAudioPluginsMigrationRegister()
{
    // v0 -> v1: {version, plugins} envelope introduced; wrapping happens in load(), no-op here
    registerMigration(0, [](const JsonArray& plugins) {
        return plugins;
    });

    // v1 -> v2: enabled boolean replaced by the state string
    registerMigration(1, [](const JsonArray& plugins) {
        JsonArray out;
        for (size_t i = 0; i < plugins.size(); ++i) {
            JsonObject obj = plugins.at(i).toObject();
            const bool enabled = obj.value("enabled").toBool();
            obj.set("state", audioPluginStateName(enabled ? AudioPluginState::Validated
                                                  : AudioPluginState::Error));

            JsonObject rebuilt;
            for (const std::string& k : obj.keys()) {
                if (k == "enabled") {
                    continue;
                }
                rebuilt.set(k, obj.value(k));
            }
            out << rebuilt;
        }
        return out;
    });

    // v2 -> v3: no-op default, apps override this slot with their own work
    registerMigration(2, [](const JsonArray& plugins) {
        return plugins;
    });

    // v3 -> v4: bundled plugins are persisted under a portable ${INSTALL_ROOT} path
    registerMigration(3, [](const JsonArray& plugins) {
        return plugins;
    });
}

void KnownAudioPluginsMigrationRegister::registerMigration(int fromVersion, PluginsMigration cb)
{
    m_migrations[fromVersion] = std::move(cb);
}

Ret KnownAudioPluginsMigrationRegister::migrate(int fromVersion, int toVersion, JsonArray& plugins) const
{
    if (fromVersion == toVersion) {
        return make_ok();
    }

    if (fromVersion > toVersion) {
        return Ret(static_cast<int>(Ret::Code::UnknownError),
                   "cache file version " + std::to_string(fromVersion)
                   + " is newer than this build's expected version "
                   + std::to_string(toVersion)
                   + " (no downgrade). Delete the file or upgrade the build.");
    }

    for (int v = fromVersion; v < toVersion; ++v) {
        auto it = m_migrations.find(v);
        if (it == m_migrations.end()) {
            return Ret(static_cast<int>(Ret::Code::UnknownError),
                       "missing migrator for cache version step "
                       + std::to_string(v) + " -> " + std::to_string(v + 1)
                       + ". If you just bumped CURRENT_KNOWN_AUDIO_PLUGINS_VERSION, "
                       + "register a migrator in your app's AudioPluginsAppConfigModule.");
        }

        plugins = it->second(plugins);
    }

    return make_ok();
}
