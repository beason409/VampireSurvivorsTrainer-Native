#pragma once

#include "stat_catalog.hpp"

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vst {

enum class CombatFeature : uint8_t { Health, Experience, Damage, Movement, Cooldown, InstantKill, NoGoldSpend, GameSpeed, SkipChestAnimation, FiveItemChests };

inline constexpr size_t kCombatFeatureCount = 10;

enum class RewardUse : uint8_t { Chest, Upgrade };
struct RewardItemDefinition {
    std::string id, name, english_name, kind = "weapon", upgrade_unsupported_reason;
    bool is_evolution = false, upgrade_allowed = true;
    int max_level = 1;
};
struct ChestRewardSnapshot {
    bool supported = false, enabled = false, upgrade_supported = false, upgrade_enabled = false;
    std::string target_item_id, target_item_name, message, upgrade_message;
    uint64_t processed_count = 0, upgrade_processed_count = 0, upgrade_granted_count = 0;
    std::string last_reward_item_id, last_reward_name, last_upgrade_item_id, last_upgrade_name;
    std::string last_upgrade_granted_item_id, last_upgrade_granted_name;
};

struct FeatureSnapshot {
    bool supported = false, enabled = false;
    double value = 1;
    std::string reason;
    uint64_t calls = 0, override_calls = 0, actual_calls = 0, overflow_skips = 0;
    bool io_known = false, result_known = false;
    float last_input = 0, last_output = 0, result_before = 0, result_after = 0;
    int32_t level_before = 0, level_after = 0;
    double actual_loss_total = 0;
};

struct NativeStatSnapshot {
    bool supported = false, enabled = false, sample_known = false, current_known = false, io_known = false, bound_known = false;
    double value = 0, original = 0, current = 0, bound = 0;
    float last_input = 0, last_output = 0;
    uint64_t game_reads = 0, override_reads = 0;
    NativeStat source = NativeStat::MaxHealth;
    bool last_is_final = false;
    std::string reason;
};

struct GoldWallet {
    uintptr_t options = 0;
    uintptr_t config = 0;
    int32_t adventure = -1;
    bool operator==(const GoldWallet&) const = default;
};

// All times are monotonic milliseconds (GetTickCount64 on Windows).
struct GameObservation {
    bool verified = false;
    bool safe_mode = false;
    bool player_ready = false;
    // A freshly verified natural-death interval for the same live-observed
    // actor. It can retain an already enabled revival budget, never arm one.
    bool revival_pending = false;
    bool paused_known = false;
    bool paused = true;
    uintptr_t player = 0;
    uintptr_t wallet = 0;
    GoldWallet gold_wallet;
    bool gold_known = false;
    bool gold_write_ready = false;
    double gold = 0;
    float armor_original = 0;
    bool health_known = false, experience_known = false, level_known = false;
    float health = 0, experience = 0;
    int32_t level = 0;
    std::array<float, kNativeStatCount> stat_original{};
    std::array<bool, kNativeStatCount> stat_known{};
    std::array<float, kNativeStatCount> stat_current{};
    std::array<bool, kNativeStatCount> stat_current_known{};
    std::string scene;
    std::string error;
};

struct Request {
    std::string id;
    std::string kind = "getState";
    std::string stat;
    std::string feature;
    std::string backup_token;
    std::string item_id;
    int32_t gold_adventure = -1;
    bool has_item_id = false;
    int catalog_offset = 0, catalog_limit = 200;
    bool has_upgrade_enabled = false, upgrade_enabled = false;
    bool has_enabled = false;
    bool enabled = false;
    bool has_value = false;
    double value = 0;
};

struct StateSnapshot {
    bool verified = false;
    bool safe_mode = false;
    bool player_ready = false;
    bool revival_pending = false;
    bool paused_known = false;
    bool paused = true;
    bool backup_authorized = false, wallet_ready = false, gold_known = false, gold_write_supported = false, gold_write_ready = false;
    int32_t gold_adventure = -1;
    double gold = 0;
    bool armor_supported = false;
    bool armor_enabled = false;
    bool armor_sample_known = false;
    bool armor_current_known = false;
    bool armor_actual_read_known = false;
    bool armor_last_was_override = false;
    uint64_t armor_game_reads = 0, armor_override_reads = 0;
    double armor_target = 20;
    float armor_original = 0;
    float armor_current = 0;
    float armor_last_input = 0, armor_last_output = 0;
    bool health_known = false, experience_known = false, level_known = false;
    float health = 0, experience = 0;
    int32_t level = 0;
    std::string scene;
    std::string error;
    std::array<FeatureSnapshot, kCombatFeatureCount> combat;
    std::array<NativeStatSnapshot, kNativeStatCount> stats;
    std::string armor_reason;
    ChestRewardSnapshot chest_reward;
};

struct Response {
    std::string id;
    bool ok = false;
    std::string error;
    StateSnapshot state;
    std::string error_scope;
    bool has_item_catalog = false;
    bool item_catalog_loading = false;
    bool item_icons_supported = false;
    int item_catalog_total = 0;
    std::vector<RewardItemDefinition> item_catalog;
    bool has_item_icon = false;
    std::string item_icon_id, item_icon_rgba_hex;
    int item_icon_width = 0, item_icon_height = 0;
    bool item_icon_pending = false;
};

class Runtime {
public:
    Runtime() noexcept;
    struct Pending;
    using Ticket = std::shared_ptr<Pending>;

    // Set only after the game ABI and both native hooks have been validated.
    void SetArmorSupported(bool supported, std::string reason = {});
    void SetFeatureSupported(CombatFeature feature, bool supported, std::string reason = {});
    void SetStatSupported(NativeStat stat, bool supported, std::string reason = {});
    void SetBackupAuthorization(uint64_t session, bool authorized, uint64_t expires_at = UINT64_MAX);
    using GoldWriter = bool (*)(void* context, const GoldWallet& wallet, double desired, double& actual, std::string& error);
    void SetGoldWriter(GoldWriter writer, void* context);
    using RewardAdapter = bool (*)(void* context, const Request& request, Response& result, std::string& error);
    void SetRewardAdapter(RewardAdapter adapter, void* context);
    void SetRewardSupported(RewardUse use, bool supported, std::string reason = {});
    // Reward callbacks run on the game thread. Returning a target requires the
    // same fresh owner/session checks as the effect hooks.
    std::string RewardTarget(RewardUse use, uintptr_t owner, uint64_t now);
    uint64_t RewardEpoch() const noexcept { return reward_epoch_.load(); }
    bool RewardContextCurrent(uintptr_t owner, uint64_t epoch, uint64_t now);
    void RecordReward(RewardUse use, uintptr_t owner, uint64_t epoch, const std::string& id,
        const std::string& name, bool granted, const std::string& message, uint64_t now);
    void RewardUnavailable(RewardUse use, std::string reason);
    // Observe and Pump run on Unity's main thread. Verified adapters perform
    // wallet saves or bounded reward-data reads/validation outside the lock.
    void Observe(const GameObservation& observation, uint64_t now);
    void Pump(uint64_t now);
    // Called after the original getter, on the owning player's native hook.
    // The latest ready main-thread observation must be no older than 1000 ms.
    // No allocation, locks, Unity calls, or saved/backing-field writes.
    float FilterArmor(uintptr_t owner, float original, uint64_t now) noexcept;
    bool StatEnabled(NativeStat stat, uintptr_t owner, uint64_t now) const noexcept;
    double StatValue(NativeStat stat) const noexcept;
    // A natural resource read reports the already consumed temporary value.
    // This records evidence only; it never creates or refills a resource.
    void ObserveStatRead(NativeStat stat, uintptr_t owner, float original, float current,
        bool overridden, uint64_t now) noexcept;
    float FilterStat(NativeStat stat, uintptr_t owner, float original, uint64_t now, bool record_read = true) noexcept;
    float FilterStatFinal(NativeStat stat, uintptr_t owner, float original, float parameter,
        uint64_t now, bool record_read = true) noexcept;
    bool FeatureEnabled(CombatFeature feature, uintptr_t owner, uint64_t now) const noexcept;
    float ActiveGameSpeed(uintptr_t owner, uint64_t now) const noexcept;
    void RecordGameSpeedWrite(uintptr_t owner, float before, float after, bool override, uint64_t now) noexcept;
    void RecordChestEvent(CombatFeature feature, uintptr_t owner, uint64_t now, int32_t equipment_slots = -1) noexcept;
    float FilterHealthDamage(uintptr_t owner, float original, uint64_t now) noexcept;
    float FilterExperience(uintptr_t owner, float original, uint64_t now) noexcept;
    float FilterDamage(uintptr_t owner, float original, bool own_weapon, uint64_t now) noexcept;
    // Getter filters run only after an actual native getter; diagnostic samples
    // bypass these functions. Weapon ownership and the basic allowlist are
    // established by the game callback before calling FilterCooldown.
    float FilterMovement(uintptr_t owner, float original, uint64_t now, bool record_read = true) noexcept;
    float FilterCooldown(uintptr_t owner, float original, bool eligible_basic_weapon, uint64_t now, bool record_read = true) noexcept;
    // Apply after FilterDamage, then call the native enemy function once with
    // this final argument. Positive finite HP is saturated without HP writes.
    float FilterInstantKill(uintptr_t owner, float incoming, float enemy_hp, bool own_weapon, uint64_t now,
        bool final_damage = false) noexcept;
    float FilterGoldSpend(uintptr_t wallet, float amount, bool main_wallet, uint64_t now) noexcept;
    void ObserveHealthResult(uintptr_t owner, float before, float after, uint64_t now) noexcept;
    void ObserveExperienceResult(uintptr_t owner, float before, float after, int32_t level_before, int32_t level_after, uint64_t now) noexcept;
    void ObserveDamageResult(uintptr_t owner, float before, float after, bool own_weapon, uint64_t now) noexcept;
    void ObserveInstantKillResult(uintptr_t owner, float before, float after, bool own_weapon, uint64_t now) noexcept;

    uint64_t BeginSession(uint64_t now);
    void EndSession(uint64_t session);
    void CheckHeartbeat(uint64_t now);
    StateSnapshot Snapshot(uint64_t now);

    // Reads and disables complete immediately. Enable/target updates are queued.
    Ticket Submit(Request request, uint64_t session, uint64_t now);
    // A timeout cancels under the same lock used by Pump; cancelled work can
    // never apply later. An already-completed command returns its real result.
    Response Await(const Ticket& ticket, uint32_t timeout_ms = 1500);
    bool TryResult(const Ticket& ticket, Response& response);
    void Cancel(const Ticket& ticket);
    void DisableAll();

private:
    mutable std::mutex mutex_;
    GameObservation observed_;
    std::string support_reason_;
    bool supported_ = false;
    bool session_active_ = false;
    bool observation_stale_ = false;
    uint64_t session_ = 0;
    uint64_t generation_ = 0;
    uint64_t last_heartbeat_ = 0;
    double target_ = 20;
    std::deque<Ticket> queue_;

    std::atomic<bool> enabled_{false}, verified_{false}, safe_mode_{false},
        player_ready_{false}, revival_pending_{false}, supported_atomic_{false}, session_atomic_{false};
    std::atomic<uintptr_t> owner_{0};
    std::atomic<uintptr_t> wallet_{0};
    std::atomic<bool> backup_authorized_{false};
    std::atomic<uint64_t> backup_expires_at_{0};
    GoldWriter gold_writer_ = nullptr;
    void* gold_writer_context_ = nullptr;
    RewardAdapter reward_adapter_ = nullptr;
    void* reward_adapter_context_ = nullptr;
    ChestRewardSnapshot rewards_;
    uint64_t reward_revision_ = 0;
    std::atomic<uint64_t> reward_epoch_{1};
    std::atomic<float> target_atomic_{20};
    std::atomic<uint64_t> heartbeat_atomic_{0};
    std::atomic<uint64_t> observation_atomic_{0};
    std::atomic<uint64_t> armor_reads_{0}, armor_override_reads_{0};
    std::atomic<uintptr_t> last_actual_owner_{0};
    std::atomic<uint64_t> last_io_{0};
    std::atomic<bool> last_was_override_{false};

    struct FeatureControl {
        bool supported = false;
        double value = 1;
        std::string reason;
        std::atomic<bool> supported_atomic{false}, enabled{false};
        std::atomic<double> value_atomic{1};
        std::atomic<uint64_t> calls{0}, overrides{0}, actual_calls{0}, overflow_skips{0};
        std::atomic<uint64_t> io{0}, result{0}, levels{0};
        std::atomic<uintptr_t> io_owner{0}, result_owner{0};
        std::atomic<double> loss_total{0};
    };
    std::array<FeatureControl, kCombatFeatureCount> combat_;

    struct StatControl {
        bool supported = false;
        double value = 0;
        std::string reason;
        std::atomic<bool> supported_atomic{false}, enabled{false};
        std::atomic<double> value_atomic{0};
        std::atomic<uint64_t> reads{0}, overrides{0}, io{0}, io_revision{0}, base_io{0}, base_revision{0};
        std::atomic<uintptr_t> io_owner{0}, base_owner{0};
        std::atomic<uint8_t> source{0};
        std::atomic<bool> final_read{false};
    };
    std::array<StatControl, kNativeStatCount> stats_;
    std::atomic<uint64_t> stats_revision_{1};

    void DisableLocked();
    void DisablePlayerEffectsLocked();
    void DisableArmorLocked();
    void DisableStatsLocked(bool preserve_revivals = false);
    bool TargetSupportedLocked(int target) const;
    bool TargetReadyLocked(int target) const;
    std::atomic<bool>& TargetEnabledLocked(int target);
    void DisableTargetLocked(int target);
    void InvalidateTargetQueuedLocked(int target, const char* reason);
    void InvalidateActualObservationsLocked();
    void ExpireLocked(uint64_t now);
    void ExpireObservationLocked(uint64_t now);
    void InvalidateQueuedLocked(const char* reason);
    StateSnapshot SnapshotLocked() const;
    Response ReplyLocked(const Request& request, bool ok, const char* error = nullptr) const;
    void CompleteLocked(const Ticket& ticket, Response response);
    uint64_t NextSessionLocked();
    bool ReadyOwner(uintptr_t owner, uint64_t now) const noexcept;
    bool ReadyStatOwner(NativeStat stat, uintptr_t owner, uint64_t now) const noexcept;
    float FilterFeature(CombatFeature feature, uintptr_t owner, float original, uint64_t now, bool record_read = true) noexcept;
    float FilterStatValue(NativeStat stat, uintptr_t owner, float original, float parameter,
        bool final, uint64_t now, bool record_read) noexcept;
    void RecordFeatureResult(CombatFeature feature, uintptr_t owner, float before, float after,
        int32_t level_before, int32_t level_after, uint64_t now) noexcept;
};

} // namespace vst
