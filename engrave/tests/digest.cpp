#include "digest.h"

#include <array>
#include <cstdlib>
#include <cstring>

#include <zlib.h>

namespace music::test {
namespace {

constexpr std::uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

std::uint32_t rotr(std::uint32_t value, int count) { return (value >> count) | (value << (32 - count)); }

std::uint32_t big_endian(const std::byte* bytes) {
    return (std::uint32_t(std::to_integer<std::uint8_t>(bytes[0])) << 24)
           | (std::uint32_t(std::to_integer<std::uint8_t>(bytes[1])) << 16)
           | (std::uint32_t(std::to_integer<std::uint8_t>(bytes[2])) << 8)
           | std::uint32_t(std::to_integer<std::uint8_t>(bytes[3]));
}

} // namespace

std::string sha256_hex(std::span<const std::byte> data) {
    std::array<std::uint32_t, 8> state = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    std::vector<std::byte> message(data.begin(), data.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(message.size()) * 8;
    message.push_back(std::byte{ 0x80 });
    while (message.size() % 64 != 56) message.push_back(std::byte{ 0 });
    for (int shift = 56; shift >= 0; shift -= 8) message.push_back(std::byte((bits >> shift) & 0xff));

    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        for (int word = 0; word < 16; ++word) schedule[word] = big_endian(message.data() + offset + word * 4);
        for (int word = 16; word < 64; ++word) {
            const std::uint32_t s0 = rotr(schedule[word - 15], 7) ^ rotr(schedule[word - 15], 18) ^ (schedule[word - 15] >> 3);
            const std::uint32_t s1 = rotr(schedule[word - 2], 17) ^ rotr(schedule[word - 2], 19) ^ (schedule[word - 2] >> 10);
            schedule[word] = schedule[word - 16] + s0 + schedule[word - 7] + s1;
        }
        auto a = state;
        for (int word = 0; word < 64; ++word) {
            const std::uint32_t s1 = rotr(a[4], 6) ^ rotr(a[4], 11) ^ rotr(a[4], 25);
            const std::uint32_t choose = (a[4] & a[5]) ^ (~a[4] & a[6]);
            const std::uint32_t t1 = a[7] + s1 + choose + kRound[word] + schedule[word];
            const std::uint32_t s0 = rotr(a[0], 2) ^ rotr(a[0], 13) ^ rotr(a[0], 22);
            const std::uint32_t majority = (a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]);
            const std::uint32_t t2 = s0 + majority;
            a = { t1 + t2, a[0], a[1], a[2], a[3] + t1, a[4], a[5], a[6] };
        }
        for (int word = 0; word < 8; ++word) state[word] += a[word];
    }

    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const std::uint32_t word : state) {
        for (int shift = 28; shift >= 0; shift -= 4) result.push_back(digits[(word >> shift) & 0xf]);
    }
    return result;
}

std::optional<Image> decode_png(std::span<const std::byte> file, std::string& error) {
    static const std::byte signature[] = { std::byte{ 0x89 }, std::byte{ 'P' }, std::byte{ 'N' }, std::byte{ 'G' },
                                           std::byte{ 0x0d }, std::byte{ 0x0a }, std::byte{ 0x1a }, std::byte{ 0x0a } };
    if (file.size() < 8 || std::memcmp(file.data(), signature, 8) != 0) {
        error = "not a PNG";
        return std::nullopt;
    }

    Image image;
    std::vector<std::byte> compressed;
    std::uint8_t bit_depth = 0, color_type = 0, interlace = 0;
    for (std::size_t offset = 8; offset + 8 <= file.size();) {
        const auto* chunk = file.data() + offset;
        const std::uint32_t length = big_endian(chunk);
        const std::string type(reinterpret_cast<const char*>(chunk + 4), 4);
        if (offset + 12 + length > file.size()) {
            error = "truncated PNG chunk";
            return std::nullopt;
        }
        const std::byte* payload = chunk + 8;
        if (type == "IHDR") {
            if (length != 13) {
                error = "bad IHDR";
                return std::nullopt;
            }
            image.width = big_endian(payload);
            image.height = big_endian(payload + 4);
            bit_depth = std::to_integer<std::uint8_t>(payload[8]);
            color_type = std::to_integer<std::uint8_t>(payload[9]);
            interlace = std::to_integer<std::uint8_t>(payload[12]);
        } else if (type == "IDAT") {
            compressed.insert(compressed.end(), payload, payload + length);
        } else if (type == "IEND") {
            break;
        }
        offset += 12 + length;
    }
    if (image.width == 0 || image.height == 0 || bit_depth != 8 || color_type != 6 || interlace != 0) {
        error = "only 8-bit non-interlaced RGBA PNGs are read";
        return std::nullopt;
    }

    const std::size_t stride = static_cast<std::size_t>(image.width) * 4;
    std::vector<std::byte> plain((stride + 1) * image.height);
    uLongf size = static_cast<uLongf>(plain.size());
    if (uncompress(reinterpret_cast<Bytef*>(plain.data()), &size, reinterpret_cast<const Bytef*>(compressed.data()),
                   static_cast<uLong>(compressed.size())) != Z_OK
        || size != plain.size()) {
        error = "cannot inflate the PNG data";
        return std::nullopt;
    }

    image.rgba.resize(stride * image.height);
    for (std::size_t row = 0; row < image.height; ++row) {
        const std::byte* source = plain.data() + row * (stride + 1);
        const auto filter = std::to_integer<std::uint8_t>(source[0]);
        source += 1;
        std::byte* target = image.rgba.data() + row * stride;
        const std::byte* previous = row > 0 ? target - stride : nullptr;
        for (std::size_t column = 0; column < stride; ++column) {
            const int left = column >= 4 ? std::to_integer<std::uint8_t>(target[column - 4]) : 0;
            const int above = previous ? std::to_integer<std::uint8_t>(previous[column]) : 0;
            const int upper_left = previous && column >= 4 ? std::to_integer<std::uint8_t>(previous[column - 4]) : 0;
            int predicted = 0;
            switch (filter) {
            case 0: break;
            case 1: predicted = left; break;
            case 2: predicted = above; break;
            case 3: predicted = (left + above) / 2; break;
            case 4: {
                const int estimate = left + above - upper_left;
                const int to_left = std::abs(estimate - left);
                const int to_above = std::abs(estimate - above);
                const int to_upper_left = std::abs(estimate - upper_left);
                predicted = to_left <= to_above && to_left <= to_upper_left ? left
                            : to_above <= to_upper_left ? above
                                                        : upper_left;
                break;
            }
            default:
                error = "unknown PNG row filter";
                return std::nullopt;
            }
            target[column] = std::byte((std::to_integer<int>(source[column]) + predicted) & 0xff);
        }
    }
    return image;
}

} // namespace music::test
