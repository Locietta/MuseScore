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
#ifndef MUSE_GLOBAL_TRANSLATABLESTRING_H
#define MUSE_GLOBAL_TRANSLATABLESTRING_H

#include "translation.h"

#include "types/string.h"


namespace muse {
//! Note: in order to make the string visible for `lupdate`,
//! you must write TranslatableString(...) explicitly.
class TranslatableString
{
public:
    const char* context = nullptr;
    String str;
    const char* disambiguation = nullptr;
    int defaultN = -1;

    TranslatableString() = default;
    TranslatableString(const char* context, const char* str, const char* disambiguation = nullptr, int n = -1)
        : context(context), str(String::fromUtf8(str)), disambiguation(disambiguation), defaultN(n) {}
    TranslatableString(const char* context, const String& str, const char* disambiguation = nullptr, int n = -1)
        : context(context), str(str), disambiguation(disambiguation), defaultN(n) {}

    static TranslatableString untranslatable(const char* str)
    {
        return TranslatableString(nullptr, str);
    }

    static TranslatableString untranslatable(const String& str)
    {
        return TranslatableString(nullptr, str);
    }

    inline bool isEmpty() const
    {
        return str.empty();
    }

    inline bool isTranslatable() const
    {
        return context && context[0];
    }

    inline String translated() const
    {
        return translated(defaultN);
    }

    inline String translated(int n) const
    {
        if (isEmpty()) {
            return {};
        }

        String res = isTranslatable() ? mtrc(context, str, disambiguation, n) : str;

        for (const auto& arg : args) {
            arg->apply(res);
        }

        return res;
    }


    template<typename ... Args>
    inline TranslatableString arg(const Args& ...) const;

    bool operator ==(const TranslatableString& other) const
    {
        return (context == other.context || (context && other.context && strcmp(context, other.context) == 0))
               && str == other.str
               && (disambiguation == other.disambiguation
                   || (disambiguation && other.disambiguation && strcmp(disambiguation, other.disambiguation) == 0))
               && defaultN == other.defaultN
               && args == other.args;
    }

    bool operator !=(const TranslatableString& other) const
    {
        return !operator==(other);
    }

private:
    struct IArg {
        virtual ~IArg() = default;

        virtual void apply(String& res) const = 0;
    };

    template<typename ... Args>
    struct Arg;

    std::vector<std::shared_ptr<const IArg> > args;
};

template<typename ... Args>
struct TranslatableString::Arg : public TranslatableString::IArg
{
    std::tuple<Args...> args;

    Arg(const Args&... args)
        : args(args ...) {}

    void apply(String& res) const override
    {
        res = std::apply([&](const Args&... args) { return res.arg(makeArg(args)...); }, args);
    }

    template<typename T>
    static inline auto makeArg(const T& t)
    {
        if constexpr (std::is_same_v<T, TranslatableString>) {
            return t.translated();
        } else {
            return t;
        }
    }

};

template<typename Int>
struct TranslatableString::Arg<Int> : public TranslatableString::IArg
{
    const Int arg;

    Arg(Int arg) : arg(arg) {
    }

    void apply(String& res) const override
    {
        res = res.arg(arg);
    }

};

template<>
struct TranslatableString::Arg<String> : public TranslatableString::IArg
{
    const String arg;

    template<typename ... T>
    Arg(const T& ... arg) : arg(arg ...) {
    }

    void apply(String& res) const override
    {
        res = res.arg(arg);
    }

};

template<>
struct TranslatableString::Arg<TranslatableString> : public TranslatableString::IArg
{
    const TranslatableString arg;

    Arg(const TranslatableString& arg) : arg(arg) {
    }

    void apply(String& res) const override
    {
        res = res.arg(arg.translated());
    }

};

template<typename ... Args>
TranslatableString TranslatableString::arg(const Args& ... args) const
{
    TranslatableString res = *this;
    res.args.push_back(std::make_shared<Arg<Args...> >(args ...));
    return res;
}
}

#endif // MUSE_GLOBAL_TRANSLATABLESTRING_H
