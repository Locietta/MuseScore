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

#include <memory>
#include <map>
#include <variant>

namespace muse {
template<typename KeyType, typename ValType>
class SharedMap
{
public:

    // like std
    using key_type = KeyType;
    using mapped_type = ValType;
    using value_type = std::pair<const KeyType, ValType>;

    using PairType = value_type;
    using Data = std::map<KeyType, ValType>;
    using DataPtr = std::shared_ptr<Data>;
    typedef typename Data::iterator iterator;
    typedef typename Data::const_iterator const_iterator;
    typedef typename Data::reverse_iterator reverse_iterator;
    typedef typename Data::const_reverse_iterator const_reverse_iterator;

    SharedMap()
    {
        m_dataPtr = std::make_shared<Data>();
    }

    SharedMap(std::initializer_list<PairType> initList)
    {
        m_dataPtr = std::make_shared<Data>(initList);
    }

    SharedMap& operator=(std::initializer_list<PairType> initList)
    {
        m_dataPtr = std::make_shared<Data>(initList);
        return *this;
    }

    SharedMap(const SharedMap&) = default;
    SharedMap(SharedMap&&) = default;
    SharedMap& operator=(const SharedMap&) = default;
    SharedMap& operator=(SharedMap&&) = default;

    const ValType& at(const KeyType& key) const
    {
        return m_dataPtr->at(key);
    }

    ValType& at(const KeyType& key)
    {
        ensureDetach();
        return m_dataPtr->at(key);
    }

    ValType& operator[](const KeyType& key)
    {
        ensureDetach();
        return m_dataPtr->operator [](key);
    }

    ValType& operator[](KeyType&& key)
    {
        ensureDetach();
        return m_dataPtr->operator [](std::forward<KeyType>(key));
    }

    iterator begin() noexcept
    {
        ensureDetach();
        return m_dataPtr->begin();
    }

    iterator end() noexcept
    {
        ensureDetach();
        return m_dataPtr->end();
    }

    const_iterator begin() const noexcept
    {
        return m_dataPtr->cbegin();
    }

    const_iterator end() const noexcept
    {
        return m_dataPtr->cend();
    }

    const_iterator cbegin() const noexcept
    {
        return m_dataPtr->cbegin();
    }

    const_iterator cend() const noexcept
    {
        return m_dataPtr->cend();
    }

    const_reverse_iterator rbegin() const noexcept
    {
        return m_dataPtr->rbegin();
    }

    const_reverse_iterator rend() const noexcept
    {
        return m_dataPtr->rend();
    }

    const_iterator find(const KeyType& key) const noexcept
    {
        return m_dataPtr->find(key);
    }

    iterator find(const KeyType& key) noexcept
    {
        ensureDetach();
        return m_dataPtr->find(key);
    }

    iterator lower_bound(const KeyType& key)
    {
        ensureDetach();
        return m_dataPtr->lower_bound(key);
    }

    const_iterator lower_bound(const KeyType& key) const
    {
        return m_dataPtr->lower_bound(key);
    }

    iterator upper_bound(const KeyType& key)
    {
        ensureDetach();
        return m_dataPtr->upper_bound(key);
    }

    const_iterator upper_bound(const KeyType& key) const
    {
        return m_dataPtr->upper_bound(key);
    }

    bool contains(const KeyType& key) const noexcept
    {
        return find(key) != end();
    }

    bool empty() const noexcept
    {
        return m_dataPtr->empty();
    }

    size_t size() const noexcept
    {
        return m_dataPtr->size();
    }

    void insert(const PairType& pair)
    {
        ensureDetach();
        m_dataPtr->insert(pair);
    }

    void insert(PairType&& pair)
    {
        ensureDetach();
        m_dataPtr->insert(std::forward<PairType>(pair));
    }

    void insert(const_iterator first, const_iterator last)
    {
        ensureDetach();
        m_dataPtr->insert(first, last);
    }

    iterator insert(const_iterator hint, const PairType& pair)
    {
        return withDetachedHint(hint, [this, &pair](const_iterator h) { return m_dataPtr->insert(h, pair); });
    }

    iterator insert(const_iterator hint, PairType&& pair)
    {
        return withDetachedHint(hint, [this, &pair](const_iterator h) {
            return m_dataPtr->insert(h, std::forward<PairType>(pair));
        });
    }

    void merge(SharedMap& source)
    {
        ensureDetach();
        source.ensureDetach();
        m_dataPtr->merge(*source.m_dataPtr);
    }

    void insert_or_assign(const KeyType& key, ValType&& val)
    {
        ensureDetach();
        m_dataPtr->insert_or_assign(key, std::forward<ValType>(val));
    }

    void insert_or_assign(const KeyType& key, const ValType& val)
    {
        ensureDetach();
        m_dataPtr->insert_or_assign(key, val);
    }

    template<typename ... Args>
    void emplace(Args&& ... args)
    {
        ensureDetach();
        m_dataPtr->emplace(std::forward<Args>(args)...);
    }

    template<typename ... Args>
    iterator emplace_hint(const_iterator hint, Args&& ... args)
    {
        return withDetachedHint(hint, [this, &args ...](const_iterator h) {
            return m_dataPtr->emplace_hint(h, std::forward<Args>(args)...);
        });
    }

    template<typename ... Args>
    std::pair<iterator, bool> try_emplace(const KeyType& key, Args&& ... args)
    {
        ensureDetach();
        return m_dataPtr->try_emplace(key, std::forward<Args>(args)...);
    }

    template<typename ... Args>
    iterator try_emplace(const_iterator hint, const KeyType& key, Args&& ... args)
    {
        return withDetachedHint(hint, [this, &key, &args ...](const_iterator h) {
            return m_dataPtr->try_emplace(h, key, std::forward<Args>(args)...);
        });
    }

    template<typename ... Args>
    iterator try_emplace(const_iterator hint, KeyType&& key, Args&& ... args)
    {
        return withDetachedHint(hint, [this, &key, &args ...](const_iterator h) {
            return m_dataPtr->try_emplace(h, std::forward<KeyType>(key), std::forward<Args>(args)...);
        });
    }

    void clear() noexcept
    {
        ensureDetach();
        m_dataPtr->clear();
    }

    void erase(const KeyType& key)
    {
        ensureDetach();
        m_dataPtr->erase(key);
    }

    iterator erase(const_iterator pos)
    {
        return withDetachedHint(pos, [this](const_iterator p) { return m_dataPtr->erase(p); });
    }

    void erase(const_iterator first, const_iterator last)
    {
        if (m_dataPtr.use_count() == 1) {
            m_dataPtr->erase(first, last);
        } else {
            auto [dfirst, dlast] = detachAndReanchorRange(first, last);
            m_dataPtr->erase(dfirst, dlast);
        }
    }

    bool operator ==(const SharedMap& another) const noexcept
    {
        if (m_dataPtr == another.m_dataPtr) {
            return true;
        }

        return *m_dataPtr == *another.m_dataPtr;
    }

    bool operator !=(const SharedMap& another) const noexcept
    {
        return !this->operator ==(another);
    }

    bool operator <(const SharedMap& another) const noexcept
    {
        if (m_dataPtr == another.m_dataPtr) {
            return false;
        }

        return *m_dataPtr < *another.m_dataPtr;
    }

    bool operator >(const SharedMap& another) const noexcept
    {
        if (m_dataPtr == another.m_dataPtr) {
            return false;
        }

        return *m_dataPtr > *another.m_dataPtr;
    }

private:
    void ensureDetach()
    {
        if (!m_dataPtr) {
            return;
        }

        if (m_dataPtr.use_count() == 1) {
            return;
        }

        m_dataPtr = std::make_shared<Data>(*m_dataPtr);
    }

    // Detach-safe descriptor of a position
    struct AtBegin {};
    struct AtEnd {};
    using Anchor = std::variant<AtBegin, AtEnd, KeyType>;

    Anchor describeAnchor(const_iterator it) const
    {
        if (it == m_dataPtr->cbegin()) {
            return AtBegin {};
        }
        if (it == m_dataPtr->cend()) {
            return AtEnd {};
        }
        return it->first;
    }

    const_iterator resolveAnchor(const Anchor& a) const
    {
        if (std::holds_alternative<AtBegin>(a)) {
            return m_dataPtr->cbegin();
        }
        if (std::holds_alternative<AtEnd>(a)) {
            return m_dataPtr->cend();
        }
        return m_dataPtr->find(std::get<KeyType>(a));
    }

    const_iterator detachAndReanchor(const_iterator hint)
    {
        const Anchor a = describeAnchor(hint);
        ensureDetach();
        return resolveAnchor(a);
    }

    template<typename Op>
    auto withDetachedHint(const_iterator hint, Op&& op)
    {
        if (m_dataPtr.use_count() == 1) {
            return op(hint);
        }

        const_iterator dhint = detachAndReanchor(hint);
        return op(dhint);
    }

    std::pair<const_iterator, const_iterator> detachAndReanchorRange(const_iterator first, const_iterator last)
    {
        const Anchor firstAnchor = describeAnchor(first);
        const Anchor lastAnchor = describeAnchor(last);

        ensureDetach();

        return { resolveAnchor(firstAnchor), resolveAnchor(lastAnchor) };
    }

    DataPtr m_dataPtr = nullptr;
};
}
