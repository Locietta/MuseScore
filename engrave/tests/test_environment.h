#pragma once

#include <string>

namespace music::test {

struct TestEnvironmentConfig {
    std::string assets_dir;
    std::string engraving_data_dir;
};

// Discover and initialize the Qt-free test environment from command-line arguments,
// environment variables, compile-time definitions, or filesystem probes.
//
// Recognizes and removes test runner arguments from argc/argv so GoogleTest
// does not fail on unrecognized flags:
//   --assets=<dir>
//   --engraving-data=<dir>
//   --data-root=<dir>
// Also unquotes any --gtest_filter="..." arguments.
void setup_test_environment(int* argc, char** argv);

// Explicit configuration entry point.
void setup_test_environment(const TestEnvironmentConfig& config);

// Cleans up registered test exports (e.g. IEngravingConfiguration mock).
void teardown_test_environment();

// Path discovery helpers:
std::string find_assets_dir(const std::string& hint = "");
std::string find_engraving_data_dir(const std::string& hint = "");

} // namespace music::test
