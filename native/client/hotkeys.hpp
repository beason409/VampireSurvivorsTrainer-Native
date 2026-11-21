#pragma once

#include "../common/stat_catalog.hpp"
#include "../common/chest_animation.hpp"
#include "../common/chest_rewards.hpp"
#include "../common/game_speed.hpp"
#include <windows.h>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace vst::client {

struct HotkeyChord {
    UINT key = 0, modifiers = 0;
    bool operator==(const HotkeyChord&) const = default;
};

inline constexpr size_t kFeatureHotkeyCount = 10;
inline constexpr size_t kHotkeyCount = kFeatureHotkeyCount + kNativeStatCount;
inline constexpr int kHotkeyMessageBase = 0x5100;
inline constexpr std::array<const char*, kFeatureHotkeyCount> kFeatureHotkeyIds{
    "health", "instantKill", "experience", "damage", "movement", "cooldown", "noGoldSpend", kGameSpeedId, kSkipChestAnimationId, kFiveItemChestsId
};
using HotkeyBindings = std::array<HotkeyChord, kHotkeyCount>;

inline bool HasUsableHotkey(const HotkeyBindings& bindings, const std::array<bool, kHotkeyCount>& eligible) noexcept {
    for (size_t action = 0; action < bindings.size(); ++action)
        if (eligible[action] && bindings[action].key) return true;
    return false;
}

inline std::string HotkeyActionId(size_t action) {
    return action < kFeatureHotkeyCount ? std::string(kFeatureHotkeyIds[action])
        : action < kHotkeyCount ? "stat:" + std::string(kNativeStats[action - kFeatureHotkeyCount].id) : "";
}
inline HotkeyBindings DefaultHotkeys() {
    HotkeyBindings result{};
    for (size_t index = 0; index < 7; ++index) result[index].key = VK_F1 + static_cast<UINT>(index);
    return result;
}
inline bool ValidHotkey(HotkeyChord chord) {
    if (!chord.key) return chord.modifiers == 0;
    if ((chord.modifiers & ~(MOD_CONTROL | MOD_ALT | MOD_SHIFT)) != 0
        || (chord.key == VK_F4 && (chord.modifiers & MOD_ALT))
        || (chord.key == VK_DELETE && (chord.modifiers & (MOD_CONTROL | MOD_ALT)) == (MOD_CONTROL | MOD_ALT))) return false;
    if ((chord.key >= VK_F1 && chord.key <= VK_F24) || (chord.key >= VK_NUMPAD0 && chord.key <= VK_DIVIDE)) return true;
    if (!chord.modifiers) return false;
    if ((chord.key >= L'A' && chord.key <= L'Z') || (chord.key >= L'0' && chord.key <= L'9')
        || (chord.key >= VK_SPACE && chord.key <= VK_HELP) || chord.key == VK_BACK || chord.key == VK_RETURN) return true;
    switch (chord.key) {
        case VK_OEM_1: case VK_OEM_PLUS: case VK_OEM_COMMA: case VK_OEM_MINUS: case VK_OEM_PERIOD:
        case VK_OEM_2: case VK_OEM_3: case VK_OEM_4: case VK_OEM_5: case VK_OEM_6:
        case VK_OEM_7: case VK_OEM_8: case VK_OEM_102: return true;
        default: return false;
    }
}
inline uint32_t EncodeHotkey(HotkeyChord chord) {
    return chord.key | ((chord.modifiers & MOD_SHIFT) ? 0x10000u : 0u)
        | ((chord.modifiers & MOD_CONTROL) ? 0x20000u : 0u) | ((chord.modifiers & MOD_ALT) ? 0x40000u : 0u);
}
inline bool DecodeHotkey(uint32_t stored, HotkeyChord& chord) {
    if (stored & ~0x7FFFFu) return false;
    chord = {stored & 0xFFFFu, ((stored & 0x10000u) ? MOD_SHIFT : 0u)
        | ((stored & 0x20000u) ? MOD_CONTROL : 0u) | ((stored & 0x40000u) ? MOD_ALT : 0u)};
    return ValidHotkey(chord);
}
inline size_t ConflictingHotkey(const HotkeyBindings& bindings, size_t target, HotkeyChord chord) {
    if (!chord.key) return kHotkeyCount;
    for (size_t index = 0; index < bindings.size(); ++index) if (index != target && bindings[index] == chord) return index;
    return kHotkeyCount;
}
inline std::wstring HotkeyLabel(HotkeyChord chord, bool english) {
    if (!chord.key) return english ? L"Unbound" : L"未绑定";
    std::wstring label;
    if (chord.modifiers & MOD_CONTROL) label += L"Ctrl+";
    if (chord.modifiers & MOD_ALT) label += L"Alt+";
    if (chord.modifiers & MOD_SHIFT) label += L"Shift+";
    if (chord.key >= VK_F1 && chord.key <= VK_F24) return label + L"F" + std::to_wstring(chord.key - VK_F1 + 1);
    if (chord.key >= VK_NUMPAD0 && chord.key <= VK_NUMPAD9) return label + L"Num " + std::to_wstring(chord.key - VK_NUMPAD0);
    if ((chord.key >= L'A' && chord.key <= L'Z') || (chord.key >= L'0' && chord.key <= L'9')) return label + static_cast<wchar_t>(chord.key);
    switch (chord.key) {
        case VK_MULTIPLY: return label + L"Num *";
        case VK_ADD: return label + L"Num +";
        case VK_SUBTRACT: return label + L"Num -";
        case VK_DECIMAL: return label + L"Num .";
        case VK_DIVIDE: return label + L"Num /";
        default: break;
    }
    wchar_t name[64]{};
    UINT scan = MapVirtualKeyW(chord.key, MAPVK_VK_TO_VSC);
    if (chord.key >= VK_PRIOR && chord.key <= VK_DOWN) scan |= 0x100;
    if (chord.key == VK_INSERT || chord.key == VK_DELETE) scan |= 0x100;
    return GetKeyNameTextW(static_cast<LONG>(scan << 16), name, static_cast<int>(std::size(name))) > 0
        ? label + name : label + L"Key " + std::to_wstring(chord.key);
}

// Registration exists only while the supported game owns the foreground.
// The platform callbacks keep OS registration out of isolated policy tests.
class HotkeyManager {
    HotkeyBindings previous_{};
    std::array<bool, kHotkeyCount> registered_{}, pressed_{}, eligible_{};
    std::array<DWORD, kHotkeyCount> errors_{};
    DWORD game_pid_ = 0;
    bool allowed_ = false;
public:
    template<class Unregister>
    void Suspend(Unregister unregister) {
        for (size_t index = 0; index < kHotkeyCount; ++index) {
            if (registered_[index]) unregister(index);
            registered_[index] = pressed_[index] = eligible_[index] = false;
        }
        game_pid_ = 0; allowed_ = false;
    }
    template<class KeyDown, class Register, class Unregister>
    bool Update(DWORD game_pid, bool allowed, const HotkeyBindings& bindings,
            const std::array<bool, kHotkeyCount>& eligible, KeyDown down, Register register_key, Unregister unregister) {
        bool changed = false;
        if (!allowed || !game_pid) {
            for (bool registered : registered_) changed = changed || registered;
            Suspend(unregister);
            return changed;
        }
        bool new_context = !allowed_ || game_pid_ != game_pid;
        if (new_context) { Suspend(unregister); errors_ = {}; }
        allowed_ = true; game_pid_ = game_pid;
        for (size_t index = 0; index < kHotkeyCount; ++index) {
            bool binding_changed = bindings[index] != previous_[index];
            bool became_eligible = eligible[index] && !eligible_[index];
            if (binding_changed || !eligible[index]) {
                if (registered_[index]) unregister(index);
                changed = changed || registered_[index] || binding_changed;
                registered_[index] = pressed_[index] = false;
            }
            if (binding_changed || became_eligible) { changed = changed || errors_[index] != 0; errors_[index] = 0; }
            previous_[index] = bindings[index]; eligible_[index] = eligible[index];
            HotkeyChord chord = bindings[index];
            if (!eligible[index] || !chord.key || !ValidHotkey(chord)) continue;
            if (registered_[index]) { if (!down(chord.key)) pressed_[index] = false; continue; }
            // A chord held while focus/session returns must first be released.
            if (errors_[index] || down(chord.key)) continue;
            DWORD error = 0;
            registered_[index] = register_key(index, chord, error);
            errors_[index] = registered_[index] ? 0 : (error ? error : ERROR_INVALID_FUNCTION);
            changed = true;
        }
        return changed;
    }
    template<class KeyDown>
    bool Accept(size_t action, HotkeyChord message, DWORD foreground_pid, bool allowed,
            const std::array<bool, kHotkeyCount>& eligible, KeyDown down) {
        if (action >= kHotkeyCount || !allowed || !allowed_ || foreground_pid != game_pid_
            || !registered_[action] || !eligible[action] || pressed_[action] || message != previous_[action]) return false;
        // WM_HOTKEY records the registered chord at its key-down edge. The
        // key and its modifiers may be released before this queued event is
        // dispatched; polling them again here would discard a fast tap.
        // Update() still requires release before registering after a resume.
        if (down(VK_LWIN) || down(VK_RWIN)) return false;
        pressed_[action] = true;
        return true;
    }
    DWORD Error(size_t action) const { return action < errors_.size() ? errors_[action] : ERROR_INVALID_INDEX; }
};

inline bool CheckHotkeyPolicy() {
    auto bindings = DefaultHotkeys();
    std::array<bool, kHotkeyCount> menu{};
    if (HasUsableHotkey(bindings, menu)) return false;
    menu[0] = true;
    if (!HasUsableHotkey(bindings, menu) || HasUsableHotkey({}, menu)) return false;
    if (HotkeyActionId(0) != "health" || HotkeyActionId(1) != "instantKill" || HotkeyActionId(6) != "noGoldSpend"
        || HotkeyActionId(7) != kGameSpeedId || bindings[7].key
        || HotkeyActionId(8) != kSkipChestAnimationId || bindings[8].key
        || HotkeyActionId(9) != kFiveItemChestsId || bindings[9].key
        || HotkeyActionId(kFeatureHotkeyCount) != "stat:maxHealth" || HotkeyActionId(kHotkeyCount) != "") return false;
    HotkeyChord chord;
    if (!DecodeHotkey(0x20000u | L'A', chord) || chord != HotkeyChord{L'A', MOD_CONTROL}
        || DecodeHotkey(0x80000u | VK_F1, chord) || ValidHotkey({VK_F4, MOD_ALT})
        || ValidHotkey({VK_DELETE, MOD_CONTROL | MOD_ALT}) || ValidHotkey({L'A', 0})
        || !ValidHotkey({VK_F24, 0}) || !ValidHotkey({VK_NUMPAD5, 0})
        || ConflictingHotkey(bindings, 2, bindings[0]) != 0) return false;
    HotkeyManager manager;
    std::array<bool, kHotkeyCount> eligible{}; eligible[0] = eligible[1] = true;
    std::array<bool, 256> held{};
    unsigned registered = 0, removed = 0;
    bool fail_second = true;
    auto down = [&](UINT key) { return key < held.size() && held[key]; };
    auto add = [&](size_t index, HotkeyChord, DWORD& error) { if (index == 1 && fail_second) { error = ERROR_HOTKEY_ALREADY_REGISTERED; return false; } ++registered; return true; };
    auto remove = [&](size_t) { ++removed; };
    held[VK_F1] = true;
    manager.Update(10, true, bindings, eligible, down, add, remove);
    if (registered != 0 || manager.Error(1) != ERROR_HOTKEY_ALREADY_REGISTERED
        || manager.Accept(0, bindings[0], 10, true, eligible, down)) return false;
    held[VK_F1] = false; manager.Update(10, true, bindings, eligible, down, add, remove);
    held[VK_F1] = true;
    if (!manager.Accept(0, bindings[0], 10, true, eligible, down)
        || manager.Accept(0, bindings[0], 10, true, eligible, down)
        || manager.Accept(0, bindings[0], 99, true, eligible, down)) return false;
    held[VK_F1] = false; manager.Update(10, true, bindings, eligible, down, add, remove);
    held[VK_F1] = true;
    if (manager.Accept(0, {VK_F1, MOD_SHIFT}, 10, true, eligible, down)) return false;
    manager.Update(10, false, bindings, eligible, down, add, remove);
    if (!removed || manager.Accept(0, bindings[0], 10, true, eligible, down)) return false;
    fail_second = false; held[VK_F1] = false; manager.Update(11, true, bindings, eligible, down, add, remove);
    held[VK_F1] = true; held[VK_LWIN] = true;
    if (manager.Error(1) != 0 || manager.Accept(0, bindings[0], 11, true, eligible, down)) return false;
    // A valid key-down event must survive key-up before message dispatch.
    held = {}; manager.Update(11, true, bindings, eligible, down, add, remove);
    if (!manager.Accept(0, bindings[0], 11, true, eligible, down)
        || manager.Accept(0, bindings[0], 11, true, eligible, down)) return false;
    manager.Update(11, false, bindings, eligible, down, add, remove);
    bindings[0] = {VK_F8, MOD_CONTROL | MOD_SHIFT};
    manager.Update(12, true, bindings, eligible, down, add, remove);
    // Both the base key and Ctrl/Shift have already been released. Use the
    // event's exact chord, while continuing to reject a mismatched payload.
    if (manager.Accept(0, {VK_F8, 0}, 12, true, eligible, down)
        || !manager.Accept(0, bindings[0], 12, true, eligible, down)
        || manager.Accept(0, bindings[0], 12, true, eligible, down)) return false;
    manager.Update(12, false, bindings, eligible, down, add, remove);
    held[VK_F8] = true;
    manager.Update(12, true, bindings, eligible, down, add, remove);
    if (manager.Accept(0, bindings[0], 12, true, eligible, down)) return false;
    held[VK_F8] = false; manager.Update(12, true, bindings, eligible, down, add, remove);
    if (manager.Accept(0, bindings[0], 99, true, eligible, down)
        || manager.Accept(0, bindings[0], 12, false, eligible, down)) return false;
    eligible[0] = false;
    if (manager.Accept(0, bindings[0], 12, true, eligible, down)) return false;
    eligible[0] = true;
    return manager.Accept(0, bindings[0], 12, true, eligible, down);
}

} // namespace vst::client
