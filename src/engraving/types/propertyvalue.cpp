/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
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
#include "propertyvalue.h"

#include "realfn.h"

#include "log.h"

using namespace mu::engraving;

bool PropertyValue::isValid() const
{
    return m_type != P_TYPE::UNDEFINED;
}

P_TYPE PropertyValue::type() const
{
    return m_type;
}

bool PropertyValue::operator ==(const PropertyValue& v) const
{
    if (v.m_type == P_TYPE::UNDEFINED || m_type == P_TYPE::UNDEFINED) {
        return v.m_type == m_type;
    }

    //! HACK Temporary hack for bool comparisons (maybe one type is bool and another type is int)
    if (v.m_type == P_TYPE::BOOL || m_type == P_TYPE::BOOL) {
        return v.value<bool>() == value<bool>();
    }

    //! HACK Temporary hack for int comparisons (maybe one type is int and another type is enum)
    if (v.m_type == P_TYPE::INT || m_type == P_TYPE::INT) {
        return v.value<int>() == value<int>();
    }

    //! HACK Temporary hack for Spatium comparisons (maybe one type is Spatium and another type is real)
    if (v.m_type == P_TYPE::SPATIUM || m_type == P_TYPE::SPATIUM) {
        return muse::RealIsEqual(v.value<double>(), value<double>());
    }

    //! HACK Temporary hack for Fraction comparisons
    if (v.m_type == P_TYPE::FRACTION) {
        assert(m_type == P_TYPE::FRACTION);
        return v.value<Fraction>().identical(value<Fraction>());
    }

    if (v.m_type == P_TYPE::REAL) {
        assert(m_type == P_TYPE::REAL);
        return muse::RealIsEqual(v.value<double>(), value<double>());
    }

    assert(m_data);
    if (!m_data) {
        return false;
    }

    assert(v.m_data);
    if (!v.m_data) {
        return false;
    }

    return v.m_type == m_type && v.m_data->equal(m_data.get());
}
