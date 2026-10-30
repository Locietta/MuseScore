#include "test_environment.h"

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <iostream>

GTEST_API_ int main(int argc, char** argv)
{
    try {
        music::test::setup_test_environment(&argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "[test_environment] Initialization failed: " << e.what() << '\n';
        return 1;
    }

    testing::InitGoogleMock(&argc, argv);
    GTEST_FLAG_SET(death_test_style, "threadsafe");

    int code = RUN_ALL_TESTS();

    music::test::teardown_test_environment();

    return code;
}
