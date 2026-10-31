// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>
#include "iodevice.h"
#include "ioretcodes.h"

namespace muse::io {
class StreamingFileDevice final : public IODevice
{
public:
    explicit StreamingFileDevice(std::filesystem::path path)
        : m_path(std::move(path)) {}

    ~StreamingFileDevice() override { close(); }

protected:
    bool doOpen(OpenMode mode) override
    {
        if (mode != WriteOnly) {
            setError(static_cast<int>(io::Err::FSWriteError), "audio output is write-only");
            return false;
        }

        // The path is UTF-8; std::filesystem::path keeps it away from the
        // process ANSI code page on Windows.
        m_stream.open(m_path, std::ios::binary | std::ios::trunc);
        if (!m_stream.is_open()) {
            setError(static_cast<int>(io::Err::FSWriteError), "cannot open audio output for writing");
            return false;
        }

        m_size = 0;
        m_streamPos = 0;
        return true;
    }

    void doClose() override
    {
        if (m_stream.is_open()) {
            m_stream.flush();
            const bool flushed = m_stream.good();
            m_stream.close();
            if (!flushed || m_stream.fail()) {
                setError(static_cast<int>(io::Err::FSWriteError), "cannot finalize audio output");
            }
        }
    }

    size_t dataSize() const override { return m_size; }
    const uint8_t* rawData() const override { return nullptr; }

    bool resizeData(size_t size) override
    {
        if (size > static_cast<size_t>(std::numeric_limits<std::streamoff>::max())) return false;
        m_size = std::max(m_size, size);
        return true;
    }

    size_t writeData(const uint8_t* data, size_t len) override
    {
        if (!m_stream.is_open() || len > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
            setError(static_cast<int>(io::Err::FSWriteError), "invalid stream write");
            return 0;
        }

        // Only seek for the non-sequential writes (the WAV header patch);
        // std::ofstream::seekp flushes the put buffer, so doing it per write
        // would turn a buffered stream into one syscall per sample.
        if (m_streamPos != pos()) {
            m_stream.seekp(static_cast<std::streamoff>(pos()));
            if (!m_stream) {
                setError(static_cast<int>(io::Err::FSWriteError), "cannot seek output");
                return 0;
            }
        }

        m_stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
        if (!m_stream) {
            setError(static_cast<int>(io::Err::FSWriteError), "cannot write output");
            return 0;
        }

        m_streamPos = pos() + len;
        return len;
    }

private:
    std::filesystem::path m_path;
    std::ofstream m_stream;
    size_t m_size = 0;
    size_t m_streamPos = 0;
};
}
