#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace vst {

enum class NativeStat : uint8_t {
    MaxHealth, Armor, Recovery, Might, ProjectileSpeed, Duration, Area,
    Cooldown, Amount, MoveSpeed, Luck, Growth, Greed, Curse, Omni,
    Magnet, Charm, Revivals, Rerolls, Skips, Banishes, Preserve, Defang
};
inline constexpr size_t kNativeStatCount = 23;
enum class StatValueKind : uint8_t { Absolute, BonusPercent, ReductionPercent, ChancePercent };

struct NativeStatDefinition {
    NativeStat stat;
    const char* id;
    const wchar_t* name_zh;
    const wchar_t* name_en;
    const wchar_t* unit;
    double minimum, maximum, initial;
    int decimals;
    StatValueKind kind;
};

inline constexpr std::array<NativeStatDefinition, kNativeStatCount> kNativeStats{{
    {NativeStat::MaxHealth, "maxHealth", L"生命上限", L"Maximum health", L"HP", 1, 1000000, 500, 2, StatValueKind::Absolute},
    {NativeStat::Armor, "armor", L"护甲", L"Armor", L"", 0, 10000, 20, 2, StatValueKind::Absolute},
    {NativeStat::Recovery, "recovery", L"生命恢复", L"Recovery", L"HP/s", 0, 10000, 5, 2, StatValueKind::Absolute},
    {NativeStat::Might, "might", L"力量加成", L"Might bonus", L"%", -100, 900, 100, 1, StatValueKind::BonusPercent},
    {NativeStat::ProjectileSpeed, "projectileSpeed", L"弹射速度加成", L"Projectile speed bonus", L"%", -100, 400, 100, 1, StatValueKind::BonusPercent},
    {NativeStat::Duration, "duration", L"持续时间加成", L"Duration bonus", L"%", -100, 400, 100, 1, StatValueKind::BonusPercent},
    {NativeStat::Area, "area", L"攻击范围加成", L"Area bonus", L"%", -100, 900, 100, 1, StatValueKind::BonusPercent},
    {NativeStat::Cooldown, "cooldown", L"冷却缩减", L"Cooldown reduction", L"%", 0, 90, 90, 1, StatValueKind::ReductionPercent},
    {NativeStat::Amount, "amount", L"额外发射数量", L"Additional projectiles", L"", 0, 100, 10, 0, StatValueKind::Absolute},
    {NativeStat::MoveSpeed, "moveSpeed", L"移动速度加成", L"Movement speed bonus", L"%", -90, 1000, 200, 1, StatValueKind::BonusPercent},
    {NativeStat::Luck, "luck", L"幸运加成", L"Luck bonus", L"%", -100, 10000, 100, 1, StatValueKind::BonusPercent},
    {NativeStat::Growth, "growth", L"经验成长加成", L"Growth bonus", L"%", 0, 10000, 100, 1, StatValueKind::BonusPercent},
    {NativeStat::Greed, "greed", L"金币收益加成", L"Greed bonus", L"%", 0, 10000, 100, 1, StatValueKind::BonusPercent},
    {NativeStat::Curse, "curse", L"诅咒加成", L"Curse bonus", L"%", -100, 10000, 0, 1, StatValueKind::BonusPercent},
    {NativeStat::Omni, "omni", L"万能统一加成", L"Unified bonuses", L"%", 0, 400, 100, 1, StatValueKind::BonusPercent},
    {NativeStat::Magnet, "magnet", L"拾取范围", L"Pickup radius", L"", 0, 10000, 300, 1, StatValueKind::Absolute},
    {NativeStat::Charm, "charm", L"魅力", L"Charm", L"", 0, 1000, 100, 0, StatValueKind::Absolute},
    {NativeStat::Revivals, "revivals", L"复活次数", L"Revivals", L"", 0, 1000, 10, 0, StatValueKind::Absolute},
    {NativeStat::Rerolls, "rerolls", L"重选次数", L"Rerolls", L"", 0, 10000, 100, 0, StatValueKind::Absolute},
    {NativeStat::Skips, "skips", L"跳过次数", L"Skips", L"", 0, 10000, 100, 0, StatValueKind::Absolute},
    {NativeStat::Banishes, "banishes", L"排除次数", L"Banishes", L"", 0, 10000, 100, 0, StatValueKind::Absolute},
    {NativeStat::Preserve, "preserve", L"次数保留概率", L"Resource preservation", L"%", 0, 100, 100, 1, StatValueKind::ChancePercent},
    {NativeStat::Defang, "defang", L"去牙概率", L"Defang chance", L"%", 0, 100, 100, 1, StatValueKind::ChancePercent}
}};

inline constexpr bool IsOmniMember(NativeStat stat) noexcept {
    return stat == NativeStat::Might || stat == NativeStat::ProjectileSpeed
        || stat == NativeStat::Duration || stat == NativeStat::Area;
}
inline const NativeStatDefinition* FindNativeStat(std::string_view id) noexcept {
    for (const auto& definition : kNativeStats) if (id == definition.id) return &definition;
    return nullptr;
}
inline constexpr double StatToRaw(const NativeStatDefinition& definition, double value) noexcept {
    return definition.kind == StatValueKind::BonusPercent ? 1 + value / 100
        : definition.kind == StatValueKind::ReductionPercent ? 1 - value / 100
        : definition.kind == StatValueKind::ChancePercent ? value / 100 : value;
}
inline constexpr double StatFromRaw(const NativeStatDefinition& definition, double value) noexcept {
    return definition.kind == StatValueKind::BonusPercent ? (value - 1) * 100
        : definition.kind == StatValueKind::ReductionPercent ? (1 - value) * 100
        : definition.kind == StatValueKind::ChancePercent ? value * 100 : value;
}
inline bool ValidStatValue(const NativeStatDefinition& definition, double value) noexcept {
    return std::isfinite(value) && value >= definition.minimum && value <= definition.maximum
        && (definition.decimals != 0 || value == std::trunc(value));
}

} // namespace vst
