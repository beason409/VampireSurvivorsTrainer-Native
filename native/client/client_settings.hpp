#pragma once

#include "hotkeys.hpp"
#include "reward_model.hpp"
#include "../vendor/yyjson/yyjson.h"
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

namespace vst::client {

inline constexpr size_t kGameSpeedPreference = 3;
inline constexpr size_t kStatPreferenceOffset = 4;
inline constexpr size_t kPreferenceValueCount = kStatPreferenceOffset + kNativeStatCount;
inline constexpr size_t kMaximumSettingsBytes = 65536;
struct ClientSettings {
    bool english = false;
    std::string reward_target_id;
    HotkeyBindings hotkeys = DefaultHotkeys();
    std::array<double, kPreferenceValueCount> values{};
    ClientSettings() {
        values[0] = values[1] = values[2] = 2;
        values[kGameSpeedPreference] = kGameSpeedDefault;
        for (size_t index = 0; index < kNativeStatCount; ++index) values[index + kStatPreferenceOffset] = kNativeStats[index].initial;
    }
};
inline std::string PreferenceValueId(size_t index) {
    return index == 0 ? "experience" : index == 1 ? "damage" : index == 2 ? "movement"
        : index == kGameSpeedPreference ? kGameSpeedId
        : index < kPreferenceValueCount ? "stat:" + std::string(kNativeStats[index - kStatPreferenceOffset].id) : "";
}
inline bool ValidPreferenceValue(size_t index, double value) {
    if (index == kGameSpeedPreference) return ValidGameSpeed(value);
    return index >= kStatPreferenceOffset && index < kPreferenceValueCount ? ValidStatValue(kNativeStats[index - kStatPreferenceOffset], value)
        : index < 3 && std::isfinite(value) && value >= (index == 2 ? 0.25 : 1) && value <= (index == 2 ? 10 : 1000);
}
inline bool SettingsKeyEquals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        char a = left[index], b = right[index];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return false;
    }
    return true;
}
inline yyjson_val* UniqueSettingsMember(yyjson_val* object, std::string_view name, bool& valid) {
    if (!yyjson_is_obj(object)) return nullptr;
    yyjson_val* found = nullptr;
    size_t index, count; yyjson_val *key, *value;
    yyjson_obj_foreach(object, index, count, key, value) {
        const char* text = yyjson_get_str(key);
        if (text && SettingsKeyEquals(std::string_view(text, yyjson_get_len(key)), name)) {
            if (found) valid = false;
            found = value;
        }
    }
    return found;
}
inline bool ParseClientSettings(std::string_view json, bool legacy, ClientSettings& result, std::string& error) {
    if (json.empty() || json.size() > kMaximumSettingsBytes) { error = "Settings file is empty or too large."; return false; }
    yyjson_doc* document = yyjson_read(json.data(), json.size(), 0);
    if (!document) { error = "Settings file is not valid JSON."; return false; }
    yyjson_val* root = yyjson_doc_get_root(document);
    bool valid = yyjson_is_obj(root);
    ClientSettings parsed;
    auto version = UniqueSettingsMember(root, "formatVersion", valid);
    if (!legacy && (!yyjson_is_uint(version) || yyjson_get_uint(version) != 1)) valid = false;
    auto language = UniqueSettingsMember(root, "language", valid);
    if (yyjson_is_str(language)) {
        std::string_view text(yyjson_get_str(language), yyjson_get_len(language));
        parsed.english = text == "en" || text == "en-US" || text == "en-GB";
    }
    auto reward_target = UniqueSettingsMember(root, "rewardTargetItemId", valid);
    if (yyjson_is_str(reward_target)) {
        std::string_view id(yyjson_get_str(reward_target), yyjson_get_len(reward_target));
        if (ValidRewardId(id)) parsed.reward_target_id = id;
    }
    auto bindings = UniqueSettingsMember(root, "hotkeys", valid);
    if (bindings && !yyjson_is_null(bindings) && !yyjson_is_obj(bindings)) valid = false;
    for (size_t index = 0; index < kHotkeyCount; ++index) {
        auto value = UniqueSettingsMember(bindings, HotkeyActionId(index), valid);
        if (!value) continue;
        HotkeyChord chord;
        if (!yyjson_is_uint(value) || yyjson_get_uint(value) > UINT32_MAX || !DecodeHotkey(static_cast<uint32_t>(yyjson_get_uint(value)), chord)) chord = {};
        parsed.hotkeys[index] = chord;
    }
    // Retain the first binding and clear duplicate later entries. Never
    // silently steal another action's key or change the legacy source file.
    for (size_t index = 0; index < kHotkeyCount; ++index) {
        if (!parsed.hotkeys[index].key) continue;
        for (size_t earlier = 0; earlier < index; ++earlier)
            if (parsed.hotkeys[index] == parsed.hotkeys[earlier]) { parsed.hotkeys[index] = {}; break; }
    }
    auto values = UniqueSettingsMember(root, "values", valid);
    if (values && !yyjson_is_null(values) && !yyjson_is_obj(values)) valid = false;
    for (size_t index = 0; index < kPreferenceValueCount; ++index) {
        auto value = UniqueSettingsMember(values, PreferenceValueId(index), valid);
        if (yyjson_is_num(value) && ValidPreferenceValue(index, yyjson_get_num(value))) parsed.values[index] = yyjson_get_num(value);
    }
    yyjson_doc_free(document);
    if (!valid) { error = "Settings format or duplicate fields are invalid."; return false; }
    result = std::move(parsed); return true;
}
inline std::string EncodeClientSettings(const ClientSettings& settings) {
    yyjson_mut_doc* document = yyjson_mut_doc_new(nullptr);
    if (!document) return {};
    auto root = yyjson_mut_obj(document); yyjson_mut_doc_set_root(document, root);
    yyjson_mut_obj_add_uint(document, root, "formatVersion", 1);
    yyjson_mut_obj_add_str(document, root, "language", settings.english ? "en" : "zh-CN");
    yyjson_mut_obj_add_strcpy(document, root, "rewardTargetItemId", settings.reward_target_id.c_str());
    auto bindings = yyjson_mut_obj(document); yyjson_mut_obj_add_val(document, root, "hotkeys", bindings);
    for (size_t index = 0; index < kHotkeyCount; ++index)
        yyjson_mut_obj_add(bindings, yyjson_mut_strcpy(document, HotkeyActionId(index).c_str()), yyjson_mut_uint(document, EncodeHotkey(settings.hotkeys[index])));
    auto values = yyjson_mut_obj(document); yyjson_mut_obj_add_val(document, root, "values", values);
    for (size_t index = 0; index < kPreferenceValueCount; ++index)
        yyjson_mut_obj_add(values, yyjson_mut_strcpy(document, PreferenceValueId(index).c_str()), yyjson_mut_real(document, settings.values[index]));
    size_t size = 0; char* text = yyjson_mut_write(document, YYJSON_WRITE_PRETTY, &size);
    std::string result = text ? std::string(text, size) + "\n" : "";
    free(text); yyjson_mut_doc_free(document);
    return result;
}
inline bool CheckClientSettingsPolicy() {
    ClientSettings parsed; std::string error;
    std::string legacy = R"({"language":"en","gameDirectory":"private-ignored","enabled":true,"hotkeys":{"health":113,"instantKill":113,"setGold":114,"stat:seal":115},"values":{"movement":0.5,"damage":2000,"setGold":999}})";
    if (!ParseClientSettings(legacy, true, parsed, error) || !parsed.english || parsed.values[2] != 0.5
        || parsed.values[1] != 2 || parsed.hotkeys[0].key != VK_F2 || parsed.hotkeys[1].key != 0) return false;
    parsed.reward_target_id = "HOLYBOOK";
    auto saved = EncodeClientSettings(parsed);
    if (saved.empty() || saved.find("private-ignored") != std::string::npos || saved.find("gameDirectory") != std::string::npos
        || saved.find("enabled") != std::string::npos || saved.find("setGold") != std::string::npos || saved.find("stat:seal") != std::string::npos) return false;
    ClientSettings roundtrip;
    if (!ParseClientSettings(saved, false, roundtrip, error) || roundtrip.hotkeys != parsed.hotkeys || roundtrip.values != parsed.values || !roundtrip.english || roundtrip.reward_target_id != "HOLYBOOK") return false;
    if (ParseClientSettings(R"({"formatVersion":2})", false, parsed, error)
        || ParseClientSettings(R"({"formatVersion":1,"language":"en","Language":"zh-CN"})", false, parsed, error)
        || ParseClientSettings(R"({"formatVersion":1,"hotkeys":{"health":112,"health":113}})", false, parsed, error)
        || ParseClientSettings(R"({"formatVersion":1,"rewardTargetItemId":"HOLYBOOK","RewardTargetItemId":"SPINACH"})", false, parsed, error)) return false;
    if (!ParseClientSettings(R"({"formatVersion":1,"rewardTargetItemId":"../bad","rewardEnabled":true,"upgradeEnabled":true})", false, parsed, error)
        || !parsed.reward_target_id.empty()) return false;
    for (size_t index = 0; index < kPreferenceValueCount; ++index) if (PreferenceValueId(index) == "setGold") return false;
    return true;
}

} // namespace vst::client
