// SPDX-License-Identifier: GPL-3.0-only
#include "global/io/buffer.h"
#include "global/serialization/zipreader.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>

int main(int argc, char** argv)
{
    try {
        if (argc != 4) throw std::invalid_argument("Usage: archive_test ARCHIVE MEMBER OUTPUT");
        std::ifstream input(std::filesystem::u8path(argv[1]), std::ios::binary);
        if (!input) throw std::runtime_error("Cannot open test archive");
        const std::string raw{std::istreambuf_iterator<char>(input), {}};
        muse::ByteArray bytes(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
        muse::io::Buffer buffer(&bytes);
        if (!buffer.open(muse::io::IODevice::ReadOnly)) throw std::runtime_error("Cannot open buffer");
        muse::ZipReader reader(&buffer);
        const bool exists = reader.fileExists(argv[2]);
        const auto extracted = reader.fileData(argv[2]);
        const bool error = reader.hasError();
        std::ofstream output(std::filesystem::u8path(argv[3]), std::ios::binary | std::ios::trunc);
        output.write(extracted.constChar(), static_cast<std::streamsize>(extracted.size()));
        output.close();
        if (!output) throw std::runtime_error("Cannot write extracted test result");
        std::cout << nlohmann::json{{"exists",exists},{"error",error},{"size",extracted.size()},{"buffer_size",buffer.size()}}.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
