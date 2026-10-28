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
#pragma once

#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "global/io/path.h"
#include "global/types/string.h"

namespace muse::audioplugins {
using PluginResourceId = std::string;
using PluginResourceIdList = std::vector<PluginResourceId>;
using PluginVendor = std::string;
using PluginAttributes = std::map<String, String>;

// Opaque app-defined plugin format identifier; the framework stores it verbatim.
using PluginType = std::string;

struct PluginMeta {
    PluginResourceId id;
    PluginVendor vendor;
    PluginAttributes attributes;
    PluginType type;

    const String& attributeVal(const String& key) const
    {
        auto search = attributes.find(key);
        if (search != attributes.cend()) {
            return search->second;
        }

        static String empty;
        return empty;
    }

    bool isValid() const
    {
        return !id.empty()
               && !vendor.empty()
               && !type.empty();
    }

    bool operator==(const PluginMeta& other) const
    {
        return id == other.id
               && vendor == other.vendor
               && type == other.type
               && attributes == other.attributes;
    }

    bool operator!=(const PluginMeta& other) const
    {
        return !(*this == other);
    }

    bool operator<(const PluginMeta& other) const
    {
        return std::tie(id, vendor, type, attributes)
               < std::tie(other.id, other.vendor, other.type, other.attributes);
    }
};

using PluginMetaList = std::vector<PluginMeta>;
using PluginMetaSet = std::set<PluginMeta>;

// Typed accessors over the string-encoded attributes ("true"/"1" -> true).
inline bool boolAttribute(const PluginMeta& meta, const String& key, bool fallback = false)
{
    const String& v = meta.attributeVal(key);
    if (v.empty()) {
        return fallback;
    }
    return v == u"true" || v == u"1";
}

inline int intAttribute(const PluginMeta& meta, const String& key, int fallback = 0)
{
    const String& v = meta.attributeVal(key);
    if (v.empty()) {
        return fallback;
    }
    bool ok = true;
    int n = v.toInt(&ok);
    return ok ? n : fallback;
}

enum class AudioPluginState {
    Undefined = -1,
    Discovered,  // found on disk; not yet validated
    Validated,   // validation succeeded; usable
    Missing,     // previously known path no longer found
    Error,       // validation failed (see errorCode)
};

namespace detail {
inline const std::map<AudioPluginState, std::string>& audioPluginStateNames()
{
    static const std::map<AudioPluginState, std::string> NAMES = {
        { AudioPluginState::Undefined, "Undefined" },
        { AudioPluginState::Discovered, "Discovered" },
        { AudioPluginState::Validated, "Validated" },
        { AudioPluginState::Missing, "Missing" },
        { AudioPluginState::Error, "Error" },
    };
    return NAMES;
}
}

inline const std::string& audioPluginStateName(AudioPluginState state)
{
    const auto& names = detail::audioPluginStateNames();
    auto it = names.find(state);
    if (it != names.end()) {
        return it->second;
    }
    static const std::string empty;
    return empty;
}

inline AudioPluginState audioPluginStateFromName(const std::string& name)
{
    for (const auto& kv : detail::audioPluginStateNames()) {
        if (kv.second == name) {
            return kv.first;
        }
    }
    return AudioPluginState::Undefined;
}

struct AudioPluginInfo {
    PluginMeta meta;
    io::path_t path;
    AudioPluginState state = AudioPluginState::Undefined;
    int errorCode = 0;
};

using AudioPluginInfoList = std::vector<AudioPluginInfo>;
}
