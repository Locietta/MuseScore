// The compiled-in policy table (engrave/src/api/op_policy.inc) against the
// harness evidence (engrave/tests/harness/op_evidence.json): every operation's
// policy is its verdict, and an operation without evidence is Snapshot.

#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <variant>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engrave/src/api/batch.h"

namespace {

using engrave::api::OpTraits;
using engrave::api::Policy;

template <typename... Ops>
std::map<std::string, Policy> policies(const std::variant<Ops...>*) {
    return {{std::string(OpTraits<Ops>::name), OpTraits<Ops>::policy}...};
}

TEST(OpPolicy, EveryPolicyIsItsEvidence) {
    std::ifstream in(ENGRAVE_TEST_SOURCE_ROOT "/engrave/tests/harness/op_evidence.json", std::ios::binary);
    const auto evidence = nlohmann::json::parse(std::string(std::istreambuf_iterator<char>(in), {}), nullptr, false);
    ASSERT_TRUE(evidence.is_object()) << "op_evidence.json is missing or not JSON";
    const auto& verdicts = evidence["verdicts"];

    const auto table = policies(static_cast<const engrave::Op*>(nullptr));
    for (const auto& [op, policy] : table) {
        const std::string expected = verdicts.contains(op) ? verdicts[op].get<std::string>() : "Snapshot";
        EXPECT_EQ(engrave::api::to_string(policy), expected) << op;
    }
    for (const auto& [op, verdict] : verdicts.items()) EXPECT_TRUE(table.contains(op)) << op << " has evidence but no row";
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
