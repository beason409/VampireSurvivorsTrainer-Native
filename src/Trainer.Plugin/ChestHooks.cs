#if GAME_INTEROP
extern alias Il2Cpp;
#endif
using Trainer.Shared;
#if GAME_INTEROP
using VampireSurvivors;
using VampireSurvivors.Data;
using VampireSurvivors.Framework;
using VampireSurvivors.Objects;
using TreasureData = VampireSurvivors.Data.Stage.Treasure;
using VSCharacter = VampireSurvivors.Objects.Characters.CharacterController;
using PrizeList = Il2Cpp::Il2CppSystem.Collections.Generic.List<VampireSurvivors.TreasurePrizeTypePair>;
#endif

namespace Trainer.Plugin;

internal sealed partial class GameAdapter
{
#if GAME_INTEROP
    [ThreadStatic] private static ChestGenerationScope? _chestGeneration;
    [ThreadStatic] private static ForcedChestPairScope? _forcedChestPair;
    private readonly Dictionary<IntPtr, PendingChestReward> _chestRewards = new();
    private IntPtr _chestTrackingPlayer, _chestTrackingManager;
#endif

    internal void InitializeChestHooks()
    {
#if GAME_INTEROP
        try
        {
            Patch(typeof(TreasureFactory), "GenerateNewPrizes", typeof(PrizeList), new[] { typeof(TreasureData) },
                nameof(ChestGenerationPrefix), nameof(ChestGenerationPostfix), finalizer: nameof(ChestGenerationFinalizer));
            Patch(typeof(TreasureFactory), "MakePrizePairFromAvailablePowerUps", typeof(TreasurePrizeTypePair),
                new[] { typeof(PrizeType), typeof(WeaponType), typeof(VSCharacter), typeof(bool) },
                nameof(ChestPairPrefix), nameof(ChestPairPostfix), finalizer: nameof(ChestPairFinalizer));
            Patch(typeof(LevelUpFactory), "PullExisting", typeof(WeaponType), new[] { typeof(WeaponType) }, nameof(ChestPullExistingPrefix));
            Patch(typeof(TreasureData), "ClaimPrizes", typeof(void), new[] { typeof(VSCharacter), typeof(bool) },
                nameof(ChestClaimPrefix), nameof(ChestClaimPostfix), finalizer: nameof(ChestClaimFinalizer));
            _runtime.SetChestCapability(true);
            Diagnostics["chest.route"] = "GenerateNewPrizes scope > one native FIXED pair > ClaimPrizes > LevelWeaponUp";
            Diagnostics["chest.patch"] = "installed; display and native accumulated reward list remain synchronized";
        }
        catch (Exception error)
        {
            _runtime.SetChestCapability(false, "箱子奖励入口无法验证：" + error.Message);
            Diagnostics["chest.patch"] = "unavailable: " + error.Message;
            _log.LogError("Chest reward hooks unavailable: " + error);
        }
#else
        _runtime.SetChestCapability(false, "需要重新编译 interop 版本。");
#endif
    }

#if GAME_INTEROP
    private static void ChestGenerationPrefix(TreasureFactory __instance, TreasureData __0, out ChestGenerationBoundary __state)
    {
        __state = new ChestGenerationBoundary(_chestGeneration, _forcedChestPair);
        _chestGeneration = null;
        _forcedChestPair = null;
        var owner = Active;
        string? targetId = owner?._runtime.ChestTarget();
        if (owner is null || targetId is null || __0 is null || owner._player is null || owner._manager is null) return;
        try
        {
            if (__0.openingPlayer is { } opening && opening.Pointer != owner._player.Pointer) return;
            owner.ResetChestTrackingForPlayer();
            if (owner._chestRewards.TryGetValue(__0.Pointer, out var prior) && prior.Claiming) return;
            // Native SetData stores its supplied Treasure reference directly.
            // A later chest may reuse the same data/template; a new generation
            // supersedes an unclaimed observation instead of suppressing it.
            owner._chestRewards.Remove(__0.Pointer);
            if (owner._chestRewards.Count >= 256)
            {
                owner._runtime.ChestRewardUnavailable("待结算箱子过多，指定奖励已关闭；当前箱子保持原规则。");
                return;
            }
            owner.ValidateChestRewardTarget(targetId);
            if (!owner.TryGetRewardType(targetId, out WeaponType type))
                throw new InvalidOperationException("所选物品当前不可用。");
            __state.Scope = new ChestGenerationScope(owner, __instance.Pointer, __0, targetId, type,
                owner.RewardDisplayName(type));
            _chestGeneration = __state.Scope;
        }
        catch (Exception error) { owner._runtime.ChestRewardUnavailable("无法指定本箱奖励：" + error.Message); }
    }

    private static void ChestGenerationPostfix(ChestGenerationBoundary? __state)
    {
        var scope = __state?.Scope;
        if (scope is null) return;
        try
        {
            if (!scope.Applied)
            {
                scope.Owner.Diagnostics["chest.lastGeneration"] = "no replaceable weapon/passive prize in this chest";
                return;
            }
            var data = scope.Treasure;
            if (data.prizes is null || data.accumulatedWeaponPrizes is null || !data.accumulatedWeaponPrizes.Contains(scope.Target))
                throw new InvalidOperationException("游戏未将指定物品加入实际结算列表。");
            scope.Owner._chestRewards[data.Pointer] = new PendingChestReward(data, data.prizes, scope.TargetId, scope.Target, scope.Name);
            scope.Owner.Diagnostics["chest.lastGeneration"] = "one fixed prize queued for native claim: " + scope.TargetId;
        }
        catch (Exception error) { scope.Owner._runtime.ChestRewardUnavailable("箱子奖励未完成核对：" + error.Message); }
    }

    private static Exception? ChestGenerationFinalizer(Exception? __exception, ChestGenerationBoundary? __state)
    {
        if (__state is null) return __exception;
        _chestGeneration = __state.PreviousGeneration;
        _forcedChestPair = __state.PreviousPair;
        if (__exception is not null && __state.Scope is { } scope)
            scope.Owner._runtime.ChestRewardUnavailable("游戏生成奖励时发生错误，指定奖励已关闭。");
        return __exception;
    }

    private static void ChestPairPrefix(TreasureFactory __instance, ref PrizeType __0, ref WeaponType __1,
        VSCharacter __2, out ChestPairBoundary __state)
    {
        __state = new ChestPairBoundary(_forcedChestPair);
        _forcedChestPair = null;
        var generation = _chestGeneration;
        if (generation is null || generation.Attempted || generation.Factory != __instance.Pointer
            || generation.Owner != Active || __2 is null || generation.Owner._player is null
            || __2.Pointer != generation.Owner._player.Pointer
            || generation.Owner._runtime.ChestTarget() != generation.TargetId) return;
        // Arcana and Survarot cards are not random weapon/passive rewards.
        if (__0 is PrizeType.EVO_ARCANA or PrizeType.SURVAROT) return;
        generation.Attempted = true;
        __state.Pair = new ForcedChestPairScope(generation);
        _forcedChestPair = __state.Pair;
        __0 = PrizeType.FIXED;
        __1 = generation.Target;
    }

    private static bool ChestPullExistingPrefix(WeaponType __0, ref WeaponType __result)
    {
        var pair = _forcedChestPair;
        if (pair is null || pair.Generation.Owner != Active || __0 != pair.Generation.Target) return true;
        // Native FIXED falls back to a random existing weapon when an unowned
        // target is absent from its store. Only this one scoped lookup differs.
        __result = pair.Generation.Target;
        return false;
    }

    private static void ChestPairPostfix(TreasurePrizeTypePair __result, ChestPairBoundary? __state)
    {
        if (__state?.Pair is not { } pair) return;
        if (__result is not null && __result.prizeType == PrizeType.FIXED && __result.prizeWeapon == pair.Generation.Target)
            pair.Generation.Applied = true;
        else pair.Generation.Owner._runtime.ChestRewardUnavailable("游戏没有生成所选奖励，指定奖励已关闭。");
    }

    private static Exception? ChestPairFinalizer(Exception? __exception, ChestPairBoundary? __state)
    {
        if (__state is not null) _forcedChestPair = __state.Previous;
        return __exception;
    }

    private static void ChestClaimPrefix(TreasureData __instance, VSCharacter __0, out ChestClaimObservation? __state)
    {
        __state = null;
        var owner = Active;
        if (owner?._player is null || __0 is null || __0.Pointer != owner._player.Pointer
            || !owner._chestRewards.TryGetValue(__instance.Pointer, out var pending) || pending.Completed || pending.Claiming
            || __instance.prizes is null || __instance.prizes.Pointer != pending.Prizes.Pointer) return;
        try
        {
            var before = owner.GetRewardEquipment(pending.Target);
            pending.Claiming = true;
            __state = new ChestClaimObservation(owner, pending, before?.Pointer ?? IntPtr.Zero, before?.Level ?? -1);
        }
        catch (Exception error) { owner._runtime.ChestRewardUnavailable("无法观察指定奖励的结算：" + error.Message); }
    }

    private static void ChestClaimPostfix(ChestClaimObservation? __state)
    {
        if (__state is null) return;
        var owner = __state.Owner;
        var pending = __state.Reward;
        try
        {
            var after = owner.GetRewardEquipment(pending.Target);
            bool granted = after is not null && (__state.Before == IntPtr.Zero || after.Pointer != __state.Before || after.Level > __state.BeforeLevel);
            if (granted)
            {
                owner._runtime.RecordChestReward(pending.TargetId, pending.Name,
                    __state.Before == IntPtr.Zero ? "已从箱子获得：" + pending.Name : "箱子已升级：" + pending.Name);
                owner.Diagnostics["chest.lastClaim"] = "native acquisition/level change confirmed: " + pending.TargetId;
            }
            else
            {
                owner._runtime.ChestRewardUnavailable("所选物品未获得或未升级（可能已满级）；指定奖励已关闭，请更换目标。");
                owner.Diagnostics["chest.lastClaim"] = "native claim returned without a target equipment change";
            }
        }
        catch (Exception error) { owner._runtime.ChestRewardUnavailable("无法核对指定奖励结果：" + error.Message); }
        finally { owner.CompleteChestReward(pending); }
    }

    private static Exception? ChestClaimFinalizer(Exception? __exception, ChestClaimObservation? __state)
    {
        if (__state is not null)
        {
            __state.Reward.Claiming = false;
            if (__exception is not null)
            {
                __state.Owner.CompleteChestReward(__state.Reward);
                __state.Owner._runtime.ChestRewardUnavailable("游戏结算箱子时发生错误，指定奖励已关闭。");
            }
        }
        return __exception;
    }

    private void ResetChestTrackingForPlayer()
    {
        IntPtr player = _player?.Pointer ?? IntPtr.Zero, manager = _manager?.Pointer ?? IntPtr.Zero;
        if (player == _chestTrackingPlayer && manager == _chestTrackingManager) return;
        _chestRewards.Clear();
        _chestTrackingPlayer = player; _chestTrackingManager = manager;
    }

    private void CompleteChestReward(PendingChestReward reward)
    {
        if (reward.Completed) return;
        reward.Completed = true;
        reward.Claiming = false;
        if (_chestRewards.TryGetValue(reward.Treasure.Pointer, out var current) && ReferenceEquals(current, reward))
            _chestRewards.Remove(reward.Treasure.Pointer);
    }

    private sealed class ChestGenerationScope(GameAdapter owner, IntPtr factory, TreasureData treasure,
        string targetId, WeaponType target, string name)
    {
        internal readonly GameAdapter Owner = owner;
        internal readonly IntPtr Factory = factory;
        internal readonly TreasureData Treasure = treasure;
        internal readonly string TargetId = targetId, Name = name;
        internal readonly WeaponType Target = target;
        internal bool Attempted, Applied;
    }

    private sealed class ForcedChestPairScope(ChestGenerationScope generation)
    {
        internal readonly ChestGenerationScope Generation = generation;
    }

    private sealed class ChestGenerationBoundary(ChestGenerationScope? previousGeneration, ForcedChestPairScope? previousPair)
    {
        internal readonly ChestGenerationScope? PreviousGeneration = previousGeneration;
        internal readonly ForcedChestPairScope? PreviousPair = previousPair;
        internal ChestGenerationScope? Scope;
    }

    private sealed class ChestPairBoundary(ForcedChestPairScope? previous)
    {
        internal readonly ForcedChestPairScope? Previous = previous;
        internal ForcedChestPairScope? Pair;
    }

    private sealed class PendingChestReward(TreasureData treasure, PrizeList prizes, string targetId, WeaponType target, string name)
    {
        internal readonly TreasureData Treasure = treasure;
        internal readonly PrizeList Prizes = prizes;
        internal readonly string TargetId = targetId, Name = name;
        internal readonly WeaponType Target = target;
        internal bool Claiming, Completed;
    }

    private sealed class ChestClaimObservation(GameAdapter owner, PendingChestReward reward, IntPtr before, int beforeLevel)
    {
        internal readonly GameAdapter Owner = owner;
        internal readonly PendingChestReward Reward = reward;
        internal readonly IntPtr Before = before;
        internal readonly int BeforeLevel = beforeLevel;
    }
#endif
}
