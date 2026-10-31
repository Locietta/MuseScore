/*
MIT License

Copyright (c) 2020 Igor Korsukov

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#pragma once
#include <cassert>
#include <string_view>
#include "logstream.h"
namespace muse::diagnostics {
bool assertionDiagnosticsEnabled();
void assertionFailure(std::string_view, const char*, int);
}
// Formatting remains lazy even when a failed condition is checked in release.
#define MUSE_ASSERT_DIAGNOSTIC_IMPL(msg, stream_name) \
    do { \
        if (muse::diagnostics::assertionDiagnosticsEnabled()) { \
            muse::logger::Stream stream_name; \
            stream_name << msg; \
            muse::diagnostics::assertionFailure(stream_name.str(), __FILE__, __LINE__); \
        } \
    } while (false)

#define MUSE_ASSERT_DIAGNOSTIC(msg) MUSE_ASSERT_DIAGNOSTIC_IMPL(msg, UNIQUE_VAR_NAME(muse_assert_message_))

//! Useful macros
#define DO_ASSERT_X_IMPL(cond, msg, var_name) \
    { \
        const bool var_name = static_cast<bool>(cond); \
        if (!(var_name)) { \
            MUSE_ASSERT_DIAGNOSTIC(msg); \
            assert(var_name && #cond); \
        } \
    }
#define DO_ASSERT_X(cond, msg) DO_ASSERT_X_IMPL(cond, msg, UNIQUE_VAR_NAME(__do_assert_))

#define DO_ASSERT(cond) DO_ASSERT_X(cond, #cond)
#define ASSERT_X(msg) DO_ASSERT_X(false, msg)

#define CONCAT_IMPL(x, y) x##y
#define CONCAT(x, y) CONCAT_IMPL(x, y)
#define UNIQUE_VAR_NAME(prefix) CONCAT(prefix, __LINE__)

#define IF_ASSERT_FAILED_X_IMPL(cond, msg, var_name) \
    const bool var_name = static_cast<bool>(cond); \
    if (!(var_name)) { \
        MUSE_ASSERT_DIAGNOSTIC(msg); \
        assert(var_name && #cond); \
    } \
    if (!(var_name))

#define IF_ASSERT_FAILED_X(cond, msg) IF_ASSERT_FAILED_X_IMPL(cond, msg, UNIQUE_VAR_NAME(__if_assert_failed_))
#define IF_ASSERT_FAILED(cond) IF_ASSERT_FAILED_X(cond, #cond)

#define IF_FAILED_IMPL(cond, var_name) \
    const bool var_name = static_cast<bool>(cond); \
    if (!(var_name)) { \
        muse::diagnostics::assertionFailure(#cond, __FILE__, __LINE__); \
    } \
    if (!(var_name))

#define IF_FAILED(cond) IF_FAILED_IMPL(cond, UNIQUE_VAR_NAME(__if_failed_))


#define UNUSED(x) (void)x;
