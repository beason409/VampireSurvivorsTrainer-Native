#pragma once
#include "../common/runtime.hpp"
#include "../common/protocol.hpp"
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace vst_wallet_tests {
using namespace vst;
#define VST_WALLET_CHECK(...) do { if (!(__VA_ARGS__)) { std::fprintf(stderr, "wallet fixture line %d: %s\n", __LINE__, #__VA_ARGS__); return false; } } while (0)
inline GameObservation Menu() {
    GameObservation observation;
    observation.verified = observation.safe_mode = true;
    observation.wallet = 0x2000;
    observation.gold_wallet = {observation.wallet, 0x2100};
    observation.gold_known = true;
    observation.gold = 123;
    observation.scene = "MainMenu";
    return observation;
}
inline Request Toggle(bool enabled = true) {
    Request request;
    request.id = "wallet-fixture"; request.kind = "setFeature";
    request.feature = "noGoldSpend"; request.has_enabled = true; request.enabled = enabled;
    return request;
}
inline bool Apply(Runtime& runtime, uint64_t session, Request request, uint64_t now) {
    auto ticket = runtime.Submit(std::move(request), session, now);
    runtime.Pump(now); return runtime.Await(ticket, 0).ok;
}
inline bool MenuRequiresVerifiedBackupAndExactWallet() {
    Runtime runtime; runtime.SetFeatureSupported(CombatFeature::NoGoldSpend, true);
    runtime.Observe(Menu(), 1000);
    auto session = runtime.BeginSession(1000);
    VST_WALLET_CHECK(!Apply(runtime, session, Toggle(), 1000));
    VST_WALLET_CHECK(runtime.FilterGoldSpend(0x2000, 25, true, 1000) == 25);
    runtime.SetBackupAuthorization(session, true);
    VST_WALLET_CHECK(Apply(runtime, session, Toggle(), 1000));
    runtime.Observe(Menu(), 1001);
    auto state = runtime.Snapshot(1001);
    VST_WALLET_CHECK(!state.player_ready && state.backup_authorized && state.combat[6].enabled);
    VST_WALLET_CHECK(state.gold_known && state.gold == 123);
    VST_WALLET_CHECK(runtime.FilterGoldSpend(0x2000, 25, true, 1001) == 0);
    VST_WALLET_CHECK(runtime.FilterGoldSpend(0x3000, 25, true, 1001) == 25);
    VST_WALLET_CHECK(runtime.FilterGoldSpend(0x2000, 25, false, 1001) == 25);
    VST_WALLET_CHECK(runtime.FilterGoldSpend(0x2000, -25, true, 1001) == -25);
    VST_WALLET_CHECK(runtime.FilterGoldSpend(0x2000, 0, true, 1001) == 0);
    VST_WALLET_CHECK(std::isinf(runtime.FilterGoldSpend(0x2000, std::numeric_limits<float>::infinity(), true, 1001)));
    state = runtime.Snapshot(1001);
    VST_WALLET_CHECK(state.combat[6].actual_calls == 2 && state.combat[6].override_calls == 1);
    VST_WALLET_CHECK(state.combat[6].io_known && state.combat[6].last_output == 0);
    runtime.SetBackupAuthorization(session, false);
    VST_WALLET_CHECK(!runtime.Snapshot(1001).combat[6].enabled);
    VST_WALLET_CHECK(runtime.FilterGoldSpend(0x2000, 25, true, 1001) == 25);
    return true;
}
inline bool WalletFreshnessSessionsAndQueuedDisables() {
    Runtime runtime; runtime.SetFeatureSupported(CombatFeature::NoGoldSpend, true);
    runtime.Observe(Menu(), 1000); auto session = runtime.BeginSession(1000);
    runtime.SetBackupAuthorization(session, true);
    VST_WALLET_CHECK(Apply(runtime, session, Toggle(), 1000));
    VST_WALLET_CHECK(runtime.FilterGoldSpend(0x2000, 10, true, 2000) == 0);
    VST_WALLET_CHECK(runtime.FilterGoldSpend(0x2000, 10, true, 2001) == 10);
    VST_WALLET_CHECK(!runtime.Snapshot(2001).combat[6].enabled);
    runtime.Observe(Menu(), 2002);
    VST_WALLET_CHECK(!runtime.Snapshot(2002).combat[6].enabled);
    auto pending = runtime.Submit(Toggle(), session, 2002);
    VST_WALLET_CHECK(Apply(runtime, session, Toggle(false), 2002));
    runtime.Pump(2002); VST_WALLET_CHECK(!runtime.Await(pending, 0).ok);
    VST_WALLET_CHECK(!runtime.Snapshot(2002).combat[6].enabled);
    auto next = runtime.BeginSession(2003);
    runtime.SetBackupAuthorization(session, true); // An old client cannot grant a new session.
    VST_WALLET_CHECK(!runtime.Snapshot(2003).backup_authorized);
    VST_WALLET_CHECK(!Apply(runtime, next, Toggle(), 2003));
    runtime.SetBackupAuthorization(next, true);
    VST_WALLET_CHECK(Apply(runtime, next, Toggle(), 2003));
    for (uint64_t tick : {3000ULL, 4000ULL, 5000ULL}) runtime.Observe(Menu(), tick);
    VST_WALLET_CHECK(runtime.FilterGoldSpend(0x2000, 10, true, 5003) == 0);
    VST_WALLET_CHECK(runtime.FilterGoldSpend(0x2000, 10, true, 5004) == 10);
    VST_WALLET_CHECK(!runtime.Snapshot(5004).backup_authorized);
    return true;
}
inline bool ResourceSamplesNeverRefillOrFabricateReads() {
    Runtime runtime;
    auto observation = Menu(); observation.player = 0x1000; observation.player_ready = true;
    const size_t index = static_cast<size_t>(NativeStat::Rerolls);
    observation.stat_known[index] = observation.stat_current_known[index] = true;
    observation.stat_original[index] = 4; observation.stat_current[index] = 3;
    runtime.SetStatSupported(NativeStat::Rerolls, true);
    runtime.Observe(observation, 1000); auto session = runtime.BeginSession(1000);
    Request request; request.id = "resource"; request.kind = "setStat"; request.stat = "rerolls";
    request.has_enabled = request.enabled = request.has_value = true; request.value = 100;
    VST_WALLET_CHECK(Apply(runtime, session, request, 1000));
    auto state = runtime.Snapshot(1000).stats[index];
    VST_WALLET_CHECK(state.bound_known && state.bound == 3 && state.game_reads == 0 && !state.current_known);
    runtime.ObserveStatRead(NativeStat::Rerolls, 0x3000, 4, 3, true, 1000);
    VST_WALLET_CHECK(runtime.Snapshot(1000).stats[index].game_reads == 0);
    runtime.ObserveStatRead(NativeStat::Rerolls, 0x1000, 4, 3, true, 1000);
    state = runtime.Snapshot(1000).stats[index];
    VST_WALLET_CHECK(state.game_reads == 1 && state.override_reads == 1 && state.current_known && state.current == 3);
    VST_WALLET_CHECK(runtime.StatValue(NativeStat::Rerolls) == 100); // Budget target is separate from remaining uses.
    request.has_enabled = false; request.value = 50;
    VST_WALLET_CHECK(Apply(runtime, session, request, 1001));
    state = runtime.Snapshot(1001).stats[index];
    VST_WALLET_CHECK(!state.current_known && state.game_reads == 1);
    return true;
}
inline bool BackupTokenWireTypesAndDuplicates() {
    Request request; std::string error;
    VST_WALLET_CHECK(ParseRequest(R"({"id":"proof","kind":"hello","backupToken":"C:\\data\\backups\\sample\\session-proof.json"})", request, error));
    VST_WALLET_CHECK(!request.backup_token.empty());
    VST_WALLET_CHECK(!ParseRequest(R"({"id":"proof","backupToken":"a","BackupToken":"b"})", request, error));
    VST_WALLET_CHECK(!ParseRequest(R"({"id":"proof","backupToken":42})", request, error));
    VST_WALLET_CHECK(!ParseRequest(R"({"id":"proof","backupToken":"bad\u0000path"})", request, error));
    return true;
}
inline bool WalletBackupLifetimeExpiresWithoutTouchingRunEffects() {
    Runtime runtime; runtime.SetFeatureSupported(CombatFeature::NoGoldSpend, true);
    runtime.SetFeatureSupported(CombatFeature::Health, true);
    auto observation = Menu(); observation.player_ready = true; observation.player = 0x1000;
    runtime.Observe(observation, 1000); auto session = runtime.BeginSession(1000);
    runtime.SetBackupAuthorization(session, true, 1100);
    VST_WALLET_CHECK(Apply(runtime, session, Toggle(), 1000));
    auto health = Toggle(); health.feature = "health";
    VST_WALLET_CHECK(Apply(runtime, session, health, 1000));
    VST_WALLET_CHECK(runtime.FilterGoldSpend(0x2000, 25, true, 1100) == 0);
    VST_WALLET_CHECK(runtime.FilterGoldSpend(0x2000, 25, true, 1101) == 25);
    auto state = runtime.Snapshot(1101);
    VST_WALLET_CHECK(!state.backup_authorized && !state.combat[6].enabled && state.combat[0].enabled);
    VST_WALLET_CHECK(runtime.FilterHealthDamage(0x1000, 25, 1101) == 0);
    return true;
}
struct GoldFixture {
    Runtime runtime;
    uint64_t session = 0;
    unsigned writes = 0;
    double wallet = 123;
    std::array<double, 2> adventures{15, 27};
    bool fail = false, throws = false, reenter = false;
    static bool Write(void* context, const GoldWallet& wallet, double desired, double& actual, std::string& error) {
        auto self = static_cast<GoldFixture*>(context); ++self->writes;
        if (self->reenter) {
            auto snapshot = self->runtime.Snapshot(1000);
            if (!snapshot.backup_authorized) { error = "fixture authorization disappeared"; return false; }
            self->runtime.CheckHeartbeat(1000);
        }
        if (self->throws) throw std::runtime_error("fixture native exception");
        if (self->fail) { actual = self->wallet; error = "fixture save rejected and rolled back"; return false; }
        double& balance = wallet.adventure < 0 ? self->wallet : self->adventures.at(static_cast<size_t>(wallet.adventure));
        balance = actual = desired; return true;
    }
    GoldFixture() {
        runtime.SetGoldWriter(&Write, this);
        auto menu = Menu(); menu.gold_write_ready = true;
        runtime.Observe(menu, 1000); session = runtime.BeginSession(1000);
        runtime.SetBackupAuthorization(session, true);
    }
    Request RequestGold(double value = 1000) {
        Request request; request.id = "gold-write"; request.kind = "setGold";
        request.has_value = true; request.value = value; return request;
    }
    void Observe(uint64_t now, bool player = false) {
        auto menu = Menu(); menu.gold = wallet; menu.gold_write_ready = !player;
        menu.player_ready = player; menu.player = player ? 0x1000 : 0;
        runtime.Observe(menu, now);
    }
};
inline bool GoldWritesRequireMenuBackupAndExactFloatInteger() {
    GoldFixture fixture;
    auto pending = fixture.runtime.Submit(fixture.RequestGold(), fixture.session, 1000);
    VST_WALLET_CHECK(fixture.writes == 0 && fixture.runtime.Snapshot(1000).gold == 123);
    fixture.runtime.Pump(1000);
    auto result = fixture.runtime.Await(pending, 0);
    VST_WALLET_CHECK(result.ok && fixture.writes == 1 && result.state.gold == 1000);
    for (double invalid : {-1., 0.5, 1000000000., 16777217., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        VST_WALLET_CHECK(!Apply(fixture.runtime, fixture.session, fixture.RequestGold(invalid), 1000));
    }
    auto missing = fixture.RequestGold(); missing.has_value = false;
    VST_WALLET_CHECK(!Apply(fixture.runtime, fixture.session, missing, 1000));
    fixture.Observe(1001, true);
    VST_WALLET_CHECK(!Apply(fixture.runtime, fixture.session, fixture.RequestGold(2000), 1001));
    fixture.Observe(1002);
    fixture.runtime.SetBackupAuthorization(fixture.session, false);
    VST_WALLET_CHECK(!Apply(fixture.runtime, fixture.session, fixture.RequestGold(2000), 1002));
    VST_WALLET_CHECK(fixture.writes == 1 && fixture.wallet == 1000);
    return true;
}
inline bool CancelledStaleAndExpiredGoldNeverWrites() {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        GoldFixture fixture;
        auto pending = fixture.runtime.Submit(fixture.RequestGold(), fixture.session, 1000);
        uint64_t now = 1001;
        if (scenario == 0) fixture.runtime.Cancel(pending);
        else if (scenario == 1) fixture.runtime.SetBackupAuthorization(fixture.session, false);
        else if (scenario == 2) fixture.runtime.BeginSession(1001);
        else if (scenario == 3) now = 2001; // Stale main-thread observation.
        else {
            for (uint64_t tick : {1500ULL, 2000ULL, 2500ULL}) fixture.Observe(tick);
            now = 2501; // Fresh observation, expired command deadline.
        }
        fixture.runtime.Pump(now);
        VST_WALLET_CHECK(!fixture.runtime.Await(pending, 0).ok && fixture.writes == 0 && fixture.wallet == 123);
    }
    return true;
}
inline bool WalletIdentityChangesCannotReactivateEffectsOrWrites() {
    GoldFixture fixture; fixture.runtime.SetFeatureSupported(CombatFeature::NoGoldSpend, true);
    VST_WALLET_CHECK(Apply(fixture.runtime, fixture.session, Toggle(), 1000));
    auto pending = fixture.runtime.Submit(fixture.RequestGold(), fixture.session, 1000);
    auto changed = Menu(); changed.wallet = 0x3000; changed.gold_write_ready = true;
    changed.gold_wallet = {changed.wallet, 0x3100};
    fixture.runtime.Observe(changed, 1001); fixture.runtime.Pump(1001);
    VST_WALLET_CHECK(!fixture.runtime.Await(pending, 0).ok && fixture.writes == 0);
    VST_WALLET_CHECK(!fixture.runtime.Snapshot(1001).combat[6].enabled);
    fixture.Observe(1002);
    VST_WALLET_CHECK(fixture.runtime.FilterGoldSpend(0x2000, 10, true, 1002) == 10);
    return true;
}
inline bool GoldWriterFailureAndRemovalRemainReviewable() {
    GoldFixture fixture; fixture.fail = true;
    auto pending = fixture.runtime.Submit(fixture.RequestGold(), fixture.session, 1000);
    fixture.runtime.Pump(1000); auto result = fixture.runtime.Await(pending, 0);
    VST_WALLET_CHECK(!result.ok && !result.error.empty() && result.state.gold == 123 && fixture.wallet == 123);
    fixture.fail = false; fixture.throws = true;
    VST_WALLET_CHECK(!Apply(fixture.runtime, fixture.session, fixture.RequestGold(), 1001));
    pending = fixture.runtime.Submit(fixture.RequestGold(), fixture.session, 1002);
    fixture.runtime.SetGoldWriter(nullptr, nullptr); fixture.runtime.Pump(1002);
    VST_WALLET_CHECK(!fixture.runtime.Await(pending, 0).ok && fixture.writes == 2);
    VST_WALLET_CHECK(!fixture.runtime.Snapshot(1002).gold_write_supported);
    GoldFixture unread; unread.throws = true;
    auto unknown = Menu(); unknown.gold_known = false; unknown.gold_write_ready = true;
    unread.runtime.Observe(unknown, 1000);
    VST_WALLET_CHECK(!Apply(unread.runtime, unread.session, unread.RequestGold(), 1000));
    VST_WALLET_CHECK(!unread.runtime.Snapshot(1000).gold_known);
    return true;
}
inline bool NativeGoldSaveCallbacksCanObserveWithoutDeadlock() {
    GoldFixture fixture; fixture.reenter = true;
    VST_WALLET_CHECK(Apply(fixture.runtime, fixture.session, fixture.RequestGold(2000), 1000));
    VST_WALLET_CHECK(fixture.writes == 1 && fixture.wallet == 2000);
    return true;
}
inline bool AdventureGoldWritesStayWithTheObservedSave() {
    GoldFixture fixture;
    auto observe_adventure = [&](int32_t id, uintptr_t config, uint64_t now) {
        auto menu = Menu();
        menu.wallet = 0;
        menu.gold_wallet = {0x2000, config, id};
        menu.gold = fixture.adventures.at(static_cast<size_t>(id));
        menu.gold_write_ready = true;
        fixture.runtime.Observe(menu, now);
    };
    Request adventure;
    std::string error;
    VST_WALLET_CHECK(ParseRequest(R"({"id":"adventure","kind":"setAdventureGold","goldAdventure":0,"value":900})", adventure, error));
    for (const char* invalid : {
        R"({"id":"adventure","goldAdventure":-1})",
        R"({"id":"adventure","goldAdventure":0.5})",
        R"({"id":"adventure","goldAdventure":2147483648})",
        R"({"id":"adventure","goldAdventure":0,"GoldAdventure":1})"
    }) {
        Request rejected;
        VST_WALLET_CHECK(!ParseRequest(invalid, rejected, error));
    }
    VST_WALLET_CHECK(!Apply(fixture.runtime, fixture.session, adventure, 1000));
    auto normal_pending = fixture.runtime.Submit(fixture.RequestGold(), fixture.session, 1000);
    observe_adventure(0, 0x3100, 1001);
    fixture.runtime.Pump(1001);
    VST_WALLET_CHECK(!fixture.runtime.Await(normal_pending, 0).ok);
    auto status = fixture.runtime.Snapshot(1001);
    VST_WALLET_CHECK(!status.wallet_ready && status.gold_write_ready && status.gold_adventure == 0 && status.gold == 15);
    VST_WALLET_CHECK(!Apply(fixture.runtime, fixture.session, fixture.RequestGold(), 1001));
    auto pending = fixture.runtime.Submit(adventure, fixture.session, 1001);
    observe_adventure(1, 0x3200, 1002);
    fixture.runtime.Pump(1002);
    VST_WALLET_CHECK(!fixture.runtime.Await(pending, 0).ok);
    VST_WALLET_CHECK(!Apply(fixture.runtime, fixture.session, adventure, 1002));
    observe_adventure(0, 0x3100, 1003);
    pending = fixture.runtime.Submit(adventure, fixture.session, 1003);
    observe_adventure(0, 0x3300, 1004);
    fixture.runtime.Pump(1004);
    VST_WALLET_CHECK(!fixture.runtime.Await(pending, 0).ok);
    pending = fixture.runtime.Submit(adventure, fixture.session, 1004);
    fixture.runtime.Pump(2005);
    VST_WALLET_CHECK(!fixture.runtime.Await(pending, 0).ok && fixture.writes == 0);
    observe_adventure(0, 0x3300, 2006);
    VST_WALLET_CHECK(Apply(fixture.runtime, fixture.session, adventure, 2006));
    status = fixture.runtime.Snapshot(2006);
    VST_WALLET_CHECK(status.gold == 900 && status.gold_adventure == 0 && fixture.writes == 1);
    VST_WALLET_CHECK(fixture.wallet == 123 && fixture.adventures[0] == 900 && fixture.adventures[1] == 27);
    fixture.Observe(2007);
    status = fixture.runtime.Snapshot(2007);
    VST_WALLET_CHECK(status.gold == 123 && status.gold_adventure == -1 && status.wallet_ready);
    return true;
}
#undef VST_WALLET_CHECK
}
