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
#ifndef MUSE_DRAW_PIXMAP_H
#define MUSE_DRAW_PIXMAP_H

#include "global/types/bytearray.h"

#include "geometry.h"


namespace muse::draw {
class Pixmap
{
public:

    Pixmap() = default;
    Pixmap(Size size)
        : m_size(size) {}
    Pixmap(const ByteArray& data, Size size)
        : m_size(size) { setData(data); }
    Pixmap(int w, int h)
        : m_size(Size(w, h)) {}

    int width() const { return m_size.width(); }
    int height() const { return m_size.height(); }
    Size size() const { return m_size; }
    ByteArray data() const { return m_data; }

    bool isNull() const { return m_data.empty(); }

    unsigned int key() const { return m_key; }

    bool operator==(const Pixmap& o) const { return m_size == o.m_size && m_key == o.m_key && m_data == o.m_data; }
    bool operator!=(const Pixmap& o) const { return !this->operator==(o); }


private:

    inline unsigned int doKey(const uint8_t* p, size_t len) const
    {
        unsigned int h = 0;
        for (size_t i = 0; i < len; ++i) {
            h = 31 * h + p[i];
        }
        return h;
    }

    void setData(const ByteArray& data)
    {
        m_data = data;
        m_key = doKey(data.constData(), data.size());
    }

    Size m_size;
    ByteArray m_data; //! usually png
    unsigned int m_key = 0;
};
}


#endif // MUSE_DRAW_PIXMAP_H
