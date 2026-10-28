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
#pragma once

#include <cstddef>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace muse::functional {
template<typename Signature, size_t Capacity = 32, size_t Alignment = alignof(std::max_align_t)>
class MoveOnlyInplaceFunction;

namespace detail {
template<typename>
struct IsMoveOnlyInplaceFunction : std::false_type {};

template<typename Signature, size_t Capacity, size_t Alignment>
struct IsMoveOnlyInplaceFunction<MoveOnlyInplaceFunction<Signature, Capacity, Alignment> > : std::true_type {};

template<typename Ret, typename ... Args>
struct MoveOnlyInplaceFunctionVTable
{
    using Invoke = Ret (*)(void*, Args&&...);
    using Relocate = void (*)(void*, void*) noexcept;
    using Destroy = void (*)(void*) noexcept;

    Invoke invoke = nullptr;
    Relocate relocate = nullptr;
    Destroy destroy = nullptr;
};

template<typename Callable, typename Ret, typename ... Args>
Ret invokeMoveOnlyInplaceFunction(void* storage, Args&&... args)
{
    if constexpr (std::is_void_v<Ret>) {
        (*static_cast<Callable*>(storage))(std::forward<Args>(args)...);
    } else {
        return (*static_cast<Callable*>(storage))(std::forward<Args>(args)...);
    }
}

template<typename Callable>
void relocateMoveOnlyInplaceFunction(void* destination, void* source) noexcept
{
    auto* callable = static_cast<Callable*>(source);
    new (destination) Callable(std::move(*callable));
    callable->~Callable();
}

template<typename Callable>
void destroyMoveOnlyInplaceFunction(void* storage) noexcept
{
    static_cast<Callable*>(storage)->~Callable();
}

template<typename Callable, typename Ret, typename ... Args>
const MoveOnlyInplaceFunctionVTable<Ret, Args...>* moveOnlyInplaceFunctionVTable() noexcept
{
    static const MoveOnlyInplaceFunctionVTable<Ret, Args...> vtable {
        &invokeMoveOnlyInplaceFunction<Callable, Ret, Args...>,
        &relocateMoveOnlyInplaceFunction<Callable>,
        &destroyMoveOnlyInplaceFunction<Callable>
    };

    return &vtable;
}
} // namespace detail

template<typename Ret, typename ... Args, size_t Capacity, size_t Alignment>
class MoveOnlyInplaceFunction<Ret(Args...), Capacity, Alignment>
{
public:
    MoveOnlyInplaceFunction() noexcept = default;

    MoveOnlyInplaceFunction(std::nullptr_t) noexcept {}

    MoveOnlyInplaceFunction(const MoveOnlyInplaceFunction&) = delete;
    MoveOnlyInplaceFunction& operator=(const MoveOnlyInplaceFunction&) = delete;

    MoveOnlyInplaceFunction(MoveOnlyInplaceFunction&& other) noexcept
    {
        moveFrom(std::move(other));
    }

    MoveOnlyInplaceFunction& operator=(MoveOnlyInplaceFunction&& other) noexcept
    {
        if (this != &other) {
            reset();
            moveFrom(std::move(other));
        }

        return *this;
    }

    template<typename Callable,
             typename Fn = std::decay_t<Callable>,
             std::enable_if_t<!detail::IsMoveOnlyInplaceFunction<Fn>::value
                              && std::is_invocable_r_v<Ret, Fn&, Args...>, int> = 0>
    MoveOnlyInplaceFunction(Callable&& callable)
    {
        static_assert(sizeof(Fn) <= Capacity, "Callable is too large for this MoveOnlyInplaceFunction");
        static_assert(alignof(Fn) <= Alignment, "Callable alignment is too large for this MoveOnlyInplaceFunction");
        static_assert(std::is_nothrow_move_constructible_v<Fn>,
                      "MoveOnlyInplaceFunction requires nothrow move-constructible callables");

        m_vtable = detail::moveOnlyInplaceFunctionVTable<Fn, Ret, Args...>();
        new (storage()) Fn(std::forward<Callable>(callable));
    }

    template<typename Callable,
             typename Fn = std::decay_t<Callable>,
             std::enable_if_t<!detail::IsMoveOnlyInplaceFunction<Fn>::value
                              && std::is_invocable_r_v<Ret, Fn&, Args...>, int> = 0>
    MoveOnlyInplaceFunction& operator=(Callable&& callable)
    {
        *this = MoveOnlyInplaceFunction(std::forward<Callable>(callable));
        return *this;
    }

    MoveOnlyInplaceFunction& operator=(std::nullptr_t) noexcept
    {
        reset();
        return *this;
    }

    ~MoveOnlyInplaceFunction() noexcept
    {
        reset();
    }

    Ret operator()(Args... args) const
    {
        if (!m_vtable) {
            throw std::bad_function_call();
        }

        if constexpr (std::is_void_v<Ret>) {
            m_vtable->invoke(storage(), std::forward<Args>(args)...);
        } else {
            return m_vtable->invoke(storage(), std::forward<Args>(args)...);
        }
    }

    explicit operator bool() const noexcept
    {
        return m_vtable != nullptr;
    }

    void reset() noexcept
    {
        if (!m_vtable) {
            return;
        }

        m_vtable->destroy(storage());
        m_vtable = nullptr;
    }

private:
    using VTable = detail::MoveOnlyInplaceFunctionVTable<Ret, Args...>;
    using Storage = std::aligned_storage_t<Capacity, Alignment>;

    void* storage() const noexcept
    {
        return const_cast<Storage*>(&m_storage);
    }

    void moveFrom(MoveOnlyInplaceFunction&& other) noexcept
    {
        if (!other.m_vtable) {
            return;
        }

        m_vtable = other.m_vtable;
        m_vtable->relocate(storage(), other.storage());
        other.m_vtable = nullptr;
    }

    const VTable* m_vtable = nullptr;
    mutable Storage m_storage {};
};

template<typename Signature, size_t Capacity, size_t Alignment>
bool operator==(const MoveOnlyInplaceFunction<Signature, Capacity, Alignment>& function, std::nullptr_t) noexcept
{
    return !function;
}

template<typename Signature, size_t Capacity, size_t Alignment>
bool operator!=(const MoveOnlyInplaceFunction<Signature, Capacity, Alignment>& function, std::nullptr_t) noexcept
{
    return bool(function);
}
} // namespace muse::functional
