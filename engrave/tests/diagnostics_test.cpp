// SPDX-License-Identifier: GPL-3.0-only
#include <gtest/gtest.h>
#include "log.h"
#include "logger.h"
#include "types/string.h"

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST(Diagnostics, DisabledMessagesDoNotEvaluateOperands)
{
    auto* logger = muse::logger::Logger::instance();
    logger->setLevel(muse::logger::Level::Off);
    int calls = 0;
    LOGE() << ++calls;
    LOGD() << ++calls;
    LOGN() << ++calls;
    logger->setLevel(muse::logger::Level::Normal);
    EXPECT_EQ(calls, 0);
}

TEST(Diagnostics, UsesStderrAndPreservesUnicode)
{
    testing::internal::CaptureStderr();
    LOGE("%s", "UTF-8: ") << muse::String(u"\u97f3\u697d");
    EXPECT_EQ(testing::internal::GetCapturedStderr(), "ERROR: UTF-8: \xe9\x9f\xb3\xe6\xa5\xbd\n");
}

TEST(Diagnostics, AssertionsEvaluateOnceAndRetainReleaseRecovery)
{
    int calls = 0;
    DO_ASSERT(++calls == 1);
    EXPECT_EQ(calls, 1);
#ifdef NDEBUG
    testing::internal::CaptureStderr();
    IF_ASSERT_FAILED_X(++calls == 99, "chain " << calls) { ++calls; }
    const auto message = testing::internal::GetCapturedStderr();
    EXPECT_EQ(calls, 3);
    EXPECT_NE(message.find("chain 2"), std::string::npos);
#endif
}

TEST(Diagnostics, DisabledScopedTimerDoesNotEvaluateLabel)
{
#ifndef ENGRAVE_PROFILE
    int calls = 0;
    TRACEFUNC_C((++calls, "scope"))
    EXPECT_EQ(calls, 0);
#endif
}
