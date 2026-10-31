// The compiled-in policy table (engrave/src/api/op_policy.inc) against the
// harness evidence (engrave/tests/harness/op_evidence.json): every operation's
// policy is its verdict, an operation without evidence is Snapshot, and a held-back
// operation is Snapshot whatever its evidence.

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

    // Operations kept on Snapshot although their evidence proves Transaction, because
    // a known input the evidence cannot afford to measure breaks the unwind.
    const std::map<std::string, std::string> held_back = {
        {"SetKeySig", "unwinding a key signature added to layout_elements.mscx or pedal.mscx "
                      "runs layout for minutes, then bad_alloc (notes/a4/s7b/REPORT.md)"},
    };

    const auto table = policies(static_cast<const engrave::Op*>(nullptr));
    for (const auto& [op, policy] : table) {
        std::string expected = verdicts.contains(op) ? verdicts[op].get<std::string>() : "Snapshot";
        if (held_back.contains(op) && expected == "Transaction") expected = "Snapshot";
        EXPECT_EQ(engrave::api::to_string(policy), expected) << op;
    }
    for (const auto& [op, verdict] : verdicts.items()) EXPECT_TRUE(table.contains(op)) << op << " has evidence but no row";
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
