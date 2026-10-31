// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <span>
#include "iodevice.h"

namespace muse::io {
// A synchronous reader over caller-owned bytes. The span must outlive the
// device and every consumer. Writes are rejected; no mutable alias is exposed.
class ByteView final : public IODevice
{
public:
    explicit ByteView(std::span<const uint8_t> bytes) : m_bytes(bytes) {}

protected:
    bool doOpen(OpenMode mode) override { return mode == ReadOnly; }
    size_t dataSize() const override { return m_bytes.size(); }
    const uint8_t* rawData() const override { return m_bytes.data(); }
    bool resizeData(size_t) override { return false; }
    size_t writeData(const uint8_t*, size_t) override { return 0; }

private:
    std::span<const uint8_t> m_bytes;
};
}
