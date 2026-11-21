#include "protocol.hpp"
#include "chest_animation.hpp"
#include "chest_rewards.hpp"
#include "game_speed.hpp"
#include "../vendor/yyjson/yyjson.h"

#include <cmath>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <iterator>

#ifndef VST_NATIVE_VERSION
#define VST_NATIVE_VERSION "native-pilot.11"
#endif

namespace vst {
namespace {

bool NextCodepoint(std::string_view text, size_t& offset, uint32_t& value) noexcept {
    if (offset >= text.size()) return false;
    auto first = static_cast<unsigned char>(text[offset++]);
    if (first < 0x80) { value = first; return true; }
    unsigned count;
    uint32_t minimum;
    if (first >= 0xC2 && first <= 0xDF) { count = 1; value = first & 0x1F; minimum = 0x80; }
    else if (first >= 0xE0 && first <= 0xEF) { count = 2; value = first & 0x0F; minimum = 0x800; }
    else if (first >= 0xF0 && first <= 0xF4) { count = 3; value = first & 7; minimum = 0x10000; }
    else return false;
    if (count > text.size() - offset) return false;
    for (unsigned index = 0; index < count; ++index) {
        auto next = static_cast<unsigned char>(text[offset++]);
        if ((next & 0xC0) != 0x80) return false;
        value = (value << 6) | (next & 0x3F);
    }
    return value >= minimum && value <= 0x10FFFF && !(value >= 0xD800 && value <= 0xDFFF);
}

bool Whitespace(uint32_t value) noexcept {
    return (value >= 9 && value <= 13) || value == 0x20 || value == 0x85 || value == 0xA0
        || value == 0x1680 || (value >= 0x2000 && value <= 0x200A) || value == 0x2028
        || value == 0x2029 || value == 0x202F || value == 0x205F || value == 0x3000;
}

bool EqualAscii(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        auto ch = static_cast<unsigned char>(left[index]);
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<unsigned char>(ch + ('a' - 'A'));
        if (ch != static_cast<unsigned char>(right[index])) return false;
    }
    return true;
}

void Text(yyjson_mut_doc* doc, yyjson_mut_val* object, const char* key, std::string_view value) {
    yyjson_mut_obj_add(object, yyjson_mut_strcpy(doc, key), yyjson_mut_strncpy(doc, value.data(), value.size()));
}

void NullableNumber(yyjson_mut_doc* doc, yyjson_mut_val* object, const char* key, bool known, double value) {
    if (known && std::isfinite(value)) yyjson_mut_obj_add_real(doc, object, key, value);
    else yyjson_mut_obj_add_null(doc, object, key);
}

void FloatText(yyjson_mut_doc* doc, yyjson_mut_val* object, const char* key, float value) {
    char buffer[64];
    auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
    if (result.ec == std::errc{}) Text(doc, object, key, std::string_view(buffer, static_cast<size_t>(result.ptr - buffer)));
}

} // namespace

bool Utf16Length(std::string_view text, size_t& length) noexcept {
    length = 0;
    size_t offset = 0;
    while (offset < text.size()) {
        uint32_t value = 0;
        if (!NextCodepoint(text, offset, value)) return false;
        length += value > 0xFFFF ? 2 : 1;
    }
    return true;
}

bool ValidRequestId(std::string_view id) noexcept {
    size_t offset = 0, length = 0;
    bool all_whitespace = true;
    while (offset < id.size()) {
        uint32_t value = 0;
        if (!NextCodepoint(id, offset, value)) return false;
        length += value > 0xFFFF ? 2 : 1;
        if (length > 128) return false;
        if (!Whitespace(value)) all_whitespace = false;
    }
    return length > 0 && !all_whitespace;
}

bool ParseRequest(std::string_view json, Request& request, std::string& error) {
    request = Request{};
    error.clear();
    size_t characters = 0;
    if (json.size() > kMaximumMessageBytes || !Utf16Length(json, characters) || characters > kMaximumMessageCharacters) {
        error = "Request is too long or is not valid UTF-8.";
        return false;
    }
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    if (!doc) { error = "Request must be one complete JSON object."; return false; }
    yyjson_val* root = yyjson_doc_get_root(doc);
    bool valid = yyjson_is_obj(root);
    unsigned seen = 0;
    if (valid) {
        yyjson_obj_iter iterator;
        yyjson_obj_iter_init(root, &iterator);
        yyjson_val* key;
        while ((key = yyjson_obj_iter_next(&iterator)) != nullptr) {
            std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
            yyjson_val* value = yyjson_obj_iter_get_val(key);
            unsigned field = EqualAscii(name, "id") ? 1 : EqualAscii(name, "kind") ? 2
                : EqualAscii(name, "stat") ? 4 : EqualAscii(name, "enabled") ? 8
                : EqualAscii(name, "value") ? 16 : EqualAscii(name, "feature") ? 32
                : EqualAscii(name, "backuptoken") ? 64 : 0;
            if (!field) field = EqualAscii(name, "itemid") ? 128 : EqualAscii(name, "catalogoffset") ? 256
                : EqualAscii(name, "cataloglimit") ? 512 : EqualAscii(name, "upgradeenabled") ? 1024
                : EqualAscii(name, "goldadventure") ? 2048 : 0;
            if (!field) continue; // Existing C# clients serialize unused nullable fields.
            if ((seen & field) != 0) { error = "Duplicate request field."; valid = false; break; }
            seen |= field;
            if (field == 1 || field == 2 || field == 4 || field == 32 || field == 64 || field == 128) {
                if ((field == 4 || field == 32 || field == 64 || field == 128) && yyjson_is_null(value)) continue;
                if (!yyjson_is_str(value)) { valid = false; break; }
                std::string text(yyjson_get_str(value), yyjson_get_len(value));
                if (field == 1) request.id = std::move(text);
                else if (field == 2) request.kind = std::move(text);
                else if (field == 4) request.stat = std::move(text);
                else if (field == 32) request.feature = std::move(text);
                else if (field == 128) {
                    if (text.size() > 128 || text.find('\0') != std::string::npos) { valid = false; break; }
                    request.item_id = std::move(text); request.has_item_id = true;
                }
                else { if (text.size() > 32767 || text.find('\0') != std::string::npos) { valid = false; break; } request.backup_token = std::move(text); }
            } else if (field == 8 || field == 1024) {
                if (yyjson_is_null(value)) continue;
                if (!yyjson_is_bool(value)) { valid = false; break; }
                if (field == 8) { request.has_enabled = true; request.enabled = yyjson_get_bool(value); }
                else { request.has_upgrade_enabled = true; request.upgrade_enabled = yyjson_get_bool(value); }
            } else if (field == 256 || field == 512) {
                if (!yyjson_is_int(value) || yyjson_get_sint(value) < 0 || yyjson_get_sint(value) > 4096) { valid = false; break; }
                if (field == 256) request.catalog_offset = static_cast<int>(yyjson_get_sint(value));
                else request.catalog_limit = static_cast<int>(yyjson_get_sint(value));
            } else if (field == 2048) {
                if (!yyjson_is_uint(value) || yyjson_get_uint(value) > INT32_MAX) { valid = false; break; }
                request.gold_adventure = static_cast<int32_t>(yyjson_get_uint(value));
            } else {
                if (yyjson_is_null(value)) continue;
                if (!yyjson_is_num(value)) { valid = false; break; }
                request.has_value = true;
                request.value = yyjson_get_num(value);
                if (!std::isfinite(request.value)) { valid = false; break; }
            }
        }
    }
    yyjson_doc_free(doc);
    if (!valid || !ValidRequestId(request.id)) {
        if (error.empty()) error = "Request fields or ID are invalid.";
        return false;
    }
    return true;
}

std::string SerializeResponse(const Response& response) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    if (!doc) return {};
    auto root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    Text(doc, root, "id", response.id);
    yyjson_mut_obj_add_bool(doc, root, "ok", response.ok);
    if (response.error.empty()) yyjson_mut_obj_add_null(doc, root, "error");
    else Text(doc, root, "error", response.error);
    if (response.error_scope.empty()) yyjson_mut_obj_add_null(doc, root, "errorScope");
    else Text(doc, root, "errorScope", response.error_scope);
    if (response.has_item_catalog) {
        yyjson_mut_obj_add_bool(doc, root, "itemCatalogLoading", response.item_catalog_loading);
        yyjson_mut_obj_add_bool(doc, root, "itemIconsSupported", response.item_icons_supported);
        auto items = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, root, "itemCatalog", items);
        yyjson_mut_obj_add_int(doc, root, "itemCatalogTotal", response.item_catalog_total);
        for (const auto& value : response.item_catalog) {
            auto item = yyjson_mut_obj(doc);
            Text(doc, item, "id", value.id); Text(doc, item, "name", value.name);
            Text(doc, item, "englishName", value.english_name); Text(doc, item, "kind", value.kind);
            yyjson_mut_obj_add_bool(doc, item, "isEvolution", value.is_evolution);
            yyjson_mut_obj_add_bool(doc, item, "upgradeAllowed", value.upgrade_allowed);
            Text(doc, item, "upgradeUnsupportedReason", value.upgrade_unsupported_reason);
            yyjson_mut_obj_add_int(doc, item, "maxLevel", value.max_level);
            yyjson_mut_arr_add_val(items, item);
        }
    }
    if (response.has_item_icon) {
        auto icon = yyjson_mut_obj(doc); yyjson_mut_obj_add_val(doc, root, "itemIcon", icon);
        Text(doc, icon, "id", response.item_icon_id);
        Text(doc, icon, "rgbaHex", response.item_icon_rgba_hex);
        yyjson_mut_obj_add_int(doc, icon, "width", response.item_icon_width);
        yyjson_mut_obj_add_int(doc, icon, "height", response.item_icon_height);
        yyjson_mut_obj_add_bool(doc, icon, "pending", response.item_icon_pending);
    }

    const auto& value = response.state;
    auto state = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "state", state);
    Text(doc, state, "gameBuild", "25984009");
    Text(doc, state, "pluginVersion", VST_NATIVE_VERSION);
    Text(doc, state, "scene", value.scene);
    yyjson_mut_obj_add_bool(doc, state, "playerReady", value.player_ready);
    NullableNumber(doc, state, "gold", value.gold_known, value.gold);
    yyjson_mut_obj_add_bool(doc, state, "backupAuthorized", value.backup_authorized);
    yyjson_mut_obj_add_bool(doc, state, "walletReady", value.wallet_ready);
    yyjson_mut_obj_add_bool(doc, state, "goldWriteSupported", value.gold_write_supported);
    yyjson_mut_obj_add_bool(doc, state, "goldWriteReady", value.gold_write_ready);
    if (value.gold_adventure >= 0) yyjson_mut_obj_add_int(doc, state, "goldAdventure", value.gold_adventure);
    auto features = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, state, "features", features);
    for (const char* id : {"health", "instantKill", "experience", "damage", "movement", "cooldown", "noGoldSpend", kGameSpeedId, kSkipChestAnimationId, kFiveItemChestsId}) {
        int index = std::strcmp(id, "health") == 0 ? 0 : std::strcmp(id, "experience") == 0 ? 1 : std::strcmp(id, "damage") == 0 ? 2
            : std::strcmp(id, "movement") == 0 ? 3 : std::strcmp(id, "cooldown") == 0 ? 4 : std::strcmp(id, "instantKill") == 0 ? 5
            : std::strcmp(id, "noGoldSpend") == 0 ? 6 : std::strcmp(id, kGameSpeedId) == 0 ? static_cast<int>(CombatFeature::GameSpeed)
            : std::strcmp(id, kSkipChestAnimationId) == 0 ? static_cast<int>(CombatFeature::SkipChestAnimation)
            : static_cast<int>(CombatFeature::FiveItemChests);
        const FeatureSnapshot* migrated = index < 0 ? nullptr : &value.combat[index];
        auto feature = yyjson_mut_obj(doc);
        Text(doc, feature, "id", id);
        yyjson_mut_obj_add_bool(doc, feature, "supported", migrated && migrated->supported);
        yyjson_mut_obj_add_bool(doc, feature, "enabled", migrated && migrated->enabled);
        yyjson_mut_obj_add_real(doc, feature, "value", migrated ? migrated->value : 1);
        if (!migrated) Text(doc, feature, "reason", "Not migrated to the native pilot.");
        else if (!migrated->reason.empty()) Text(doc, feature, "reason", migrated->reason);
        else if (!migrated->supported) Text(doc, feature, "reason", "Waiting for native effect entry verification.");
        else if (index == 6 && !value.backup_authorized) Text(doc, feature, "reason", "Create a verified backup before starting this game session.");
        else if (index == 6 && !value.safe_mode) Text(doc, feature, "reason", "Waiting for a supported main-game wallet.");
        else if (index != 6 && !value.player_ready) Text(doc, feature, "reason", "Enter a supported single-player run.");
        else yyjson_mut_obj_add_null(doc, feature, "reason");
        yyjson_mut_arr_add_val(features, feature);
    }
    auto stats = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, state, "stats", stats);
    for (const char* id : {"maxHealth", "armor", "recovery", "might", "projectileSpeed", "duration", "area", "cooldown",
            "amount", "moveSpeed", "magnet", "luck", "growth", "greed", "curse", "revivals", "rerolls", "skips",
            "banishes", "charm", "defang", "preserve", "omni"}) {
        bool armor = std::strcmp(id, "armor") == 0;
        const auto* definition = FindNativeStat(id);
        const auto* migrated = definition ? &value.stats[static_cast<size_t>(definition->stat)] : nullptr;
        auto stat = yyjson_mut_obj(doc);
        Text(doc, stat, "id", id);
        yyjson_mut_obj_add_bool(doc, stat, "supported", armor ? value.armor_supported : migrated && migrated->supported);
        yyjson_mut_obj_add_bool(doc, stat, "enabled", armor ? value.armor_enabled : migrated && migrated->enabled);
        yyjson_mut_obj_add_real(doc, stat, "value", armor ? value.armor_target : migrated ? migrated->value : 0);
        NullableNumber(doc, stat, "originalValue", armor ? value.armor_sample_known : migrated && migrated->sample_known,
            armor ? value.armor_original : migrated ? migrated->original : 0);
        NullableNumber(doc, stat, "currentValue", armor ? value.armor_current_known : migrated && migrated->current_known,
            armor ? value.armor_current : migrated ? migrated->current : 0);
        NullableNumber(doc, stat, "boundValue", migrated && migrated->bound_known, migrated ? migrated->bound : 0);
        if (!migrated) Text(doc, stat, "reason", "Not migrated to the native pilot.");
        else if (!value.error.empty()) Text(doc, stat, "reason", value.error);
        else if (armor && !value.armor_reason.empty()) Text(doc, stat, "reason", value.armor_reason);
        else if (!armor && !migrated->reason.empty()) Text(doc, stat, "reason", migrated->reason);
        else if (!(armor ? value.armor_supported : migrated->supported)) Text(doc, stat, "reason", "Waiting for native game entry verification.");
        else if (!value.player_ready) Text(doc, stat, "reason", "Enter a supported single-player run.");
        else yyjson_mut_obj_add_null(doc, stat, "reason");
        yyjson_mut_arr_add_val(stats, stat);
    }
    auto chest = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, state, "chestReward", chest);
    const auto& reward = value.chest_reward;
    yyjson_mut_obj_add_bool(doc, chest, "supported", reward.supported);
    yyjson_mut_obj_add_bool(doc, chest, "enabled", reward.enabled);
    yyjson_mut_obj_add_bool(doc, chest, "upgradeSupported", reward.upgrade_supported);
    yyjson_mut_obj_add_bool(doc, chest, "upgradeEnabled", reward.upgrade_enabled);
    Text(doc, chest, "targetItemId", reward.target_item_id); Text(doc, chest, "targetItemName", reward.target_item_name);
    Text(doc, chest, "message", reward.message); Text(doc, chest, "upgradeMessage", reward.upgrade_message);
    yyjson_mut_obj_add_uint(doc, chest, "processedCount", reward.processed_count);
    yyjson_mut_obj_add_uint(doc, chest, "upgradeProcessedCount", reward.upgrade_processed_count);
    yyjson_mut_obj_add_uint(doc, chest, "upgradeGrantedCount", reward.upgrade_granted_count);
    Text(doc, chest, "lastRewardItemId", reward.last_reward_item_id); Text(doc, chest, "lastRewardName", reward.last_reward_name);
    Text(doc, chest, "lastUpgradeItemId", reward.last_upgrade_item_id); Text(doc, chest, "lastUpgradeName", reward.last_upgrade_name);
    Text(doc, chest, "lastUpgradeGrantedItemId", reward.last_upgrade_granted_item_id);
    Text(doc, chest, "lastUpgradeGrantedName", reward.last_upgrade_granted_name);
    auto diagnostics = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, state, "diagnostics", diagnostics);
    Text(doc, diagnostics, "backend.kind", "native-pilot");
    Text(doc, diagnostics, "backend.gameVerified", value.verified ? "true" : "false");
    Text(doc, diagnostics, "backend.safeMode", value.safe_mode ? "true" : "false");
    if (value.paused_known) Text(doc, diagnostics, "game.paused", value.paused ? "true" : "false");
    Text(doc, diagnostics, "game.revivalPending", value.revival_pending ? "true" : "false");
    Text(doc, diagnostics, "stat.armor.gameReads", std::to_string(value.armor_game_reads));
    Text(doc, diagnostics, "native.armor.overrideReads", std::to_string(value.armor_override_reads));
    Text(doc, diagnostics, "stat.armor.overrideReads", std::to_string(value.armor_override_reads));
    if (value.armor_actual_read_known) {
        FloatText(doc, diagnostics, "native.armor.lastInput", value.armor_last_input);
        FloatText(doc, diagnostics, "native.armor.lastOutput", value.armor_last_output);
        Text(doc, diagnostics, "native.armor.lastWasOverride", value.armor_last_was_override ? "true" : "false");
    }
    for (const auto& definition : kNativeStats) {
        if (definition.stat == NativeStat::Armor) continue;
        const auto& stat = value.stats[static_cast<size_t>(definition.stat)];
        std::string prefix = std::string("stat.") + definition.id + ".";
        Text(doc, diagnostics, (prefix + "gameReads").c_str(), std::to_string(stat.game_reads));
        Text(doc, diagnostics, (prefix + "overrideReads").c_str(), std::to_string(stat.override_reads));
        if (stat.io_known) {
            FloatText(doc, diagnostics, (prefix + "lastInput").c_str(), stat.last_input);
            FloatText(doc, diagnostics, (prefix + "lastOutput").c_str(), stat.last_output);
            Text(doc, diagnostics, (prefix + "source").c_str(), kNativeStats[static_cast<size_t>(stat.source)].id);
            Text(doc, diagnostics, (prefix + "lastMethod").c_str(), stat.last_is_final ? "final" : "base");
        }
    }
    const char* names[] = {"health", "experience", "damage", "movement", "cooldown", "instantKill", "noGoldSpend", kGameSpeedId, kSkipChestAnimationId, kFiveItemChestsId};
    static_assert(std::size(names) == kCombatFeatureCount);
    for (size_t index = 0; index < kCombatFeatureCount; ++index) {
        const auto& feature = value.combat[index];
        std::string prefix = std::string("native.") + names[index] + ".";
        Text(doc, diagnostics, (prefix + "calls").c_str(), std::to_string(feature.calls));
        Text(doc, diagnostics, (prefix + "overrideCalls").c_str(), std::to_string(feature.override_calls));
        Text(doc, diagnostics, (prefix + "actualCalls").c_str(), std::to_string(feature.actual_calls));
        Text(doc, diagnostics, (prefix + "overflowSkips").c_str(), std::to_string(feature.overflow_skips));
        if (index == 3 || index == 4) {
            Text(doc, diagnostics, (prefix + "gameReads").c_str(), std::to_string(feature.calls));
            Text(doc, diagnostics, (prefix + "overrideReads").c_str(), std::to_string(feature.override_calls));
        }
        if (index == 5) Text(doc, diagnostics, (prefix + "priorityCalls").c_str(), std::to_string(feature.override_calls));
        if (feature.io_known) {
            FloatText(doc, diagnostics, (prefix + "lastInput").c_str(), feature.last_input);
            FloatText(doc, diagnostics, (prefix + "lastOutput").c_str(), feature.last_output);
        }
        if (feature.result_known) {
            const char* before = index == 1 ? "lastXpBefore" : "lastHpBefore";
            const char* after = index == 1 ? "lastXpAfter" : "lastHpAfter";
            FloatText(doc, diagnostics, (prefix + before).c_str(), feature.result_before);
            FloatText(doc, diagnostics, (prefix + after).c_str(), feature.result_after);
            if (index == 1) {
                Text(doc, diagnostics, (prefix + "lastLevelBefore").c_str(), std::to_string(feature.level_before));
                Text(doc, diagnostics, (prefix + "lastLevelAfter").c_str(), std::to_string(feature.level_after));
            } else Text(doc, diagnostics, (prefix + "actualLossTotal").c_str(), std::to_string(feature.actual_loss_total));
        }
    }
    if (value.health_known) FloatText(doc, diagnostics, "sample.player.hp", value.health);
    if (value.experience_known) FloatText(doc, diagnostics, "sample.player.xp", value.experience);
    if (value.level_known) Text(doc, diagnostics, "sample.player.level", std::to_string(value.level));
    if (!value.error.empty()) Text(doc, diagnostics, "backend.error", value.error);

    size_t size = 0;
    char* json = yyjson_mut_write(doc, 0, &size);
    std::string output;
    if (json) { output.assign(json, size); std::free(json); }
    yyjson_mut_doc_free(doc);
    return output;
}

} // namespace vst
