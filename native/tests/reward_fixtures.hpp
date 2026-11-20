#pragma once
#include "../common/runtime.hpp"
#include "../common/protocol.hpp"
#include "../vendor/yyjson/yyjson.h"
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace vst_reward_tests {
using namespace vst;
#define REWARD_CHECK(expression) do { if (!(expression)) return false; } while (0)

struct Adapter {
    Runtime* runtime = nullptr;
    unsigned calls = 0;
    bool replace_session = false, disable = false, fail = false;
    Request seen;
    static bool Run(void* context, const Request& request, Response& result, std::string& error) {
        auto& self = *static_cast<Adapter*>(context);
        ++self.calls; self.seen = request;
        if (self.replace_session) self.runtime->BeginSession(1000);
        if (self.disable) self.runtime->DisableAll();
        if (self.fail) { result.error_scope = "rewardTarget"; error = "Fixture target is maxed."; return false; }
        if (request.kind == "getItemCatalog") {
            result.has_item_catalog = true; result.item_catalog_total = 2;
            RewardItemDefinition item; item.id = "WHIP"; item.name = "鞭子"; item.english_name = "Whip";
            result.item_catalog.push_back(item);
        }
        result.state.chest_reward.target_item_name = "鞭子";
        return true;
    }
};
inline GameObservation Observe() {
    GameObservation value; value.verified = value.safe_mode = value.player_ready = true;
    value.player = 0x1000; value.scene = "Reward fixture"; return value;
}
inline void Ready(Runtime& runtime, Adapter& adapter) {
    adapter.runtime = &runtime; runtime.SetRewardAdapter(Adapter::Run, &adapter);
    runtime.SetRewardSupported(RewardUse::Chest, true); runtime.SetRewardSupported(RewardUse::Upgrade, true);
    runtime.Observe(Observe(), 1000);
}
inline Request Target(bool chest, bool upgrade) {
    Request request; request.id = "reward"; request.kind = "setChestReward";
    request.item_id = "WHIP"; request.has_item_id = true;
    request.has_enabled = request.has_upgrade_enabled = true;
    request.enabled = chest; request.upgrade_enabled = upgrade; return request;
}
inline Response Apply(Runtime& runtime, uint64_t session, Request request, uint64_t now = 1000) {
    auto ticket = runtime.Submit(std::move(request), session, now); runtime.Pump(now); return runtime.Await(ticket, 0);
}
inline bool SharedTargetAndIndependentRoutes() {
    Runtime runtime; Adapter adapter; Ready(runtime, adapter); auto session = runtime.BeginSession(1000);
    auto response = Apply(runtime, session, Target(true, false));
    REWARD_CHECK(response.ok && response.state.chest_reward.enabled && !response.state.chest_reward.upgrade_enabled);
    Request update; update.id = "value-only"; update.kind = "setChestReward"; update.has_item_id = true; update.item_id = "AXE";
    response = Apply(runtime, session, update);
    REWARD_CHECK(response.ok && response.state.chest_reward.target_item_id == "AXE" && response.state.chest_reward.enabled);
    REWARD_CHECK(adapter.seen.has_enabled && adapter.seen.enabled && adapter.seen.has_upgrade_enabled && !adapter.seen.upgrade_enabled);
    runtime.DisableAll(); response = Apply(runtime, session, update);
    REWARD_CHECK(response.ok && !response.state.chest_reward.enabled && !response.state.chest_reward.upgrade_enabled);
    REWARD_CHECK(runtime.RewardTarget(RewardUse::Chest, 0x1000, 1000).empty());
    return true;
}
inline bool OffCancelsQueuedAndNeverValidatesOtherRoute() {
    Runtime runtime; Adapter adapter; Ready(runtime, adapter); auto session = runtime.BeginSession(1000);
    REWARD_CHECK(Apply(runtime, session, Target(true, true)).ok);
    auto queued = runtime.Submit(Target(true, true), session, 1000);
    Request off; off.id = "off"; off.kind = "setChestReward"; off.has_enabled = true; off.enabled = false;
    adapter.fail = true;
    auto response = runtime.Await(runtime.Submit(off, session, 1000), 0);
    REWARD_CHECK(response.ok && !response.state.chest_reward.enabled && response.state.chest_reward.upgrade_enabled);
    REWARD_CHECK(!runtime.Await(queued, 0).ok && adapter.calls == 1);
    runtime.Pump(1000);
    REWARD_CHECK(!runtime.Snapshot(1000).chest_reward.enabled);
    return true;
}
inline bool QueuedPartialRoutesMergeAtExecution() {
    Runtime runtime; Adapter adapter; Ready(runtime, adapter); auto session = runtime.BeginSession(1000);
    auto chest = Target(true, false); chest.has_upgrade_enabled = false;
    Request upgrade; upgrade.id = "upgrade-only-field"; upgrade.kind = "setChestReward";
    upgrade.has_upgrade_enabled = upgrade.upgrade_enabled = true;
    auto first = runtime.Submit(chest, session, 1000);
    auto second = runtime.Submit(upgrade, session, 1000);
    runtime.Pump(1000); runtime.Pump(1000);
    REWARD_CHECK(runtime.Await(first, 0).ok && runtime.Await(second, 0).ok);
    REWARD_CHECK(adapter.seen.item_id == "WHIP" && adapter.seen.enabled && adapter.seen.upgrade_enabled);
    auto state = runtime.Snapshot(1000).chest_reward;
    REWARD_CHECK(state.enabled && state.upgrade_enabled);
    Request target; target.id = "target-only-timeout"; target.kind = "setChestReward";
    target.has_item_id = true; target.item_id = "AXE";
    auto timed = runtime.Await(runtime.Submit(target, session, 1000), 0);
    REWARD_CHECK(!timed.ok && timed.error_scope == "rewardTarget");
    runtime.Pump(1000);
    REWARD_CHECK(runtime.Snapshot(1000).chest_reward.target_item_id == "WHIP");
    return true;
}
inline bool ExpiredCancelledOrReplacedNeverArms() {
    for (int kind = 0; kind < 4; ++kind) {
        Runtime runtime; Adapter adapter; Ready(runtime, adapter); auto session = runtime.BeginSession(1000);
        auto ticket = runtime.Submit(Target(true, true), session, 1000);
        if (kind == 0) runtime.Cancel(ticket);
        if (kind == 1) runtime.Await(ticket, 0);
        if (kind == 2) runtime.BeginSession(1001);
        if (kind == 3) { runtime.Observe(Observe(), 2501); }
        runtime.Pump(kind == 3 ? 2501 : 1001);
        REWARD_CHECK(!runtime.Await(ticket, 0).ok && adapter.calls == 0);
        REWARD_CHECK(!runtime.Snapshot(kind == 3 ? 2501 : 1001).chest_reward.enabled);
    }
    return true;
}
inline bool CallbackCannotRearmAfterSessionOrDisable() {
    for (bool replace : {false, true}) {
        Runtime runtime; Adapter adapter; Ready(runtime, adapter); auto session = runtime.BeginSession(1000);
        adapter.replace_session = replace; adapter.disable = !replace;
        auto response = Apply(runtime, session, Target(true, true));
        REWARD_CHECK(!response.ok && !response.state.chest_reward.enabled && !response.state.chest_reward.upgrade_enabled);
    }
    return true;
}
inline bool EpochScopeAndActualGrantEvidence() {
    Runtime runtime; Adapter adapter; Ready(runtime, adapter); auto session = runtime.BeginSession(1000);
    REWARD_CHECK(Apply(runtime, session, Target(true, true)).ok);
    auto epoch = runtime.RewardEpoch();
    REWARD_CHECK(runtime.RewardContextCurrent(0x1000, epoch, 1000));
    REWARD_CHECK(!runtime.RewardContextCurrent(0x2000, epoch, 1000));
    runtime.RecordReward(RewardUse::Upgrade, 0x1000, epoch, "WHIP", "鞭子", false, "offered", 1000);
    auto state = runtime.Snapshot(1000).chest_reward;
    REWARD_CHECK(state.upgrade_processed_count == 1 && state.upgrade_granted_count == 0 && state.processed_count == 0);
    runtime.RecordReward(RewardUse::Upgrade, 0x1000, epoch, "WHIP", "鞭子", true, "granted", 1000);
    runtime.RecordReward(RewardUse::Chest, 0x1000, epoch, "WHIP", "鞭子", true, "claimed", 1000);
    REWARD_CHECK(runtime.Snapshot(1000).chest_reward.upgrade_granted_count == 1 && runtime.Snapshot(1000).chest_reward.processed_count == 1);
    runtime.DisableAll(); REWARD_CHECK(runtime.RewardEpoch() == epoch);
    auto observation = Observe(); observation.scene = "New run"; runtime.Observe(observation, 1001);
    REWARD_CHECK(runtime.RewardEpoch() != epoch && !runtime.RewardContextCurrent(0x1000, epoch, 1001));
    runtime.RecordReward(RewardUse::Chest, 0x1000, epoch, "AXE", "Axe", true, "stale", 1001);
    REWARD_CHECK(runtime.Snapshot(1001).chest_reward.processed_count == 1);
    runtime.EndSession(session); REWARD_CHECK(runtime.RewardEpoch() != epoch);
    REWARD_CHECK(!runtime.RewardContextCurrent(0x1000, runtime.RewardEpoch(), 1001));
    runtime.RecordReward(RewardUse::Upgrade, 0x1000, runtime.RewardEpoch(), "AXE", "Axe", true, "closed", 1001);
    REWARD_CHECK(runtime.Snapshot(1001).chest_reward.upgrade_granted_count == 1);
    return true;
}
inline bool CatalogPagesAndWireTypes() {
    Runtime runtime; Adapter adapter; Ready(runtime, adapter); auto session = runtime.BeginSession(1000);
    Request request; request.id = "catalog"; request.kind = "getItemCatalog";
    auto response = Apply(runtime, session, request);
    REWARD_CHECK(response.ok && response.has_item_catalog && response.item_catalog_total == 2 && adapter.calls == 1);
    auto wire = SerializeResponse(response); auto* doc = yyjson_read(wire.data(), wire.size(), 0);
    REWARD_CHECK(doc != nullptr);
    auto* root = yyjson_doc_get_root(doc); bool valid = yyjson_get_len(yyjson_obj_get(root, "itemCatalog")) == 1;
    yyjson_doc_free(doc); REWARD_CHECK(valid);
    request.catalog_limit = 201; response = Apply(runtime, session, request);
    REWARD_CHECK(!response.ok && response.error_scope == "catalog" && adapter.calls == 1);
    std::string error;
    for (const char* json : {R"({"id":"x","kind":"getItemCatalog","catalogOffset":1.5})", R"({"id":"x","itemId":"WHIP","ItemID":"AXE"})", R"({"id":"x","upgradeEnabled":1})", R"({"id":"x","catalogLimit":-1})"})
        REWARD_CHECK(!ParseRequest(json, request, error));
    REWARD_CHECK(ParseRequest(R"({"id":"x","kind":"setChestReward","itemId":"WHIP","enabled":false,"upgradeEnabled":true,"catalogLimit":200})", request, error));
    REWARD_CHECK(request.has_item_id && request.item_id == "WHIP" && request.has_upgrade_enabled && request.upgrade_enabled);
    return true;
}
struct BlockingAdapter {
    std::mutex mutex;
    std::condition_variable ready;
    bool entered = false, released = false;
    static bool Run(void* context, const Request&, Response& result, std::string&) {
        auto& self = *static_cast<BlockingAdapter*>(context);
        std::unique_lock lock(self.mutex);
        self.entered = true;
        self.ready.notify_all();
        self.ready.wait(lock, [&] { return self.released; });
        result.state.chest_reward.target_item_name = "Whip";
        return true;
    }
    bool WaitForEntry() {
        std::unique_lock lock(mutex);
        return ready.wait_for(lock, std::chrono::seconds(2), [&] { return entered; });
    }
    void Release() {
        std::lock_guard lock(mutex);
        released = true;
        ready.notify_all();
    }
};
inline uint64_t ReadyBlocking(Runtime& runtime, BlockingAdapter& adapter) {
    runtime.SetRewardAdapter(BlockingAdapter::Run, &adapter);
    runtime.SetRewardSupported(RewardUse::Chest, true);
    runtime.SetRewardSupported(RewardUse::Upgrade, true);
    runtime.SetFeatureSupported(CombatFeature::Health, true);
    runtime.Observe(Observe(), 1000);
    const auto session = runtime.BeginSession(1000);
    Request health; health.id = "health"; health.kind = "setFeature"; health.feature = "health";
    health.has_enabled = health.enabled = true;
    Apply(runtime, session, health);
    return session;
}
inline bool InflightRewardAndCatalogCancelNeverCommit() {
    for (bool catalog : {false, true}) {
        Runtime runtime; BlockingAdapter adapter;
        const auto session = ReadyBlocking(runtime, adapter);
        Request request = Target(true, true);
        if (catalog) { request = {}; request.id = "catalog-cancel"; request.kind = "getItemCatalog"; }
        auto ticket = runtime.Submit(request, session, 1000);
        std::thread pump([&] { runtime.Pump(1000); });
        const bool entered = adapter.WaitForEntry();
        runtime.Cancel(ticket);
        Response cancelled;
        const bool completed_before_release = runtime.TryResult(ticket, cancelled);
        adapter.Release(); pump.join();
        REWARD_CHECK(entered && completed_before_release && !cancelled.ok);
        const auto final = runtime.Await(ticket, 0);
        REWARD_CHECK(!final.ok && final.error == cancelled.error);
        const auto state = runtime.Snapshot(1000);
        REWARD_CHECK(!state.chest_reward.enabled && !state.chest_reward.upgrade_enabled);
        REWARD_CHECK(state.chest_reward.target_item_id.empty());
        REWARD_CHECK(state.combat[static_cast<size_t>(CombatFeature::Health)].enabled);
    }
    return true;
}
inline bool InflightRewardAndCatalogTimeoutNeverWaitForAdapterOrCommit() {
    for (bool catalog : {false, true}) {
        Runtime runtime; BlockingAdapter adapter;
        const auto session = ReadyBlocking(runtime, adapter);
        Request request = Target(true, true);
        if (catalog) { request = {}; request.id = "catalog-timeout"; request.kind = "getItemCatalog"; }
        auto ticket = runtime.Submit(request, session, 1000);
        std::thread pump([&] { runtime.Pump(1000); });
        const bool entered = adapter.WaitForEntry();
        std::promise<Response> answer;
        auto result = answer.get_future();
        std::thread waiter([&] { answer.set_value(runtime.Await(ticket, 0)); });
        // A broken uncancellable adapter still exits this fixture: release it
        // after the bounded wait, join both threads, then report the failure.
        const bool completed_before_release = result.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        adapter.Release(); pump.join(); waiter.join();
        const auto response = result.get();
        REWARD_CHECK(entered && completed_before_release && !response.ok);
        REWARD_CHECK(response.error.find("timed out") != std::string::npos);
        REWARD_CHECK(!runtime.Await(ticket, 0).ok);
        const auto state = runtime.Snapshot(1000);
        REWARD_CHECK(!state.chest_reward.enabled && !state.chest_reward.upgrade_enabled);
        REWARD_CHECK(state.chest_reward.target_item_id.empty());
        REWARD_CHECK(state.combat[static_cast<size_t>(CombatFeature::Health)].enabled);
    }
    return true;
}
#undef REWARD_CHECK
} // namespace vst_reward_tests
