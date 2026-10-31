// SPDX-License-Identifier: GPL-3.0-only
#include <gtest/gtest.h>
#include <array>
#include "io/byteview.h"

TEST(ByteView, BorrowsBytesAndPreservesSeekBounds)
{
    const std::array<uint8_t, 5> data{0, 0xff, 3, 4, 5};
    muse::io::ByteView view(data);
    ASSERT_TRUE(view.open());
    EXPECT_EQ(view.readData(), data.data());
    EXPECT_EQ(view.size(), data.size());
    ASSERT_TRUE(view.seek(1));
    uint8_t read[8]{};
    EXPECT_EQ(view.read(read, 8), 4u);
    EXPECT_EQ(read[0], 0xff);
    EXPECT_FALSE(view.seek(6));
    EXPECT_EQ(view.pos(), 5u);
    EXPECT_TRUE(view.seek(0));
    EXPECT_EQ(view.readAll().size(), data.size());
    view.close();
    EXPECT_FALSE(view.open(muse::io::IODevice::WriteOnly));
    EXPECT_FALSE(view.open(muse::io::IODevice::ReadWrite));
    ASSERT_TRUE(view.open());
    EXPECT_EQ(view.pos(), 0u);
}

TEST(ByteView, EmptyInput)
{
    muse::io::ByteView view(std::span<const uint8_t>{});
    ASSERT_TRUE(view.open());
    EXPECT_TRUE(view.seek(0));
    EXPECT_FALSE(view.seek(1));
    EXPECT_TRUE(view.readAll().empty());
}

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#include "io/streamfile.h"
#include <chrono>

TEST(StreamingFile, UnicodePathAndHeaderSeek)
{
    const auto path = std::filesystem::temp_directory_path()
        / (std::filesystem::path(u"engrave-\u97f3\U0001D11E-\U0001F600-")
           += std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code ec; std::filesystem::remove(p, ec); } } cleanup{path};
    muse::io::StreamingFileDevice out(path);
    ASSERT_TRUE(out.open(muse::io::IODevice::WriteOnly));
    const uint8_t data[]{1, 2, 3, 4};
    ASSERT_EQ(out.write(data, 4), 4u);
    ASSERT_TRUE(out.seek(0));
    ASSERT_EQ(out.write(data + 3, 1), 1u);
    out.close();
    ASSERT_FALSE(out.hasError()) << out.errorString();
    std::ifstream input(path, std::ios::binary);
    const std::string result{std::istreambuf_iterator<char>(input), {}};
    EXPECT_EQ(result, std::string("\x04\x02\x03\x04", 4));
}

#ifndef _WIN32
TEST(StreamingFile, FinalizationFailurePropagates)
{
    muse::io::StreamingFileDevice out("/dev/full");
    ASSERT_TRUE(out.open(muse::io::IODevice::WriteOnly));
    const uint8_t value = 1;
    out.write(&value, 1);
    out.close();
    EXPECT_TRUE(out.hasError());
}
#endif
