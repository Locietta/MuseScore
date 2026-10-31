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
#include "logger.h"
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
namespace kors::logger {
namespace {
    std::atomic<Level> threshold { Level::Normal };
    std::mutex sinkMutex;
}
bool enabled(Level value) { return value != Level::Off && value <= threshold.load(std::memory_order_relaxed); }
Logger* Logger::instance()
{
    static Logger logger;
    return &logger;
}
Logger::~Logger() { clearDests(); }
void Logger::setLevel(Level value) { threshold.store(value, std::memory_order_relaxed); }
Level Logger::level() const { return threshold.load(std::memory_order_relaxed); }
bool Logger::isLevel(Level value) const { return enabled(value); }
void Logger::addDest(LogDest* dest)
{
    std::lock_guard lock(sinkMutex);
    m_dests.push_back(dest);
}
void Logger::removeDest(LogDest* dest)
{
    std::lock_guard lock(sinkMutex);
    std::erase(m_dests, dest);
}
void Logger::clearDests()
{
    std::lock_guard lock(sinkMutex);
    for (auto* dest : m_dests)
        delete dest;
    m_dests.clear();
}
void Logger::write(const LogMsg& msg)
{
    std::lock_guard lock(sinkMutex);
    if (m_dests.empty()) {
        std::fprintf(stderr, "%.*s: %s\n", int(msg.type.size()), msg.type.data(), msg.message.c_str());
    } else
        for (auto* dest : m_dests)
            dest->write(msg);
}
std::string LogLayout::output(const LogMsg& msg) const
{
    std::string result = m_format;
    auto replace = [&](std::string_view key, std::string_view value) {
        size_t pos = 0;
        while ((pos = result.find(key, pos)) != std::string::npos) {
            result.replace(pos, key.size(), value);
            pos += value.size();
        }
    };
    replace("${type}", msg.type);
    replace("${tag}", msg.tag);
    replace("${message}", msg.message);
    return result;
}
LogInput::LogInput(std::string_view type, std::string_view tag, Color)
    : m_type(type)
    , m_tag(tag)
{
}
LogInput::~LogInput() { Logger::instance()->write({ m_type, m_tag, m_stream.str() }); }
Stream& LogInput::stream(const char* format, ...)
{
    char buffer[2048] {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return m_stream << buffer;
}
}

#include "logstream.h"
#include <sstream>
namespace kors::logger {
struct Stream::Impl {
    std::ostringstream stream;
};
Stream::Stream()
    : m_impl(new Impl)
{
}
Stream::~Stream() { delete m_impl; }
std::string Stream::str() const { return m_impl->stream.str(); }
Stream& Stream::operator<<(bool value)
{
    m_impl->stream << value;
    return *this;
}
Stream& Stream::operator<<(char value)
{
    m_impl->stream << value;
    return *this;
}
Stream& Stream::operator<<(short value)
{
    m_impl->stream << value;
    return *this;
}
Stream& Stream::operator<<(unsigned short value)
{
    m_impl->stream << value;
    return *this;
}
Stream& Stream::operator<<(char16_t value)
{
    m_impl->stream << static_cast<unsigned int>(value);
    return *this;
}
Stream& Stream::operator<<(char32_t value)
{
    m_impl->stream << static_cast<unsigned int>(value);
    return *this;
}
Stream& Stream::operator<<(int value)
{
    m_impl->stream << value;
    return *this;
}
Stream& Stream::operator<<(unsigned int value)
{
    m_impl->stream << value;
    return *this;
}
Stream& Stream::operator<<(long value)
{
    m_impl->stream << value;
    return *this;
}
Stream& Stream::operator<<(unsigned long value)
{
    m_impl->stream << value;
    return *this;
}
Stream& Stream::operator<<(long long value)
{
    m_impl->stream << value;
    return *this;
}
Stream& Stream::operator<<(unsigned long long value)
{
    m_impl->stream << value;
    return *this;
}
Stream& Stream::operator<<(float value)
{
    m_impl->stream << value;
    return *this;
}
Stream& Stream::operator<<(double value)
{
    m_impl->stream << value;
    return *this;
}
Stream& Stream::operator<<(const void* value)
{
    m_impl->stream << value;
    return *this;
}
Stream& Stream::operator<<(const char* value)
{
    m_impl->stream << value;
    return *this;
}
Stream& Stream::operator<<(const std::string& value)
{
    m_impl->stream << value;
    return *this;
}
Stream& Stream::operator<<(const std::string_view& value)
{
    m_impl->stream << value;
    return *this;
}
}

// SPDX-License-Identifier: GPL-3.0-only
#include "asserts.h"
#include <cstdio>
namespace muse::diagnostics {
bool assertionDiagnosticsEnabled()
{
    return kors::logger::enabled(kors::logger::Level::Normal);
}
void assertionFailure(std::string_view msg, const char* file, int line)
{
    if (!assertionDiagnosticsEnabled()) {
        return;
    }
    std::fprintf(stderr, "ASSERT FAILED: %.*s    %s:%d\n", int(msg.size()), msg.data(), file, line);
}
}

// SPDX-License-Identifier: GPL-3.0-only
#include "profiler.h"
#ifdef ENGRAVE_PROFILE
#include <chrono>
#include <cstdio>
namespace muse::profiler {
static long long now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
Scope::Scope(const char* name)
    : m_name(name)
    , m_start(now())
{
}
Scope::~Scope() { std::fprintf(stderr, "TIMER: %s %lld ns\n", m_name, now() - m_start); }
}
#endif
