// Test helpers: SHA-256 and a PNG reader, so a render can be compared with the
// hashes the A2 goldens recorded (scripts/golden.py: sha256 over the RGBA bytes
// PIL decodes, and sha256 over the SVG text).

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace music::test {

std::string sha256_hex(std::span<const std::byte> data);

struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::byte> rgba; // 8-bit RGBA, row-major, no row padding
};

// A PNG cairo writes: 8-bit RGBA, no interlacing.
std::optional<Image> decode_png(std::span<const std::byte> file, std::string& error);

} // namespace music::test
