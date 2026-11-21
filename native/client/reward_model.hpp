#pragma once

#include "../common/win_util.hpp"
#include "../common/weapon_recipes.hpp"
#include "../vendor/yyjson/yyjson.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace vst::client {

inline constexpr size_t kRewardCatalogLimit = 4096, kRewardPageSize = 200;
struct RewardCatalogLoad {
    static constexpr unsigned kMaxAttempts = 4;

    bool CanLoadAutomatically(bool ready, uint64_t received_at) const {
        return ready && (!requested_ || (failed_at_ && received_at > failed_at_));
    }

    bool WaitingForSource(bool ready) const { return failed_at_ || (!requested_ && !ready); }

    void Start(bool automatic) {
        requested_ = true;
        failed_at_ = 0;
        if (!automatic) attempts_ = 0;
    }

    bool Wait(std::string_view error, uint64_t received_at) {
        if (!received_at || attempts_ >= kMaxAttempts) return false;
        if (error != "Wait for fresh single-player game data before reading or changing rewards."
            && error != "The single-player reward context is no longer ready."
            && error != "The main-thread game observation expired."
            && error != "Connection, player or command deadline changed."
            && error != "The reward context or target changed while validating the request."
            && error != "The current run's reward data is not ready."
            && error != "The menu's reward data is still loading."
            && error != "The game's equipment catalog is still loading."
            && error != "Wait for the single-player menu or run before reading equipment."
            && error != "The single-player menu changed while reading equipment.") return false;
        ++attempts_;
        failed_at_ = received_at;
        return true;
    }

private:
    bool requested_ = false;
    unsigned attempts_ = 0;
    uint64_t failed_at_ = 0;
};

struct RewardRequest {
    std::string item_id;
    int enabled = -1, upgrade_enabled = -1;
    size_t catalog_offset = 0;
};
inline bool ValidRewardId(std::string_view id) {
    return !id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}
inline std::wstring FoldRewardSearch(std::wstring_view value) {
    if (value.empty()) return {};
    int length = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
    if (length <= 0) return std::wstring(value);
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr, 0)) return std::wstring(value);
    return result;
}
inline std::string RewardText(yyjson_val* object, const char* key, size_t maximum = 4096) {
    auto value = yyjson_obj_get(object, key);
    return yyjson_is_str(value) && yyjson_get_len(value) <= maximum ? std::string(yyjson_get_str(value), yyjson_get_len(value)) : "";
}
struct RewardItem {
    std::string id, kind;
    std::wstring name, english_name, wide_id, search, level, upgrade_reason;
    bool evolution = false, upgrade_known = false, upgrade_allowed = false;
    int max_level = 0;
    std::wstring chinese_name;
    std::vector<WeaponRecipeMaterial> recipe;
    bool reference_only = false;
    RecipeRequirement recipe_requirement = RecipeRequirement::None;
};
inline void PrepareRewardItem(RewardItem& item) {
    item.wide_id = vst::Wide(item.id);
    item.search = FoldRewardSearch(item.name + L" " + item.english_name + L" " + item.chinese_name + L" " + item.wide_id);
    item.level = item.max_level > 0 ? std::to_wstring(item.max_level) : L"—";
}
enum class RewardFilter { All, Weapons, Passives, Evolutions };
inline std::vector<size_t> FilterRewardItems(const std::vector<RewardItem>& items, std::wstring_view query, RewardFilter filter) {
    while (!query.empty() && iswspace(query.front())) query.remove_prefix(1);
    while (!query.empty() && iswspace(query.back())) query.remove_suffix(1);
    std::wstring folded = FoldRewardSearch(query);
    std::vector<size_t> result; result.reserve(items.size());
    for (size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        bool category = filter == RewardFilter::All || (filter == RewardFilter::Weapons && item.kind == "weapon")
            || (filter == RewardFilter::Passives && item.kind == "passive") || (filter == RewardFilter::Evolutions && item.evolution);
        if (!item.reference_only && category && (folded.empty() || item.search.find(folded) != std::wstring::npos)) result.push_back(index);
    }
    return result;
}
struct RewardCatalogPage {
    bool present = false, valid = false, loading = false;
    bool icons_supported = false;
    size_t total = 0, returned = 0;
    std::vector<RewardItem> items;
    std::string error;
};
inline RewardCatalogPage ParseRewardCatalogPage(yyjson_val* response) {
    RewardCatalogPage page;
    auto catalog = yyjson_obj_get(response, "itemCatalog"), total = yyjson_obj_get(response, "itemCatalogTotal"), loading = yyjson_obj_get(response, "itemCatalogLoading");
    page.loading = yyjson_get_bool(loading);
    page.icons_supported = yyjson_get_bool(yyjson_obj_get(response, "itemIconsSupported"));
    page.present = catalog != nullptr || page.loading;
    if (!page.present) return page;
    if (!yyjson_is_arr(catalog) || yyjson_arr_size(catalog) > kRewardPageSize || !yyjson_is_uint(total) || yyjson_get_uint(total) > 65536
        || (loading && !yyjson_is_bool(loading))) {
        page.error = "The game returned an invalid item catalog page."; return page;
    }
    page.total = static_cast<size_t>(yyjson_get_uint(total)); page.returned = yyjson_arr_size(catalog);
    if (page.loading) {
        if (page.total || page.returned) { page.error = "The game returned an invalid item catalog page."; return page; }
        page.valid = true; return page;
    }
    size_t index, count; yyjson_val* value;
    yyjson_arr_foreach(catalog, index, count, value) {
        RewardItem item;
        item.id = RewardText(value, "id", 128); item.kind = RewardText(value, "kind", 16);
        if (!ValidRewardId(item.id) || (item.kind != "weapon" && item.kind != "passive")) { page.error = "The game returned an invalid reward item."; return page; }
        item.name = vst::Wide(RewardText(value, "name")); item.english_name = vst::Wide(RewardText(value, "englishName"));
        item.chinese_name = vst::Wide(RewardText(value, "chineseName"));
        item.reference_only = yyjson_get_bool(yyjson_obj_get(value, "referenceOnly"));
        if (auto requirement = yyjson_obj_get(value, "recipeRequirement")) {
            if (!yyjson_is_uint(requirement) || yyjson_get_uint(requirement) > static_cast<uint8_t>(RecipeRequirement::SixMaxEvolutionsAndPassives)) {
                page.error = "The game returned an invalid weapon recipe requirement."; return page;
            }
            item.recipe_requirement = static_cast<RecipeRequirement>(yyjson_get_uint(requirement));
        }
        if (auto recipe = yyjson_obj_get(value, "recipe")) {
            if (!yyjson_is_arr(recipe) || yyjson_arr_size(recipe) > kRecipeMaterialLimit) {
                page.error = "The game returned an invalid weapon recipe."; return page;
            }
            size_t part_index, part_count; yyjson_val* part;
            yyjson_arr_foreach(recipe, part_index, part_count, part) {
                auto id = RewardText(part, "id", 128);
                if (!ValidRewardId(id) || id == item.id) { page.error = "The game returned an invalid weapon material."; return page; }
                AddRecipeMaterial(item.recipe, std::move(id), yyjson_get_bool(yyjson_obj_get(part, "maximum")));
            }
        }
        auto level = yyjson_obj_get(value, "maxLevel");
        item.max_level = yyjson_is_int(level) ? static_cast<int>(std::clamp<int64_t>(yyjson_get_sint(level), 0, 1000)) : 0;
        item.evolution = yyjson_get_bool(yyjson_obj_get(value, "isEvolution"));
        auto allowed = yyjson_obj_get(value, "upgradeAllowed"); item.upgrade_known = yyjson_is_bool(allowed); item.upgrade_allowed = yyjson_get_bool(allowed);
        item.upgrade_reason = vst::Wide(RewardText(value, "upgradeUnsupportedReason"));
        PrepareRewardItem(item); page.items.push_back(std::move(item));
    }
    page.valid = true; return page;
}
struct RewardIcon {
    std::string id;
    std::vector<unsigned char> rgba;
    int width = 0, height = 0;
    bool pending = false;
};

inline RewardIcon ParseRewardIcon(yyjson_val* response) {
    auto icon = yyjson_obj_get(response, "itemIcon");
    RewardIcon result;
    result.id = RewardText(icon, "id", 128);
    if (!ValidRewardId(result.id)) return {};
    result.pending = yyjson_get_bool(yyjson_obj_get(icon, "pending"));
    if (result.pending) return result;
    auto w = yyjson_obj_get(icon, "width"), h = yyjson_obj_get(icon, "height");
    if (!yyjson_is_int(w) || !yyjson_is_int(h)) return result;
    const int64_t width = yyjson_get_sint(w), height = yyjson_get_sint(h);
    if (width < 1 || width > 32 || height < 1 || height > 32) return result;
    auto encoded = yyjson_obj_get(icon, "rgbaHex");
    auto bytes = static_cast<size_t>(width * height * 4);
    if (!yyjson_is_str(encoded) || yyjson_get_len(encoded) != bytes * 2) return result;
    auto hex = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1; };
    auto text = yyjson_get_str(encoded);
    result.rgba.resize(bytes);
    for (size_t i = 0; i < bytes; ++i) {
        int hi = hex(text[2 * i]), lo = hex(text[2 * i + 1]);
        if (hi < 0 || lo < 0) { result.rgba.clear(); return result; }
        result.rgba[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);
    return result;
}

inline void MergeRewardPage(std::vector<RewardItem>& catalog, const RewardCatalogPage& page) {
    // Reserve before making any string_views into vector-owned strings.
    catalog.reserve(std::min(kRewardCatalogLimit, catalog.size() + page.items.size()));
    std::unordered_set<std::string_view> known; known.reserve(catalog.size() + page.items.size());
    for (const auto& item : catalog) known.insert(item.id);
    for (const auto& item : page.items) {
        if (catalog.size() >= kRewardCatalogLimit) break;
        if (known.contains(item.id)) continue;
        catalog.push_back(item); known.insert(catalog.back().id);
    }
}
enum class RewardPageDisposition { Invalid, Loading, Ready };
struct RewardPageApplication {
    RewardPageDisposition disposition = RewardPageDisposition::Invalid;
    size_t next_offset = 0;
    bool complete = false;
    std::string error;
};
inline RewardPageApplication ApplyRewardCatalogPage(std::vector<RewardItem>& catalog, size_t expected_offset, size_t actual_offset, const RewardCatalogPage& page) {
    RewardPageApplication result; result.next_offset = expected_offset;
    if (!page.valid) { result.error = page.error; return result; }
    if (actual_offset != expected_offset) { result.error = "The item catalog changed while loading. Refresh the list."; return result; }
    // A bounded in-progress scan deliberately has an empty page and total 0.
    // Keep the old view intact, and retry the same offset after the UI delay.
    if (page.loading) { result.disposition = RewardPageDisposition::Loading; return result; }
    if (actual_offset > page.total || (page.returned == 0 && actual_offset < page.total)) {
        result.error = "The item catalog changed while loading. Refresh the list."; return result;
    }
    if (actual_offset == 0) catalog.clear();
    MergeRewardPage(catalog, page);
    result.disposition = RewardPageDisposition::Ready; result.next_offset = actual_offset + page.returned;
    result.complete = result.next_offset >= page.total || result.next_offset >= kRewardCatalogLimit || page.returned == 0;
    return result;
}
struct RewardState {
    bool supported = false, enabled = false, upgrade_supported = false, upgrade_enabled = false;
    uint64_t processed = 0, offered = 0, granted = 0;
    std::string target_id, last_reward_id, last_offer_id, last_granted_id;
    std::wstring target_name, message, upgrade_message, last_reward_name, last_offer_name, last_granted_name;
};
inline RewardState ParseRewardState(yyjson_val* object) {
    RewardState state;
    state.supported = yyjson_get_bool(yyjson_obj_get(object, "supported")); state.enabled = yyjson_get_bool(yyjson_obj_get(object, "enabled"));
    state.upgrade_supported = yyjson_get_bool(yyjson_obj_get(object, "upgradeSupported")); state.upgrade_enabled = yyjson_get_bool(yyjson_obj_get(object, "upgradeEnabled"));
    state.processed = yyjson_get_uint(yyjson_obj_get(object, "processedCount")); state.offered = yyjson_get_uint(yyjson_obj_get(object, "upgradeProcessedCount"));
    state.granted = yyjson_get_uint(yyjson_obj_get(object, "upgradeGrantedCount"));
    state.target_id = RewardText(object, "targetItemId", 128); state.target_name = vst::Wide(RewardText(object, "targetItemName"));
    state.message = vst::Wide(RewardText(object, "message")); state.upgrade_message = vst::Wide(RewardText(object, "upgradeMessage"));
    state.last_reward_id = RewardText(object, "lastRewardItemId", 128); state.last_reward_name = vst::Wide(RewardText(object, "lastRewardName"));
    state.last_offer_id = RewardText(object, "lastUpgradeItemId", 128); state.last_offer_name = vst::Wide(RewardText(object, "lastUpgradeName"));
    state.last_granted_id = RewardText(object, "lastUpgradeGrantedItemId", 128); state.last_granted_name = vst::Wide(RewardText(object, "lastUpgradeGrantedName"));
    return state;
}
inline bool CheckRewardModelPolicy() {
    constexpr auto temporary = "Wait for fresh single-player game data before reading or changing rewards.";
    constexpr auto unavailable = "The native equipment catalog is unavailable.";
    RewardCatalogLoad load;
    if (load.CanLoadAutomatically(false, 1) || !load.WaitingForSource(false) || !load.CanLoadAutomatically(true, 2)) return false;
    load.Start(true);
    if (load.CanLoadAutomatically(true, 3) || load.Wait(unavailable, 3) || load.CanLoadAutomatically(true, 4)) return false;
    for (unsigned attempt = 0; attempt < RewardCatalogLoad::kMaxAttempts; ++attempt) {
        const uint64_t failed_at = 5 + 2 * attempt;
        if (!load.Wait(temporary, failed_at) || load.CanLoadAutomatically(true, failed_at)
            || load.CanLoadAutomatically(false, failed_at + 1) || !load.WaitingForSource(false)
            || !load.CanLoadAutomatically(true, failed_at + 1)) return false;
        load.Start(true);
        if (load.CanLoadAutomatically(true, failed_at + 1) || load.WaitingForSource(true)) return false;
    }
    if (load.Wait(temporary, 100) || load.CanLoadAutomatically(true, 101)) return false;
    load.Start(false);
    if (!load.Wait(temporary, 102)) return false;
    load = {};
    if (!load.CanLoadAutomatically(true, 103)) return false;

    if (!ValidRewardId("HOLYBOOK") || ValidRewardId("bad/id") || ValidRewardId(std::string(129, 'A')) || ValidRewardId("")) return false;
    const char* json = R"({"itemCatalog":[{"id":"MAGIC_MISSILE","name":"魔杖","englishName":"Magic Wand","kind":"weapon","isEvolution":false,"maxLevel":8,"upgradeAllowed":true},{"id":"SPINACH","name":"菠菜","englishName":"Spinach","kind":"passive","maxLevel":5,"upgradeAllowed":true},{"id":"EVOLVED_WAND","name":"圣魔杖","englishName":"Holy Wand","kind":"weapon","isEvolution":true,"maxLevel":1,"upgradeAllowed":true}],"itemCatalogTotal":3})";
    auto doc = yyjson_read(json, strlen(json), 0); if (!doc) return false;
    auto page = ParseRewardCatalogPage(yyjson_doc_get_root(doc)); yyjson_doc_free(doc);
    if (!page.valid || page.total != 3 || page.returned != 3) return false;
    std::vector<RewardItem> catalog; MergeRewardPage(catalog, page); MergeRewardPage(catalog, page);
    if (catalog.size() != 3 || FilterRewardItems(catalog, L"魔杖", RewardFilter::All).size() != 2
        || FilterRewardItems(catalog, L"MAGIC_missile", RewardFilter::All) != std::vector<size_t>{0}
        || FilterRewardItems(catalog, L" WAND ", RewardFilter::Weapons) != std::vector<size_t>({0, 2})
        || FilterRewardItems(catalog, L"", RewardFilter::Passives) != std::vector<size_t>{1}
        || FilterRewardItems(catalog, L"", RewardFilter::Evolutions) != std::vector<size_t>{2}
        || !FilterRewardItems(catalog, L"not present", RewardFilter::All).empty()) return false;
    const char* state_json = R"({"supported":true,"enabled":false,"upgradeSupported":true,"upgradeEnabled":true,"targetItemId":"SPINACH","upgradeProcessedCount":4,"lastUpgradeItemId":"SPINACH","upgradeGrantedCount":1,"lastUpgradeGrantedItemId":"SPINACH"})";
    doc = yyjson_read(state_json, strlen(state_json), 0); if (!doc) return false;
    auto state = ParseRewardState(yyjson_doc_get_root(doc)); yyjson_doc_free(doc);
    if (!state.supported || state.enabled || !state.upgrade_enabled || state.offered != 4 || state.granted != 1 || state.processed != 0) return false;
    const char* invalid = R"({"itemCatalog":[{"id":"../bad","kind":"weapon"}],"itemCatalogTotal":1})";
    doc = yyjson_read(invalid, strlen(invalid), 0); if (!doc) return false;
    bool rejected = !ParseRewardCatalogPage(yyjson_doc_get_root(doc)).valid; yyjson_doc_free(doc);
    if (!rejected) return false;
    const char* loading_json = R"({"itemCatalogLoading":true,"itemCatalog":[],"itemCatalogTotal":0})";
    doc = yyjson_read(loading_json, strlen(loading_json), 0); if (!doc) return false;
    auto loading = ParseRewardCatalogPage(yyjson_doc_get_root(doc)); yyjson_doc_free(doc);
    const auto original_id = catalog.front().id; const auto original_name = catalog.front().name;
    auto application = ApplyRewardCatalogPage(catalog, 0, 0, loading);
    if (!loading.valid || !loading.loading || application.disposition != RewardPageDisposition::Loading || application.complete || application.next_offset != 0
        || catalog.size() != 3 || catalog.front().id != original_id || catalog.front().name != original_name) return false;
    application = ApplyRewardCatalogPage(catalog, 200, 200, loading);
    if (application.disposition != RewardPageDisposition::Loading || application.next_offset != 200 || catalog.size() != 3) return false;
    application = ApplyRewardCatalogPage(catalog, 0, 0, page);
    if (application.disposition != RewardPageDisposition::Ready || !application.complete || catalog.size() != 3) return false;
    const char* empty_json = R"({"itemCatalogLoading":false,"itemCatalog":[],"itemCatalogTotal":0})";
    doc = yyjson_read(empty_json, strlen(empty_json), 0); if (!doc) return false;
    auto empty = ParseRewardCatalogPage(yyjson_doc_get_root(doc)); yyjson_doc_free(doc);
    application = ApplyRewardCatalogPage(catalog, 0, 0, empty);
    if (!empty.valid || empty.loading || application.disposition != RewardPageDisposition::Ready || !application.complete || !catalog.empty()) return false;
    const char* recipe_json = R"({"itemCatalog":[{"id":"EXAMPLE_RESULT","name":"Example result","chineseName":"示例成品","kind":"weapon","referenceOnly":true,"recipeRequirement":2,"recipe":[{"id":"MAT_ONE"},{"id":"MAT_ONE","maximum":true},{"id":"MAT_TWO"}]}],"itemCatalogTotal":1})";
    doc = yyjson_read(recipe_json, strlen(recipe_json), 0); if (!doc) return false;
    const auto recipe_page = ParseRewardCatalogPage(yyjson_doc_get_root(doc)); yyjson_doc_free(doc);
    if (!recipe_page.valid || recipe_page.items.size() != 1) return false;
    const auto& recipe = recipe_page.items.front();
    if (!recipe.reference_only || recipe.chinese_name != L"示例成品" || recipe.recipe.size() != 2
        || recipe.recipe_requirement != RecipeRequirement::AnyMaterial || recipe.recipe[0].id != "MAT_ONE" || !recipe.recipe[0].maximum
        || !FilterRewardItems(recipe_page.items, L"", RewardFilter::All).empty()) return false;
    auto parse_icon = [](std::string_view json) {
        auto parsed = yyjson_read(json.data(), json.size(), 0);
        if (!parsed) return RewardIcon{};
        auto icon = ParseRewardIcon(yyjson_doc_get_root(parsed));
        yyjson_doc_free(parsed);
        return icon;
    };
    const auto icon = parse_icon(R"({"itemIcon":{"id":"ARMOR","width":1,"height":1,"rgbaHex":"ff200080"}})");
    const auto pending = parse_icon(R"({"itemIcon":{"id":"ARMOR","pending":true}})");
    const auto malformed = parse_icon(R"({"itemIcon":{"id":"ARMOR","width":1,"height":1,"rgbaHex":"ff2g0080"}})");
    const auto oversized = parse_icon(R"({"itemIcon":{"id":"ARMOR","width":65536,"height":1,"rgbaHex":"ff200080"}})");
    return icon.id == "ARMOR" && icon.width == 1 && icon.height == 1
        && icon.rgba == std::vector<unsigned char>{255, 32, 0, 128}
        && pending.pending && pending.rgba.empty() && malformed.rgba.empty() && oversized.rgba.empty();
}

} // namespace vst::client
