#include "settings.h"
#include <stdexcept>
#include <utility>

namespace muse {

Settings* Settings::instance()
{
    static Settings s;
    return &s;
}

Settings::Settings() = default;
Settings::~Settings() = default;

Settings::Key::Key(std::string moduleName, std::string key)
    : moduleName(std::move(moduleName)), key(std::move(key))
{
}

bool Settings::Key::isNull() const
{
    return key.empty();
}

bool Settings::Key::operator==(const Key& k) const
{
    return key == k.key;
}

bool Settings::Key::operator<(const Key& k) const
{
    return key < k.key;
}

const Settings::Items& Settings::items() const
{
    return m_isTransactionStarted ? m_localSettings : m_items;
}

void Settings::load()
{
    auto loaded = readItems();
    for (const auto& [k, v] : loaded) {
        m_items[k] = v;
    }
}

void Settings::reload()
{
    load();
}

void Settings::reset(bool keepDefaultSettings, bool notifyAboutChanges, bool notifyOtherInstances)
{
    (void)keepDefaultSettings;
    (void)notifyOtherInstances;
    (void)notifyAboutChanges;
    m_localSettings.clear();
    m_isTransactionStarted = false;
    for (auto& [k, item] : m_items) {
        item.value = item.defaultValue;
    }
}

const Val& Settings::value(const Key& key) const
{
    return findItem(key).value;
}

const Val& Settings::defaultValue(const Key& key) const
{
    return findItem(key).defaultValue;
}

void Settings::setSharedValue(const Key& key, const Val& value)
{
    setLocalValue(key, value);
}

void Settings::setLocalValue(const Key& key, const Val& value)
{
    if (key.isNull()) {
        return;
    }
    Item& item = findItem(key);
    if (item.isNull()) {
        insertNewItem(key, value);
    } else {
        if (item.value == value) {
            return;
        }
        item.value = value;
    }
    if (!m_isTransactionStarted) {
        writeValue(key, value);
    }
}

void Settings::setDefaultValue(const Key& key, const Val& value)
{
    Item& item = findItem(key);
    if (item.isNull()) {
        (m_isTransactionStarted ? m_localSettings : m_items)[key] = Item{ key, value, value, "", false, Val(), Val() };
    } else {
        item.defaultValue = value;
        item.value.setType(value.type());
    }
}

void Settings::setDescription(const Key& key, const std::string& value)
{
    Item& item = findItem(key);
    if (!item.isNull()) {
        item.description = value;
    }
}

void Settings::setCanBeManuallyEdited(const Settings::Key& key, bool canBeManuallyEdited, const Val& minValue, const Val& maxValue)
{
    Item& item = findItem(key);
    if (item.isNull()) {
        (m_isTransactionStarted ? m_localSettings : m_items)[key] = Item{ key, Val(), Val(), "", canBeManuallyEdited, minValue, maxValue };
    } else {
        item.canBeManuallyEdited = canBeManuallyEdited;
        item.minValue = minValue;
        item.maxValue = maxValue;
    }
}

void Settings::beginTransaction(bool notifyToOtherInstances)
{
    (void)notifyToOtherInstances;
    if (m_isTransactionStarted) {
        throw std::logic_error("Transaction is already started");
    }
    m_localSettings = m_items;
    m_isTransactionStarted = true;
}

void Settings::commitTransaction(bool notifyToOtherInstances)
{
    (void)notifyToOtherInstances;
    if (!m_isTransactionStarted) return;
    m_items = std::move(m_localSettings);
    m_isTransactionStarted = false;
    m_localSettings.clear();
}

void Settings::rollbackTransaction(bool notifyToOtherInstances)
{
    (void)notifyToOtherInstances;
    m_isTransactionStarted = false;
    m_localSettings.clear();
}


io::path_t Settings::filePath() const
{
    return io::path_t();
}

Settings::Item& Settings::findItem(const Key& key) const
{
    Items& items = m_isTransactionStarted ? m_localSettings : m_items;
    auto it = items.find(key);
    if (it == items.end()) {
        static Item null;
        return null;
    }
    return it->second;
}


void Settings::insertNewItem(const Settings::Key& key, const Val& value)
{
    Item item = Item{ key, value, value, "", false, Val(), Val() };
    if (m_isTransactionStarted) {
        m_localSettings[key] = item;
    } else {
        m_items[key] = item;
    }
}

Settings::Items Settings::readItems() const
{
    return m_items;
}

void Settings::writeValue(const Key& key, const Val& value)
{
    (void)key;
    (void)value;
}


} // namespace muse
