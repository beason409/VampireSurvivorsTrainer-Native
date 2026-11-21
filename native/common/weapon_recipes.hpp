#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace vst {

inline constexpr std::size_t kRecipeMaterialLimit = 32;

enum class RecipeRequirement : uint8_t { None, Special, AnyMaterial, FiveMaxPassives, SixMaxEvolutionsAndPassives };

struct WeaponRecipeMaterial {
    std::string id;
    bool maximum = false;
};

inline void AddRecipeMaterial(std::vector<WeaponRecipeMaterial>& recipe, std::string id, bool maximum) {
    if (id.empty()) return;
    auto found = std::ranges::find(recipe, id, &WeaponRecipeMaterial::id);
    if (found != recipe.end()) found->maximum |= maximum;
    else if (recipe.size() < kRecipeMaterialLimit) recipe.push_back({std::move(id), maximum});
}

} // namespace vst
