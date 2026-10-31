/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
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

#include <string>

#include "types/val.h"
#include "io/path.h"


//! NOTE We are gradually abandoning Qt in non-GUI classes.
//! This settings interface is almost independent of Qt,
//! QSettings are used only in the implementation for compatibility with current settings.
//! Perhaps in the future this will be changed.

class QSettings;

namespace muse {
class Settings
{
public:
    static Settings* instance();

    struct Key
    {
        std::string moduleName;
        std::string key;

        Key() = default;
        Key(std::string moduleName, std::string key);

        bool isNull() const;
        bool operator==(const Key& k) const;
        bool operator<(const Key& k) const;
    };

    struct Item
    {
        Key key;
        Val value;
        Val defaultValue;
        std::string description;

        bool canBeManuallyEdited = false;
        Val minValue;
        Val maxValue;

        bool isNull() const { return key.isNull(); }
    };

    using Items = std::map<Key, Item>;

    const Items& items() const;

    void reload();
    void load();

    void reset(bool keepDefaultSettings = false, bool notifyAboutChanges = true, bool notifyOtherInstances = true);

    const Val& value(const Key& key) const;
    const Val& defaultValue(const Key& key) const;

    //! NOTE Will be write to global config and sync between all instances
    void setSharedValue(const Key& key, const Val& value);

    //! NOTE Will be write to global config and NOT sync between instances
    void setLocalValue(const Key& key, const Val& value);

    void setDefaultValue(const Key& key, const Val& value);

    void setDescription(const Key& key, const std::string& value);

    void setCanBeManuallyEdited(const Settings::Key& key, bool canBeManuallyEdited, const Val& minValue = Val(),
                                const Val& maxValue = Val());

    void beginTransaction(bool notifyToOtherInstances = true);
    void commitTransaction(bool notifyToOtherInstances = true);
    void rollbackTransaction(bool notifyToOtherInstances = true);


    io::path_t filePath() const;

private:
    Settings();
    ~Settings();

    Item& findItem(const Key& key) const;

    void insertNewItem(const Key& key, const Val& value);

    Items readItems() const;
    void writeValue(const Key& key, const Val& value);


    QSettings* m_settings = nullptr;
    mutable Items m_items;
    mutable Items m_localSettings;
    mutable bool m_isTransactionStarted = false;
};

inline Settings* settings()
{
    return Settings::instance();
}
}
