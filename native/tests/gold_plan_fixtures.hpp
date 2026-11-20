#pragma once
#include "../backend/gold_plan.hpp"
#include <cstdio>

namespace vst_gold_plan_tests {
#define VST_GOLD_CHECK(...) do { if (!(__VA_ARGS__)) { std::fprintf(stderr, "gold plan fixture line %d: %s\n", __LINE__, #__VA_ARGS__); return false; } } while (0)
inline bool ExecutesExactly(double current, double desired, size_t expected_steps = 3) {
    vst::GoldPlan plan;
    VST_GOLD_CHECK(vst::PlanGoldChange(current, desired, plan) && plan.count <= 2);
    if (expected_steps <= 2) VST_GOLD_CHECK(plan.count == expected_steps);
    float actual = static_cast<float>(current);
    for (size_t index = 0; index < plan.count; ++index) {
        const auto& step = plan.steps[index];
        VST_GOLD_CHECK(std::isfinite(step.amount) && step.amount > 0);
        actual = step.operation == vst::GoldOperation::Add ? actual + step.amount : actual - step.amount;
        VST_GOLD_CHECK(std::isfinite(actual) && actual >= 0 && actual == step.expected);
    }
    VST_GOLD_CHECK(static_cast<double>(actual) == desired);
    return true;
}
inline bool LargeBalancesReachSmallRepresentableTargets() {
    VST_GOLD_CHECK(ExecutesExactly(950000000, 10000, 2));
    VST_GOLD_CHECK(ExecutesExactly(950000000, 12000, 2));
    VST_GOLD_CHECK(ExecutesExactly(10000, 950000000, 2));
    VST_GOLD_CHECK(ExecutesExactly(10000, 12000, 1));
    VST_GOLD_CHECK(ExecutesExactly(12000, 10000, 1));
    VST_GOLD_CHECK(ExecutesExactly(10000, 0, 1));
    VST_GOLD_CHECK(ExecutesExactly(0, 10000, 1));
    VST_GOLD_CHECK(ExecutesExactly(10000, 10000, 0));
    return true;
}
inline bool BoundedNativePlansRetainPrecisionAndRejectInvalidValues() {
    vst::GoldPlan plan;
    for (double invalid : {-1., 16777217., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        VST_GOLD_CHECK(!vst::PlanGoldChange(invalid, 0, plan));
        VST_GOLD_CHECK(!vst::PlanGoldChange(0, invalid, plan));
    }
    uint32_t random = 0x13579BDF;
    for (unsigned index = 0; index < 1024; ++index) {
        random = random * 1664525u + 1013904223u;
        double current = static_cast<float>(random % 900000000u);
        random = random * 1664525u + 1013904223u;
        double desired = static_cast<float>(random % 900000000u);
        VST_GOLD_CHECK(ExecutesExactly(current, desired));
    }
    return true;
}
#undef VST_GOLD_CHECK
}
