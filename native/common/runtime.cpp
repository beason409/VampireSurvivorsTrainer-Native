#include "runtime.hpp"
#include "chest_animation.hpp"
#include "chest_rewards.hpp"
#include "game_speed.hpp"
#include "protocol.hpp"

#include <chrono>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace vst {

static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uintptr_t>::is_always_lock_free
    && std::atomic<float>::is_always_lock_free && std::atomic<double>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
    "The native getter requires lock-free atomics on its target architecture.");

struct Runtime::Pending {
    Runtime* owner = nullptr;
    Request request;
    uint64_t session = 0, generation = 0, queued_at = 0;
    bool desired_enabled = false;
    bool executing = false;
    int target = -1; // -1: armor; otherwise CombatFeature index.
    bool completed = false;
    Response response;
    std::condition_variable ready;
};

namespace {
constexpr int kGoldTarget = -4;
constexpr int kRewardTarget = -5;
constexpr int kCatalogTarget = -6;
bool ValidItemId(std::string_view id) {
    return !id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
    });
}
bool Elapsed(uint64_t now, uint64_t then, uint64_t limit) noexcept {
    // Callers sample the monotonic clock before taking the control lock (or
    // reading hook atomics). A newer timestamp can therefore publish first;
    // that ordering is not elapsed time and must not invalidate the session.
    return now >= then && now - then > limit;
}
int FeatureIndex(std::string_view name) noexcept {
    return name == "health" ? 0 : name == "experience" ? 1 : name == "damage" ? 2
        : name == "movement" ? 3 : name == "cooldown" ? 4 : name == "instantKill" ? 5 : name == "noGoldSpend" ? 6
        : name == kGameSpeedId ? static_cast<int>(CombatFeature::GameSpeed)
        : name == kSkipChestAnimationId ? static_cast<int>(CombatFeature::SkipChestAnimation)
        : name == kFiveItemChestsId ? static_cast<int>(CombatFeature::FiveItemChests) : -1;
}
uint64_t Pack(float before, float after) noexcept {
    return static_cast<uint64_t>(std::bit_cast<uint32_t>(before))
        | (static_cast<uint64_t>(std::bit_cast<uint32_t>(after)) << 32);
}
float First(uint64_t pair) noexcept { return std::bit_cast<float>(static_cast<uint32_t>(pair)); }
float Second(uint64_t pair) noexcept { return std::bit_cast<float>(static_cast<uint32_t>(pair >> 32)); }
}

Runtime::Runtime() noexcept {
    auto& game_speed = combat_[static_cast<size_t>(CombatFeature::GameSpeed)];
    game_speed.value = kGameSpeedDefault;
    game_speed.value_atomic.store(kGameSpeedDefault);
    for (const auto& definition : kNativeStats) {
        auto& control = stats_[static_cast<size_t>(definition.stat)];
        control.value = definition.initial;
        control.value_atomic.store(definition.initial);
    }
}

void Runtime::DisableArmorLocked() { enabled_.store(false); }
void Runtime::DisableStatsLocked(bool preserve_revivals) {
    DisableArmorLocked();
    for (size_t index = 0; index < stats_.size(); ++index)
        if (!preserve_revivals || index != static_cast<size_t>(NativeStat::Revivals)) stats_[index].enabled.store(false);
    if (!preserve_revivals) revival_pending_.store(false);
    stats_revision_.fetch_add(1);
}
void Runtime::DisableLocked() {
    DisableStatsLocked();
    for (auto& feature : combat_) feature.enabled.store(false);
    rewards_.enabled = rewards_.upgrade_enabled = false;
    ++reward_revision_;
}

void Runtime::DisablePlayerEffectsLocked() {
    DisableStatsLocked();
    for (size_t index = 0; index < combat_.size(); ++index)
        if (index != static_cast<size_t>(CombatFeature::NoGoldSpend)) combat_[index].enabled.store(false);
    rewards_.enabled = rewards_.upgrade_enabled = false;
    ++reward_revision_;
}

bool Runtime::TargetReadyLocked(int target) const {
    if (target == static_cast<int>(CombatFeature::NoGoldSpend))
        return !revival_pending_.load() && verified_.load() && safe_mode_.load() && wallet_.load() != 0 && backup_authorized_.load();
    return safe_mode_.load() && player_ready_.load();
}

void Runtime::SetBackupAuthorization(uint64_t session, bool authorized, uint64_t expires_at) {
    std::lock_guard lock(mutex_);
    if (!session_active_ || session != session_) return;
    backup_authorized_.store(authorized);
    backup_expires_at_.store(authorized ? expires_at : 0);
    if (!authorized) {
        DisableTargetLocked(static_cast<int>(CombatFeature::NoGoldSpend));
        InvalidateTargetQueuedLocked(static_cast<int>(CombatFeature::NoGoldSpend), "The startup backup is unavailable.");
        InvalidateTargetQueuedLocked(kGoldTarget, "The startup backup is unavailable.");
    }
}

void Runtime::SetGoldWriter(GoldWriter writer, void* context) {
    std::lock_guard lock(mutex_);
    gold_writer_ = writer; gold_writer_context_ = context;
    if (!writer) InvalidateTargetQueuedLocked(kGoldTarget, "The native wallet writer is unavailable.");
}

void Runtime::SetRewardAdapter(RewardAdapter adapter, void* context) {
    std::lock_guard lock(mutex_);
    reward_adapter_ = adapter; reward_adapter_context_ = context;
    if (!adapter) {
        rewards_.enabled = rewards_.upgrade_enabled = false;
        ++reward_revision_;
        InvalidateTargetQueuedLocked(kRewardTarget, "The native reward adapter is unavailable.");
        InvalidateTargetQueuedLocked(kCatalogTarget, "The native item catalog is unavailable.");
    }
}

void Runtime::SetRewardSupported(RewardUse use, bool supported, std::string reason) {
    std::lock_guard lock(mutex_);
    if (use == RewardUse::Chest) {
        rewards_.supported = supported; rewards_.message = std::move(reason);
        if (!supported) rewards_.enabled = false;
    } else {
        rewards_.upgrade_supported = supported; rewards_.upgrade_message = std::move(reason);
        if (!supported) rewards_.upgrade_enabled = false;
    }
    if (!supported) {
        ++reward_revision_;
        InvalidateTargetQueuedLocked(kRewardTarget, "A native reward entry is unavailable.");
    }
}

std::string Runtime::RewardTarget(RewardUse use, uintptr_t owner, uint64_t now) {
    std::lock_guard lock(mutex_);
    ExpireLocked(now);
    if (!session_active_ || !ReadyOwner(owner, now)) return {};
    return (use == RewardUse::Chest ? rewards_.supported && rewards_.enabled
        : rewards_.upgrade_supported && rewards_.upgrade_enabled) ? rewards_.target_item_id : std::string{};
}

bool Runtime::RewardContextCurrent(uintptr_t owner, uint64_t epoch, uint64_t now) {
    std::lock_guard lock(mutex_);
    ExpireLocked(now);
    return session_active_ && epoch == reward_epoch_.load() && ReadyOwner(owner, now);
}

void Runtime::RecordReward(RewardUse use, uintptr_t owner, uint64_t epoch, const std::string& id,
    const std::string& name, bool granted, const std::string& message, uint64_t now) {
    std::lock_guard lock(mutex_);
    ExpireLocked(now);
    if (!session_active_ || epoch != reward_epoch_.load() || !ReadyOwner(owner, now)) return;
    if (use == RewardUse::Chest && granted) {
        ++rewards_.processed_count; rewards_.last_reward_item_id = id; rewards_.last_reward_name = name;
        rewards_.message = message;
    } else if (use == RewardUse::Upgrade) {
        if (granted) {
            ++rewards_.upgrade_granted_count;
            rewards_.last_upgrade_granted_item_id = id; rewards_.last_upgrade_granted_name = name;
        } else {
            ++rewards_.upgrade_processed_count;
            rewards_.last_upgrade_item_id = id; rewards_.last_upgrade_name = name;
        }
        rewards_.upgrade_message = message;
    }
}

void Runtime::RewardUnavailable(RewardUse use, std::string reason) {
    std::lock_guard lock(mutex_);
    if (use == RewardUse::Chest) { rewards_.enabled = false; rewards_.message = std::move(reason); }
    else { rewards_.upgrade_enabled = false; rewards_.upgrade_message = std::move(reason); }
    ++reward_revision_;
    InvalidateTargetQueuedLocked(kRewardTarget, "The reward target is no longer eligible.");
}

std::atomic<bool>& Runtime::TargetEnabledLocked(int target) {
    if (target == -1) return enabled_;
    if (static_cast<size_t>(target) < combat_.size()) return combat_[target].enabled;
    return stats_[static_cast<size_t>(target) - kCombatFeatureCount].enabled;
}

bool Runtime::TargetSupportedLocked(int target) const {
    if (target == -1) return supported_ && std::isfinite(observed_.armor_original);
    if (target >= 0 && static_cast<size_t>(target) < combat_.size()) return combat_[target].supported;
    size_t index = static_cast<size_t>(target) - kCombatFeatureCount;
    if (index >= stats_.size() || !stats_[index].supported) return false;
    if (index == static_cast<size_t>(NativeStat::Omni))
        for (const auto& definition : kNativeStats)
            if (IsOmniMember(definition.stat) && !stats_[static_cast<size_t>(definition.stat)].supported) return false;
    return true;
}

void Runtime::DisableTargetLocked(int target) {
    TargetEnabledLocked(target).store(false);
    if (target == static_cast<int>(kCombatFeatureCount) + static_cast<int>(NativeStat::Revivals)) revival_pending_.store(false);
    if (target >= static_cast<int>(kCombatFeatureCount)) stats_revision_.fetch_add(1);
}

uint64_t Runtime::NextSessionLocked() {
    if (++session_ == 0) ++session_;
    reward_epoch_.fetch_add(1);
    return session_;
}

void Runtime::CompleteLocked(const Ticket& ticket, Response response) {
    if (ticket->completed) return;
    ticket->response = std::move(response);
    ticket->completed = true;
    ticket->ready.notify_all();
}

void Runtime::InvalidateQueuedLocked(const char* reason) {
    while (!queue_.empty()) {
        auto ticket = std::move(queue_.front());
        queue_.pop_front();
        CompleteLocked(ticket, ReplyLocked(ticket->request, false, reason));
    }
}

void Runtime::InvalidateTargetQueuedLocked(int target, const char* reason) {
    for (auto iterator = queue_.begin(); iterator != queue_.end();) {
        if ((*iterator)->target != target) { ++iterator; continue; }
        auto ticket = std::move(*iterator);
        iterator = queue_.erase(iterator);
        CompleteLocked(ticket, ReplyLocked(ticket->request, false, reason));
    }
}

void Runtime::InvalidateActualObservationsLocked() {
    last_actual_owner_.store(0);
    for (auto& feature : combat_) {
        feature.io_owner.store(0);
        feature.result_owner.store(0);
    }
    for (auto& stat : stats_) {
        stat.io_owner.store(0);
        stat.base_owner.store(0);
    }
}

void Runtime::SetArmorSupported(bool supported, std::string reason) {
    std::lock_guard lock(mutex_);
    supported_ = supported;
    supported_atomic_.store(supported);
    support_reason_ = std::move(reason);
    if (!supported) {
        DisableArmorLocked();
        InvalidateTargetQueuedLocked(-1, "Native armor entry is unavailable.");
    }
}

void Runtime::SetFeatureSupported(CombatFeature feature, bool supported, std::string reason) {
    size_t index = static_cast<size_t>(feature);
    if (index >= combat_.size()) return;
    std::lock_guard lock(mutex_);
    auto& state = combat_[index];
    state.supported = supported;
    state.reason = std::move(reason);
    state.supported_atomic.store(supported);
    if (!supported) {
        state.enabled.store(false);
        InvalidateTargetQueuedLocked(static_cast<int>(index), "Native feature entry is unavailable.");
    }
}

void Runtime::SetStatSupported(NativeStat stat, bool supported, std::string reason) {
    if (stat == NativeStat::Armor) { SetArmorSupported(supported, std::move(reason)); return; }
    size_t index = static_cast<size_t>(stat);
    if (index >= stats_.size()) return;
    std::lock_guard lock(mutex_);
    auto& control = stats_[index];
    control.supported = supported;
    control.supported_atomic.store(supported);
    control.reason = std::move(reason);
    if (!supported) {
        DisableTargetLocked(static_cast<int>(kCombatFeatureCount + index));
        InvalidateTargetQueuedLocked(static_cast<int>(kCombatFeatureCount + index), "Native stat entry is unavailable.");
        if (IsOmniMember(stat)) {
            int omni = static_cast<int>(kCombatFeatureCount) + static_cast<int>(NativeStat::Omni);
            DisableTargetLocked(omni);
            InvalidateTargetQueuedLocked(omni, "A unified bonus entry is unavailable.");
        }
    }
}

void Runtime::Observe(const GameObservation& observation, uint64_t now) {
    std::lock_guard lock(mutex_);
    ExpireLocked(now);
    bool ready = observation.verified && observation.safe_mode && observation.player_ready && observation.player != 0;
    const bool was_pending = revival_pending_.load();
    const bool pending = !ready && observation.verified && observation.safe_mode && observation.revival_pending
        && observation.player != 0 && observed_.player == observation.player && observed_.scene == observation.scene
        && observed_.verified && observed_.safe_mode && (player_ready_.load() || was_pending)
        && stats_[static_cast<size_t>(NativeStat::Revivals)].enabled.load();
    bool boundary = observed_.player != observation.player
        || (player_ready_.load() && !ready && !pending)
        || ((player_ready_.load() || was_pending) && observed_.scene != observation.scene)
        || observed_.verified != observation.verified || observed_.safe_mode != observation.safe_mode;
    if (observed_.wallet != observation.wallet) {
        DisableTargetLocked(static_cast<int>(CombatFeature::NoGoldSpend));
        InvalidateTargetQueuedLocked(static_cast<int>(CombatFeature::NoGoldSpend), "The main-game wallet changed.");
    }
    if (observed_.gold_wallet != observation.gold_wallet)
        InvalidateTargetQueuedLocked(kGoldTarget, "The selected gold wallet changed.");
    if (boundary) DisableLocked();
    else if (pending) {
        DisableStatsLocked(true);
        for (auto& feature : combat_) feature.enabled.store(false);
        rewards_.enabled = rewards_.upgrade_enabled = false;
        ++reward_revision_;
    }
    else if (!ready) DisablePlayerEffectsLocked();
    observed_ = observation;
    observation_stale_ = false;
    observation_atomic_.store(now);
    verified_.store(observation.verified);
    safe_mode_.store(observation.verified && observation.safe_mode);
    player_ready_.store(ready);
    revival_pending_.store(pending && !boundary);
    owner_.store(ready || (pending && !boundary) ? observation.player : 0);
    wallet_.store(observation.verified && observation.safe_mode ? observation.wallet : 0);
    if (!std::isfinite(observation.armor_original)) {
        DisableArmorLocked();
        last_actual_owner_.store(0);
        InvalidateTargetQueuedLocked(-1, "The native armor sample is unavailable.");
    }
    if (boundary || was_pending != (pending && !boundary)) {
        InvalidateActualObservationsLocked();
        ++generation_;
        reward_epoch_.fetch_add(1);
        InvalidateQueuedLocked("Game or player state changed before the command ran.");
    }
}

uint64_t Runtime::BeginSession(uint64_t now) {
    std::lock_guard lock(mutex_);
    DisableLocked();
    rewards_.processed_count = rewards_.upgrade_processed_count = rewards_.upgrade_granted_count = 0;
    rewards_.last_reward_item_id.clear(); rewards_.last_reward_name.clear();
    rewards_.last_upgrade_item_id.clear(); rewards_.last_upgrade_name.clear();
    rewards_.last_upgrade_granted_item_id.clear(); rewards_.last_upgrade_granted_name.clear();
    backup_authorized_.store(false);
    session_active_ = false;
    session_atomic_.store(false);
    NextSessionLocked();
    InvalidateQueuedLocked("Connection was replaced.");
    last_heartbeat_ = now;
    heartbeat_atomic_.store(now);
    session_active_ = true;
    session_atomic_.store(true);
    return session_;
}

void Runtime::EndSession(uint64_t session) {
    std::lock_guard lock(mutex_);
    if (!session_active_ || session != session_) return;
    DisableLocked();
    backup_authorized_.store(false);
    session_active_ = false;
    session_atomic_.store(false);
    NextSessionLocked();
    InvalidateQueuedLocked("Connection ended.");
}

void Runtime::ExpireLocked(uint64_t now) {
    if (backup_authorized_.load() && now > backup_expires_at_.load()) {
        backup_authorized_.store(false);
        DisableTargetLocked(static_cast<int>(CombatFeature::NoGoldSpend));
        InvalidateTargetQueuedLocked(static_cast<int>(CombatFeature::NoGoldSpend), "The startup backup authorization expired.");
        InvalidateTargetQueuedLocked(kGoldTarget, "The startup backup authorization expired.");
    }
    if (session_active_ && Elapsed(now, last_heartbeat_, 3000)) {
        DisableLocked();
        backup_authorized_.store(false);
        session_active_ = false;
        session_atomic_.store(false);
        NextSessionLocked();
        InvalidateQueuedLocked("Connection heartbeat expired.");
    }
    ExpireObservationLocked(now);
}

void Runtime::ExpireObservationLocked(uint64_t now) {
    if (observation_stale_ || (!player_ready_.load() && !revival_pending_.load() && wallet_.load() == 0 && !observed_.gold_wallet.config)
        || !Elapsed(now, observation_atomic_.load(), 1000)) return;
    DisableLocked();
    player_ready_.store(false);
    revival_pending_.store(false);
    owner_.store(0);
    wallet_.store(0);
    safe_mode_.store(false);
    InvalidateActualObservationsLocked();
    observation_stale_ = true;
    ++generation_;
    reward_epoch_.fetch_add(1);
    InvalidateQueuedLocked("The main-thread game observation expired.");
}

void Runtime::CheckHeartbeat(uint64_t now) {
    std::lock_guard lock(mutex_);
    ExpireLocked(now);
}

StateSnapshot Runtime::SnapshotLocked() const {
    StateSnapshot state;
    state.verified = verified_.load();
    state.safe_mode = safe_mode_.load();
    state.player_ready = player_ready_.load();
    state.revival_pending = revival_pending_.load() && !observation_stale_;
    state.paused_known = observed_.paused_known && !observation_stale_;
    state.paused = observed_.paused;
    state.backup_authorized = backup_authorized_.load() && session_active_;
    state.wallet_ready = state.verified && state.safe_mode && !observation_stale_ && wallet_.load() != 0;
    state.gold_known = state.verified && state.safe_mode && !observation_stale_ && observed_.gold_known && std::isfinite(observed_.gold);
    state.gold = observed_.gold;
    state.gold_adventure = state.gold_known ? observed_.gold_wallet.adventure : -1;
    state.gold_write_supported = gold_writer_ != nullptr && state.verified;
    state.gold_write_ready = state.gold_write_supported && state.safe_mode && !state.player_ready && !state.revival_pending
        && state.gold_known && observed_.gold_write_ready && observed_.gold_wallet.options && observed_.gold_wallet.config
        && state.backup_authorized;
    state.armor_supported = supported_ && state.verified && std::isfinite(observed_.armor_original);
    state.armor_enabled = enabled_.load() && session_active_ && state.player_ready && state.armor_supported;
    state.armor_sample_known = state.player_ready && std::isfinite(observed_.armor_original);
    state.armor_target = target_;
    state.armor_original = observed_.armor_original;
    state.armor_game_reads = armor_reads_.load();
    state.armor_override_reads = armor_override_reads_.load();
    state.armor_actual_read_known = state.player_ready && last_actual_owner_.load() == owner_.load();
    uint64_t io = last_io_.load();
    state.armor_last_input = std::bit_cast<float>(static_cast<uint32_t>(io));
    state.armor_last_output = std::bit_cast<float>(static_cast<uint32_t>(io >> 32));
    state.armor_last_was_override = last_was_override_.load();
    state.armor_current_known = state.armor_actual_read_known && state.armor_last_was_override == state.armor_enabled
        && (!state.armor_enabled || state.armor_last_output == target_atomic_.load());
    state.armor_current = state.armor_last_output;
    state.health_known = state.player_ready && observed_.health_known && std::isfinite(observed_.health);
    state.experience_known = state.player_ready && observed_.experience_known && std::isfinite(observed_.experience);
    state.level_known = state.player_ready && observed_.level_known;
    state.health = observed_.health;
    state.experience = observed_.experience;
    state.level = observed_.level;
    state.scene = observed_.scene;
    state.error = observation_stale_ ? "Waiting for a fresh main-thread game observation."
        : observed_.error;
    state.armor_reason = support_reason_;
    state.chest_reward = rewards_;
    state.chest_reward.supported = rewards_.supported && state.verified;
    state.chest_reward.upgrade_supported = rewards_.upgrade_supported && state.verified;
    state.chest_reward.enabled = rewards_.enabled && state.player_ready && session_active_ && state.chest_reward.supported;
    state.chest_reward.upgrade_enabled = rewards_.upgrade_enabled && state.player_ready && session_active_ && state.chest_reward.upgrade_supported;
    for (size_t index = 0; index < combat_.size(); ++index) {
        const auto& feature = combat_[index];
        auto& snapshot = state.combat[index];
        snapshot.supported = feature.supported && state.verified;
        snapshot.enabled = feature.enabled.load() && session_active_ && TargetReadyLocked(static_cast<int>(index)) && snapshot.supported;
        snapshot.value = feature.value;
        snapshot.reason = feature.reason;
        snapshot.calls = feature.calls.load();
        snapshot.override_calls = feature.overrides.load();
        snapshot.actual_calls = feature.actual_calls.load();
        snapshot.overflow_skips = feature.overflow_skips.load();
        snapshot.io_known = index == static_cast<size_t>(CombatFeature::NoGoldSpend)
            ? state.safe_mode && wallet_.load() != 0 && feature.io_owner.load() == wallet_.load()
            : state.player_ready && feature.io_owner.load() == owner_.load();
        snapshot.result_known = state.player_ready && feature.result_owner.load() == owner_.load();
        uint64_t io = feature.io.load(), result = feature.result.load(), levels = feature.levels.load();
        snapshot.last_input = First(io); snapshot.last_output = Second(io);
        snapshot.result_before = First(result); snapshot.result_after = Second(result);
        snapshot.level_before = std::bit_cast<int32_t>(static_cast<uint32_t>(levels));
        snapshot.level_after = std::bit_cast<int32_t>(static_cast<uint32_t>(levels >> 32));
        snapshot.actual_loss_total = feature.loss_total.load();
    }
    const uint64_t revision = stats_revision_.load();
    for (const auto& definition : kNativeStats) {
        size_t index = static_cast<size_t>(definition.stat);
        auto& snapshot = state.stats[index];
        const auto& control = stats_[index];
        if (definition.stat == NativeStat::Armor) {
            snapshot.supported = state.armor_supported;
            snapshot.enabled = state.armor_enabled;
            snapshot.value = state.armor_target;
            snapshot.sample_known = state.armor_sample_known;
            snapshot.original = state.armor_original;
            snapshot.current_known = state.armor_current_known;
            snapshot.current = state.armor_current;
            snapshot.game_reads = state.armor_game_reads;
            snapshot.override_reads = state.armor_override_reads;
            snapshot.io_known = state.armor_actual_read_known;
            snapshot.last_input = state.armor_last_input;
            snapshot.last_output = state.armor_last_output;
            snapshot.reason = state.armor_reason;
            snapshot.source = NativeStat::Armor;
            continue;
        }
        const bool stat_ready = state.player_ready || (definition.stat == NativeStat::Revivals && state.revival_pending);
        snapshot.supported = state.verified && TargetSupportedLocked(static_cast<int>(kCombatFeatureCount + index));
        snapshot.enabled = snapshot.supported && stat_ready && session_active_ && control.enabled.load();
        snapshot.value = control.value;
        snapshot.reason = control.reason;
        snapshot.sample_known = definition.stat != NativeStat::Omni && stat_ready
            && observed_.stat_known[index] && std::isfinite(observed_.stat_original[index]);
        if (snapshot.sample_known) snapshot.original = StatFromRaw(definition, observed_.stat_original[index]);
        snapshot.bound_known = stat_ready && observed_.stat_current_known[index] && std::isfinite(observed_.stat_current[index]);
        if (snapshot.bound_known) snapshot.bound = StatFromRaw(definition, observed_.stat_current[index]);
        snapshot.game_reads = control.reads.load();
        snapshot.override_reads = control.overrides.load();
        snapshot.io_known = stat_ready && control.io_owner.load() == owner_.load();
        uint64_t io = control.io.load();
        snapshot.last_input = First(io);
        snapshot.last_output = Second(io);
        auto source = control.source.load();
        snapshot.source = source < kNativeStatCount ? static_cast<NativeStat>(source) : definition.stat;
        snapshot.last_is_final = control.final_read.load();
        // Final methods include a weapon-specific factor/floor. Their raw
        // evidence must never be displayed as the character's percentage.
        snapshot.current_known = definition.stat != NativeStat::Omni && stat_ready
            && control.base_owner.load() == owner_.load() && control.base_revision.load() == revision;
        if (snapshot.current_known) snapshot.current = StatFromRaw(definition, Second(control.base_io.load()));
    }
    return state;
}

StateSnapshot Runtime::Snapshot(uint64_t now) {
    std::lock_guard lock(mutex_);
    ExpireLocked(now);
    return SnapshotLocked();
}

Response Runtime::ReplyLocked(const Request& request, bool ok, const char* error) const {
    Response response;
    response.id = request.id;
    response.ok = ok;
    if (error) response.error = error;
    if (!ok) {
        if (request.kind == "getItemCatalog" || request.kind == "getItemIcon") response.error_scope = "catalog";
        else if (request.kind == "setChestReward") response.error_scope = request.has_upgrade_enabled && request.upgrade_enabled
            ? "upgrade" : request.has_enabled && request.enabled ? "chest" : "rewardTarget";
    }
    response.state = SnapshotLocked();
    return response;
}

Runtime::Ticket Runtime::Submit(Request request, uint64_t session, uint64_t now) {
    auto ticket = std::make_shared<Pending>();
    ticket->owner = this;
    ticket->request = std::move(request);
    std::lock_guard lock(mutex_);
    ExpireLocked(now);
    auto reject = [&](const char* reason, int target = -2) {
        if (target == -2) {
            DisableLocked();
            InvalidateQueuedLocked(reason);
        } else if (target == -1) {
            DisableArmorLocked();
            InvalidateTargetQueuedLocked(target, reason);
        } else if (target >= 0 && static_cast<size_t>(target) < kCombatFeatureCount + kNativeStatCount) {
            DisableTargetLocked(target);
            InvalidateTargetQueuedLocked(target, reason);
        }
        CompleteLocked(ticket, ReplyLocked(ticket->request, false, reason));
    };
    const auto& input = ticket->request;
    if (!session_active_ || session != session_) {
        reject("Connection is no longer active.", -3);
        return ticket;
    }
    if (!ValidRequestId(input.id)) {
        reject("Request ID is invalid.");
        return ticket;
    }
    bool observation = input.kind == "hello" || input.kind == "heartbeat" || input.kind == "getState" || input.kind == "getGold";
    bool disable = input.kind == "disableAll" || input.kind == "disableStats";
    bool catalog = input.kind == "getItemCatalog" || input.kind == "getItemIcon";
    bool reward = input.kind == "setChestReward";
    const bool gold = input.kind == "setGold" || input.kind == "setAdventureGold";
    if (!observation && !disable && !catalog && !reward && !gold && input.kind != "setStat" && input.kind != "setFeature") {
        reject("Command is not supported by the native pilot.");
        return ticket;
    }
    // A delayed request must not replace a newer accepted heartbeat sample.
    last_heartbeat_ = std::max(last_heartbeat_, now);
    heartbeat_atomic_.store(last_heartbeat_);
    if (disable) {
        if (input.kind == "disableAll") {
            DisableLocked();
            InvalidateQueuedLocked("A disable request cancelled queued changes.");
        } else {
            DisableStatsLocked();
            InvalidateTargetQueuedLocked(-1, "Stat overrides were disabled.");
            for (size_t index = 0; index < stats_.size(); ++index)
                InvalidateTargetQueuedLocked(static_cast<int>(kCombatFeatureCount + index), "Stat overrides were disabled.");
        }
        CompleteLocked(ticket, ReplyLocked(input, true));
        return ticket;
    }
    if (observation) {
        CompleteLocked(ticket, ReplyLocked(input, true));
        return ticket;
    }
    if (reward || catalog) {
        auto fail = [&](const char* reason) { CompleteLocked(ticket, ReplyLocked(input, false, reason)); };
        if (reward) {
            // An explicit off is immediate, including when the other route's
            // target has become maxed or its native entry is unavailable.
            bool off = (input.has_enabled && !input.enabled) || (input.has_upgrade_enabled && !input.upgrade_enabled);
            if (off) {
                if (input.has_enabled && !input.enabled) rewards_.enabled = false;
                if (input.has_upgrade_enabled && !input.upgrade_enabled) rewards_.upgrade_enabled = false;
                ++reward_revision_;
                InvalidateTargetQueuedLocked(kRewardTarget, "A reward disable cancelled older target changes.");
            }
            if ((!input.has_item_id || input.item_id.empty()) && !(input.has_enabled && input.enabled)
                && !(input.has_upgrade_enabled && input.upgrade_enabled)) {
                CompleteLocked(ticket, ReplyLocked(input, true)); return ticket;
            }
            bool chest = input.has_enabled ? input.enabled : rewards_.enabled;
            bool upgrade = input.has_upgrade_enabled ? input.upgrade_enabled : rewards_.upgrade_enabled;
            std::string id = input.has_item_id ? input.item_id : rewards_.target_item_id;
            if (!input.has_item_id) for (auto queued = queue_.rbegin(); queued != queue_.rend(); ++queued) {
                if ((*queued)->target == kRewardTarget && !(*queued)->completed && (*queued)->request.has_item_id) {
                    id = (*queued)->request.item_id; break;
                }
            }
            if (!ValidItemId(id)) { fail("Select a valid weapon or passive item first."); return ticket; }
            if ((chest && !rewards_.supported) || (upgrade && !rewards_.upgrade_supported)) {
                fail("The requested native reward entry is unavailable."); return ticket;
            }
            if ((chest || upgrade) && !player_ready_.load()) {
                fail("Enter a supported single-player run before enabling specified rewards."); return ticket;
            }
            // Keep omitted fields until execution. A prior queued change may
            // update the shared target or the other independent route first.
        } else if (input.kind == "getItemCatalog" && (input.catalog_offset < 0 || input.catalog_offset > 4096
            || input.catalog_limit < 1 || input.catalog_limit > 200)) {
            fail("Catalog requests require an offset from 0 to 4096 and a page size from 1 to 200."); return ticket;
        } else if (input.kind == "getItemIcon" && !ValidItemId(input.item_id)) {
            fail("The item icon ID is invalid."); return ticket;
        }
        if (!verified_.load() || !safe_mode_.load() || observation_stale_ || Elapsed(now, observation_atomic_.load(), 1000)) {
            fail("Wait for fresh single-player game data before reading or changing rewards."); return ticket;
        }
        if (!reward_adapter_) { fail("The native reward adapter is unavailable."); return ticket; }
        if (queue_.size() >= 64) { fail("Native main-thread command queue is full."); return ticket; }
        ticket->session = session; ticket->target = reward ? kRewardTarget : kCatalogTarget;
        ticket->generation = generation_; ticket->queued_at = now;
        queue_.push_back(ticket); return ticket;
    }
    if (gold) {
        auto gold_reject = [&](const char* reason) { CompleteLocked(ticket, ReplyLocked(input, false, reason)); };
        if (!input.has_value || input.has_enabled || !std::isfinite(input.value) || input.value < 0
            || input.value > 999999999 || input.value != std::trunc(input.value)
            || static_cast<double>(static_cast<float>(input.value)) != input.value) {
            gold_reject("Gold must be an exactly representable integer from 0 to 999999999."); return ticket;
        }
        if ((input.kind == "setAdventureGold") != (input.gold_adventure >= 0)
            || input.gold_adventure != observed_.gold_wallet.adventure) {
            gold_reject("The selected gold wallet changed."); return ticket;
        }
        if (!SnapshotLocked().gold_write_ready) {
            gold_reject("A verified pre-start backup and a single-player menu are required."); return ticket;
        }
        if (queue_.size() >= 64) { gold_reject("Native main-thread command queue is full."); return ticket; }
        ticket->session = session; ticket->target = kGoldTarget; ticket->generation = generation_;
        ticket->queued_at = now; queue_.push_back(ticket); return ticket;
    }
    const auto* stat_definition = input.kind == "setStat" ? FindNativeStat(input.stat) : nullptr;
    int target = input.kind == "setFeature" ? FeatureIndex(input.feature)
        : stat_definition && stat_definition->stat != NativeStat::Armor
            ? static_cast<int>(kCombatFeatureCount) + static_cast<int>(stat_definition->stat) : -1;
    if (input.kind == "setStat" && !stat_definition) {
        reject("This stat has not been migrated to the native backend.");
        return ticket;
    }
    if (input.kind == "setFeature" && target < 0) {
        reject("This feature has not been migrated to the native backend.");
        return ticket;
    }
    auto& enabled = TargetEnabledLocked(target);
    if (input.has_enabled && !input.enabled) {
        DisableTargetLocked(target);
        InvalidateTargetQueuedLocked(target, "An explicit disable cancelled older changes to this effect.");
    }
    bool toggle = !stat_definition && (target == 0 || target == 4 || target == 5 || target == 6
        || target == static_cast<int>(CombatFeature::SkipChestAnimation) || target == static_cast<int>(CombatFeature::FiveItemChests));
    const bool game_speed = target == static_cast<int>(CombatFeature::GameSpeed);
    double minimum = target < 0 ? 0 : target == 3 ? 0.25 : 1;
    double maximum = target < 0 ? 10000 : toggle ? 1 : target == 3 ? 10 : 1000;
    if (input.has_value && (stat_definition ? !ValidStatValue(*stat_definition, input.value)
            : game_speed ? !ValidGameSpeed(input.value)
            : !std::isfinite(input.value) || input.value < minimum || input.value > maximum)) {
        reject(target < 0 ? "Armor must be a finite number from 0 to 10000."
            : stat_definition ? "The stat target is outside its supported range or requires an integer."
            : toggle ? "This toggle has a fixed value of 1."
            : game_speed ? "Game speed must be a finite number from 1 to 10."
            : target == 3 ? "The movement multiplier must be a finite number from 0.25 to 10."
            : "The multiplier must be a finite number from 1 to 1000.", target);
        return ticket;
    }
    bool enabling = input.has_enabled ? input.enabled : enabled.load();
    if (!enabling && !input.has_value) {
        InvalidateTargetQueuedLocked(target, "A disable request cancelled older changes to this effect.");
        CompleteLocked(ticket, ReplyLocked(input, true));
        return ticket;
    }
    bool supported = TargetSupportedLocked(target);
    if (!supported || !verified_.load()) {
        reject("The native effect entry or supported game verification is unavailable.", target);
        return ticket;
    }
    if (enabling && !TargetReadyLocked(target)) {
        if (stat_definition && stat_definition->stat == NativeStat::Revivals && revival_pending_.load()) {
            reject("Wait for the character to revive before changing the revival target.", -3);
            return ticket;
        }
        reject(target == 6 ? "A verified main-game wallet and a valid pre-start backup are required."
            : "Enter a supported single-player run before enabling this effect.", target);
        return ticket;
    }
    if (queue_.size() >= 64) {
        reject("Native main-thread command queue is full.");
        return ticket;
    }
    ticket->session = session;
    ticket->target = target;
    ticket->generation = generation_;
    ticket->queued_at = now;
    ticket->desired_enabled = enabling;
    queue_.push_back(ticket);
    return ticket;
}

void Runtime::Pump(uint64_t now) {
    std::unique_lock lock(mutex_);
    ExpireLocked(now);
    for (unsigned count = 0; count < 32 && !queue_.empty(); ++count) {
        auto ticket = std::move(queue_.front());
        queue_.pop_front();
        if (ticket->completed) continue;
        if (!session_active_ || ticket->session != session_ || ticket->generation != generation_
            || Elapsed(now, ticket->queued_at, 1500)) {
            CompleteLocked(ticket, ReplyLocked(ticket->request, false, "Connection, player or command deadline changed."));
            continue;
        }
        if (ticket->target == kRewardTarget || ticket->target == kCatalogTarget) {
            const bool changing = ticket->target == kRewardTarget;
            const auto& request = ticket->request;
            Request effective = request;
            if (changing) {
                if (!effective.has_item_id) effective.item_id = rewards_.target_item_id;
                effective.enabled = request.has_enabled ? request.enabled : rewards_.enabled;
                effective.upgrade_enabled = request.has_upgrade_enabled ? request.upgrade_enabled : rewards_.upgrade_enabled;
                effective.has_item_id = effective.has_enabled = effective.has_upgrade_enabled = true;
            }
            if (!reward_adapter_ || !verified_.load() || !safe_mode_.load() || observation_stale_
                || Elapsed(now, observation_atomic_.load(), 1000)
                || (changing && ((effective.enabled || effective.upgrade_enabled) && !player_ready_.load()))) {
                CompleteLocked(ticket, ReplyLocked(request, false, "The single-player reward context is no longer ready.")); continue;
            }
            auto adapter = reward_adapter_; auto context = reward_adapter_context_;
            const auto revision = reward_revision_; const auto epoch = reward_epoch_.load();
            bool ok = false; std::string error; Response result;
            const auto started = std::chrono::steady_clock::now();
            // This adapter only reads/validates game data. Unlike a native gold
            // save it remains cancellable while outside the control lock.
            lock.unlock();
            try { ok = adapter(context, effective, result, error); }
            catch (...) { error = "The native reward adapter failed."; }
            lock.lock();
            if (ticket->completed) break;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            const uint64_t elapsed_ms = elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0;
            const uint64_t finished = now <= UINT64_MAX - elapsed_ms ? now + elapsed_ms : UINT64_MAX;
            ExpireLocked(finished);
            if (!session_active_ || ticket->session != session_ || epoch != reward_epoch_.load()
                || Elapsed(finished, ticket->queued_at, 1500) || !safe_mode_.load()
                || (changing && revision != reward_revision_)) {
                ok = false; error = "The reward context or target changed while validating the request.";
            }
            if (ok && changing) {
                rewards_.target_item_id = effective.item_id;
                rewards_.target_item_name = result.state.chest_reward.target_item_name;
                rewards_.enabled = effective.enabled && rewards_.supported;
                rewards_.upgrade_enabled = effective.upgrade_enabled && rewards_.upgrade_supported;
                rewards_.message = rewards_.enabled ? "等待下一个可替换的宝箱奖励。" : "宝箱指定已关闭。";
                rewards_.upgrade_message = rewards_.upgrade_enabled ? "等待下一次普通升级或重选；需在游戏内选择。" : "升级指定已关闭。";
                ++reward_revision_;
            }
            Response reply = ReplyLocked(request, ok, error.empty() ? (ok ? nullptr : "The reward request failed.") : error.c_str());
            if (ok) {
                reply.has_item_catalog = result.has_item_catalog;
                reply.item_catalog_loading = result.item_catalog_loading;
                reply.item_icons_supported = result.item_icons_supported;
                reply.item_catalog_total = result.item_catalog_total;
                reply.item_catalog = std::move(result.item_catalog);
                reply.has_item_icon = result.has_item_icon;
                reply.item_icon_id = std::move(result.item_icon_id);
                reply.item_icon_rgba_hex = std::move(result.item_icon_rgba_hex);
                reply.item_icon_width = result.item_icon_width;
                reply.item_icon_height = result.item_icon_height;
                reply.item_icon_pending = result.item_icon_pending;
            }
            if (!ok && !result.error_scope.empty()) reply.error_scope = std::move(result.error_scope);
            CompleteLocked(ticket, std::move(reply));
            break; // One bounded native/catalog operation per fresh frame.
        }
        if (ticket->target == kGoldTarget) {
            if (!SnapshotLocked().gold_write_ready) {
                CompleteLocked(ticket, ReplyLocked(ticket->request, false, "The selected wallet or startup backup is no longer ready."));
                continue;
            }
            double actual = std::numeric_limits<double>::quiet_NaN(); std::string error;
            bool ok = false;
            auto writer = gold_writer_; auto context = gold_writer_context_;
            GoldWallet wallet = observed_.gold_wallet; uint64_t generation = generation_;
            // Once execution starts, a native save must finish with its real
            // result. Keep the 1.5 s deadline on starting queued writes, while
            // allowing native callbacks to observe/disable without deadlock.
            ticket->executing = true;
            lock.unlock();
            try { ok = writer(context, wallet, ticket->request.value, actual, error); }
            catch (...) { error = "The native wallet writer failed."; }
            lock.lock(); ticket->executing = false;
            if (generation == generation_ && wallet == observed_.gold_wallet && std::isfinite(actual)) {
                observed_.gold = actual; observed_.gold_known = true;
            }
            if (ok && actual != ticket->request.value) { ok = false; error = "The game did not retain the requested gold value."; }
            CompleteLocked(ticket, ReplyLocked(ticket->request, ok, error.empty() ? (ok ? nullptr : "The native wallet write failed.") : error.c_str()));
            break; // Resample monotonic time on the next frame after native I/O.
        }
        bool supported = TargetSupportedLocked(ticket->target);
        if (!supported || !verified_.load()
            || (ticket->desired_enabled && !TargetReadyLocked(ticket->target))) {
            DisableTargetLocked(ticket->target);
            CompleteLocked(ticket, ReplyLocked(ticket->request, false, "The single-player effect is no longer ready."));
            continue;
        }
        if (ticket->request.has_value) {
            if (ticket->target < 0) {
                target_ = ticket->request.value;
                target_atomic_.store(static_cast<float>(target_));
            } else if (static_cast<size_t>(ticket->target) < combat_.size()) {
                auto& feature = combat_[ticket->target];
                feature.value = ticket->request.value;
                feature.value_atomic.store(feature.value);
            } else {
                auto& stat = stats_[static_cast<size_t>(ticket->target) - kCombatFeatureCount];
                stat.value = ticket->request.value;
                stat.value_atomic.store(stat.value);
            }
        }
        TargetEnabledLocked(ticket->target).store(ticket->desired_enabled);
        if (ticket->target >= static_cast<int>(kCombatFeatureCount)) stats_revision_.fetch_add(1);
        CompleteLocked(ticket, ReplyLocked(ticket->request, true));
    }
}

Response Runtime::Await(const Ticket& ticket, uint32_t timeout_ms) {
    std::unique_lock lock(mutex_);
    if (!ticket || ticket->owner != this) return Response{};
    if (!ticket->completed) ticket->ready.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return ticket->completed; });
    // Match the pre-existing atomic native-save semantics: do not report a
    // cancelled write after its game-side transaction has already started.
    if (ticket->executing) ticket->ready.wait(lock, [&] { return ticket->completed; });
    if (!ticket->completed) CompleteLocked(ticket, ReplyLocked(ticket->request, false, "Native main-thread command timed out and was cancelled."));
    return ticket->response;
}

bool Runtime::TryResult(const Ticket& ticket, Response& response) {
    std::lock_guard lock(mutex_);
    if (!ticket || ticket->owner != this || !ticket->completed) return false;
    response = ticket->response;
    return true;
}

void Runtime::Cancel(const Ticket& ticket) {
    std::lock_guard lock(mutex_);
    if (ticket && ticket->owner == this && !ticket->completed && !ticket->executing)
        CompleteLocked(ticket, ReplyLocked(ticket->request, false, "Native command was cancelled."));
}

void Runtime::DisableAll() {
    std::lock_guard lock(mutex_);
    DisableLocked();
    InvalidateQueuedLocked("All native overrides were disabled.");
}

float Runtime::FilterArmor(uintptr_t owner, float original, uint64_t now) noexcept {
    if (!ReadyOwner(owner, now) || !std::isfinite(original)) return original;
    bool overridden = enabled_.load() && supported_atomic_.load() && session_atomic_.load()
        && !Elapsed(now, heartbeat_atomic_.load(), 3000);
    float output = overridden ? target_atomic_.load() : original;
    armor_reads_.fetch_add(1);
    if (overridden) armor_override_reads_.fetch_add(1);
    last_io_.store(Pack(original, output));
    last_was_override_.store(overridden);
    last_actual_owner_.store(owner);
    return output;
}

bool Runtime::ReadyOwner(uintptr_t owner, uint64_t now) const noexcept {
    return verified_.load() && safe_mode_.load() && player_ready_.load() && owner != 0 && owner == owner_.load()
        && !Elapsed(now, observation_atomic_.load(), 1000);
}

bool Runtime::ReadyStatOwner(NativeStat stat, uintptr_t owner, uint64_t now) const noexcept {
    return ReadyOwner(owner, now) || (stat == NativeStat::Revivals && revival_pending_.load()
        && verified_.load() && safe_mode_.load() && owner != 0 && owner == owner_.load()
        && !Elapsed(now, observation_atomic_.load(), 1000));
}

bool Runtime::FeatureEnabled(CombatFeature feature, uintptr_t owner, uint64_t now) const noexcept {
    size_t index = static_cast<size_t>(feature);
    bool ready = feature == CombatFeature::NoGoldSpend
        ? !revival_pending_.load() && verified_.load() && safe_mode_.load() && owner != 0 && owner == wallet_.load() && backup_authorized_.load()
            && now <= backup_expires_at_.load() && !Elapsed(now, observation_atomic_.load(), 1000)
        : ReadyOwner(owner, now);
    return index < combat_.size() && ready && session_atomic_.load()
        && !Elapsed(now, heartbeat_atomic_.load(), 3000)
        && combat_[index].supported_atomic.load() && combat_[index].enabled.load();
}

float Runtime::ActiveGameSpeed(uintptr_t owner, uint64_t now) const noexcept {
    if (!FeatureEnabled(CombatFeature::GameSpeed, owner, now)) return 0;
    return static_cast<float>(combat_[static_cast<size_t>(CombatFeature::GameSpeed)].value_atomic.load());
}

void Runtime::RecordGameSpeedWrite(uintptr_t owner, float before, float after, bool override, uint64_t now) noexcept {
    if (!ReadyOwner(owner, now) || !std::isfinite(before) || !std::isfinite(after)) return;
    auto& control = combat_[static_cast<size_t>(CombatFeature::GameSpeed)];
    control.io.store(Pack(before, after));
    control.io_owner.store(owner);
    control.calls.fetch_add(1);
    if (override) control.overrides.fetch_add(1);
    control.actual_calls.fetch_add(1);
}

void Runtime::RecordChestEvent(CombatFeature feature, uintptr_t owner, uint64_t now, int32_t equipment_slots) noexcept {
    if (!ReadyOwner(owner, now)) return;
    auto& control = combat_[static_cast<size_t>(feature)];
    if (feature == CombatFeature::FiveItemChests) {
        control.io.store(Pack(kFiveItemChestCount, static_cast<float>(equipment_slots)));
        control.io_owner.store(equipment_slots >= 0 && equipment_slots <= kFiveItemChestCount ? owner : 0);
    }
    control.calls.fetch_add(1);
    control.overrides.fetch_add(1);
    control.actual_calls.fetch_add(1);
}

bool Runtime::StatEnabled(NativeStat stat, uintptr_t owner, uint64_t now) const noexcept {
    size_t index = static_cast<size_t>(stat);
    if (index >= stats_.size() || !ReadyStatOwner(stat, owner, now) || !session_atomic_.load()
        || Elapsed(now, heartbeat_atomic_.load(), 3000)) return false;
    if (stat == NativeStat::Armor) return enabled_.load() && supported_atomic_.load();
    if (!stats_[index].supported_atomic.load() || !stats_[index].enabled.load()) return false;
    if (stat == NativeStat::Omni)
        for (const auto& definition : kNativeStats)
            if (IsOmniMember(definition.stat) && !stats_[static_cast<size_t>(definition.stat)].supported_atomic.load()) return false;
    return true;
}

double Runtime::StatValue(NativeStat stat) const noexcept {
    size_t index = static_cast<size_t>(stat);
    return stat == NativeStat::Armor ? target_atomic_.load()
        : index < stats_.size() ? stats_[index].value_atomic.load() : 0;
}

void Runtime::ObserveStatRead(NativeStat stat, uintptr_t owner, float original, float current,
        bool overridden, uint64_t now) noexcept {
    size_t index = static_cast<size_t>(stat);
    if (index >= stats_.size() || stat == NativeStat::Armor || stat == NativeStat::Omni
        || !ReadyStatOwner(stat, owner, now) || !std::isfinite(original) || !std::isfinite(current)) return;
    auto& control = stats_[index];
    control.reads.fetch_add(1);
    if (overridden) control.overrides.fetch_add(1);
    control.io.store(Pack(original, current));
    control.source.store(static_cast<uint8_t>(stat));
    control.final_read.store(false);
    uint64_t revision = stats_revision_.load();
    control.io_revision.store(revision); control.io_owner.store(owner);
    control.base_io.store(Pack(original, current));
    control.base_revision.store(revision); control.base_owner.store(owner);
}

float Runtime::FilterGoldSpend(uintptr_t wallet, float amount, bool main_wallet, uint64_t now) noexcept {
    if (!main_wallet || !std::isfinite(amount) || amount <= 0 || !wallet || wallet != wallet_.load()
        || !verified_.load() || !safe_mode_.load() || Elapsed(now, observation_atomic_.load(), 1000)) return amount;
    auto& feature = combat_[static_cast<size_t>(CombatFeature::NoGoldSpend)];
    bool overridden = FeatureEnabled(CombatFeature::NoGoldSpend, wallet, now);
    float result = overridden ? 0 : amount;
    feature.calls.fetch_add(1); feature.actual_calls.fetch_add(1);
    if (overridden) feature.overrides.fetch_add(1);
    feature.io.store(Pack(amount, result)); feature.io_owner.store(wallet);
    return result;
}

float Runtime::FilterStatValue(NativeStat stat, uintptr_t owner, float original, float parameter,
        bool final, uint64_t now, bool record_read) noexcept {
    size_t index = static_cast<size_t>(stat);
    if (index >= stats_.size() || stat == NativeStat::Omni || !ReadyOwner(owner, now)
        || !std::isfinite(original) || (final && !std::isfinite(parameter))) return original;
    if (stat == NativeStat::Armor) {
        if (record_read) return FilterArmor(owner, original, now);
        return StatEnabled(stat, owner, now) ? target_atomic_.load() : original;
    }
    uint64_t revision = stats_revision_.load();
    auto& control = stats_[index];
    NativeStat source = stat;
    bool overridden = StatEnabled(stat, owner, now);
    if (!overridden && IsOmniMember(stat) && StatEnabled(NativeStat::Omni, owner, now)) {
        source = NativeStat::Omni;
        overridden = true;
    }
    float output = original;
    if (overridden) {
        const auto& definition = kNativeStats[static_cast<size_t>(source)];
        double raw = StatToRaw(definition, StatValue(source));
        double changed = final && stat == NativeStat::Area ? raw * parameter
            : final && stat == NativeStat::Cooldown ? std::max(raw, static_cast<double>(parameter)) : raw;
        float result = static_cast<float>(changed);
        if (std::isfinite(changed) && std::isfinite(result)) output = result;
        else overridden = false;
    }
    if (!record_read) return output;
    control.reads.fetch_add(1);
    if (overridden) control.overrides.fetch_add(1);
    control.io.store(Pack(original, output));
    control.source.store(static_cast<uint8_t>(overridden ? source : stat));
    control.final_read.store(final);
    control.io_revision.store(revision);
    control.io_owner.store(owner);
    if (!final) {
        control.base_io.store(Pack(original, output));
        control.base_revision.store(revision);
        control.base_owner.store(owner);
    }
    if (overridden && source == NativeStat::Omni) {
        auto& omni = stats_[static_cast<size_t>(NativeStat::Omni)];
        omni.reads.fetch_add(1);
        omni.overrides.fetch_add(1);
    }
    return output;
}

float Runtime::FilterStat(NativeStat stat, uintptr_t owner, float original, uint64_t now, bool record_read) noexcept {
    return FilterStatValue(stat, owner, original, 1, false, now, record_read);
}

float Runtime::FilterStatFinal(NativeStat stat, uintptr_t owner, float original, float parameter,
        uint64_t now, bool record_read) noexcept {
    if (stat != NativeStat::Area && stat != NativeStat::Cooldown) return original;
    return FilterStatValue(stat, owner, original, parameter, true, now, record_read);
}

float Runtime::FilterFeature(CombatFeature feature, uintptr_t owner, float original, uint64_t now, bool record_read) noexcept {
    size_t index = static_cast<size_t>(feature);
    if (index >= combat_.size() || !ReadyOwner(owner, now) || !std::isfinite(original)) return original;
    auto& state = combat_[index];
    float output = original;
    bool overridden = FeatureEnabled(feature, owner, now) && (feature == CombatFeature::Movement || original > 0);
    if (overridden) {
        if (feature == CombatFeature::Health) output = 0;
        else if (feature == CombatFeature::Cooldown) output = std::min(original, 100.0f);
        else {
            float scaled = static_cast<float>(static_cast<double>(original) * state.value_atomic.load());
            if (std::isfinite(scaled)) output = scaled;
            else { overridden = false; if (record_read) state.overflow_skips.fetch_add(1); }
        }
    }
    if (!record_read) return output;
    state.calls.fetch_add(1);
    if (feature == CombatFeature::Movement || feature == CombatFeature::Cooldown) state.actual_calls.fetch_add(1);
    if (overridden) state.overrides.fetch_add(1);
    state.io.store(Pack(original, output));
    state.io_owner.store(owner);
    return output;
}

float Runtime::FilterHealthDamage(uintptr_t owner, float original, uint64_t now) noexcept {
    return FilterFeature(CombatFeature::Health, owner, original, now);
}

float Runtime::FilterExperience(uintptr_t owner, float original, uint64_t now) noexcept {
    return FilterFeature(CombatFeature::Experience, owner, original, now);
}

float Runtime::FilterDamage(uintptr_t owner, float original, bool own_weapon, uint64_t now) noexcept {
    return own_weapon ? FilterFeature(CombatFeature::Damage, owner, original, now) : original;
}

float Runtime::FilterMovement(uintptr_t owner, float original, uint64_t now, bool record_read) noexcept {
    return FilterFeature(CombatFeature::Movement, owner, original, now, record_read);
}

float Runtime::FilterCooldown(uintptr_t owner, float original, bool eligible_basic_weapon, uint64_t now, bool record_read) noexcept {
    return eligible_basic_weapon ? FilterFeature(CombatFeature::Cooldown, owner, original, now, record_read) : original;
}

float Runtime::FilterInstantKill(uintptr_t owner, float incoming, float enemy_hp, bool own_weapon, uint64_t now,
        bool final_damage) noexcept {
    if (!own_weapon || !ReadyOwner(owner, now) || !std::isfinite(incoming) || incoming <= 0
        || !std::isfinite(enemy_hp) || enemy_hp <= 0) return incoming;
    auto& state = combat_[static_cast<size_t>(CombatFeature::InstantKill)];
    bool overridden = FeatureEnabled(CombatFeature::InstantKill, owner, now);
    float output = incoming;
    if (overridden) {
        float lethal = final_damage ? enemy_hp : static_cast<float>(std::min(
            static_cast<double>(std::numeric_limits<float>::max()), static_cast<double>(enemy_hp) * 100.0));
        output = std::max(incoming, lethal);
    }
    state.calls.fetch_add(1);
    if (overridden) state.overrides.fetch_add(1);
    state.io.store(Pack(incoming, output));
    state.io_owner.store(owner);
    return output;
}

void Runtime::RecordFeatureResult(CombatFeature feature, uintptr_t owner, float before, float after,
        int32_t level_before, int32_t level_after, uint64_t now) noexcept {
    size_t index = static_cast<size_t>(feature);
    if (index >= combat_.size() || !ReadyOwner(owner, now) || !std::isfinite(before) || !std::isfinite(after)) return;
    auto& state = combat_[index];
    state.actual_calls.fetch_add(1);
    state.result.store(Pack(before, after));
    state.levels.store(static_cast<uint64_t>(std::bit_cast<uint32_t>(level_before))
        | (static_cast<uint64_t>(std::bit_cast<uint32_t>(level_after)) << 32));
    if (feature != CombatFeature::Experience) state.loss_total.fetch_add(std::max(0.0, static_cast<double>(before) - after));
    state.result_owner.store(owner);
}

void Runtime::ObserveHealthResult(uintptr_t owner, float before, float after, uint64_t now) noexcept {
    RecordFeatureResult(CombatFeature::Health, owner, before, after, 0, 0, now);
}

void Runtime::ObserveExperienceResult(uintptr_t owner, float before, float after, int32_t level_before,
        int32_t level_after, uint64_t now) noexcept {
    RecordFeatureResult(CombatFeature::Experience, owner, before, after, level_before, level_after, now);
}

void Runtime::ObserveDamageResult(uintptr_t owner, float before, float after, bool own_weapon, uint64_t now) noexcept {
    if (own_weapon) RecordFeatureResult(CombatFeature::Damage, owner, before, after, 0, 0, now);
}

void Runtime::ObserveInstantKillResult(uintptr_t owner, float before, float after, bool own_weapon, uint64_t now) noexcept {
    if (own_weapon) RecordFeatureResult(CombatFeature::InstantKill, owner, before, after, 0, 0, now);
}

} // namespace vst
