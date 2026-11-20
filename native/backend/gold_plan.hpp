#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace vst {

enum class GoldOperation : uint8_t { Add, Remove };
struct GoldStep {
    GoldOperation operation = GoldOperation::Add;
    float amount = 0;
    float expected = 0;
};
struct GoldPlan {
    std::array<GoldStep, 2> steps{};
    size_t count = 0;
};

// Plan at most two original game calls. There is no save between steps.
// Both balances must already be exactly representable by the native wallet.
inline bool PlanGoldChange(double current, double desired, GoldPlan& plan) noexcept {
    plan = {};
    constexpr double limit = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(current) || !std::isfinite(desired) || current < 0 || desired < 0
        || current > limit || desired > limit) return false;
    const float native_current = static_cast<float>(current), native_desired = static_cast<float>(desired);
    if (static_cast<double>(native_current) != current || static_cast<double>(native_desired) != desired) return false;
    if (current == desired) return true;
    const bool adding = desired > current;
    const double distance = std::fabs(desired - current);
    const float amount = static_cast<float>(distance);
    const float projected = adding ? native_current + amount : native_current - amount;
    if (static_cast<double>(amount) == distance && projected == native_desired) {
        plan.steps[0] = {adding ? GoldOperation::Add : GoldOperation::Remove, amount, native_desired};
        plan.count = 1;
        return true;
    }
    // A representable target can still have an unrepresentable difference.
    // Isolate that rounding by clearing the current float exactly, then add
    // the target float. The backend verifies zero before attempting the add.
    if (native_current > 0) plan.steps[plan.count++] = {GoldOperation::Remove, native_current, 0};
    if (native_desired > 0) plan.steps[plan.count++] = {GoldOperation::Add, native_desired, native_desired};
    return true;
}

} // namespace vst
