#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace vst {

inline constexpr char kFiveItemChestsId[] = "fiveItemChests";
inline constexpr int32_t kFiveItemChestLevel = 3;
inline constexpr int32_t kFiveItemChestCount = 5;

struct FiveItemChestPlan {
    bool eligible = false;
    bool pad = false;
    std::array<int32_t, kFiveItemChestCount> slots{};
};

inline FiveItemChestPlan PlanFiveItemChest(int32_t level, bool arcana,
    std::span<const std::optional<int32_t>> native_slots, int32_t existing_any,
    int32_t arcana_prize, int32_t survarot_prize) noexcept {
    if (level < 1 || level > kFiveItemChestLevel || arcana) return {};
    FiveItemChestPlan plan;
    plan.slots.fill(existing_any);
    std::size_t present = 0;
    for (auto slot : native_slots) {
        if (!slot) continue;
        if (*slot == arcana_prize || *slot == survarot_prize) return {};
        if (present < plan.slots.size()) plan.slots[present] = *slot;
        ++present;
    }
    plan.eligible = present != 0;
    plan.pad = present < plan.slots.size();
    return plan;
}

} // namespace vst
