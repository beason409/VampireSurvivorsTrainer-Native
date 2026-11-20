#pragma once

#include "../common/runtime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string_view>
#include <utility>

namespace vst_stat_tests {
using vst::CombatFeature;
using vst::GameObservation;
using vst::NativeStat;
using vst::Request;
using vst::Runtime;
using vst::StateSnapshot;

inline bool CheckStat(bool passed, const char* expression, int line) {
    if (!passed) std::fprintf(stderr, "  stat fixture line %d: %s\n", line, expression);
    return passed;
}
#define VST_STAT_CHECK(...) do { if (!CheckStat((__VA_ARGS__), #__VA_ARGS__, __LINE__)) return false; } while (0)

inline constexpr uintptr_t kOwner = 0x51000;
inline size_t Index(NativeStat stat) { return static_cast<size_t>(stat); }
inline bool Near(double actual, double expected) {
    return std::isfinite(actual) && std::abs(actual - expected) <= 0.0001 * std::max(1.0, std::abs(expected));
}

struct ExpectedStat {
    const char* id;
    double minimum, maximum, initial;
    int decimals;
    double target, raw, alternate;
};
// These values describe the public units/ranges, independently of the native
// catalogue. Runtime commands below must enforce the same contract.
inline constexpr std::array<ExpectedStat, 23> kExpected{{
    {"maxHealth", 1, 1000000, 500, 2, 500, 500, 50},
    {"armor", 0, 10000, 20, 2, 20, 20, 30},
    {"recovery", 0, 10000, 5, 2, 5, 5, 10},
    {"might", -100, 900, 100, 1, 100, 2, 50},
    {"projectileSpeed", -100, 400, 100, 1, 25, 1.25, 50},
    {"duration", -100, 400, 100, 1, 50, 1.5, 75},
    {"area", -100, 900, 100, 1, 100, 2, 200},
    {"cooldown", 0, 90, 90, 1, 90, 0.1, 50},
    {"amount", 0, 100, 10, 0, 10, 10, 20},
    {"moveSpeed", -90, 1000, 200, 1, 200, 3, 100},
    {"luck", -100, 10000, 100, 1, 100, 2, 50},
    {"growth", 0, 10000, 100, 1, 100, 2, 50},
    {"greed", 0, 10000, 100, 1, 150, 2.5, 50},
    {"curse", -100, 10000, 0, 1, 50, 1.5, 75},
    {"omni", 0, 400, 100, 1, 100, 2, 200},
    {"magnet", 0, 10000, 300, 1, 300, 300, 150},
    {"charm", 0, 1000, 100, 0, 100, 100, 50},
    {"revivals", 0, 1000, 10, 0, 10, 10, 5},
    {"rerolls", 0, 10000, 100, 0, 100, 100, 50},
    {"skips", 0, 10000, 100, 0, 100, 100, 50},
    {"banishes", 0, 10000, 100, 0, 100, 100, 50},
    {"preserve", 0, 100, 100, 1, 75, 0.75, 50},
    {"defang", 0, 100, 100, 1, 50, 0.5, 25}
}};
static_assert(kExpected.size() == vst::kNativeStatCount);

inline GameObservation StatObservation(uintptr_t owner = kOwner, bool samples_known = true) {
    GameObservation result;
    result.verified = result.safe_mode = result.player_ready = result.paused_known = true;
    result.paused = false;
    result.player = owner;
    result.scene = "Native stat fixture";
    result.armor_original = 4;
    for (const auto& definition : vst::kNativeStats) {
        auto index = Index(definition.stat);
        result.stat_original[index] = 1.1f;
        result.stat_known[index] = samples_known && definition.stat != NativeStat::Omni;
    }
    result.stat_original[Index(NativeStat::MaxHealth)] = 160;
    result.stat_original[Index(NativeStat::Armor)] = 4;
    result.stat_original[Index(NativeStat::Recovery)] = 0.6f;
    result.stat_original[Index(NativeStat::Cooldown)] = 0.95f;
    result.stat_original[Index(NativeStat::Amount)] = 1;
    return result;
}

inline void ReadyStats(Runtime& runtime, uint64_t now = 1000, bool samples_known = true) {
    for (const auto& definition : vst::kNativeStats) runtime.SetStatSupported(definition.stat, true);
    for (auto feature : {CombatFeature::Health, CombatFeature::Experience, CombatFeature::Damage,
            CombatFeature::Movement, CombatFeature::Cooldown, CombatFeature::InstantKill})
        runtime.SetFeatureSupported(feature, true);
    runtime.Observe(StatObservation(kOwner, samples_known), now);
}

inline Request StatCommand(const char* kind) {
    Request result;
    result.id = "stat-fixture";
    result.kind = kind;
    return result;
}

inline Request EditStat(NativeStat stat, bool enabled, double value = 0) {
    auto result = StatCommand("setStat");
    result.stat = vst::kNativeStats[Index(stat)].id;
    result.has_enabled = true;
    result.enabled = enabled;
    result.has_value = enabled;
    result.value = value;
    return result;
}

inline Request StatValueOnly(NativeStat stat, double value) {
    auto result = StatCommand("setStat");
    result.stat = vst::kNativeStats[Index(stat)].id;
    result.has_value = true;
    result.value = value;
    return result;
}

inline Request FeatureCommand(const char* id, bool enabled, double value = 1) {
    auto result = StatCommand("setFeature");
    result.feature = id;
    result.has_enabled = true;
    result.enabled = enabled;
    result.has_value = enabled;
    result.value = value;
    return result;
}

inline bool Apply(Runtime& runtime, uint64_t session, Request request, uint64_t now = 1000) {
    auto ticket = runtime.Submit(std::move(request), session, now);
    runtime.Pump(now);
    return runtime.Await(ticket, 0).ok;
}

inline bool EnableEveryStat(Runtime& runtime, uint64_t session, uint64_t now = 1000) {
    for (size_t index = 0; index < kExpected.size(); ++index)
        if (!Apply(runtime, session, EditStat(static_cast<NativeStat>(index), true, kExpected[index].target), now)) return false;
    return true;
}

inline bool AllStatsEnabled(const StateSnapshot& state, bool expected) {
    for (const auto& stat : state.stats) if (stat.enabled != expected) return false;
    return state.armor_enabled == expected;
}

inline bool CatalogBoundsAndRawUnits() {
    for (size_t index = 0; index < kExpected.size(); ++index) {
        const auto& expected = kExpected[index];
        const auto& definition = vst::kNativeStats[index];
        auto stat = static_cast<NativeStat>(index);
        VST_STAT_CHECK(std::string_view(definition.id) == expected.id && definition.stat == stat);
        VST_STAT_CHECK(definition.minimum == expected.minimum && definition.maximum == expected.maximum);
        VST_STAT_CHECK(definition.initial == expected.initial && definition.decimals == expected.decimals);
        VST_STAT_CHECK(vst::FindNativeStat(expected.id) == &definition);
        VST_STAT_CHECK(Near(vst::StatToRaw(definition, expected.target), expected.raw));
        VST_STAT_CHECK(Near(vst::StatFromRaw(definition, expected.raw), expected.target));

        Runtime bounds;
        ReadyStats(bounds);
        auto session = bounds.BeginSession(1000);
        VST_STAT_CHECK(Apply(bounds, session, EditStat(stat, true, expected.minimum)));
        VST_STAT_CHECK(bounds.Snapshot(1000).stats[index].enabled);
        VST_STAT_CHECK(bounds.StatValue(stat) == expected.minimum);
        VST_STAT_CHECK(Apply(bounds, session, EditStat(stat, true, expected.maximum), 1001));
        VST_STAT_CHECK(bounds.Snapshot(1001).stats[index].enabled);
        VST_STAT_CHECK(bounds.StatValue(stat) == expected.maximum);
        for (double bad : {expected.minimum - 1, expected.maximum + 1,
                std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
            auto response = bounds.Await(bounds.Submit(EditStat(stat, true, bad), session, 1002), 0);
            VST_STAT_CHECK(!response.ok && bounds.StatValue(stat) == expected.maximum);
            bounds.Pump(1002);
            VST_STAT_CHECK(!bounds.Snapshot(1002).stats[index].enabled);
        }
    }
    Runtime counts;
    ReadyStats(counts);
    auto session = counts.BeginSession(1000);
    VST_STAT_CHECK(!counts.Await(counts.Submit(EditStat(NativeStat::Amount, true, 1.5), session, 1000), 0).ok);
    VST_STAT_CHECK(Apply(counts, session, EditStat(NativeStat::Recovery, true, 1.5)));
    VST_STAT_CHECK(Near(counts.FilterStat(NativeStat::Recovery, kOwner, 0.6f, 1000), 1.5));
    VST_STAT_CHECK(!vst::FindNativeStat("seal") && vst::FindNativeStat("magnet") && vst::FindNativeStat("charm"));
    return true;
}

inline bool StatGettersComposeWithCommonEffects() {
    Runtime runtime;
    ReadyStats(runtime);
    auto session = runtime.BeginSession(1000);
    VST_STAT_CHECK(EnableEveryStat(runtime, session));
    auto observation = StatObservation();
    for (size_t index = 0; index < kExpected.size(); ++index) {
        auto stat = static_cast<NativeStat>(index);
        if (stat == NativeStat::Omni) continue;
        VST_STAT_CHECK(Near(runtime.FilterStat(stat, kOwner, observation.stat_original[index], 1000), kExpected[index].raw));
        auto sample = runtime.Snapshot(1000).stats[index];
        VST_STAT_CHECK(sample.enabled && sample.current_known && Near(sample.current, kExpected[index].target));
    }
    VST_STAT_CHECK(Apply(runtime, session, FeatureCommand("movement", true, 2)));
    VST_STAT_CHECK(Apply(runtime, session, FeatureCommand("experience", true, 3)));
    VST_STAT_CHECK(Apply(runtime, session, FeatureCommand("damage", true, 3)));
    VST_STAT_CHECK(Near(runtime.FilterMovement(kOwner, runtime.FilterStat(NativeStat::MoveSpeed, kOwner, 1.1f, 1000), 1000), 6));
    VST_STAT_CHECK(Near(runtime.FilterExperience(kOwner, runtime.FilterStat(NativeStat::Growth, kOwner, 1.1f, 1000), 1000), 6));
    auto damage = runtime.FilterDamage(kOwner, runtime.FilterStat(NativeStat::Might, kOwner, 1.1f, 1000), true, 1000);
    VST_STAT_CHECK(Near(damage, 6));
    VST_STAT_CHECK(Apply(runtime, session, FeatureCommand("instantKill", true)));
    VST_STAT_CHECK(Near(runtime.FilterInstantKill(kOwner, damage, 20, true, 1000), 2000));
    VST_STAT_CHECK(Apply(runtime, session, FeatureCommand("instantKill", false)));
    VST_STAT_CHECK(Near(runtime.FilterInstantKill(kOwner, damage, 20, true, 1000), 6));

    VST_STAT_CHECK(Apply(runtime, session, EditStat(NativeStat::MoveSpeed, false)));
    VST_STAT_CHECK(Near(runtime.FilterMovement(kOwner, runtime.FilterStat(NativeStat::MoveSpeed, kOwner, 1.1f, 1000), 1000), 2.2));
    VST_STAT_CHECK(Apply(runtime, session, EditStat(NativeStat::MoveSpeed, true, 200)));
    VST_STAT_CHECK(Apply(runtime, session, FeatureCommand("movement", false)));
    VST_STAT_CHECK(Near(runtime.FilterMovement(kOwner, runtime.FilterStat(NativeStat::MoveSpeed, kOwner, 1.1f, 1000), 1000), 3));

    float interval = 1000 * runtime.FilterStatFinal(NativeStat::Cooldown, kOwner, 0.95f, 0.2f, 1000);
    VST_STAT_CHECK(Near(interval, 200));
    VST_STAT_CHECK(Apply(runtime, session, FeatureCommand("cooldown", true)));
    VST_STAT_CHECK(Near(runtime.FilterCooldown(kOwner, interval, true, 1000), 100));
    VST_STAT_CHECK(Apply(runtime, session, FeatureCommand("cooldown", false)));
    VST_STAT_CHECK(Near(runtime.FilterCooldown(kOwner, interval, true, 1000), 200));
    return true;
}

inline bool OmniOverridesRespectIndividualAndCapabilityBoundaries() {
    for (auto missing : {NativeStat::Might, NativeStat::ProjectileSpeed, NativeStat::Duration, NativeStat::Area}) {
        Runtime runtime;
        ReadyStats(runtime);
        auto session = runtime.BeginSession(1000);
        VST_STAT_CHECK(Apply(runtime, session, EditStat(NativeStat::Omni, true, 100)));
        VST_STAT_CHECK(Near(runtime.FilterStat(NativeStat::Might, kOwner, 1.1f, 1000), 2));
        VST_STAT_CHECK(runtime.Snapshot(1000).stats[Index(NativeStat::Might)].source == NativeStat::Omni);
        VST_STAT_CHECK(runtime.Snapshot(1000).stats[Index(NativeStat::Omni)].game_reads == 1);
        VST_STAT_CHECK(Apply(runtime, session, EditStat(NativeStat::Might, true, 25)));
        VST_STAT_CHECK(Near(runtime.FilterStat(NativeStat::Might, kOwner, 1.1f, 1000), 1.25));
        VST_STAT_CHECK(runtime.Snapshot(1000).stats[Index(NativeStat::Might)].source == NativeStat::Might);
        VST_STAT_CHECK(runtime.Snapshot(1000).stats[Index(NativeStat::Omni)].game_reads == 1);
        VST_STAT_CHECK(Apply(runtime, session, StatValueOnly(NativeStat::Omni, 200)));
        VST_STAT_CHECK(Near(runtime.FilterStat(NativeStat::Might, kOwner, 1.1f, 1000), 1.25));
        VST_STAT_CHECK(Near(runtime.FilterStat(NativeStat::ProjectileSpeed, kOwner, 1.1f, 1000), 3));
        VST_STAT_CHECK(Near(runtime.FilterStatFinal(NativeStat::Area, kOwner, 1, 2, 1000), 6));
        VST_STAT_CHECK(Apply(runtime, session, EditStat(NativeStat::Might, false)));
        VST_STAT_CHECK(Near(runtime.FilterStat(NativeStat::Might, kOwner, 1.1f, 1000), 3));
        VST_STAT_CHECK(Apply(runtime, session, EditStat(NativeStat::Recovery, true, 5)));
        auto pending = runtime.Submit(EditStat(NativeStat::Omni, true, 100), session, 1001);
        runtime.SetStatSupported(missing, false, "Fixture route unavailable");
        VST_STAT_CHECK(!runtime.Await(pending, 0).ok);
        auto snapshot = runtime.Snapshot(1001);
        VST_STAT_CHECK(!snapshot.stats[Index(NativeStat::Omni)].supported && !snapshot.stats[Index(NativeStat::Omni)].enabled);
        VST_STAT_CHECK(snapshot.stats[Index(NativeStat::Recovery)].enabled);
        VST_STAT_CHECK(Near(runtime.FilterStat(NativeStat::Recovery, kOwner, 0.6f, 1001), 5));
        VST_STAT_CHECK(Near(runtime.FilterStat(NativeStat::Might, kOwner, 1.1f, 1001), 1.1));
        VST_STAT_CHECK(!runtime.Await(runtime.Submit(EditStat(NativeStat::Omni, true, 100), session, 1001), 0).ok);
        runtime.SetStatSupported(missing, true);
        snapshot = runtime.Snapshot(1001);
        VST_STAT_CHECK(snapshot.stats[Index(NativeStat::Omni)].supported && !snapshot.stats[Index(NativeStat::Omni)].enabled);
        VST_STAT_CHECK(Apply(runtime, session, EditStat(NativeStat::Omni, true, 100), 1002));
        VST_STAT_CHECK(Near(runtime.FilterStat(NativeStat::Area, kOwner, 1.1f, 1002), 2));
    }
    return true;
}

inline bool FinalMethodsPreserveWeaponFactorsAndFloors() {
    Runtime runtime;
    ReadyStats(runtime);
    auto session = runtime.BeginSession(1000);
    VST_STAT_CHECK(Apply(runtime, session, EditStat(NativeStat::Area, true, 100)));
    VST_STAT_CHECK(Apply(runtime, session, EditStat(NativeStat::Cooldown, true, 90)));
    VST_STAT_CHECK(Near(runtime.FilterStatFinal(NativeStat::Area, kOwner, 9, 3, 1000), 6));
    VST_STAT_CHECK(Near(runtime.FilterStatFinal(NativeStat::Cooldown, kOwner, 0.9f, 0.05f, 1000), 0.1));
    VST_STAT_CHECK(Near(runtime.FilterStatFinal(NativeStat::Cooldown, kOwner, 0.9f, 0.2f, 1000), 0.2));
    VST_STAT_CHECK(Near(runtime.FilterStatFinal(NativeStat::Cooldown, kOwner, 1.8f, 1.5f, 1000), 1.5));
    // A native Final can already contain a nested overridden base getter.
    float nested_area = runtime.FilterStat(NativeStat::Area, kOwner, 1.25f, 1000) * 3;
    VST_STAT_CHECK(Near(runtime.FilterStatFinal(NativeStat::Area, kOwner, nested_area, 3, 1000), 6));
    VST_STAT_CHECK(Near(runtime.FilterStatFinal(NativeStat::Recovery, kOwner, 7, 3, 1000), 7));
    auto before = runtime.Snapshot(1000).stats[Index(NativeStat::Area)];
    VST_STAT_CHECK(Near(runtime.FilterStatFinal(NativeStat::Area, kOwner, 7, std::numeric_limits<float>::infinity(), 1000), 7));
    VST_STAT_CHECK(std::isnan(runtime.FilterStatFinal(NativeStat::Area, kOwner,
        std::numeric_limits<float>::quiet_NaN(), 3, 1000)));
    VST_STAT_CHECK(runtime.Snapshot(1000).stats[Index(NativeStat::Area)].game_reads == before.game_reads);
    VST_STAT_CHECK(Apply(runtime, session, EditStat(NativeStat::Area, true, 900)));
    auto overrides = runtime.Snapshot(1000).stats[Index(NativeStat::Area)].override_reads;
    VST_STAT_CHECK(Near(runtime.FilterStatFinal(NativeStat::Area, kOwner, 7, std::numeric_limits<float>::max(), 1000), 7));
    VST_STAT_CHECK(runtime.Snapshot(1000).stats[Index(NativeStat::Area)].override_reads == overrides);
    VST_STAT_CHECK(Near(runtime.FilterStat(NativeStat::Area, kOwner + 1, 1.25f, 1000), 1.25));
    VST_STAT_CHECK(Near(runtime.FilterStat(NativeStat::Area, 0, 1.25f, 1000), 1.25));
    return true;
}

inline bool SamplesAndFinalReadsKeepHonestBaseEvidence() {
    Runtime runtime;
    ReadyStats(runtime);
    auto session = runtime.BeginSession(1000);
    VST_STAT_CHECK(EnableEveryStat(runtime, session));
    VST_STAT_CHECK(Apply(runtime, session, FeatureCommand("movement", true, 2)));
    auto observation = StatObservation();
    for (size_t index = 0; index < kExpected.size(); ++index) {
        auto stat = static_cast<NativeStat>(index);
        if (stat == NativeStat::Omni) continue;
        VST_STAT_CHECK(Near(runtime.FilterStat(stat, kOwner, observation.stat_original[index], 1000, false), kExpected[index].raw));
    }
    VST_STAT_CHECK(Near(runtime.FilterStatFinal(NativeStat::Area, kOwner, 1, 3, 1000, false), 6));
    VST_STAT_CHECK(Near(runtime.FilterMovement(kOwner, 3, 1000, false), 6));
    auto snapshot = runtime.Snapshot(1000);
    for (size_t index = 0; index < kExpected.size(); ++index) {
        const auto& state = snapshot.stats[index];
        VST_STAT_CHECK(state.game_reads == 0 && state.override_reads == 0 && !state.io_known && !state.current_known);
        VST_STAT_CHECK(state.sample_known == (static_cast<NativeStat>(index) != NativeStat::Omni));
    }
    const auto& movement = snapshot.combat[static_cast<size_t>(CombatFeature::Movement)];
    VST_STAT_CHECK(movement.calls == 0 && movement.override_calls == 0 && movement.actual_calls == 0);
    VST_STAT_CHECK(Near(snapshot.stats[Index(NativeStat::Area)].original, 10));

    VST_STAT_CHECK(Apply(runtime, session, FeatureCommand("movement", true, 10)));
    const auto movement_before = runtime.Snapshot(1000).combat[static_cast<size_t>(CombatFeature::Movement)];
    const float extreme_movement = std::numeric_limits<float>::max();
    VST_STAT_CHECK(runtime.FilterMovement(kOwner, extreme_movement, 1000, false) == extreme_movement);
    auto movement_after = runtime.Snapshot(1000).combat[static_cast<size_t>(CombatFeature::Movement)];
    VST_STAT_CHECK(movement_after.calls == movement_before.calls
        && movement_after.actual_calls == movement_before.actual_calls
        && movement_after.override_calls == movement_before.override_calls
        && movement_after.overflow_skips == movement_before.overflow_skips);
    VST_STAT_CHECK(runtime.FilterMovement(kOwner, extreme_movement, 1000) == extreme_movement);
    movement_after = runtime.Snapshot(1000).combat[static_cast<size_t>(CombatFeature::Movement)];
    VST_STAT_CHECK(movement_after.calls == movement_before.calls + 1
        && movement_after.actual_calls == movement_before.actual_calls + 1
        && movement_after.override_calls == movement_before.override_calls
        && movement_after.overflow_skips == movement_before.overflow_skips + 1);

    VST_STAT_CHECK(Near(runtime.FilterStatFinal(NativeStat::Area, kOwner, 1, 3, 1000), 6));
    auto area = runtime.Snapshot(1000).stats[Index(NativeStat::Area)];
    VST_STAT_CHECK(area.io_known && area.last_is_final && area.game_reads == 1 && !area.current_known);
    VST_STAT_CHECK(Near(area.last_output, 6) && Near(area.original, 10));
    VST_STAT_CHECK(Near(runtime.FilterStat(NativeStat::Area, kOwner, 1.1f, 1000), 2));
    area = runtime.Snapshot(1000).stats[Index(NativeStat::Area)];
    VST_STAT_CHECK(area.current_known && Near(area.current, 100) && area.game_reads == 2);
    VST_STAT_CHECK(Near(runtime.FilterStatFinal(NativeStat::Area, kOwner, 2, 3, 1000), 6));
    area = runtime.Snapshot(1000).stats[Index(NativeStat::Area)];
    VST_STAT_CHECK(area.current_known && Near(area.current, 100) && area.last_is_final);
    VST_STAT_CHECK(Apply(runtime, session, StatValueOnly(NativeStat::Area, 200), 1001));
    VST_STAT_CHECK(!runtime.Snapshot(1001).stats[Index(NativeStat::Area)].current_known);
    VST_STAT_CHECK(Near(runtime.FilterStat(NativeStat::Area, kOwner, 1.1f, 1001, false), 3));
    VST_STAT_CHECK(Near(runtime.FilterStatFinal(NativeStat::Area, kOwner, 2, 2, 1001), 6));
    VST_STAT_CHECK(!runtime.Snapshot(1001).stats[Index(NativeStat::Area)].current_known);
    VST_STAT_CHECK(Near(runtime.FilterStat(NativeStat::Area, kOwner, 1.1f, 1001), 3));
    VST_STAT_CHECK(Near(runtime.Snapshot(1001).stats[Index(NativeStat::Area)].current, 200));

    Runtime unknown;
    ReadyStats(unknown, 1000, false);
    auto unknown_session = unknown.BeginSession(1000);
    VST_STAT_CHECK(Apply(unknown, unknown_session, EditStat(NativeStat::Might, true, 100)));
    VST_STAT_CHECK(Near(unknown.FilterStat(NativeStat::Might, kOwner, 1.1f, 1000, false), 2));
    auto might = unknown.Snapshot(1000).stats[Index(NativeStat::Might)];
    VST_STAT_CHECK(!might.sample_known && !might.current_known && !might.io_known && might.game_reads == 0);
    return true;
}

inline bool LiveValuesNeverReopenDisabledStats() {
    for (size_t index = 0; index < kExpected.size(); ++index) {
        auto stat = static_cast<NativeStat>(index);
        Runtime runtime;
        ReadyStats(runtime);
        auto session = runtime.BeginSession(1000);
        VST_STAT_CHECK(Apply(runtime, session, EditStat(stat, true, kExpected[index].target)));
        auto edit = runtime.Submit(StatValueOnly(stat, kExpected[index].alternate), session, 1001);
        vst::Response response;
        VST_STAT_CHECK(!runtime.TryResult(edit, response));
        VST_STAT_CHECK(runtime.StatValue(stat) == kExpected[index].target);
        runtime.Pump(1001);
        VST_STAT_CHECK(runtime.Await(edit, 0).ok && runtime.StatValue(stat) == kExpected[index].alternate);
        VST_STAT_CHECK(runtime.Snapshot(1001).stats[index].enabled);
        auto old_edit = runtime.Submit(StatValueOnly(stat, kExpected[index].target), session, 1002);
        VST_STAT_CHECK(runtime.Await(runtime.Submit(EditStat(stat, false), session, 1003), 0).ok);
        VST_STAT_CHECK(!runtime.Await(old_edit, 0).ok);
        runtime.Pump(1003);
        VST_STAT_CHECK(!runtime.Snapshot(1003).stats[index].enabled);
        VST_STAT_CHECK(Apply(runtime, session, StatValueOnly(stat, kExpected[index].alternate), 1004));
        VST_STAT_CHECK(!runtime.Snapshot(1004).stats[index].enabled && runtime.StatValue(stat) == kExpected[index].alternate);

        auto old_enable = runtime.Submit(EditStat(stat, true, kExpected[index].target), session, 1005);
        auto trailing_edit = runtime.Submit(StatValueOnly(stat, kExpected[index].alternate), session, 1006);
        VST_STAT_CHECK(runtime.Await(runtime.Submit(EditStat(stat, false), session, 1007), 0).ok);
        VST_STAT_CHECK(!runtime.Await(old_enable, 0).ok && !runtime.Await(trailing_edit, 0).ok);
        runtime.Pump(1007);
        VST_STAT_CHECK(!runtime.Snapshot(1007).stats[index].enabled);
    }
    return true;
}

inline bool DisableStatsKeepsCommonFeatureControls() {
    Runtime runtime;
    ReadyStats(runtime);
    auto session = runtime.BeginSession(1000);
    VST_STAT_CHECK(EnableEveryStat(runtime, session));
    for (const char* id : {"health", "experience", "damage", "movement", "cooldown", "instantKill"}) {
        double value = std::string_view(id) == "experience" || std::string_view(id) == "damage" || std::string_view(id) == "movement" ? 2 : 1;
        VST_STAT_CHECK(Apply(runtime, session, FeatureCommand(id, true, value)));
    }
    std::array<Runtime::Ticket, vst::kNativeStatCount> edits;
    for (size_t index = 0; index < edits.size(); ++index)
        edits[index] = runtime.Submit(StatValueOnly(static_cast<NativeStat>(index), kExpected[index].alternate), session, 1001);
    auto damage = runtime.Submit(FeatureCommand("damage", true, 3), session, 1001);
    VST_STAT_CHECK(runtime.Await(runtime.Submit(StatCommand("disableStats"), session, 1002), 0).ok);
    for (const auto& edit : edits) VST_STAT_CHECK(!runtime.Await(edit, 0).ok);
    runtime.Pump(1002);
    VST_STAT_CHECK(runtime.Await(damage, 0).ok);
    auto snapshot = runtime.Snapshot(1002);
    VST_STAT_CHECK(AllStatsEnabled(snapshot, false));
    for (size_t index = 0; index < 6; ++index) VST_STAT_CHECK(snapshot.combat[index].enabled);
    VST_STAT_CHECK(!snapshot.combat[6].enabled);
    VST_STAT_CHECK(Near(runtime.FilterHealthDamage(kOwner, 5, 1002), 0));
    VST_STAT_CHECK(Near(runtime.FilterExperience(kOwner, 1, 1002), 2));
    VST_STAT_CHECK(Near(runtime.FilterDamage(kOwner, 1, true, 1002), 3));
    VST_STAT_CHECK(Near(runtime.FilterMovement(kOwner, runtime.FilterStat(NativeStat::MoveSpeed, kOwner, 1.1f, 1002), 1002), 2.2));
    VST_STAT_CHECK(runtime.Await(runtime.Submit(StatCommand("disableAll"), session, 1003), 0).ok);
    snapshot = runtime.Snapshot(1003);
    VST_STAT_CHECK(AllStatsEnabled(snapshot, false));
    for (const auto& feature : snapshot.combat) VST_STAT_CHECK(!feature.enabled);
    return true;
}

inline bool StatActorAndSceneBoundariesCancelQueuedWork() {
    for (unsigned boundary = 0; boundary < 5; ++boundary) {
        Runtime runtime;
        ReadyStats(runtime);
        auto session = runtime.BeginSession(1000);
        VST_STAT_CHECK(EnableEveryStat(runtime, session));
        std::array<Runtime::Ticket, vst::kNativeStatCount> edits;
        for (size_t index = 0; index < edits.size(); ++index)
            edits[index] = runtime.Submit(StatValueOnly(static_cast<NativeStat>(index), kExpected[index].alternate), session, 1100);
        auto observation = StatObservation();
        if (boundary == 0) observation.player = kOwner + 1;
        if (boundary == 1) observation.scene = "Different fixture scene";
        if (boundary == 2) observation.player_ready = false;
        if (boundary == 3) observation.safe_mode = false;
        if (boundary == 4) observation.verified = false;
        runtime.Observe(observation, 1101);
        runtime.Pump(1101);
        for (const auto& edit : edits) VST_STAT_CHECK(!runtime.Await(edit, 0).ok);
        VST_STAT_CHECK(AllStatsEnabled(runtime.Snapshot(1101), false));
        runtime.Observe(StatObservation(), 1102);
        VST_STAT_CHECK(AllStatsEnabled(runtime.Snapshot(1102), false));
        VST_STAT_CHECK(Near(runtime.FilterStat(NativeStat::Might, kOwner, 1.1f, 1102), 1.1));
    }
    return true;
}

inline bool StatFreshnessAndRequestDeadlineBoundaries() {
    Runtime fresh;
    ReadyStats(fresh);
    auto session = fresh.BeginSession(1000);
    VST_STAT_CHECK(EnableEveryStat(fresh, session));
    VST_STAT_CHECK(fresh.Await(fresh.Submit(StatCommand("heartbeat"), session, 1800), 0).ok);
    VST_STAT_CHECK(AllStatsEnabled(fresh.Snapshot(2000), true));
    VST_STAT_CHECK(Near(fresh.FilterStat(NativeStat::Might, kOwner, 1.1f, 2001), 1.1));
    VST_STAT_CHECK(AllStatsEnabled(fresh.Snapshot(2001), false) && !fresh.Snapshot(2001).player_ready);
    fresh.Observe(StatObservation(), 2002);
    VST_STAT_CHECK(AllStatsEnabled(fresh.Snapshot(2002), false));

    for (uint64_t delay : {1500ULL, 1501ULL}) {
        Runtime runtime;
        ReadyStats(runtime);
        auto queued_session = runtime.BeginSession(1000);
        std::array<Runtime::Ticket, vst::kNativeStatCount> enables;
        for (size_t index = 0; index < enables.size(); ++index)
            enables[index] = runtime.Submit(EditStat(static_cast<NativeStat>(index), true, kExpected[index].target), queued_session, 1000);
        for (uint64_t tick : {1500ULL, 2000ULL, 2500ULL}) runtime.Observe(StatObservation(), tick);
        runtime.Pump(1000 + delay);
        for (const auto& enable : enables) VST_STAT_CHECK(runtime.Await(enable, 0).ok == (delay == 1500));
        VST_STAT_CHECK(AllStatsEnabled(runtime.Snapshot(1000 + delay), delay == 1500));
    }
    return true;
}

inline bool StatHeartbeatAndSessionBoundaries() {
    Runtime runtime;
    ReadyStats(runtime);
    auto session = runtime.BeginSession(1000);
    VST_STAT_CHECK(EnableEveryStat(runtime, session));
    for (uint64_t tick : {1900ULL, 2800ULL, 3700ULL, 4000ULL}) runtime.Observe(StatObservation(), tick);
    runtime.CheckHeartbeat(4000);
    VST_STAT_CHECK(AllStatsEnabled(runtime.Snapshot(4000), true));
    runtime.CheckHeartbeat(4001);
    VST_STAT_CHECK(AllStatsEnabled(runtime.Snapshot(4001), false));
    runtime.Observe(StatObservation(), 4002);
    VST_STAT_CHECK(!runtime.Await(runtime.Submit(StatCommand("heartbeat"), session, 4002), 0).ok);
    VST_STAT_CHECK(AllStatsEnabled(runtime.Snapshot(4002), false));
    auto new_session = runtime.BeginSession(4003);
    VST_STAT_CHECK(new_session != session && AllStatsEnabled(runtime.Snapshot(4003), false));
    VST_STAT_CHECK(Apply(runtime, new_session, EditStat(NativeStat::Might, true, 100), 4003));
    VST_STAT_CHECK(!runtime.Await(runtime.Submit(EditStat(NativeStat::Might, false), session, 4004), 0).ok);
    VST_STAT_CHECK(runtime.Snapshot(4004).stats[Index(NativeStat::Might)].enabled);
    VST_STAT_CHECK(Near(runtime.FilterStat(NativeStat::Might, kOwner, 1.1f, 4004), 2));

    std::array<Runtime::Ticket, vst::kNativeStatCount> pending;
    for (size_t index = 0; index < pending.size(); ++index)
        pending[index] = runtime.Submit(EditStat(static_cast<NativeStat>(index), true, kExpected[index].target), new_session, 4005);
    runtime.EndSession(new_session);
    runtime.Pump(4006);
    for (const auto& ticket : pending) VST_STAT_CHECK(!runtime.Await(ticket, 0).ok);
    VST_STAT_CHECK(AllStatsEnabled(runtime.Snapshot(4006), false));
    auto replacement = runtime.BeginSession(4007);
    VST_STAT_CHECK(replacement != new_session && AllStatsEnabled(runtime.Snapshot(4007), false));
    return true;
}

inline bool StatAwaitAndCancelCannotApplyLater() {
    for (bool explicit_cancel : {false, true}) {
        Runtime runtime;
        ReadyStats(runtime);
        auto session = runtime.BeginSession(1000);
        std::array<Runtime::Ticket, vst::kNativeStatCount> pending;
        for (size_t index = 0; index < pending.size(); ++index)
            pending[index] = runtime.Submit(EditStat(static_cast<NativeStat>(index), true, kExpected[index].target), session, 1000);
        for (const auto& ticket : pending) {
            if (explicit_cancel) runtime.Cancel(ticket);
            VST_STAT_CHECK(!runtime.Await(ticket, 0).ok);
        }
        runtime.Pump(1001);
        VST_STAT_CHECK(AllStatsEnabled(runtime.Snapshot(1001), false));
    }
    return true;
}

#undef VST_STAT_CHECK
} // namespace vst_stat_tests
