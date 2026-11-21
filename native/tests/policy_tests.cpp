#include "../common/runtime.hpp"
#include "../common/protocol.hpp"
#include "stat_fixtures.hpp"
#include "wallet_fixtures.hpp"
#include "../common/chest_animation.hpp"
#include "../common/chest_rewards.hpp"
#include "../common/enemy_hit_targets.hpp"
#include "../common/enemy_hit_cache.hpp"
#include "../common/game_speed.hpp"
#include "resource_fixtures.hpp"
#include "revival_fixtures.hpp"
#include "gc_root_fixtures.hpp"
#include "gold_plan_fixtures.hpp"
#include "reference_field_fixtures.hpp"
#include "reward_fixtures.hpp"
#ifdef _WIN32
#include "backup_fixtures.hpp"
#endif
#include "../vendor/yyjson/yyjson.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <iterator>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include "../backend/pipe_server.hpp"
#endif

using namespace vst;
namespace {

bool Check(bool passed, const char* expression, int line) {
    if (!passed) std::fprintf(stderr, "  line %d: %s\n", line, expression);
    return passed;
}
#define CHECK(...) do { if (!Check((__VA_ARGS__), #__VA_ARGS__, __LINE__)) return false; } while (0)

GameObservation Observation(uintptr_t owner = 0x1000) {
    GameObservation value;
    value.verified = value.safe_mode = value.player_ready = value.paused_known = true;
    value.paused = false;
    value.player = owner;
    value.armor_original = 5;
    value.scene = "Offline fixture";
    return value;
}

void Ready(Runtime& runtime, uint64_t now = 1000) {
    runtime.SetArmorSupported(true);
    runtime.Observe(Observation(), now);
}

Request Command(const char* kind, const char* id = "fixture") {
    Request request;
    request.kind = kind;
    request.id = id;
    return request;
}

Request Armor(bool enabled, double value = 20) {
    auto request = Command("setStat");
    request.stat = "armor";
    request.has_enabled = true;
    request.enabled = enabled;
    request.has_value = enabled;
    request.value = value;
    return request;
}

bool Enable(Runtime& runtime, uint64_t session, uint64_t now = 1000, double value = 20) {
    auto ticket = runtime.Submit(Armor(true, value), session, now);
    runtime.Pump(now);
    return runtime.Await(ticket, 0).ok;
}

void ReadyCombat(Runtime& runtime, uint64_t now = 1000) {
    Ready(runtime, now);
    for (auto feature : {CombatFeature::Health, CombatFeature::Experience, CombatFeature::Damage})
        runtime.SetFeatureSupported(feature, true);
}

void ReadyAll(Runtime& runtime, uint64_t now = 1000) {
    ReadyCombat(runtime, now);
    for (auto feature : {CombatFeature::Movement, CombatFeature::Cooldown, CombatFeature::InstantKill})
        runtime.SetFeatureSupported(feature, true);
}

Request Feature(const char* id, bool enabled, double value = 1) {
    auto request = Command("setFeature");
    request.feature = id;
    request.has_enabled = true;
    request.enabled = enabled;
    request.has_value = enabled;
    request.value = value;
    return request;
}

bool EnableFeature(Runtime& runtime, uint64_t session, const char* id, double value = 1, uint64_t now = 1000) {
    auto ticket = runtime.Submit(Feature(id, true, value), session, now);
    runtime.Pump(now);
    return runtime.Await(ticket, 0).ok;
}

bool GameSpeedOwnershipAndPause() {
    GameSpeedOverride time;
    CHECK(time.Next(2, 4) == 4 && !time.OwnsValue());
    time.ConfirmWrite(4);
    CHECK(time.Next(4, 6) == 6);
    time.ConfirmWrite(6);
    CHECK(time.Next(0, 6) == 0 && time.Next(0, 0) == 0 && time.OwnsValue());
    CHECK(time.Next(6, 0) == 2);
    time.ConfirmWrite(2);
    CHECK(!time.OwnsValue());
    CHECK(time.Next(2, 4) == 4);
    time.ConfirmWrite(4);
    CHECK(time.Next(3, 4) == 4 && !time.OwnsValue());
    time.ConfirmWrite(4);
    CHECK(time.Next(4, 0) == 3);
    CHECK(time.Next(1, 0) == 1 && !time.OwnsValue());
    return true;
}

bool GameSpeedQueuedLiveUpdatesAndExpiry() {
    Runtime runtime;
    ReadyAll(runtime);
    runtime.SetFeatureSupported(CombatFeature::GameSpeed, true);
    const auto session = runtime.BeginSession(1000);
    const uintptr_t owner = Observation().player;
    auto speed = Feature(kGameSpeedId, true, 4);
    auto pending = runtime.Submit(speed, session, 1000);
    CHECK(runtime.ActiveGameSpeed(owner, 1000) == 0);
    runtime.Pump(1000);
    CHECK(runtime.Await(pending, 0).ok && runtime.ActiveGameSpeed(owner, 1000) == 4);
    CHECK(runtime.ActiveGameSpeed(owner + 1, 1000) == 0);
    for (double invalid : {0.9, 10.1, std::numeric_limits<double>::infinity()}) {
        speed.value = invalid;
        CHECK(!runtime.Await(runtime.Submit(speed, session, 1000), 0).ok);
    }
    CHECK(runtime.ActiveGameSpeed(owner, 1000) == 0);
    CHECK(EnableFeature(runtime, session, kGameSpeedId, 4));
    speed.has_enabled = false;
    speed.value = 6.5;
    pending = runtime.Submit(speed, session, 1000);
    runtime.Pump(1000);
    CHECK(runtime.Await(pending, 0).ok && runtime.ActiveGameSpeed(owner, 1000) == 6.5);
    runtime.RecordGameSpeedWrite(owner, 4, 6.5f, true, 1000);
    const auto snapshot = runtime.Snapshot(1000).combat[static_cast<size_t>(CombatFeature::GameSpeed)];
    CHECK(snapshot.actual_calls == 1 && snapshot.io_known && snapshot.last_input == 4 && snapshot.last_output == 6.5f);
    CHECK(runtime.ActiveGameSpeed(owner, 2001) == 0);
    runtime.Observe(Observation(), 4001);
    CHECK(runtime.ActiveGameSpeed(owner, 4001) == 0);
    return true;
}

bool ChestSkipRequiresNativeReadyStateAndLiveOwner() {
    ChestAnimationState chest;
    chest.playing = true;
    chest.level = 1;
    CHECK(!chest.CanSkip());
    chest.at_skip_point = true;
    CHECK(chest.CanSkip());
    chest.skipped = true;
    CHECK(!chest.CanSkip());
    chest.skipped = false;
    chest.finished = true;
    CHECK(!chest.CanSkip());
    chest.finished = false;
    chest.arcana = true;
    CHECK(!chest.CanSkip());
    chest.arcana = false;
    chest.level = 4;
    CHECK(!chest.CanSkip());
    chest.level = 3;
    CHECK(chest.CanSkip());

    Runtime runtime;
    auto state = Observation();
    state.paused = true;
    runtime.Observe(state, 1000);
    constexpr auto feature = CombatFeature::SkipChestAnimation;
    runtime.SetFeatureSupported(feature, true);
    const auto session = runtime.BeginSession(1000);
    CHECK(EnableFeature(runtime, session, kSkipChestAnimationId));
    CHECK(runtime.FeatureEnabled(feature, state.player, 1000));
    CHECK(!runtime.FeatureEnabled(feature, state.player + 1, 1000));
    runtime.RecordChestEvent(feature, state.player, 1000);
    const auto snapshot = runtime.Snapshot(1000).combat[static_cast<size_t>(feature)];
    CHECK(snapshot.enabled && snapshot.actual_calls == 1 && snapshot.override_calls == 1);
    CHECK(!runtime.FeatureEnabled(feature, state.player, 2001));
    return true;
}

bool FiveItemChestPreservesNativeSlotsAndSpecialChests() {
    constexpr int32_t existing_any = 2, arcana = 7, survarot = 9;
    const std::array<std::optional<int32_t>, 5> full{6, 1, 2, 2, 5};
    for (int32_t level : {1, 2, 3}) {
        const auto plan = PlanFiveItemChest(level, false, full, existing_any, arcana, survarot);
        CHECK(plan.eligible && !plan.pad);
        CHECK((plan.slots == std::array<int32_t, 5>{6, 1, 2, 2, 5}));
    }
    const std::array<std::optional<int32_t>, 4> short_list{6, std::nullopt, 1, 5};
    const auto padded = PlanFiveItemChest(1, false, short_list, existing_any, arcana, survarot);
    CHECK(padded.eligible && padded.pad);
    CHECK((padded.slots == std::array<int32_t, 5>{6, 1, 5, 2, 2}));
    CHECK(!PlanFiveItemChest(1, true, full, existing_any, arcana, survarot).eligible);
    CHECK(!PlanFiveItemChest(0, false, full, existing_any, arcana, survarot).eligible);
    for (int32_t special : {arcana, survarot}) {
        const std::array<std::optional<int32_t>, 2> slots{6, special};
        CHECK(!PlanFiveItemChest(1, false, slots, existing_any, arcana, survarot).eligible);
    }
    return true;
}

bool MovementBoundsAndRealGetterReads() {
    static_assert(static_cast<size_t>(CombatFeature::Health) == 0 && static_cast<size_t>(CombatFeature::Experience) == 1
        && static_cast<size_t>(CombatFeature::Damage) == 2 && static_cast<size_t>(CombatFeature::Movement) == 3
        && static_cast<size_t>(CombatFeature::Cooldown) == 4 && static_cast<size_t>(CombatFeature::InstantKill) == 5);
    Runtime runtime;
    ReadyAll(runtime);
    auto session = runtime.BeginSession(1000);
    auto pending = runtime.Submit(Feature("movement", true, 2), session, 1000);
    CHECK(runtime.FilterMovement(0x1000, 4, 1000) == 4);
    runtime.Pump(1000);
    CHECK(runtime.Await(pending, 0).ok && runtime.FilterMovement(0x1000, 4, 1000) == 8);
    CHECK(EnableFeature(runtime, session, "movement", 0.25));
    CHECK(runtime.FilterMovement(0x1000, 4, 1000) == 1 && runtime.FilterMovement(0x1000, -4, 1000) == -1);
    CHECK(EnableFeature(runtime, session, "movement", 10));
    CHECK(runtime.FilterMovement(0x1000, 4, 1000) == 40);
    float large = std::numeric_limits<float>::max();
    CHECK(runtime.FilterMovement(0x1000, large, 1000) == large);
    auto snapshot = runtime.Snapshot(1000).combat[3];
    CHECK(snapshot.calls == 6 && snapshot.actual_calls == 6 && snapshot.override_calls == 4 && snapshot.overflow_skips == 1);
    CHECK(snapshot.io_known && !snapshot.result_known);
    CHECK(runtime.FilterMovement(0x2000, 4, 1000) == 4);
    CHECK(std::isnan(runtime.FilterMovement(0x1000, std::numeric_limits<float>::quiet_NaN(), 1000)));
    CHECK(runtime.Snapshot(1000).combat[3].calls == snapshot.calls);
    CHECK(EnableFeature(runtime, session, "health"));
    for (double value : {0.0, 0.249, 10.001, std::numeric_limits<double>::infinity()}) {
        CHECK(!runtime.Await(runtime.Submit(Feature("movement", true, value), session, 1000), 0).ok);
        CHECK(!runtime.Snapshot(1000).combat[3].enabled && runtime.Snapshot(1000).combat[0].enabled);
    }
    return true;
}

bool CooldownAllowlistCapAndIndependentDisable() {
    Runtime runtime;
    ReadyAll(runtime);
    auto session = runtime.BeginSession(1000);
    CHECK(EnableFeature(runtime, session, "movement", 2) && EnableFeature(runtime, session, "cooldown"));
    CHECK(runtime.FilterCooldown(0x1000, 500, true, 1000) == 100);
    CHECK(runtime.FilterCooldown(0x1000, 80, true, 1000) == 80);
    CHECK(runtime.FilterCooldown(0x1000, 0, true, 1000) == 0 && runtime.FilterCooldown(0x1000, -5, true, 1000) == -5);
    auto count = runtime.Snapshot(1000).combat[4].calls;
    CHECK(runtime.FilterCooldown(0x1000, 500, false, 1000) == 500 && runtime.FilterCooldown(0x2000, 500, true, 1000) == 500);
    CHECK(std::isinf(runtime.FilterCooldown(0x1000, std::numeric_limits<float>::infinity(), true, 1000)));
    CHECK(runtime.Snapshot(1000).combat[4].calls == count);
    auto queued = runtime.Submit(Feature("cooldown", true), session, 1000);
    auto off = Feature("cooldown", false); off.has_value = true; off.value = 1;
    auto disabled = runtime.Submit(off, session, 1000);
    CHECK(!runtime.Await(queued, 0).ok && !runtime.FeatureEnabled(CombatFeature::Cooldown, 0x1000, 1000));
    runtime.Pump(1000);
    CHECK(runtime.Await(disabled, 0).ok && runtime.FilterCooldown(0x1000, 500, true, 1000) == 500);
    CHECK(runtime.FilterMovement(0x1000, 4, 1000) == 8);
    for (const char* id : {"cooldown", "instantKill"})
        CHECK(!runtime.Await(runtime.Submit(Feature(id, true, 2), session, 1000), 0).ok);
    CHECK(runtime.Snapshot(1000).combat[3].enabled);
    return true;
}

bool InstantKillPrioritySaturationAndScope() {
    Runtime runtime;
    ReadyAll(runtime);
    auto session = runtime.BeginSession(1000);
    CHECK(EnableFeature(runtime, session, "damage", 3) && EnableFeature(runtime, session, "instantKill"));
    float intermediate = runtime.FilterDamage(0x1000, 2, true, 1000);
    CHECK(intermediate == 6 && runtime.FilterInstantKill(0x1000, intermediate, 20, true, 1000) == 2000);
    runtime.ObserveDamageResult(0x1000, 20, 0, true, 1000);
    runtime.ObserveInstantKillResult(0x1000, 20, 0, true, 1000);
    auto state = runtime.Snapshot(1000);
    CHECK(state.combat[2].last_output == 6 && state.combat[5].last_input == 6 && state.combat[5].last_output == 2000);
    CHECK(state.combat[5].actual_calls == 1 && state.combat[5].actual_loss_total == 20);
    float maximum = std::numeric_limits<float>::max();
    CHECK(runtime.FilterInstantKill(0x1000, 1, maximum, true, 1000) == maximum);
    CHECK(runtime.FilterInstantKill(0x1000, maximum, 1, true, 1000) == maximum);
    auto count = runtime.Snapshot(1000).combat[5].calls;
    CHECK(runtime.FilterInstantKill(0x1000, 2, 20, false, 1000) == 2 && runtime.FilterInstantKill(0x2000, 2, 20, true, 1000) == 2);
    for (float hp : {0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        CHECK(runtime.FilterInstantKill(0x1000, 2, hp, true, 1000) == 2);
    CHECK(runtime.FilterInstantKill(0x1000, -2, 20, true, 1000) == -2 && runtime.FilterInstantKill(0x1000, 0, 20, true, 1000) == 0);
    CHECK(std::isinf(runtime.FilterInstantKill(0x1000, std::numeric_limits<float>::infinity(), 20, true, 1000)));
    CHECK(runtime.Snapshot(1000).combat[5].calls == count);
    CHECK(runtime.Await(runtime.Submit(Feature("instantKill", false), session, 1000), 0).ok);
    CHECK(runtime.FilterInstantKill(0x1000, runtime.FilterDamage(0x1000, 2, true, 1000), 20, true, 1000) == 6);
    CHECK(EnableFeature(runtime, session, "instantKill"));
    CHECK(runtime.Await(runtime.Submit(Feature("damage", false), session, 1000), 0).ok);
    CHECK(runtime.FilterInstantKill(0x1000, runtime.FilterDamage(0x1000, 2, true, 1000), 20, true, 1000) == 2000);
    return true;
}

bool ReaperInstantKillUsesFinalNativeDamage() {
    CHECK(IsInstantKillEnemyType(60, true) && IsInstantKillEnemyType(245, true));
    Runtime runtime;
    ReadyAll(runtime);
    auto session = runtime.BeginSession(1000);
    CHECK(EnableFeature(runtime, session, "instantKill"));
    constexpr uintptr_t player = 0x1000;
    constexpr float hp = 65535000.0f, incoming = 5.0f;
    auto damage = runtime.FilterInstantKill(player, incoming, hp, true, 1000, /*final_damage=*/true);
    // DealDamage subtracts this number before invoking the original death routine.
    CHECK(hp - damage == 0);
    CHECK(runtime.FilterInstantKill(player, hp * 2, hp, true, 1000, /*final_damage=*/true) == hp * 2);
    CHECK(runtime.FilterInstantKill(player, incoming, hp, false, 1000, /*final_damage=*/true) == incoming);
    CHECK(runtime.FilterInstantKill(player, 0, hp, true, 1000, /*final_damage=*/true) == 0);
    runtime.ObserveInstantKillResult(player, hp, 0, true, 1000);
    auto state = runtime.Snapshot(1000).combat[static_cast<size_t>(CombatFeature::InstantKill)];
    CHECK(state.calls == 2 && state.override_calls == 2 && state.actual_calls == 1 && state.actual_loss_total == hp);
    CHECK(runtime.Await(runtime.Submit(Feature("instantKill", false), session, 1000), 0).ok);
    CHECK(runtime.FilterInstantKill(player, incoming, hp, true, 1000, /*final_damage=*/true) == incoming);
    CHECK(EnableFeature(runtime, session, "instantKill"));
    CHECK(runtime.FilterInstantKill(player, incoming, hp, true, 2001, /*final_damage=*/true) == incoming);
    return true;
}

bool InstantKillIncludesAuditedBossesAndSpecialTargets() {
    CHECK(std::ranges::is_sorted(kExcludedInstantKillTypes) && std::ranges::is_sorted(kZeroXpEnemyTypes));
    for (auto type : kZeroXpEnemyTypes) CHECK(IsInstantKillEnemyType(type, true));
    for (auto type : kExcludedInstantKillTypes) {
        CHECK(!IsInstantKillEnemyType(type, true));
        CHECK(!IsInstantKillEnemyType(type, false));
    }
    for (int32_t type : {61, 71, 247, 265, 276, 396, 535, 803, 1001, 1004, 1079, 1160, 1830})
        CHECK(IsInstantKillEnemyType(type, true)); // Base, scripted fight targets and selected DLC cases.
    CHECK(IsInstantKillEnemyType(57, false)); // Positive-XP boss keeps the normal target route.
    CHECK(!IsInstantKillEnemyType(57, true));
    CHECK(!IsInstantKillEnemyType(-1, false));
    return true;
}

bool EnemyHitClassCachePreservesClassificationOnCollisions() {
    EnemyHitClassCache cache;
    const auto enemy = reinterpret_cast<const void*>(uintptr_t{0x1000});
    const auto projectile = reinterpret_cast<const void*>(uintptr_t{0x41000});
    unsigned lookups = 0;
    const auto resolve_enemy = [&] { ++lookups; return EnemyHitClass{true, true}; };
    const auto resolve_projectile = [&] { ++lookups; return EnemyHitClass{}; };
    CHECK(cache.Get(enemy, resolve_enemy).custom_hit);
    for (unsigned i = 0; i < 2048; ++i) CHECK(cache.Get(enemy, resolve_enemy).target);
    CHECK(lookups == 1 && !cache.Get(nullptr, resolve_enemy).target);
    CHECK(!cache.Get(projectile, resolve_projectile).target);
    CHECK(!cache.Get(projectile, resolve_projectile).custom_hit && lookups == 2);
    CHECK(cache.Get(enemy, resolve_enemy).custom_hit && lookups == 3);
    return true;
}

bool NewFeaturesLifecycleQueueAndSessions() {
    const char* names[] = {"movement", "cooldown", "instantKill"};
    for (size_t selected = 0; selected < 3; ++selected) {
        Runtime runtime;
        ReadyAll(runtime);
        auto session = runtime.BeginSession(1000);
        for (const char* name : names) CHECK(EnableFeature(runtime, session, name));
        CHECK(runtime.Await(runtime.Submit(Command("disableStats"), session, 1000), 0).ok);
        for (size_t i = 3; i < 6; ++i) CHECK(runtime.Snapshot(1000).combat[i].enabled);
        auto queued = runtime.Submit(Feature(names[selected], true), session, 1000);
        runtime.SetFeatureSupported(static_cast<CombatFeature>(selected + 3), false, "fixture unavailable");
        CHECK(!runtime.Await(queued, 0).ok);
        for (size_t i = 0; i < 3; ++i) CHECK(runtime.Snapshot(1000).combat[i + 3].enabled == (i != selected));
        runtime.SetFeatureSupported(static_cast<CombatFeature>(selected + 3), true);
        CHECK(EnableFeature(runtime, session, names[selected]));
        auto pending = runtime.Submit(Feature(names[selected], true), session, 1000);
        CHECK(runtime.FilterMovement(0x1000, 4, 2001) == 4 && runtime.FilterCooldown(0x1000, 500, true, 2001) == 500
            && runtime.FilterInstantKill(0x1000, 2, 20, true, 2001) == 2);
        CHECK(runtime.Await(runtime.Submit(Command("heartbeat"), session, 2001), 0).ok);
        CHECK(!runtime.Await(pending, 0).ok);
        runtime.Observe(Observation(), 2001); runtime.Pump(2001);
        for (size_t i = 3; i < 6; ++i) CHECK(!runtime.Snapshot(2001).combat[i].enabled);
        for (const char* name : names) CHECK(EnableFeature(runtime, session, name, 1, 2001));
        auto stale_session = session;
        session = runtime.BeginSession(2001);
        CHECK(EnableFeature(runtime, session, "movement", 2, 2001));
        CHECK(!runtime.Await(runtime.Submit(Feature("movement", false), stale_session, 2001), 0).ok);
        CHECK(runtime.Snapshot(2001).combat[3].enabled);
        runtime.DisableAll();
        for (const auto& feature : runtime.Snapshot(2001).combat) CHECK(!feature.enabled);
    }
    return true;
}

bool NewFeaturesCommonSafetyBoundariesAndDeadlines() {
    for (int boundary = 0; boundary < 5; ++boundary) {
        Runtime runtime;
        ReadyAll(runtime);
        auto session = runtime.BeginSession(1000);
        for (const char* name : {"health", "experience", "damage", "movement", "cooldown", "instantKill"})
            CHECK(EnableFeature(runtime, session, name));
        auto pending = runtime.Submit(Feature("movement", true, 9), session, 1000);
        auto observation = Observation();
        if (boundary == 0) observation.player = 0x2000;
        if (boundary == 1) observation.scene = "Next fixture";
        if (boundary == 2) observation.safe_mode = false;
        if (boundary == 3) observation.verified = false;
        if (boundary == 4) observation.player_ready = false;
        runtime.Observe(observation, 1000); runtime.Pump(1000);
        CHECK(!runtime.Await(pending, 0).ok);
        for (const auto& feature : runtime.Snapshot(1000).combat) CHECK(!feature.enabled);
    }
    Runtime runtime;
    ReadyAll(runtime);
    auto session = runtime.BeginSession(1000);
    for (const char* name : {"movement", "cooldown", "instantKill"}) CHECK(EnableFeature(runtime, session, name));
    auto pending = runtime.Submit(Feature("movement", true, 9), session, 1000);
    runtime.Observe(Observation(), 1800);
    CHECK(runtime.Await(runtime.Submit(Command("heartbeat"), session, 1800), 0).ok);
    runtime.Pump(2501);
    CHECK(!runtime.Await(pending, 0).ok && runtime.FilterMovement(0x1000, 4, 2501) == 4);
    for (size_t i = 3; i < 6; ++i) CHECK(runtime.Snapshot(2501).combat[i].enabled);
    runtime.Observe(Observation(), 2600);
    runtime.Observe(Observation(), 3400);
    runtime.Observe(Observation(), 4200);
    runtime.CheckHeartbeat(4801);
    for (const auto& feature : runtime.Snapshot(4801).combat) CHECK(!feature.enabled);
    return true;
}

bool NewGetterAndPriorityConcurrentReads() {
    Runtime runtime;
    ReadyAll(runtime);
    auto session = runtime.BeginSession(1000);
    std::atomic<bool> finished{false}, valid{true};
    std::thread reader([&] {
        while (!finished.load()) {
            float movement = runtime.FilterMovement(0x1000, 4, 1000);
            float cooldown = runtime.FilterCooldown(0x1000, 500, true, 1000);
            float damage = runtime.FilterDamage(0x1000, 2, true, 1000);
            float final = runtime.FilterInstantKill(0x1000, damage, 20, true, 1000);
            if ((movement != 4 && movement != 8) || (cooldown != 500 && cooldown != 100)
                || (final != 2 && final != 6 && final != 2000)) valid.store(false);
        }
    });
    for (int iteration = 0; iteration < 120; ++iteration) {
        if (!EnableFeature(runtime, session, "movement", 2) || !EnableFeature(runtime, session, "cooldown")
            || !EnableFeature(runtime, session, "damage", 3) || !EnableFeature(runtime, session, "instantKill")) valid.store(false);
        runtime.DisableAll();
    }
    finished.store(true); reader.join();
    CHECK(valid.load());
    return true;
}

bool CombatQueuesAndIndependentDisables() {
    Runtime runtime;
    ReadyCombat(runtime);
    auto session = runtime.BeginSession(1000);
    auto health = runtime.Submit(Feature("health", true), session, 1000);
    CHECK(runtime.FilterHealthDamage(0x1000, 10, 1000) == 10);
    runtime.Pump(1000);
    CHECK(runtime.Await(health, 0).ok && runtime.FilterHealthDamage(0x1000, 10, 1000) == 0);
    CHECK(EnableFeature(runtime, session, "experience", 2));
    CHECK(EnableFeature(runtime, session, "damage", 3));
    CHECK(Enable(runtime, session));
    CHECK(runtime.Await(runtime.Submit(Feature("health", false), session, 1000), 0).ok);
    CHECK(runtime.FilterHealthDamage(0x1000, 10, 1000) == 10);
    CHECK(runtime.FilterExperience(0x1000, 5, 1000) == 10);
    CHECK(runtime.FilterDamage(0x1000, 5, true, 1000) == 15);
    CHECK(runtime.FilterArmor(0x1000, 5, 1000) == 20);
    CHECK(runtime.Await(runtime.Submit(Command("disableStats"), session, 1000), 0).ok);
    CHECK(runtime.FilterArmor(0x1000, 5, 1000) == 5);
    CHECK(runtime.FilterExperience(0x1000, 5, 1000) == 10 && runtime.FilterDamage(0x1000, 5, true, 1000) == 15);
    runtime.SetArmorSupported(false, "unavailable armor");
    runtime.SetFeatureSupported(CombatFeature::Health, false, "unavailable health");
    CHECK(runtime.FilterExperience(0x1000, 5, 1000) == 10 && runtime.FilterDamage(0x1000, 5, true, 1000) == 15);
    CHECK(runtime.Await(runtime.Submit(Command("disableAll"), session, 1000), 0).ok);
    CHECK(runtime.FilterExperience(0x1000, 5, 1000) == 5 && runtime.FilterDamage(0x1000, 5, true, 1000) == 5);
    return true;
}

bool CombatScopeRangesAndOverflow() {
    Runtime runtime;
    ReadyCombat(runtime);
    auto session = runtime.BeginSession(1000);
    for (double multiplier : {1.0, 2.5, 1000.0}) {
        CHECK(EnableFeature(runtime, session, "experience", multiplier));
        CHECK(EnableFeature(runtime, session, "damage", multiplier));
        CHECK(runtime.FilterExperience(0x1000, 3, 1000) == static_cast<float>(3 * multiplier));
        CHECK(runtime.FilterDamage(0x1000, 3, true, 1000) == static_cast<float>(3 * multiplier));
    }
    CHECK(EnableFeature(runtime, session, "health"));
    for (float value : {0.0f, -1.0f}) {
        CHECK(runtime.FilterHealthDamage(0x1000, value, 1000) == value);
        CHECK(runtime.FilterExperience(0x1000, value, 1000) == value);
        CHECK(runtime.FilterDamage(0x1000, value, true, 1000) == value);
    }
    CHECK(runtime.FilterHealthDamage(0x2000, 5, 1000) == 5);
    CHECK(runtime.FilterExperience(0x2000, 5, 1000) == 5);
    CHECK(runtime.FilterDamage(0x2000, 5, true, 1000) == 5);
    auto count = runtime.Snapshot(1000).combat[2].calls;
    CHECK(runtime.FilterDamage(0x1000, 5, false, 1000) == 5 && runtime.Snapshot(1000).combat[2].calls == count);
    auto huge = std::numeric_limits<float>::max();
    CHECK(runtime.FilterExperience(0x1000, huge, 1000) == huge);
    CHECK(runtime.FilterDamage(0x1000, huge, true, 1000) == huge);
    CHECK(runtime.Snapshot(1000).combat[1].overflow_skips == 1 && runtime.Snapshot(1000).combat[2].overflow_skips == 1);
    auto nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(std::isnan(runtime.FilterHealthDamage(0x1000, nan, 1000)));
    CHECK(std::isnan(runtime.FilterExperience(0x1000, nan, 1000)));
    CHECK(std::isnan(runtime.FilterDamage(0x1000, nan, true, 1000)));
    for (double value : {0.0, 1000.1, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        CHECK(!runtime.Await(runtime.Submit(Feature("experience", true, value), session, 1000), 0).ok);
        CHECK(!runtime.Snapshot(1000).combat[1].enabled && runtime.Snapshot(1000).combat[0].enabled);
    }
    CHECK(!runtime.Await(runtime.Submit(Feature("health", true, 2), session, 1000), 0).ok);
    return true;
}

bool CombatActualObservationsAndRawSamples() {
    Runtime runtime;
    ReadyCombat(runtime);
    auto observation = Observation();
    observation.health_known = observation.experience_known = observation.level_known = true;
    observation.health = 80; observation.experience = 1.5f; observation.level = 2;
    runtime.Observe(observation, 1000);
    for (int index = 0; index < 20; ++index) runtime.Observe(observation, 1000);
    auto state = runtime.Snapshot(1000);
    CHECK(state.health_known && state.health == 80 && state.experience == 1.5f && state.level == 2);
    for (const auto& feature : state.combat) CHECK(feature.calls == 0 && feature.actual_calls == 0 && !feature.io_known && !feature.result_known);
    auto session = runtime.BeginSession(1000);
    CHECK(runtime.FilterHealthDamage(0x1000, 2, 1000) == 2);
    runtime.ObserveHealthResult(0x1000, 80, 78, 1000);
    CHECK(EnableFeature(runtime, session, "health"));
    CHECK(runtime.FilterHealthDamage(0x1000, 5, 1000) == 0);
    runtime.ObserveHealthResult(0x1000, 78, 78, 1000);
    CHECK(EnableFeature(runtime, session, "experience", 2));
    CHECK(runtime.FilterExperience(0x1000, 1.5f, 1000) == 3);
    runtime.ObserveExperienceResult(0x1000, 1.5f, 4.5f, 2, 2, 1000);
    CHECK(EnableFeature(runtime, session, "damage", 3));
    CHECK(runtime.FilterDamage(0x1000, 2.5f, true, 1000) == 7.5f);
    runtime.ObserveDamageResult(0x1000, 20, 12.5f, true, 1000);
    runtime.ObserveDamageResult(0x1000, 99, 0, false, 1000);
    runtime.ObserveHealthResult(0x2000, 99, 0, 1000);
    runtime.ObserveExperienceResult(0x2000, 99, 100, 2, 2, 1000);
    state = runtime.Snapshot(1000);
    CHECK(state.combat[0].calls == 2 && state.combat[0].override_calls == 1 && state.combat[0].actual_calls == 2);
    CHECK(state.combat[0].result_before == 78 && state.combat[0].result_after == 78 && state.combat[0].actual_loss_total == 2);
    CHECK(state.combat[1].last_input == 1.5f && state.combat[1].last_output == 3 && state.combat[1].result_after == 4.5f);
    CHECK(state.combat[1].level_before == 2 && state.combat[1].level_after == 2 && state.combat[1].actual_calls == 1);
    CHECK(state.combat[2].result_before == 20 && state.combat[2].result_after == 12.5f && state.combat[2].actual_calls == 1);
    CHECK(state.combat[2].actual_loss_total == 7.5);
    return true;
}

bool CombatDoesNotRequireArmorAndHasLifecycleGuards() {
    Runtime runtime;
    ReadyCombat(runtime);
    auto session = runtime.BeginSession(1000);
    auto observation = Observation();
    observation.armor_original = std::numeric_limits<float>::quiet_NaN();
    runtime.SetArmorSupported(false, "unknown armor route");
    runtime.Observe(observation, 1000);
    CHECK(runtime.Snapshot(1000).player_ready && !runtime.Snapshot(1000).armor_supported && !runtime.Snapshot(1000).armor_sample_known);
    CHECK(EnableFeature(runtime, session, "health") && EnableFeature(runtime, session, "experience", 2) && EnableFeature(runtime, session, "damage", 3));
    CHECK(runtime.FilterHealthDamage(0x1000, 5, 1000) == 0);
    CHECK(runtime.FilterExperience(0x1000, 5, 1000) == 10);
    CHECK(runtime.FilterDamage(0x1000, 5, true, 1000) == 15);
    CHECK(runtime.Await(runtime.Submit(Command("heartbeat"), session, 1800), 0).ok);
    CHECK(runtime.FilterHealthDamage(0x1000, 5, 2001) == 5);
    CHECK(runtime.FilterExperience(0x1000, 5, 2001) == 5 && runtime.FilterDamage(0x1000, 5, true, 2001) == 5);
    runtime.Observe(observation, 2100);
    auto state = runtime.Snapshot(2100);
    for (const auto& feature : state.combat) CHECK(!feature.enabled);
    CHECK(EnableFeature(runtime, session, "health", 1, 2100));
    observation.player = 0x2000;
    runtime.Observe(observation, 2100);
    CHECK(!runtime.Snapshot(2100).combat[0].enabled);
    return true;
}

bool CombatQueuedDisableCancellationAndIsolation() {
    Runtime runtime;
    ReadyCombat(runtime);
    auto session = runtime.BeginSession(1000);
    auto health = runtime.Submit(Feature("health", true), session, 1000);
    auto experience = runtime.Submit(Feature("experience", true, 2), session, 1000);
    auto damage = runtime.Submit(Feature("damage", true, 3), session, 1000);
    CHECK(runtime.Await(runtime.Submit(Feature("health", false), session, 1000), 0).ok);
    CHECK(!runtime.Await(health, 0).ok);
    runtime.Pump(1000);
    CHECK(runtime.Await(experience, 0).ok && runtime.Await(damage, 0).ok);
    CHECK(!runtime.Snapshot(1000).combat[0].enabled && runtime.Snapshot(1000).combat[1].enabled && runtime.Snapshot(1000).combat[2].enabled);
    auto expired = runtime.Submit(Feature("health", true), session, 1000);
    for (uint64_t now : {1500ULL, 2000ULL, 2500ULL}) runtime.Observe(Observation(), now);
    runtime.Pump(2501);
    CHECK(!runtime.Await(expired, 0).ok && !runtime.Snapshot(2501).combat[0].enabled);
    CHECK(runtime.Snapshot(2501).combat[1].enabled && runtime.Snapshot(2501).combat[2].enabled);
    auto previous = session;
    session = runtime.BeginSession(2600);
    CHECK(EnableFeature(runtime, session, "experience", 2, 2600));
    runtime.EndSession(previous);
    CHECK(runtime.FilterExperience(0x1000, 5, 2600) == 10);
    auto stale = runtime.Await(runtime.Submit(Feature("damage", true, 5), previous, 2600), 0);
    CHECK(!stale.ok && runtime.Snapshot(2600).combat[1].enabled);
    runtime.EndSession(session);
    CHECK(runtime.FilterExperience(0x1000, 5, 2600) == 5);
    return true;
}

bool CombatAllSafetyBoundaries() {
    for (int boundary = 0; boundary < 4; ++boundary) {
        Runtime runtime;
        ReadyCombat(runtime);
        auto session = runtime.BeginSession(1000);
        CHECK(EnableFeature(runtime, session, "health") && EnableFeature(runtime, session, "experience", 2) && EnableFeature(runtime, session, "damage", 3));
        auto queued = runtime.Submit(Feature("damage", true, 99), session, 1000);
        auto observation = Observation();
        if (boundary == 0) observation.player_ready = false;
        if (boundary == 1) observation.safe_mode = false;
        if (boundary == 2) observation.verified = false;
        if (boundary == 3) observation.scene = "New run";
        runtime.Observe(observation, 1000);
        runtime.Pump(1000);
        CHECK(!runtime.Await(queued, 0).ok);
        for (const auto& feature : runtime.Snapshot(1000).combat) CHECK(!feature.enabled);
        CHECK(runtime.FilterHealthDamage(0x1000, 5, 1000) == 5);
        CHECK(runtime.FilterExperience(0x1000, 5, 1000) == 5 && runtime.FilterDamage(0x1000, 5, true, 1000) == 5);
    }
    Runtime heartbeat;
    ReadyCombat(heartbeat);
    auto session = heartbeat.BeginSession(1000);
    CHECK(EnableFeature(heartbeat, session, "health") && EnableFeature(heartbeat, session, "experience", 2) && EnableFeature(heartbeat, session, "damage", 3));
    for (uint64_t tick : {1500ULL, 2300ULL, 3100ULL, 3900ULL}) heartbeat.Observe(Observation(), tick);
    heartbeat.CheckHeartbeat(4001);
    for (const auto& feature : heartbeat.Snapshot(4001).combat) CHECK(!feature.enabled);
    CHECK(!heartbeat.Await(heartbeat.Submit(Feature("health", true), session, 4001), 0).ok);
    return true;
}

bool CombatConcurrentHookReads() {
    Runtime runtime;
    ReadyCombat(runtime);
    auto session = runtime.BeginSession(1000);
    std::atomic<bool> stop{false}, valid{true};
    std::thread reader([&] {
        while (!stop.load()) {
            float health = runtime.FilterHealthDamage(0x1000, 5, 1000);
            float xp = runtime.FilterExperience(0x1000, 5, 1000);
            float damage = runtime.FilterDamage(0x1000, 5, true, 1000);
            if ((health != 5 && health != 0) || (xp != 5 && xp != 10) || (damage != 5 && damage != 15)) valid.store(false);
            if (runtime.FilterDamage(0x1000, 5, false, 1000) != 5) valid.store(false);
        }
    });
    for (int index = 0; index < 500; ++index) {
        if (!EnableFeature(runtime, session, "health") || !EnableFeature(runtime, session, "experience", 2)
            || !EnableFeature(runtime, session, "damage", 3)) valid.store(false);
        runtime.Snapshot(1000);
        runtime.DisableAll();
    }
    stop.store(true); reader.join();
    CHECK(valid.load());
    return true;
}

struct Json {
    explicit Json(const std::string& text) : doc(yyjson_read(text.data(), text.size(), 0)) {}
    ~Json() { if (doc) yyjson_doc_free(doc); }
    yyjson_val* root() const { return doc ? yyjson_doc_get_root(doc) : nullptr; }
    yyjson_doc* doc;
};

bool ImmediateObservationAndQueuedMutation() {
    Runtime runtime;
    auto session = runtime.BeginSession(1000);
    auto hello = runtime.Await(runtime.Submit(Command("hello"), session, 1000), 0);
    CHECK(hello.ok && !hello.state.player_ready && !hello.state.armor_supported);
    Ready(runtime);
    auto ticket = runtime.Submit(Armor(true), session, 1000);
    Response response;
    CHECK(!runtime.TryResult(ticket, response));
    CHECK(runtime.FilterArmor(0x1000, 5, 1000) == 5);
    runtime.Pump(1000);
    CHECK(runtime.TryResult(ticket, response) && response.ok && response.state.armor_enabled);
    CHECK(!response.state.armor_current_known);
    CHECK(runtime.FilterArmor(0x1000, 5, 1000) == 20);
    CHECK(runtime.Snapshot(1000).armor_current_known);
    return true;
}

bool NumericBoundsAndUnsupportedCommands() {
    Runtime runtime;
    Ready(runtime);
    auto session = runtime.BeginSession(1000);
    for (double value : {0.0, 10000.0, 20.125}) {
        CHECK(Enable(runtime, session, 1000, value));
        CHECK(runtime.FilterArmor(0x1000, 5, 1000) == static_cast<float>(value));
    }
    for (double value : {-0.1, 10000.1, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto response = runtime.Await(runtime.Submit(Armor(true, value), session, 1000), 0);
        CHECK(!response.ok && !response.state.armor_enabled);
        runtime.Pump(1000);
        CHECK(runtime.FilterArmor(0x1000, 5, 1000) == 5);
    }
    for (const char* kind : {"setFeature", "unknown"}) {
        CHECK(Enable(runtime, session));
        auto response = runtime.Await(runtime.Submit(Command(kind), session, 1000), 0);
        CHECK(!response.ok && !runtime.Snapshot(1000).armor_enabled);
    }
    auto request = Armor(true);
    request.stat = "might";
    CHECK(!runtime.Await(runtime.Submit(request, session, 1000), 0).ok);
    return true;
}

bool GetterScopeAndNaturalRestoration() {
    Runtime runtime;
    Ready(runtime);
    auto session = runtime.BeginSession(1000);
    CHECK(Enable(runtime, session));
    CHECK(runtime.FilterArmor(0x2000, 5, 1000) == 5);
    CHECK(runtime.FilterArmor(0, 5, 1000) == 5);
    CHECK(std::isnan(runtime.FilterArmor(0x1000, std::numeric_limits<float>::quiet_NaN(), 1000)));
    CHECK(runtime.FilterArmor(0x1000, 7, 1000) == 20);
    auto disabled = runtime.Await(runtime.Submit(Armor(false), session, 1000), 0);
    CHECK(disabled.ok && !disabled.state.armor_enabled);
    CHECK(runtime.FilterArmor(0x1000, 8, 1000) == 8);
    auto state = runtime.Snapshot(1000);
    CHECK(state.armor_game_reads == 2 && state.armor_override_reads == 1);
    CHECK(state.armor_last_input == 8 && state.armor_last_output == 8 && !state.armor_last_was_override);
    return true;
}

bool ActualReadCountersExcludeSamples() {
    Runtime runtime;
    Ready(runtime);
    auto session = runtime.BeginSession(1000);
    for (int index = 0; index < 100; ++index) runtime.Observe(Observation(), 1000);
    CHECK(runtime.Snapshot(1000).armor_game_reads == 0);
    CHECK(!runtime.Snapshot(1000).armor_current_known);
    CHECK(runtime.FilterArmor(0x1000, 0.1f, 1000) == 0.1f);
    CHECK(Enable(runtime, session, 1000, 20.5));
    CHECK(!runtime.Snapshot(1000).armor_current_known);
    CHECK(runtime.FilterArmor(0x1000, 0.2f, 1000) == 20.5f);
    runtime.Observe(Observation(), 1000);
    auto state = runtime.Snapshot(1000);
    CHECK(state.armor_game_reads == 2 && state.armor_override_reads == 1);
    CHECK(state.armor_last_input == 0.2f && state.armor_last_output == 20.5f && state.armor_current_known);
    Json doc(SerializeResponse(Response{"actual", true, {}, state}));
    CHECK(doc.doc);
    auto diagnostics = yyjson_obj_get(yyjson_obj_get(doc.root(), "state"), "diagnostics");
    CHECK(std::strcmp(yyjson_get_str(yyjson_obj_get(diagnostics, "stat.armor.gameReads")), "2") == 0);
    CHECK(std::strcmp(yyjson_get_str(yyjson_obj_get(diagnostics, "native.armor.lastInput")), "0.2") == 0);
    return true;
}

bool LifecycleClearsEnabledAndQueuedWork() {
    Runtime runtime;
    Ready(runtime);
    auto session = runtime.BeginSession(1000);
    CHECK(Enable(runtime, session));
    auto queued = runtime.Submit(Armor(true, 99), session, 1000);
    auto observation = Observation(0x2000);
    runtime.Observe(observation, 1000);
    runtime.Pump(1000);
    CHECK(!runtime.Await(queued, 0).ok && !runtime.Snapshot(1000).armor_enabled);
    CHECK(runtime.FilterArmor(0x2000, 6, 1000) == 6);
    for (int boundary = 0; boundary < 4; ++boundary) {
        Ready(runtime);
        CHECK(Enable(runtime, session));
        observation = Observation();
        if (boundary == 0) observation.player_ready = false;
        if (boundary == 1) observation.safe_mode = false;
        if (boundary == 2) observation.verified = false;
        if (boundary == 3) observation.scene = "Another run";
        runtime.Observe(observation, 1000);
        CHECK(!runtime.Snapshot(1000).armor_enabled);
        CHECK(runtime.FilterArmor(0x1000, 6, 1000) == 6);
    }
    runtime.SetArmorSupported(false, "Missing native entry");
    CHECK(!runtime.Snapshot(1000).armor_supported);
    CHECK(runtime.Await(runtime.Submit(Armor(false), session, 1000), 0).ok);
    return true;
}

bool DeadlineCancellationAndHeartbeat() {
    Runtime runtime;
    Ready(runtime);
    auto session = runtime.BeginSession(1000);
    auto expired = runtime.Submit(Armor(true), session, 1000);
    for (uint64_t tick : {1500ULL, 2000ULL, 2500ULL}) runtime.Observe(Observation(), tick);
    runtime.Pump(2501);
    CHECK(!runtime.Await(expired, 0).ok && !runtime.Snapshot(2501).armor_enabled);
    auto cancelled = runtime.Submit(Armor(true), session, 2501);
    CHECK(!runtime.Await(cancelled, 0).ok);
    runtime.Pump(2502);
    CHECK(!runtime.Snapshot(2502).armor_enabled);
    CHECK(Enable(runtime, session, 2502));
    for (uint64_t tick : {3000ULL, 3800ULL, 4600ULL, 5400ULL}) runtime.Observe(Observation(), tick);
    CHECK(runtime.FilterArmor(0x1000, 5, 5502) == 20);
    CHECK(runtime.FilterArmor(0x1000, 5, 5503) == 5);
    CHECK(!runtime.Snapshot(5503).armor_enabled);
    CHECK(!runtime.Await(runtime.Submit(Armor(true), session, 5503), 0).ok);
    auto next = runtime.BeginSession(6000);
    CHECK(Enable(runtime, next, 6000));
    runtime.EndSession(session);
    CHECK(runtime.FilterArmor(0x1000, 5, 6000) == 20);
    CHECK(!runtime.Await(runtime.Submit(Armor(true, 99), session, 6000), 0).ok);
    CHECK(runtime.FilterArmor(0x1000, 5, 6000) == 20);
    runtime.EndSession(next);
    CHECK(runtime.FilterArmor(0x1000, 5, 6000) == 5);
    return true;
}

bool FreshObservationIsIndependentOfHeartbeats() {
    Runtime runtime;
    Ready(runtime);
    auto session = runtime.BeginSession(1000);
    CHECK(Enable(runtime, session));
    auto queued = runtime.Submit(Armor(true, 99), session, 1000);
    for (uint64_t tick : {1400ULL, 1800ULL})
        CHECK(runtime.Await(runtime.Submit(Command("heartbeat"), session, tick), 0).ok);
    CHECK(runtime.FilterArmor(0x1000, 5, 2000) == 20);
    CHECK(runtime.FilterArmor(0x1000, 5, 2001) == 5);
    auto state = runtime.Snapshot(2001);
    CHECK(!state.armor_enabled && !state.player_ready && !state.paused_known);
    CHECK(!runtime.Await(queued, 0).ok);
    CHECK(!runtime.Await(runtime.Submit(Armor(true), session, 2002), 0).ok);
    runtime.Observe(Observation(), 2100);
    runtime.Pump(2100);
    CHECK(runtime.Snapshot(2100).player_ready && !runtime.Snapshot(2100).armor_enabled);
    CHECK(runtime.FilterArmor(0x1000, 5, 2100) == 5);
    CHECK(Enable(runtime, session, 2101));
    runtime.Observe(Observation(), 3000);
    runtime.CheckHeartbeat(4001);
    CHECK(!runtime.Snapshot(4001).armor_enabled);
    runtime.Observe(Observation(), 4002);
    CHECK(!runtime.Snapshot(4002).armor_enabled);

    Runtime resume;
    Ready(resume, 10000);
    auto resumed_session = resume.BeginSession(10000);
    CHECK(Enable(resume, resumed_session, 10000));
    CHECK(resume.Await(resume.Submit(Command("heartbeat"), resumed_session, 11000), 0).ok);
    auto before_resume = resume.Submit(Armor(true, 99), resumed_session, 11000);
    // No Snapshot/watchdog has invalidated the stale observation yet. Observe
    // itself must expire the old state before publishing a fresh sample.
    resume.Observe(Observation(), 11002);
    resume.Pump(11002);
    CHECK(!resume.Await(before_resume, 0).ok && !resume.Snapshot(11002).armor_enabled);

    Runtime stalled_pump;
    Ready(stalled_pump, 5000);
    auto stalled_session = stalled_pump.BeginSession(5000);
    CHECK(Enable(stalled_pump, stalled_session, 5000));
    auto stalled_queue = stalled_pump.Submit(Armor(true, 99), stalled_session, 5000);
    CHECK(stalled_pump.Await(stalled_pump.Submit(Command("heartbeat"), stalled_session, 5500), 0).ok);
    stalled_pump.Pump(6001);
    CHECK(!stalled_pump.Await(stalled_queue, 0).ok);
    stalled_pump.Observe(Observation(), 6002);
    CHECK(!stalled_pump.Snapshot(6002).armor_enabled);

    Runtime menu;
    menu.SetArmorSupported(true);
    auto observation = Observation(0);
    observation.player_ready = false;
    observation.scene = "Main menu";
    menu.Observe(observation, 1000);
    auto menu_session = menu.BeginSession(1000);
    for (uint64_t tick : {2000ULL, 4000ULL, 5000ULL}) {
        auto reply = menu.Await(menu.Submit(Command("heartbeat"), menu_session, tick), 0);
        CHECK(reply.ok && !reply.state.player_ready && reply.state.error.empty());
    }
    return true;
}

bool EnableAllEffects(Runtime& runtime, uint64_t session, uint64_t now = 1000) {
    if (!Enable(runtime, session, now)) return false;
    for (const char* name : {"health", "experience", "damage", "movement", "cooldown", "instantKill"}) {
        double value = name == std::string_view("experience") || name == std::string_view("movement") ? 2
            : name == std::string_view("damage") ? 3 : 1;
        if (!EnableFeature(runtime, session, name, value, now)) return false;
    }
    return true;
}

bool AllEffectsActive(const StateSnapshot& state) {
    if (!state.player_ready || !state.armor_enabled) return false;
    for (size_t index = 0; index < 6; ++index) if (!state.combat[index].enabled) return false;
    return true;
}

bool ReorderedFrameAndWatchdogSamplesKeepLiveSession() {
    for (unsigned operation = 0; operation < 4; ++operation) {
        Runtime runtime;
        ReadyAll(runtime);
        auto session = runtime.BeginSession(1000);
        CHECK(EnableAllEffects(runtime, session));
        // The frame/watchdog sampled 1005 before taking the runtime lock. The
        // pipe's newer heartbeat acquired that lock first at 1010.
        CHECK(runtime.Await(runtime.Submit(Command("heartbeat"), session, 1010), 0).ok);
        if (operation == 0) runtime.Observe(Observation(), 1005);
        else if (operation == 1) runtime.Pump(1005);
        else if (operation == 2) runtime.CheckHeartbeat(1005);
        else CHECK(AllEffectsActive(runtime.Snapshot(1005)));
        CHECK(AllEffectsActive(runtime.Snapshot(1011)));
        CHECK(runtime.Await(runtime.Submit(Command("heartbeat"), session, 1011), 0).ok);
    }
    return true;
}

bool ReorderedQueueAndHookSamplesKeepAllEffects() {
    Runtime runtime;
    ReadyAll(runtime);
    auto session = runtime.BeginSession(1000);
    CHECK(EnableAllEffects(runtime, session));
    runtime.Observe(Observation(), 1010);
    CHECK(runtime.Await(runtime.Submit(Command("heartbeat"), session, 1010), 0).ok);
    // A getter can load a newer observation/heartbeat after sampling its now.
    CHECK(runtime.FilterArmor(0x1000, 5, 1005) == 20);
    CHECK(runtime.FilterHealthDamage(0x1000, 5, 1005) == 0);
    CHECK(runtime.FilterExperience(0x1000, 5, 1005) == 10);
    CHECK(runtime.FilterDamage(0x1000, 5, true, 1005) == 15);
    CHECK(runtime.FilterMovement(0x1000, 4, 1005) == 8);
    CHECK(runtime.FilterCooldown(0x1000, 500, true, 1005) == 100);
    CHECK(runtime.FilterInstantKill(0x1000, 5, 20, true, 1005) == 2000);
    CHECK(AllEffectsActive(runtime.Snapshot(1005)));

    runtime.DisableAll();
    std::vector<Runtime::Ticket> tickets{runtime.Submit(Armor(true), session, 1020)};
    for (const char* name : {"health", "experience", "damage", "movement", "cooldown", "instantKill"})
        tickets.push_back(runtime.Submit(Feature(name, true), session, 1020));
    // Pump executes later in the same frame, using the frame's earlier sample.
    runtime.Pump(1015);
    for (const auto& ticket : tickets) CHECK(runtime.Await(ticket, 0).ok);
    CHECK(AllEffectsActive(runtime.Snapshot(1021)));
    return true;
}

bool LateHeartbeatSamplesKeepNewestDeadline() {
    Runtime runtime;
    ReadyAll(runtime);
    auto session = runtime.BeginSession(1000);
    CHECK(EnableAllEffects(runtime, session));
    runtime.Observe(Observation(), 2000);
    CHECK(runtime.Await(runtime.Submit(Command("heartbeat"), session, 2010), 0).ok);
    // A delayed request must not move the accepted heartbeat backwards.
    CHECK(runtime.Await(runtime.Submit(Command("heartbeat"), session, 2005), 0).ok);
    for (uint64_t tick : {2900ULL, 3800ULL, 4700ULL}) runtime.Observe(Observation(), tick);
    runtime.CheckHeartbeat(5010);
    CHECK(AllEffectsActive(runtime.Snapshot(5010)));
    CHECK(runtime.FilterArmor(0x1000, 5, 5010) == 20);
    runtime.CheckHeartbeat(5011);
    auto state = runtime.Snapshot(5011);
    CHECK(state.player_ready && !state.armor_enabled);
    for (const auto& feature : state.combat) CHECK(!feature.enabled);
    CHECK(!runtime.Await(runtime.Submit(Command("heartbeat"), session, 5011), 0).ok);
    return true;
}

bool ReorderedSamplesRetainAllSafetyDeadlines() {
    Runtime fresh;
    ReadyAll(fresh);
    auto session = fresh.BeginSession(1000);
    CHECK(EnableAllEffects(fresh, session));
    CHECK(fresh.Await(fresh.Submit(Command("heartbeat"), session, 1800), 0).ok);
    fresh.CheckHeartbeat(1795);
    CHECK(AllEffectsActive(fresh.Snapshot(2000)));
    CHECK(fresh.FilterArmor(0x1000, 5, 2001) == 5);
    auto stale = fresh.Snapshot(2001);
    CHECK(!stale.player_ready && !stale.armor_enabled);
    for (const auto& feature : stale.combat) CHECK(!feature.enabled);

    for (uint64_t delay : {1500ULL, 1501ULL}) {
        Runtime queued;
        ReadyAll(queued);
        auto queued_session = queued.BeginSession(1000);
        std::vector<Runtime::Ticket> tickets{queued.Submit(Armor(true), queued_session, 1000)};
        for (const char* name : {"health", "experience", "damage", "movement", "cooldown", "instantKill"})
            tickets.push_back(queued.Submit(Feature(name, true), queued_session, 1000));
        for (uint64_t tick : {1500ULL, 2000ULL, 2500ULL}) queued.Observe(Observation(), tick);
        queued.Pump(1000 + delay);
        for (const auto& ticket : tickets) CHECK(queued.Await(ticket, 0).ok == (delay == 1500));
        auto state = queued.Snapshot(1000 + delay);
        CHECK(state.player_ready && state.armor_enabled == (delay == 1500));
        for (size_t index = 0; index < 6; ++index) CHECK(state.combat[index].enabled == (delay == 1500));
    }
    return true;
}

bool QueueBudgetAndEmergencyDisable() {
    Runtime runtime;
    Ready(runtime);
    auto session = runtime.BeginSession(1000);
    std::vector<Runtime::Ticket> tickets;
    for (int index = 0; index < 40; ++index) tickets.push_back(runtime.Submit(Armor(true, index), session, 1000));
    runtime.Pump(1000);
    Response response;
    CHECK(runtime.TryResult(tickets[31], response) && !runtime.TryResult(tickets[32], response));
    CHECK(runtime.FilterArmor(0x1000, 5, 1000) == 31);
    CHECK(runtime.Await(runtime.Submit(Command("disableAll"), session, 1000), 0).ok);
    runtime.Pump(1000);
    CHECK(runtime.FilterArmor(0x1000, 5, 1000) == 5);
    CHECK(!runtime.Await(tickets[39], 0).ok);
    tickets.clear();
    for (int index = 0; index < 40; ++index) tickets.push_back(runtime.Submit(Armor(true, index), session, 1000));
    auto disable_with_target = Armor(false);
    disable_with_target.has_value = true;
    disable_with_target.value = 25;
    auto disable_ticket = runtime.Submit(disable_with_target, session, 1000);
    CHECK(!runtime.Snapshot(1000).armor_enabled && !runtime.Await(tickets[0], 0).ok);
    runtime.Pump(1000);
    CHECK(runtime.Await(disable_ticket, 0).ok && !runtime.Snapshot(1000).armor_enabled);
    CHECK(runtime.Snapshot(1000).armor_target == 25 && runtime.FilterArmor(0x1000, 5, 1000) == 5);
    for (int index = 0; index < 64; ++index) runtime.Submit(Armor(true), session, 1000);
    CHECK(!runtime.Await(runtime.Submit(Armor(true), session, 1000), 0).ok);
    runtime.Pump(1000);
    CHECK(!runtime.Snapshot(1000).armor_enabled);
    return true;
}

bool SnapshotOwnershipAndConcurrentReads() {
    Runtime runtime;
    Ready(runtime);
    auto session = runtime.BeginSession(1000);
    auto copy = runtime.Snapshot(1000);
    copy.scene = "external mutation";
    CHECK(runtime.Snapshot(1000).scene == "Offline fixture");
    std::atomic<bool> stop{false}, valid{true};
    std::thread reader([&] {
        while (!stop.load()) {
            float value = runtime.FilterArmor(0x1000, 5, 1000);
            if (value != 5 && value != 20) valid.store(false);
            if (runtime.FilterArmor(0x2000, 7, 1000) != 7) valid.store(false);
        }
    });
    for (int index = 0; index < 1000; ++index) {
        if (!Enable(runtime, session)) valid.store(false);
        runtime.Snapshot(1000);
        runtime.DisableAll();
    }
    stop.store(true);
    reader.join();
    CHECK(valid.load());
    return true;
}

bool JsonInputBoundaries() {
    Request request;
    std::string error;
    CHECK(ParseRequest(R"({"Id":"unicode-界","Kind":"setStat","Stat":"armor","Enabled":true,"Value":20.25,"feature":null,"backupToken":null})", request, error));
    CHECK(request.kind == "setStat" && request.has_enabled && request.enabled && request.has_value && request.value == 20.25);
    CHECK(ParseRequest(R"({"id":"x","kind":"hello","enabled":null,"value":null,"stat":null})", request, error));
    CHECK(!request.has_enabled && !request.has_value && request.stat.empty());
    for (const char* json : {"null", "[]", "{}", R"({"id":" ","kind":"hello"})", R"({"id":"x","ID":"y"})",
            R"({"id":"x","enabled":"true"})", R"({"id":"x","value":"NaN"})", R"({"id":"x","value":1e999})",
            R"({"id":"x","kind":null})", R"({"id":"x"} {"id":"y"})", R"({"id":"\ud800"})"})
        CHECK(!ParseRequest(json, request, error));
    CHECK(!ParseRequest(std::string("{\"id\":\"") + '\xC0' + '\xAF' + "\"}", request, error));
    size_t units = 0;
    CHECK(Utf16Length("界\xF0\x9F\x98\x80", units) && units == 3);
    std::string emoji;
    for (int index = 0; index < 64; ++index) emoji += "\xF0\x9F\x98\x80";
    CHECK(ValidRequestId(emoji));
    emoji += "\xF0\x9F\x98\x80";
    CHECK(!ValidRequestId(emoji) && !ValidRequestId("\xE3\x80\x80"));
    std::string maximum = R"({"id":"x","kind":"hello"})";
    maximum.append(kMaximumMessageCharacters - maximum.size(), ' ');
    CHECK(ParseRequest(maximum, request, error));
    maximum += ' ';
    CHECK(!ParseRequest(maximum, request, error));
    return true;
}

bool JsonCapabilityAndEscapingContract() {
    Runtime runtime;
    Ready(runtime);
    auto state = runtime.Snapshot(1000);
    state.paused_known = false;
    Response response{std::string("a\0b", 3), true, {}, state};
    Json doc(SerializeResponse(response));
    CHECK(doc.doc && yyjson_get_len(yyjson_obj_get(doc.root(), "id")) == 3);
    auto value = yyjson_obj_get(doc.root(), "state");
    CHECK(yyjson_is_null(yyjson_obj_get(value, "gold")) && !yyjson_get_bool(yyjson_obj_get(value, "backupAuthorized")));
    auto features = yyjson_obj_get(value, "features"), stats = yyjson_obj_get(value, "stats");
    CHECK(yyjson_arr_size(features) == kCombatFeatureCount && yyjson_arr_size(stats) == 23);
    size_t supported = 0;
    for (size_t index = 0; index < yyjson_arr_size(stats); ++index) {
        auto stat = yyjson_arr_get(stats, index);
        if (yyjson_get_bool(yyjson_obj_get(stat, "supported"))) {
            ++supported;
            CHECK(std::strcmp(yyjson_get_str(yyjson_obj_get(stat, "id")), "armor") == 0);
            CHECK(yyjson_is_null(yyjson_obj_get(stat, "currentValue")));
        }
    }
    CHECK(supported == 1);
    for (size_t index = 0; index < yyjson_arr_size(features); ++index)
        CHECK(!yyjson_get_bool(yyjson_obj_get(yyjson_arr_get(features, index), "supported")));
    auto diagnostics = yyjson_obj_get(value, "diagnostics");
    CHECK(yyjson_obj_get(diagnostics, "game.paused") == nullptr);
    auto chest = yyjson_obj_get(value, "chestReward");
    CHECK(!yyjson_get_bool(yyjson_obj_get(chest, "supported")) && !yyjson_get_bool(yyjson_obj_get(chest, "upgradeSupported")));
    return true;
}

bool CombatWireFeaturesAndMeasuredDiagnostics() {
    Request request;
    std::string error;
    CHECK(ParseRequest(R"({"id":"f","kind":"setFeature","feature":"experience","enabled":true,"value":2.5})", request, error));
    CHECK(request.feature == "experience" && request.stat.empty() && request.value == 2.5);
    CHECK(!ParseRequest(R"({"id":"f","feature":"health","Feature":"damage"})", request, error));
    CHECK(!ParseRequest(R"({"id":"f","feature":true})", request, error));
    Runtime runtime;
    ReadyCombat(runtime);
    auto observation = Observation();
    observation.health_known = observation.experience_known = observation.level_known = true;
    observation.health = 100; observation.experience = 0.5f; observation.level = 1;
    runtime.Observe(observation, 1000);
    auto session = runtime.BeginSession(1000);
    CHECK(EnableFeature(runtime, session, "health") && EnableFeature(runtime, session, "experience", 2.5) && EnableFeature(runtime, session, "damage", 3));
    runtime.FilterHealthDamage(0x1000, 2, 1000); runtime.ObserveHealthResult(0x1000, 100, 100, 1000);
    runtime.FilterExperience(0x1000, 0.5f, 1000); runtime.ObserveExperienceResult(0x1000, 0.5f, 1.75f, 1, 1, 1000);
    runtime.FilterDamage(0x1000, 2, true, 1000); runtime.ObserveDamageResult(0x1000, 10, 4, true, 1000);
    std::string wire = SerializeResponse(Response{"features", true, {}, runtime.Snapshot(1000)});
    Json doc(wire);
    CHECK(doc.doc);
    auto state = yyjson_obj_get(doc.root(), "state");
    auto features = yyjson_obj_get(state, "features");
    CHECK(yyjson_arr_size(features) == kCombatFeatureCount);
    size_t supported = 0;
    for (size_t index = 0; index < yyjson_arr_size(features); ++index) {
        auto feature = yyjson_arr_get(features, index);
        const char* id = yyjson_get_str(yyjson_obj_get(feature, "id"));
        bool migrated = std::strcmp(id, "health") == 0 || std::strcmp(id, "experience") == 0 || std::strcmp(id, "damage") == 0;
        CHECK(yyjson_get_bool(yyjson_obj_get(feature, "supported")) == migrated);
        CHECK(yyjson_get_bool(yyjson_obj_get(feature, "enabled")) == migrated);
        if (migrated) ++supported;
    }
    CHECK(supported == 3);
    auto diagnostics = yyjson_obj_get(state, "diagnostics");
    for (const char* name : {"native.health.calls", "native.health.overrideCalls", "native.health.actualCalls",
            "native.experience.calls", "native.experience.overrideCalls", "native.experience.actualCalls",
            "native.damage.calls", "native.damage.overrideCalls", "native.damage.actualCalls"})
        CHECK(yyjson_is_str(yyjson_obj_get(diagnostics, name)) && std::strcmp(yyjson_get_str(yyjson_obj_get(diagnostics, name)), "1") == 0);
    CHECK(std::strcmp(yyjson_get_str(yyjson_obj_get(diagnostics, "native.health.lastOutput")), "0") == 0);
    CHECK(std::strcmp(yyjson_get_str(yyjson_obj_get(diagnostics, "native.health.lastHpBefore")), "100") == 0);
    CHECK(std::strcmp(yyjson_get_str(yyjson_obj_get(diagnostics, "native.experience.lastOutput")), "1.25") == 0);
    CHECK(std::strcmp(yyjson_get_str(yyjson_obj_get(diagnostics, "native.experience.lastXpAfter")), "1.75") == 0);
    CHECK(std::strcmp(yyjson_get_str(yyjson_obj_get(diagnostics, "native.experience.lastLevelAfter")), "1") == 0);
    CHECK(std::strcmp(yyjson_get_str(yyjson_obj_get(diagnostics, "native.damage.lastOutput")), "6") == 0);
    CHECK(std::strcmp(yyjson_get_str(yyjson_obj_get(diagnostics, "native.damage.lastHpAfter")), "4") == 0);
    CHECK(std::strcmp(yyjson_get_str(yyjson_obj_get(diagnostics, "sample.player.hp")), "100") == 0);
    CHECK(std::strcmp(yyjson_get_str(yyjson_obj_get(diagnostics, "sample.player.xp")), "0.5") == 0);
    CHECK(std::strcmp(yyjson_get_str(yyjson_obj_get(diagnostics, "sample.player.level")), "1") == 0);
    return true;
}

bool MovementCooldownInstantKillWireEvidence() {
    Runtime runtime;
    ReadyAll(runtime);
    auto session = runtime.BeginSession(1000);
    for (const auto& feature : runtime.Snapshot(1000).combat)
        CHECK(feature.calls == 0 && feature.actual_calls == 0 && !feature.io_known && !feature.result_known);
    for (const char* name : {"health", "experience", "damage", "movement", "cooldown", "instantKill"})
        CHECK(EnableFeature(runtime, session, name, name == std::string_view("movement") ? 2 : name == std::string_view("damage") ? 3 : 1));
    runtime.Observe(Observation(), 1000);
    CHECK(runtime.Snapshot(1000).combat[3].calls == 0 && runtime.Snapshot(1000).combat[4].calls == 0);
    CHECK(runtime.FilterMovement(0x1000, 4, 1000) == 8 && runtime.FilterCooldown(0x1000, 500, true, 1000) == 100);
    float intermediate = runtime.FilterDamage(0x1000, 2, true, 1000);
    CHECK(runtime.FilterInstantKill(0x1000, intermediate, 20, true, 1000) == 2000);
    runtime.ObserveDamageResult(0x1000, 20, 0, true, 1000);
    runtime.ObserveInstantKillResult(0x1000, 20, 0, true, 1000);
    auto wire = SerializeResponse(Response{"new-features", true, {}, runtime.Snapshot(1000)});
    Json doc(wire);
    CHECK(doc.doc);
    auto state = yyjson_obj_get(doc.root(), "state"), features = yyjson_obj_get(state, "features");
    CHECK(yyjson_arr_size(features) == kCombatFeatureCount);
    size_t supported = 0;
    for (size_t index = 0; index < yyjson_arr_size(features); ++index) {
        auto feature = yyjson_arr_get(features, index);
        const char* id = yyjson_get_str(yyjson_obj_get(feature, "id"));
        bool migrated = std::strcmp(id, "noGoldSpend") != 0 && std::strcmp(id, kGameSpeedId) != 0
            && std::strcmp(id, kSkipChestAnimationId) != 0 && std::strcmp(id, kFiveItemChestsId) != 0;
        CHECK(yyjson_get_bool(yyjson_obj_get(feature, "supported")) == migrated);
        CHECK(yyjson_get_bool(yyjson_obj_get(feature, "enabled")) == migrated);
        if (migrated) ++supported;
    }
    CHECK(supported == 6);
    auto diagnostics = yyjson_obj_get(state, "diagnostics");
    auto equals = [&](const char* key, const char* text) {
        const char* actual = yyjson_get_str(yyjson_obj_get(diagnostics, key));
        return actual && std::strcmp(actual, text) == 0;
    };
    CHECK(equals("native.movement.calls", "1") && equals("native.movement.gameReads", "1")
        && equals("native.movement.actualCalls", "1") && equals("native.movement.overrideReads", "1")
        && equals("native.movement.lastInput", "4") && equals("native.movement.lastOutput", "8"));
    CHECK(equals("native.cooldown.gameReads", "1") && equals("native.cooldown.actualCalls", "1")
        && equals("native.cooldown.lastInput", "500") && equals("native.cooldown.lastOutput", "100"));
    CHECK(!yyjson_obj_get(diagnostics, "native.movement.lastHpBefore") && !yyjson_obj_get(diagnostics, "native.cooldown.lastHpAfter"));
    CHECK(equals("native.damage.lastOutput", "6") && equals("native.instantKill.lastInput", "6")
        && equals("native.instantKill.lastOutput", "2000") && equals("native.instantKill.priorityCalls", "1")
        && equals("native.instantKill.actualCalls", "1") && equals("native.instantKill.lastHpBefore", "20")
        && equals("native.instantKill.lastHpAfter", "0"));
    CHECK(!yyjson_obj_get(diagnostics, "sample.movement") && !yyjson_obj_get(diagnostics, "sample.cooldown"));
    return true;
}

#ifdef _WIN32
struct WinHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~WinHandle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
};

bool WaitIo(HANDLE file, OVERLAPPED& operation, DWORD& transferred, DWORD timeout = 2500) {
    if (WaitForSingleObject(operation.hEvent, timeout) == WAIT_OBJECT_0)
        return GetOverlappedResult(file, &operation, &transferred, FALSE) != FALSE;
    CancelIoEx(file, &operation);
    WaitForSingleObject(operation.hEvent, INFINITE);
    GetOverlappedResult(file, &operation, &transferred, FALSE);
    return false;
}

HANDLE OpenClient() {
    auto name = L"\\\\.\\pipe\\VampireSurvivors.Trainer.v1." + std::to_wstring(GetCurrentProcessId());
    uint64_t deadline = GetTickCount64() + 3000;
    while (GetTickCount64() < deadline) {
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        WaitNamedPipeW(name.c_str(), 50);
        Sleep(10);
    }
    return INVALID_HANDLE_VALUE;
}

bool WriteFrame(HANDLE pipe, const std::string& frame) {
    WinHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    OVERLAPPED operation{};
    operation.hEvent = event.value;
    DWORD written = 0;
    BOOL result = WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, &operation);
    if (!result && (GetLastError() != ERROR_IO_PENDING || !WaitIo(pipe, operation, written))) return false;
    return written == frame.size();
}

std::string ReadFrame(HANDLE pipe, DWORD timeout = 2500) {
    std::string line;
    WinHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    uint64_t deadline = GetTickCount64() + timeout;
    while (GetTickCount64() < deadline && line.size() < 65536) {
        ResetEvent(event.value);
        OVERLAPPED operation{};
        operation.hEvent = event.value;
        char byte = 0;
        DWORD read = 0;
        BOOL result = ReadFile(pipe, &byte, 1, &read, &operation);
        uint64_t now = GetTickCount64();
        DWORD remaining = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
        if (!result && (GetLastError() != ERROR_IO_PENDING || !WaitIo(pipe, operation, read, remaining))) return {};
        if (!read) return {};
        if (byte == '\n') return line;
        line += byte;
    }
    return {};
}

bool ReplyOk(HANDLE pipe, const char* frame) {
    if (!WriteFrame(pipe, std::string(frame) + "\n")) return false;
    Json response(ReadFrame(pipe));
    return response.doc && yyjson_get_bool(yyjson_obj_get(response.root(), "ok"));
}

bool NativePipeAclAndSession() {
    Runtime runtime;
    Ready(runtime, GetTickCount64());
    PipeServer server(runtime), duplicate(runtime);
    CHECK(server.Start());
    CHECK(!duplicate.Start());
    WinHandle client{OpenClient()};
    CHECK(client.value != INVALID_HANDLE_VALUE);
    ULONG pid = 0;
    CHECK(GetNamedPipeServerProcessId(client.value, &pid) && pid == GetCurrentProcessId());
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    DWORD security = GetSecurityInfo(client.value, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor);
    if (security != ERROR_SUCCESS) security = GetSecurityInfo(client.value, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor);
    CHECK(security == ERROR_SUCCESS && dacl && dacl->AceCount == 1);
    void* raw_ace = nullptr;
    CHECK(GetAce(dacl, 0, &raw_ace) && static_cast<ACE_HEADER*>(raw_ace)->AceType == ACCESS_ALLOWED_ACE_TYPE);
    HANDLE raw_token = nullptr;
    CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token));
    WinHandle token{raw_token};
    DWORD bytes = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> data(bytes);
    CHECK(GetTokenInformation(token.value, TokenUser, data.data(), bytes, &bytes));
    bool same_user = EqualSid(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,
        &static_cast<ACCESS_ALLOWED_ACE*>(raw_ace)->SidStart) != FALSE;
    LocalFree(descriptor);
    CHECK(same_user);
    CHECK(ReplyOk(client.value, R"({"id":"h","kind":"hello"})"));
    std::atomic<bool> stop{false};
    runtime.Observe(Observation(), GetTickCount64());
    std::thread pump([&] { while (!stop.load()) { auto now = GetTickCount64(); runtime.Observe(Observation(), now); runtime.Pump(now); Sleep(1); } });
    bool enabled = ReplyOk(client.value, R"({"id":"s","kind":"setStat","stat":"armor","enabled":true,"value":20})");
    stop.store(true);
    pump.join();
    CHECK(enabled && runtime.FilterArmor(0x1000, 5, GetTickCount64()) == 20);
    CHECK(!ReplyOk(client.value, R"({"id":"repeat","kind":"hello"})"));
    CHECK(runtime.FilterArmor(0x1000, 5, GetTickCount64()) == 5);
    server.Stop();
    return true;
}

bool NativePipeBoundariesAndWatchdog() {
    Runtime runtime;
    Ready(runtime, GetTickCount64());
    PipeServer server(runtime);
    CHECK(server.Start());
    {
        WinHandle client{OpenClient()};
        CHECK(client.value != INVALID_HANDLE_VALUE);
        CHECK(!ReplyOk(client.value, R"({"id":"before","kind":"getState"})"));
    }
    {
        WinHandle client{OpenClient()};
        CHECK(client.value != INVALID_HANDLE_VALUE && ReplyOk(client.value, R"({"id":"h","kind":"hello"})"));
        CHECK(!ReplyOk(client.value, R"({"id":"gold","kind":"setGold","value":100})"));
        CHECK(!ReplyOk(client.value, R"({"id":"adventure","kind":"setAdventureGold","goldAdventure":0,"value":100})"));
    }
    {
        WinHandle client{OpenClient()};
        CHECK(client.value != INVALID_HANDLE_VALUE && ReplyOk(client.value, R"({"id":"h","kind":"hello"})"));
        std::string invalid = "{\"id\":\"";
        invalid += '\xC0'; invalid += '\xAF'; invalid += "\"}\n";
        CHECK(WriteFrame(client.value, invalid));
        CHECK(ReadFrame(client.value).empty());
    }
    {
        WinHandle client{OpenClient()};
        CHECK(client.value != INVALID_HANDLE_VALUE && ReplyOk(client.value, R"({"id":"h","kind":"hello"})"));
        std::atomic<bool> stop{false};
        runtime.Observe(Observation(), GetTickCount64());
        std::thread pump([&] { while (!stop.load()) { auto now = GetTickCount64(); runtime.Observe(Observation(), now); runtime.Pump(now); Sleep(1); } });
        bool enabled = ReplyOk(client.value, R"({"id":"a","kind":"setStat","stat":"armor","enabled":true,"value":20})");
        Sleep(3250);
        stop.store(true); pump.join();
        CHECK(enabled);
        CHECK(!runtime.Snapshot(GetTickCount64()).armor_enabled);
        CHECK(runtime.FilterArmor(0x1000, 5, GetTickCount64()) == 5);
        CHECK(!ReplyOk(client.value, R"({"id":"late","kind":"heartbeat"})"));
    }
    {
        WinHandle client{OpenClient()};
        CHECK(client.value != INVALID_HANDLE_VALUE);
        CHECK(WriteFrame(client.value, "{"));
        CHECK(ReadFrame(client.value, 3000).empty());
    }
    {
        WinHandle client{OpenClient()};
        CHECK(client.value != INVALID_HANDLE_VALUE && ReplyOk(client.value, R"({"id":"h","kind":"hello"})"));
        auto before = GetTickCount64();
        server.Stop();
        CHECK(GetTickCount64() - before < 2000);
    }
    return true;
}

bool NativePipeCombatCommands() {
    Runtime runtime;
    ReadyAll(runtime, GetTickCount64());
    runtime.SetFeatureSupported(CombatFeature::GameSpeed, true);
    runtime.SetFeatureSupported(CombatFeature::SkipChestAnimation, true);
    runtime.SetFeatureSupported(CombatFeature::FiveItemChests, true);
    PipeServer server(runtime);
    CHECK(server.Start());
    WinHandle client{OpenClient()};
    CHECK(client.value != INVALID_HANDLE_VALUE && ReplyOk(client.value, R"({"id":"h","kind":"hello"})"));
    std::atomic<bool> stop{false};
    std::thread pump([&] { while (!stop.load()) { auto now = GetTickCount64(); runtime.Observe(Observation(), now); runtime.Pump(now); Sleep(1); } });
    bool health = ReplyOk(client.value, R"({"id":"health","kind":"setFeature","feature":"health","enabled":true})");
    bool xp = ReplyOk(client.value, R"({"id":"xp","kind":"setFeature","feature":"experience","enabled":true,"value":2})");
    bool damage = ReplyOk(client.value, R"({"id":"damage","kind":"setFeature","feature":"damage","enabled":true,"value":3})");
    bool movement = ReplyOk(client.value, R"({"id":"movement","kind":"setFeature","feature":"movement","enabled":true,"value":0.25})");
    bool cooldown = ReplyOk(client.value, R"({"id":"cooldown","kind":"setFeature","feature":"cooldown","enabled":true})");
    bool instant = ReplyOk(client.value, R"({"id":"instant","kind":"setFeature","feature":"instantKill","enabled":true})");
    bool speed = ReplyOk(client.value, R"({"id":"speed","kind":"setFeature","feature":"gameSpeed","enabled":true,"value":4})")
        && ReplyOk(client.value, R"({"id":"speed-live","kind":"setFeature","feature":"gameSpeed","value":6})")
        && runtime.ActiveGameSpeed(0x1000, GetTickCount64()) == 6;
    bool chest_animation = ReplyOk(client.value, R"({"id":"chest-skip","kind":"setFeature","feature":"skipChestAnimation","enabled":true})")
        && runtime.FeatureEnabled(CombatFeature::SkipChestAnimation, 0x1000, GetTickCount64())
        && ReplyOk(client.value, R"({"id":"chest-skip-off","kind":"setFeature","feature":"skipChestAnimation","enabled":false})")
        && !runtime.FeatureEnabled(CombatFeature::SkipChestAnimation, 0x1000, GetTickCount64());
    bool five_chests = !ReplyOk(client.value, R"({"id":"five-invalid","kind":"setFeature","feature":"fiveItemChests","enabled":true,"value":2})")
        && ReplyOk(client.value, R"({"id":"five-on","kind":"setFeature","feature":"fiveItemChests","enabled":true})")
        && runtime.FeatureEnabled(CombatFeature::FiveItemChests, 0x1000, GetTickCount64())
        && !runtime.FeatureEnabled(CombatFeature::SkipChestAnimation, 0x1000, GetTickCount64());
    bool stat_disable = ReplyOk(client.value, R"({"id":"stat-clear","kind":"disableStats"})");
    bool health_off = ReplyOk(client.value, R"({"id":"health-off","kind":"setFeature","feature":"health","enabled":false})");
    bool state_valid = runtime.FilterHealthDamage(0x1000, 5, GetTickCount64()) == 5
        && runtime.FilterExperience(0x1000, 5, GetTickCount64()) == 10
        && runtime.FilterDamage(0x1000, 5, true, GetTickCount64()) == 15
        && runtime.FilterMovement(0x1000, 4, GetTickCount64()) == 1
        && runtime.FilterCooldown(0x1000, 500, true, GetTickCount64()) == 100
        && runtime.FilterInstantKill(0x1000, 15, 20, true, GetTickCount64()) == 2000;
    bool movement_off = ReplyOk(client.value, R"({"id":"movement-off","kind":"setFeature","feature":"movement","enabled":false})");
    bool cooldown_off = ReplyOk(client.value, R"({"id":"cooldown-off","kind":"setFeature","feature":"cooldown","enabled":false})");
    bool independence = runtime.FilterMovement(0x1000, 4, GetTickCount64()) == 4
        && runtime.FilterCooldown(0x1000, 500, true, GetTickCount64()) == 500
        && runtime.FilterInstantKill(0x1000, 15, 20, true, GetTickCount64()) == 2000;
    bool clear = ReplyOk(client.value, R"({"id":"clear","kind":"disableAll"})");
    stop.store(true); pump.join();
    CHECK(health && xp && damage && movement && cooldown && instant && speed && chest_animation && five_chests && stat_disable && health_off && state_valid
        && movement_off && cooldown_off && independence && clear);
    auto state = runtime.Snapshot(GetTickCount64());
    for (const auto& feature : state.combat) CHECK(!feature.enabled);
    server.Stop();
    return true;
}

bool NativePipeStatCommandsAndLiveValues() {
    Runtime runtime;
    auto observation = Observation();
    for (const auto& definition : kNativeStats) {
        runtime.SetStatSupported(definition.stat, true);
        observation.stat_known[static_cast<size_t>(definition.stat)] = definition.stat != NativeStat::Omni;
        observation.stat_original[static_cast<size_t>(definition.stat)] = 1;
    }
    runtime.SetFeatureSupported(CombatFeature::Health, true);
    runtime.Observe(observation, GetTickCount64());
    PipeServer server(runtime);
    CHECK(server.Start());
    WinHandle client{OpenClient()};
    CHECK(client.value != INVALID_HANDLE_VALUE && ReplyOk(client.value, R"({"id":"h","kind":"hello"})"));
    std::atomic<bool> stop{false};
    std::thread pump([&] {
        while (!stop.load()) {
            auto now = GetTickCount64();
            runtime.Observe(observation, now);
            runtime.Pump(now);
            Sleep(1);
        }
    });
    bool valid = ReplyOk(client.value, R"({"id":"health","kind":"setFeature","feature":"health","enabled":true})");
    for (const auto& definition : kNativeStats) {
        std::string request = std::string("{\"id\":\"") + definition.id
            + "\",\"kind\":\"setStat\",\"stat\":\"" + definition.id
            + "\",\"enabled\":true,\"value\":" + std::to_string(definition.initial) + "}";
        valid = ReplyOk(client.value, request.c_str()) && valid;
    }
    auto active = runtime.Snapshot(GetTickCount64());
    for (const auto& stat : active.stats) valid = stat.enabled && valid;
    valid = ReplyOk(client.value, R"({"id":"live","kind":"setStat","stat":"moveSpeed","value":75})") && valid;
    valid = runtime.FilterStat(NativeStat::MoveSpeed, 0x1000, 1, GetTickCount64()) == 1.75f && valid;
    valid = ReplyOk(client.value, R"({"id":"stats-off","kind":"disableStats"})") && valid;
    auto off = runtime.Snapshot(GetTickCount64());
    for (const auto& stat : off.stats) valid = !stat.enabled && valid;
    valid = off.combat[0].enabled && valid;
    valid = ReplyOk(client.value, R"({"id":"late-value","kind":"setStat","stat":"moveSpeed","value":100})") && valid;
    valid = !runtime.Snapshot(GetTickCount64()).stats[static_cast<size_t>(NativeStat::MoveSpeed)].enabled && valid;
    valid = !ReplyOk(client.value, R"({"id":"seal","kind":"setStat","stat":"seal","enabled":true,"value":1})") && valid;
    stop.store(true);
    pump.join();
    CHECK(valid);
    CHECK(!runtime.Snapshot(GetTickCount64()).combat[0].enabled);
    server.Stop();
    return true;
}
#endif

} // namespace

int main() {
    struct Test { const char* name; bool (*run)(); };
    const Test tests[] = {
        {"reward shared targets keep chest and upgrade routes independent", vst_reward_tests::SharedTargetAndIndependentRoutes},
        {"reward off cancels queued changes without validating the other route", vst_reward_tests::OffCancelsQueuedAndNeverValidatesOtherRoute},
        {"queued partial reward controls merge at execution without losing the other route", vst_reward_tests::QueuedPartialRoutesMergeAtExecution},
        {"cancelled expired and replaced reward commands never apply later", vst_reward_tests::ExpiredCancelledOrReplacedNeverArms},
        {"native reward callbacks cannot rearm after disable or session replacement", vst_reward_tests::CallbackCannotRearmAfterSessionOrDisable},
        {"reward epochs reject stale offers and separate actual grants from choices", vst_reward_tests::EpochScopeAndActualGrantEvidence},
        {"bounded reward catalog pages and wire types preserve v1 compatibility", vst_reward_tests::CatalogPagesAndWireTypes},
        {"inflight reward and catalog cancellation completes before native validation returns", vst_reward_tests::InflightRewardAndCatalogCancelNeverCommit},
        {"inflight reward and catalog timeout does not wait or commit after its response", vst_reward_tests::InflightRewardAndCatalogTimeoutNeverWaitForAdapterOrCommit},
        {"observation reads are immediate and enables wait for the main thread", ImmediateObservationAndQueuedMutation},
        {"numeric bounds and unsupported commands fail closed", NumericBoundsAndUnsupportedCommands},
        {"getter scope and natural upgrades restore without backing writes", GetterScopeAndNaturalRestoration},
        {"actual getter counters exclude active sampling and preserve float evidence", ActualReadCountersExcludeSamples},
        {"actor scene readiness and capability boundaries clear queued work", LifecycleClearsEnabledAndQueuedWork},
        {"deadlines cancellation heartbeat and stale sessions cannot revive overrides", DeadlineCancellationAndHeartbeat},
        {"fresh game observations expire independently from live pipe heartbeats", FreshObservationIsIndependentOfHeartbeats},
        {"reordered frame watchdog and snapshot samples preserve seven active effects and the session", ReorderedFrameAndWatchdogSamplesKeepLiveSession},
        {"reordered queued enables and hook samples retain seven effect values", ReorderedQueueAndHookSamplesKeepAllEffects},
        {"late heartbeat samples preserve the newest exact three-second deadline", LateHeartbeatSamplesKeepNewestDeadline},
        {"reordered samples retain exact observation and command expiry boundaries", ReorderedSamplesRetainAllSafetyDeadlines},
        {"frame queue budget and immediate disable cancel pending changes", QueueBudgetAndEmergencyDisable},
        {"snapshots own values and hook reads tolerate concurrent control", SnapshotOwnershipAndConcurrentReads},
        {"JSON types duplicate fields UTF8 IDs and exact character limits", JsonInputBoundaries},
        {"wire state exposes only migrated capabilities and measured values", JsonCapabilityAndEscapingContract},
        {"combat commands queue and individual disables preserve other effects", CombatQueuesAndIndependentDisables},
        {"combat ranges scope invalid input and float overflow remain finite", CombatScopeRangesAndOverflow},
        {"combat callback receipts exclude raw frame samples and other owners", CombatActualObservationsAndRawSamples},
        {"combat does not depend on armor and stale observations never revive it", CombatDoesNotRequireArmorAndHasLifecycleGuards},
        {"combat queue cancellation expiry and stale sessions preserve independence", CombatQueuedDisableCancellationAndIsolation},
        {"all combat effects close at actor scene mode verification and heartbeat boundaries", CombatAllSafetyBoundaries},
        {"concurrent combat hook reads preserve independent effect values", CombatConcurrentHookReads},
        {"combat wire features and independent actual result diagnostics round-trip", CombatWireFeaturesAndMeasuredDiagnostics},
        {"movement range and finite math count only real getter reads", MovementBoundsAndRealGetterReads},
        {"game speed preserves native scale, pause zero and external changes", GameSpeedOwnershipAndPause},
        {"game speed queues live edits and respects owner and real-time deadlines", GameSpeedQueuedLiveUpdatesAndExpiry},
        {"chest animation skip waits for playable ordinary chest and a live local owner", ChestSkipRequiresNativeReadyStateAndLiveOwner},
        {"five-reward chests preserve native slots, pad short templates and exclude cards", FiveItemChestPreservesNativeSlotsAndSpecialChests},
        {"cooldown caps only eligible positive intervals and disables independently", CooldownAllowlistCapAndIndependentDisable},
        {"instant kill runs after damage with owned scope and finite saturation", InstantKillPrioritySaturationAndScope},
        {"Red Death instant kill applies at the final native HP subtraction", ReaperInstantKillUsesFinalNativeDamage},
        {"Instant kill includes zero-XP elites and boss parts while excluding projectiles", InstantKillIncludesAuditedBossesAndSpecialTargets},
        {"Enemy hit class cache reuses lookups without sharing classification on collisions", EnemyHitClassCachePreservesClassificationOnCollisions},
        {"new features preserve independent capability queue freshness and session rules", NewFeaturesLifecycleQueueAndSessions},
        {"all six effects clear at common boundaries and expired changes never apply", NewFeaturesCommonSafetyBoundariesAndDeadlines},
        {"new getter and combined enemy filters tolerate concurrent controls", NewGetterAndPriorityConcurrentReads},
        {"all six wire capabilities preserve real getters and instant-kill precedence evidence", MovementCooldownInstantKillWireEvidence},
        {"native stat catalog ranges integer rules and raw units match the managed contract", vst_stat_tests::CatalogBoundsAndRawUnits},
        {"stat getters compose with movement experience damage and cooldown features", vst_stat_tests::StatGettersComposeWithCommonEffects},
        {"unified bonuses retain single-stat priority and capability boundaries", vst_stat_tests::OmniOverridesRespectIndividualAndCapabilityBoundaries},
        {"final stat methods retain weapon factors floors and finite guards", vst_stat_tests::FinalMethodsPreserveWeaponFactorsAndFloors},
        {"original and effective samples never fabricate real base-getter evidence", vst_stat_tests::SamplesAndFinalReadsKeepHonestBaseEvidence},
        {"live stat values preserve enabled state and never revive disabled overrides", vst_stat_tests::LiveValuesNeverReopenDisabledStats},
        {"disableStats closes all native stats while common features remain active", vst_stat_tests::DisableStatsKeepsCommonFeatureControls},
        {"actor and scene changes cancel pending native stat work", vst_stat_tests::StatActorAndSceneBoundariesCancelQueuedWork},
        {"native stat freshness and request deadlines retain exact boundaries", vst_stat_tests::StatFreshnessAndRequestDeadlineBoundaries},
        {"native stat heartbeat and replaced sessions never restore old overrides", vst_stat_tests::StatHeartbeatAndSessionBoundaries},
        {"cancelled and timed-out stat requests cannot apply later", vst_stat_tests::StatAwaitAndCancelCannotApplyLater},
        {"main-game wallet requires backup authorization and exact scope", vst_wallet_tests::MenuRequiresVerifiedBackupAndExactWallet},
        {"wallet observation freshness sessions and queued disables fail closed", vst_wallet_tests::WalletFreshnessSessionsAndQueuedDisables},
        {"resource remaining samples never refill or fabricate natural reads", vst_wallet_tests::ResourceSamplesNeverRefillOrFabricateReads},
        {"backup token wire types reject duplicates and embedded nulls", vst_wallet_tests::BackupTokenWireTypesAndDuplicates},
        {"backup lifetime expiry disables wallet protection while run effects remain active", vst_wallet_tests::WalletBackupLifetimeExpiresWithoutTouchingRunEffects},
        {"queued gold writes require a startup backup normal menu and exact integer", vst_wallet_tests::GoldWritesRequireMenuBackupAndExactFloatInteger},
        {"cancelled stale and expired gold requests never write later", vst_wallet_tests::CancelledStaleAndExpiredGoldNeverWrites},
        {"wallet identity changes cancel writes and never reactivate protection", vst_wallet_tests::WalletIdentityChangesCannotReactivateEffectsOrWrites},
        {"native wallet writer failures and removal fail closed", vst_wallet_tests::GoldWriterFailureAndRemovalRemainReviewable},
        {"native wallet save callbacks can observe runtime without deadlock", vst_wallet_tests::NativeGoldSaveCallbacksCanObserveWithoutDeadlock},
        {"adventure gold requests are isolated from normal and other adventure saves", vst_wallet_tests::AdventureGoldWritesStayWithTheObservedSave},
        {"consumable native replacements never refill and disable restores normal binding", vst_resource_tests::ConsumptionNeverRefillsAndDisableRestoresNormal},
        {"native resource upgrades retain spent temporary budget and preserve normal changes", vst_resource_tests::NativeUpgradeChangesNormalAndRetainsSpentBudget},
        {"resource target changes only reset temporary budget and respect native scopes", vst_resource_tests::TargetChangesResetOnlyTemporaryAndSuspendIsolated},
        {"disabling during native resource mutation preserves normal upgrades", vst_resource_tests::DisableDuringNativeScopeLeavesNativeUpgrade},
        {"resource failures retain normal roots and allow protected restoration", vst_resource_tests::FailuresRetainRestorationAndNormalRoots},
        {"same-character owner replacement keeps spent budgets and its new normal baseline", vst_resource_tests::OwnerReplacementRetainsSpentBudgetAndUsesItsOwnNormalBaseline},
        {"revival total and used bindings migrate together without refilling", vst_resource_tests::RevivalTotalAndUsedMigrateTogetherWithoutRefilling},
        {"revival samples report native remaining without subtracting used twice", vst_resource_tests::RevivalSamplingReportsNativeRemainingWithoutSubtractingUsedAgain},
        {"outer character mutations clone normal data before migrating spent budgets", vst_resource_tests::OuterCharacterMutationClonesNormalBeforeMigratingSpentBudget},
        {"scalar charm binding preserves normal upgrades and live targets independently", vst_resource_tests::ScalarCharmBindingKeepsNormalUpgradeAndLiveTargetSeparate},
        {"verified natural death retains only existing revival and its consumed remainder", vst_revival_tests::SameActorDeathRetainsOnlyExistingRevivalAndConsumedRemainder},
        {"revival wait cannot arm or retarget and explicit disable remains authoritative", vst_revival_tests::PendingDeathCannotArmOrRetargetAndExplicitDisableWins},
        {"revival retention still closes on every hard lifecycle and session boundary", vst_revival_tests::RevivalRetentionStillHonorsAllHardBoundaries},
        {"64-bit GC handles retain high bits across copy move lookup and release", vst_gc_tests::HighHandleBitsSurviveCopyMoveLookupAndFree},
        {"GC root allocation failure preserves prior ownership", vst_gc_tests::RootAllocationFailurePreservesExistingOwnership},
        {"large native gold balances reach small exactly representable targets", vst_gold_plan_tests::LargeBalancesReachSmallRepresentableTargets},
        {"bounded native gold plans preserve precision and reject invalid values", vst_gold_plan_tests::BoundedNativePlansRetainPrecisionAndRejectInvalidValues},
        {"reference setters pass raw objects to the exact managed GC barrier slot", vst_reference_tests::ReferenceWritesUseRawObjectAndExactManagedSlot},
        {"reference setters reject invalid offsets and detect readback mismatch", vst_reference_tests::ReferenceWritesRejectBadOffsetsAndDetectReadbackMismatch},
#ifdef _WIN32
        {"proof time size and fixed-game fingerprint bounds", vst_backup_tests::ProofBoundsTimeAndFingerprint},
        {"proof tamper duplicate and path escape rejection", vst_backup_tests::ProofTamperDuplicateAndEscapeRejected},
        {"native backup capture restore and cloud bookkeeping preservation", vst_backup_tests::MaintenanceCaptureRestoreAndCloudPreservation},
        {"legacy proof overlay and missing save recovery", vst_backup_tests::MaintenanceLegacyProofOverlayAndMissingSaveRecovery},
        {"native backup process tamper and save path whitelist guards", vst_backup_tests::MaintenanceGuardsTamperAndWhitelist},
        {"native restore rollback after a partial failure", vst_backup_tests::MaintenanceRollbackAfterPartialApply},
        {"native backup rejects links and ambiguous Steam accounts", vst_backup_tests::MaintenanceReparseAndAmbiguousAccountsRejected},
        {"real local pipe ACL first-instance PID handshake and queued command", NativePipeAclAndSession},
        {"real local pipe malformed requests watchdog deadlines and stop", NativePipeBoundariesAndWatchdog},
        {"real local pipe handles combat features and independent disables", NativePipeCombatCommands},
        {"real local pipe handles all stats live values independent clears and excludes seal", NativePipeStatCommandsAndLiveValues},
#endif
    };
    unsigned passed = 0;
    for (const auto& test : tests) {
        bool result = test.run();
        std::printf("%s %s\n", result ? "PASS" : "FAIL", test.name);
        std::fflush(stdout);
        if (result) ++passed;
    }
    std::printf("%u/%zu native policy/JSON/pipe fixtures passed; no game was loaded.\n", passed, std::size(tests));
    return passed == std::size(tests) ? 0 : 1;
}
