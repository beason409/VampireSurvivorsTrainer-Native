#pragma once

#include "stat_fixtures.hpp"
#include <cstdio>

namespace vst_revival_tests {
using namespace vst_stat_tests;
#define VST_REVIVAL_CHECK(...) do { if (!(__VA_ARGS__)) { std::fprintf(stderr, "revival fixture line %d: %s\n", __LINE__, #__VA_ARGS__); return false; } } while (0)

inline GameObservation PendingDeath(float remaining = 2) {
    auto observation = StatObservation();
    observation.player_ready = false;
    observation.revival_pending = true;
    observation.paused = true;
    observation.stat_known.fill(false);
    observation.stat_current_known.fill(false);
    observation.stat_known[Index(NativeStat::Revivals)] = true;
    observation.stat_current_known[Index(NativeStat::Revivals)] = true;
    observation.stat_original[Index(NativeStat::Revivals)] = 1;
    observation.stat_current[Index(NativeStat::Revivals)] = remaining;
    return observation;
}
inline bool OnlyRevivalEnabled(const StateSnapshot& snapshot) {
    for (size_t index = 0; index < snapshot.stats.size(); ++index)
        if (snapshot.stats[index].enabled != (index == Index(NativeStat::Revivals))) return false;
    for (const auto& feature : snapshot.combat) if (feature.enabled) return false;
    return !snapshot.player_ready && snapshot.revival_pending;
}

inline bool SameActorDeathRetainsOnlyExistingRevivalAndConsumedRemainder() {
    Runtime runtime; ReadyStats(runtime);
    auto session = runtime.BeginSession(1000);
    VST_REVIVAL_CHECK(EnableEveryStat(runtime, session));
    VST_REVIVAL_CHECK(Apply(runtime, session, EditStat(NativeStat::Revivals, true, 2)));
    for (const char* feature : {"health", "experience", "damage", "movement", "cooldown", "instantKill"})
        VST_REVIVAL_CHECK(Apply(runtime, session, FeatureCommand(feature, true, 1)));
    auto old_command = runtime.Submit(EditStat(NativeStat::Magnet, true, 400), session, 1001);
    runtime.Observe(PendingDeath(), 1100);
    VST_REVIVAL_CHECK(OnlyRevivalEnabled(runtime.Snapshot(1100)));
    VST_REVIVAL_CHECK(!runtime.Await(old_command, 0).ok);
    VST_REVIVAL_CHECK(runtime.StatEnabled(NativeStat::Revivals, kOwner, 1100));
    VST_REVIVAL_CHECK(!runtime.StatEnabled(NativeStat::Revivals, kOwner + 8, 1100));
    VST_REVIVAL_CHECK(runtime.FilterHealthDamage(kOwner, 5, 1100) == 5);
    runtime.Observe(PendingDeath(1), 1500);
    runtime.ObserveStatRead(NativeStat::Revivals, kOwner, 1, 1, true, 1500);
    auto waiting = runtime.Snapshot(1500).stats[Index(NativeStat::Revivals)];
    VST_REVIVAL_CHECK(waiting.enabled && waiting.bound_known && waiting.bound == 1);
    VST_REVIVAL_CHECK(waiting.io_known && waiting.game_reads == 1 && waiting.override_reads == 1);
    VST_REVIVAL_CHECK(waiting.current_known && waiting.current == 1); // Native return already reports remaining.
    auto resumed = StatObservation();
    resumed.stat_current_known[Index(NativeStat::Revivals)] = true;
    resumed.stat_current[Index(NativeStat::Revivals)] = 1;
    runtime.Observe(resumed, 1600);
    auto after = runtime.Snapshot(1600);
    VST_REVIVAL_CHECK(after.player_ready && !after.revival_pending && after.stats[Index(NativeStat::Revivals)].enabled);
    VST_REVIVAL_CHECK(after.stats[Index(NativeStat::Revivals)].bound == 1);
    VST_REVIVAL_CHECK(!after.stats[Index(NativeStat::Magnet)].enabled && !after.combat[0].enabled);
    return true;
}

inline bool PendingDeathCannotArmOrRetargetAndExplicitDisableWins() {
    Runtime runtime; ReadyStats(runtime);
    auto session = runtime.BeginSession(1000);
    runtime.Observe(PendingDeath(), 1100);
    VST_REVIVAL_CHECK(!runtime.Snapshot(1100).revival_pending);
    VST_REVIVAL_CHECK(!Apply(runtime, session, EditStat(NativeStat::Revivals, true, 2), 1101));
    runtime.Observe(StatObservation(), 1200);
    VST_REVIVAL_CHECK(Apply(runtime, session, EditStat(NativeStat::Revivals, true, 2), 1200));
    runtime.Observe(PendingDeath(), 1300);
    VST_REVIVAL_CHECK(!Apply(runtime, session, StatValueOnly(NativeStat::Revivals, 5), 1301));
    VST_REVIVAL_CHECK(runtime.StatEnabled(NativeStat::Revivals, kOwner, 1301) && runtime.StatValue(NativeStat::Revivals) == 2);
    VST_REVIVAL_CHECK(!Apply(runtime, session, EditStat(NativeStat::Revivals, true, 5), 1302));
    VST_REVIVAL_CHECK(runtime.StatEnabled(NativeStat::Revivals, kOwner, 1302));
    VST_REVIVAL_CHECK(Apply(runtime, session, EditStat(NativeStat::Revivals, false), 1303));
    runtime.Observe(PendingDeath(), 1400);
    VST_REVIVAL_CHECK(!runtime.Snapshot(1400).revival_pending && !runtime.StatEnabled(NativeStat::Revivals, kOwner, 1400));
    runtime.Observe(StatObservation(), 1500);
    VST_REVIVAL_CHECK(!runtime.Snapshot(1500).stats[Index(NativeStat::Revivals)].enabled);
    return true;
}

inline bool RevivalRetentionStillHonorsAllHardBoundaries() {
    for (unsigned boundary = 0; boundary < 12; ++boundary) {
        Runtime runtime; ReadyStats(runtime);
        auto session = runtime.BeginSession(1000);
        VST_REVIVAL_CHECK(Apply(runtime, session, EditStat(NativeStat::Revivals, true, 2)));
        runtime.Observe(PendingDeath(), 1100);
        VST_REVIVAL_CHECK(runtime.Snapshot(1100).revival_pending);
        auto changed = PendingDeath(1);
        if (boundary == 0) changed.player += 8;
        if (boundary == 1) changed.scene = "Other scene";
        if (boundary == 2) changed.safe_mode = false;
        if (boundary == 3) changed.verified = false;
        if (boundary == 4) changed.revival_pending = false;
        if (boundary <= 4) runtime.Observe(changed, 1200);
        if (boundary == 5) runtime.CheckHeartbeat(2101); // Observation stale, heartbeat still valid.
        if (boundary == 6) {
            for (uint64_t now : {1500ULL, 2000ULL, 2500ULL, 3000ULL, 3500ULL}) runtime.Observe(PendingDeath(), now);
            runtime.Observe(PendingDeath(), 4001); // Heartbeat expired despite fresh observations.
        }
        if (boundary == 7) runtime.EndSession(session);
        if (boundary == 8) runtime.BeginSession(1200);
        if (boundary == 9) runtime.Submit(StatCommand("disableStats"), session, 1200);
        if (boundary == 10) runtime.Submit(StatCommand("disableAll"), session, 1200);
        if (boundary == 11) {
            GameObservation teardown; teardown.verified = true; teardown.scene = "Waiting for game scene";
            runtime.Observe(teardown, 1200); // The same pointer cannot survive a teardown generation.
        }
        const uint64_t check_at = boundary == 5 ? 2101 : boundary == 6 ? 4001 : 1200;
        VST_REVIVAL_CHECK(!runtime.StatEnabled(NativeStat::Revivals, kOwner, check_at));
        VST_REVIVAL_CHECK(!runtime.Snapshot(check_at).stats[Index(NativeStat::Revivals)].enabled);
        runtime.Observe(StatObservation(), check_at + 1);
        VST_REVIVAL_CHECK(!runtime.Snapshot(check_at + 1).stats[Index(NativeStat::Revivals)].enabled);
    }
    return true;
}
#undef VST_REVIVAL_CHECK
} // namespace vst_revival_tests
