#if GAME_INTEROP
extern alias Il2Cpp;
#endif
#if GAME_INTEROP
using VampireSurvivors;
using VampireSurvivors.Data;
using VampireSurvivors.Data.Weapons;
using VampireSurvivors.Framework;
using VampireSurvivors.Tools;
using VSCharacter = VampireSurvivors.Objects.Characters.CharacterController;
using VSLevelUpPage = VampireSurvivors.UI.LevelUpPage;
using CharacterChoices = Il2Cpp::Il2CppSystem.Collections.Generic.List<VampireSurvivors.Objects.Characters.CharacterController>;
using WeaponChoices = Il2Cpp::Il2CppSystem.Collections.Generic.List<VampireSurvivors.Data.WeaponType>;
#endif

namespace Trainer.Plugin;

internal sealed partial class GameAdapter
{
#if GAME_INTEROP
    [ThreadStatic] private static UpgradeMenuScope? _upgradeMenu;
    [ThreadStatic] private static UpgradePopulateScope? _upgradePopulation;
    private readonly Dictionary<IntPtr, PendingUpgradeOffer> _upgradeOffers = new();
    private IntPtr _upgradeTrackingPlayer, _upgradeTrackingManager;
#endif

    internal void InitializeUpgradeRewardHooks()
    {
#if GAME_INTEROP
        try
        {
            Patch(typeof(VSLevelUpPage), "Populate", typeof(void), Type.EmptyTypes, nameof(UpgradePopulatePrefix),
                finalizer: nameof(UpgradePopulateFinalizer));
            Patch(typeof(LevelUpFactory), "HasPowerupsInStore", typeof(bool), new[] { typeof(VSCharacter) },
                null, nameof(UpgradeNativeAvailabilityPostfix));
            Patch(typeof(VSLevelUpPage), "PickRandomLevelUps", typeof(void), Type.EmptyTypes,
                nameof(UpgradeInitialPrefix), nameof(UpgradeMenuPostfix), finalizer: nameof(UpgradeMenuFinalizer));
            Patch(typeof(LevelUpFactory), "FindFriendshipAmuletTargets", typeof(CharacterChoices), new[] { typeof(bool) },
                null, nameof(UpgradeBeforeInitialEnumerationPostfix));
            Patch(typeof(VSLevelUpPage), "ResetLevelUpViewsAfterReRoll", typeof(void), Type.EmptyTypes,
                nameof(UpgradeRerollPrefix), nameof(UpgradeMenuPostfix), finalizer: nameof(UpgradeMenuFinalizer));
            Patch(typeof(VSLevelUpPage), "SpawnWeapon", typeof(void), new[] { typeof(WeaponData), typeof(WeaponType), typeof(int) },
                null, nameof(UpgradeSpawnWeaponPostfix));
            Patch(typeof(VSLevelUpPage), "SelectWeapon", typeof(void), new[] { typeof(WeaponType), typeof(LevelUpItemUI) },
                nameof(UpgradeSelectionPrefix), nameof(UpgradeSelectionPostfix), finalizer: nameof(UpgradeSelectionFinalizer));
            _runtime.SetUpgradeCapability(true);
            Diagnostics["upgrade.route"] = "one page currentWeapons candidate > native SpawnWeapon/SetWeaponData > native SelectWeapon";
            Diagnostics["upgrade.patch"] = "installed; offline normal/evolved weapon choices and rerolls; native grant only after selection";
        }
        catch (Exception error)
        {
            _runtime.SetUpgradeCapability(false, "升级选项入口无法验证：" + error.Message);
            Diagnostics["upgrade.patch"] = "unavailable: " + error.Message;
            _log.LogError("Upgrade choice hooks unavailable: " + error);
        }
#else
        _runtime.SetUpgradeCapability(false, "需要重新编译 interop 版本。");
#endif
    }

    internal void ValidateUpgradeRewardTarget(string id)
    {
#if GAME_INTEROP
        ValidateChestRewardTarget(id);
        if (_manager is null || _player is null || !TryGetRewardType(id, out var type))
            throw new InvalidOperationException("所选升级物品当前不可用，请刷新列表。");
        var data = _manager.DataManager;
        if (data is null) throw new InvalidOperationException("游戏物品数据尚未就绪。");
        var source = data.GetConvertedWeapons();
        if (source is null || !source.ContainsKey(type) || source[type] is not { Count: > 0 } levels || levels[0] is not { } header)
            throw new InvalidOperationException("所选物品缺少完整升级数据。");
        ValidateEvolutionUpgradeData(type, header, data);
        // HasPowerupsInStore recalculates global weights and can roll a lucky
        // gift. Validation must not call it: observe the game's own call when
        // Populate chooses between ordinary choices and its fallback menu.
#else
        throw new InvalidOperationException("需要重新编译带游戏接口的插件，当前不能指定升级选项。");
#endif
    }

#if GAME_INTEROP
    private void ValidateEvolutionUpgradeData(WeaponType type, WeaponData header, VampireSurvivors.Data.DataManager data)
    {
        if (!header.isEvolution) return;
        if (header.isPowerUp)
            throw new InvalidOperationException("当前仅适配进化武器，尚未适配进化被动装备。");
        var raw = data.AllWeaponData;
        if (raw is null || !raw.ContainsKey(type) || raw[type] is not { Count: > 0 } levels)
            throw new InvalidOperationException("所选进化武器缺少当前升级页面所需的原始等级数据。");
        int nextIndex = GetRewardEquipment(type)?.Level ?? 0;
        if (nextIndex is < 0 or > 10000 || !DataHelper.GetWeaponDataForLevel(levels, nextIndex, out var nextData) || nextData is null)
            throw new InvalidOperationException("游戏无法解析所选进化武器的下一级数据。");
        var factory = _manager?.WeaponsFacade?._weaponFactory;
        if (factory is null) throw new InvalidOperationException("游戏武器预制体尚未就绪，请进入单人关卡后重试。");
        // Native GetWeaponPrefab only searches loaded factories/dictionaries.
        // It does not instantiate equipment, grant rewards, or consume RNG.
        var prefab = factory.GetWeaponPrefab(type, out var actualType);
        if (prefab is null || prefab.Pointer == IntPtr.Zero || actualType != type)
            throw new InvalidOperationException("当前已加载内容没有所选进化武器的有效预制体。");
    }

    private static void UpgradePopulatePrefix(VSLevelUpPage __instance, out UpgradePopulateBoundary __state)
    {
        __state = new UpgradePopulateBoundary(_upgradePopulation);
        _upgradePopulation = null;
        var owner = Active;
        if (owner is not null && __instance is not null)
        {
            owner.ResetUpgradeTrackingForPlayer();
            owner._upgradeOffers.Remove(__instance.Pointer);
        }
        var targetId = owner?._runtime.UpgradeTarget();
        if (owner is null || targetId is null || __instance is null) return;
        try
        {
            if (!owner.IsLocal(__instance.GetCharacterControllingUi())) return;
            owner.ValidateUpgradeRewardTarget(targetId);
            __state.Scope = new UpgradePopulateScope(owner, __instance, targetId);
            _upgradePopulation = __state.Scope;
        }
        catch (Exception error) { owner._runtime.UpgradeRewardUnavailable("本次升级保持原规则：" + error.Message); }
    }

    private static void UpgradeNativeAvailabilityPostfix(LevelUpFactory __instance, VSCharacter __0, bool __result)
    {
        var scope = _upgradePopulation;
        if (scope is null || scope.Observed || scope.Owner != Active || !scope.Owner.IsLocal(__0)
            || scope.Page._levelUpFactory is not { } factory || factory.Pointer != __instance.Pointer
            || scope.Owner._runtime.UpgradeTarget() != scope.TargetId) return;
        scope.Observed = true;
        if (!__result)
            scope.Owner._runtime.UpgradeRewardUnavailable("当前已进入金币、食物或极限突破阶段；本次菜单保持原规则，指定升级选项已关闭。");
    }

    private static Exception? UpgradePopulateFinalizer(Exception? __exception, UpgradePopulateBoundary? __state)
    {
        if (__state is null) return __exception;
        _upgradePopulation = __state.Previous;
        if (__exception is not null && __state.Scope is { } scope)
            scope.Owner._runtime.UpgradeRewardUnavailable("游戏创建升级菜单时发生错误，指定升级选项已关闭。");
        return __exception;
    }

    private static void UpgradeInitialPrefix(VSLevelUpPage __instance, out UpgradeMenuBoundary __state)
        => BeginUpgradeMenu(__instance, false, out __state);

    private static void UpgradeRerollPrefix(VSLevelUpPage __instance, out UpgradeMenuBoundary __state)
    {
        BeginUpgradeMenu(__instance, true, out __state);
        if (__state.Scope is not { } scope) return;
        try { ForceOneUpgradeChoice(scope); }
        catch (Exception error) { FailUpgradeMenu(scope, "无法指定重选选项：" + error.Message); }
    }

    private static void BeginUpgradeMenu(VSLevelUpPage page, bool reroll, out UpgradeMenuBoundary boundary)
    {
        // Every invocation is a boundary, including unsupported nested menus.
        // An inactive nested call must never inherit an outer forced target.
        boundary = new UpgradeMenuBoundary(_upgradeMenu);
        _upgradeMenu = null;
        var owner = Active;
        if (owner is not null && page is not null)
        {
            owner.ResetUpgradeTrackingForPlayer();
            owner._upgradeOffers.Remove(page.Pointer);
        }
        var targetId = owner?._runtime.UpgradeTarget();
        if (owner is null || targetId is null || page is null) return;
        try
        {
            if (!owner.IsLocal(page.GetCharacterControllingUi())) return;
            owner.ValidateUpgradeRewardTarget(targetId);
            if (!owner.TryGetRewardType(targetId, out var target))
                throw new InvalidOperationException("所选升级物品当前不可用。");
            boundary.Scope = new UpgradeMenuScope(owner, page, targetId, target, owner.RewardDisplayName(target), reroll);
            _upgradeMenu = boundary.Scope;
        }
        catch (Exception error) { owner._runtime.UpgradeRewardUnavailable("无法指定升级选项：" + error.Message); }
    }

    private static void UpgradeBeforeInitialEnumerationPostfix(LevelUpFactory __instance, bool __0)
    {
        var scope = _upgradeMenu;
        if (scope is null || scope.Reroll || scope.Attempted || !__0 || scope.Owner != Active
            || scope.Page._levelUpFactory is not { } factory || factory.Pointer != __instance.Pointer) return;
        // Native PickRandomLevelUps has assigned and shuffled currentWeapons.
        // Its next step may remove the last slot for Friendship Amulet, so the
        // chosen target is placed at the first slot before enumeration starts.
        try { ForceOneUpgradeChoice(scope); }
        catch (Exception error) { FailUpgradeMenu(scope, "无法指定升级选项：" + error.Message); }
    }

    private static void ForceOneUpgradeChoice(UpgradeMenuScope scope)
    {
        scope.Attempted = true;
        if (scope.Owner != Active || scope.Owner._runtime.UpgradeTarget() != scope.TargetId)
            throw new InvalidOperationException("指定升级选项已关闭。");
        scope.Owner.ValidateUpgradeRewardTarget(scope.TargetId);
        var choices = scope.Page._currentWeapons;
        if (choices is null || choices.Count is <= 0 or > 32)
            throw new InvalidOperationException("游戏未生成可替换的普通升级选项。");
        var source = scope.Page._weaponData;
        if (source is null || !source.ContainsKey(scope.Target) || source[scope.Target] is not { Count: > 0 } levels || levels[0] is null)
            throw new InvalidOperationException("当前升级页面没有所选物品的数据。");
        var header = levels[0];
        if (scope.Page._data is null) throw new InvalidOperationException("当前升级页面的数据来源尚未就绪。");
        scope.Owner.ValidateEvolutionUpgradeData(scope.Target, header, scope.Page._data);
        scope.Evolution = header.isEvolution;
        scope.BaseTypes = ReadUpgradeBaseTypes(header);
        int existing = -1;
        for (int index = 0; index < choices.Count; index++)
            if (choices[index] == scope.Target)
            {
                if (existing >= 0) throw new InvalidOperationException("游戏生成了重复目标选项，已保留原列表。");
                existing = index;
            }
        // Change at most one candidate. If the game already offered the target,
        // only move it ahead of the optional removal of the final native slot.
        if (existing > 0) choices[existing] = choices[0];
        choices[0] = scope.Target;
        scope.Choices = choices;
        scope.TargetData = levels[0].Pointer;
        scope.Applied = true;
        scope.Owner.Diagnostics["upgrade.lastGeneration"] = "one selectable target queued: " + scope.TargetId;
    }

    private static void UpgradeSpawnWeaponPostfix(VSLevelUpPage __instance, WeaponData __0, WeaponType __1, int __2)
    {
        var scope = _upgradeMenu;
        if (scope is null || scope.Failed || !scope.Applied || scope.Owner != Active || __instance.Pointer != scope.Page.Pointer
            || __1 != scope.Target) return;
        try
        {
            if (__0 is null || __0.Pointer != scope.TargetData)
                throw new InvalidOperationException("升级展示数据与目标物品不一致。");
            var items = __instance.LevelUpItems;
            if (items is null || items.Count == 0) throw new InvalidOperationException("游戏没有创建升级选项按钮。");
            var item = items[items.Count - 1];
            if (item is null || item.GetWeaponType() != scope.Target || item.Index != __2
                || item._data is null || item._data.Pointer != scope.TargetData || item._isLimitBreak)
                throw new InvalidOperationException("升级按钮的实际选择绑定未能核对。");
            scope.TargetButton = item.Pointer;
            scope.Rendered++;
        }
        catch (Exception error) { FailUpgradeMenu(scope, "指定升级选项未完成核对：" + error.Message); }
    }

    private static void UpgradeMenuPostfix(UpgradeMenuBoundary? __state)
    {
        var scope = __state?.Scope;
        if (scope is null || scope.Failed) return;
        try
        {
            var choices = scope.Page._currentWeapons;
            if (!scope.Applied || scope.Rendered != 1 || choices is null || scope.Choices is null
                || choices.Pointer != scope.Choices.Pointer || !choices.Contains(scope.Target))
                throw new InvalidOperationException("游戏没有生成且绑定唯一的指定升级选项。");
            int targetButtons = 0;
            var items = scope.Page.LevelUpItems;
            if (items is not null)
                foreach (var item in items)
                    if (item is not null && !item._isLimitBreak && item.GetWeaponType() == scope.Target) targetButtons++;
            if (targetButtons != 1) throw new InvalidOperationException("指定物品按钮数量与实际选项不一致。");
            if (scope.Owner._player is null || scope.Owner._manager is null)
                throw new InvalidOperationException("角色已退出当前关卡，指定升级选项未确认。");
            if (scope.Owner._upgradeOffers.Count >= 32) scope.Owner._upgradeOffers.Clear();
            scope.Owner._upgradeOffers[scope.Page.Pointer] = new PendingUpgradeOffer(scope.TargetButton,
                scope.Owner._player.Pointer, scope.Owner._manager.Pointer, scope.TargetId, scope.Target, scope.Name,
                scope.Evolution, scope.BaseTypes);
            // This records an offered choice only. Selecting another button,
            // skipping, banishing, or closing does not award the target here.
            scope.Owner._runtime.RecordUpgradeReward(scope.TargetId, scope.Name, "升级选项已包含：" + scope.Name + "；需在游戏内选择后才会获得。");
        }
        catch (Exception error) { FailUpgradeMenu(scope, "指定升级选项未完成核对：" + error.Message); }
    }

    private static Exception? UpgradeMenuFinalizer(Exception? __exception, UpgradeMenuBoundary? __state)
    {
        if (__state is null) return __exception;
        _upgradeMenu = __state.Previous;
        if (__exception is not null && __state.Scope is { } scope)
            FailUpgradeMenu(scope, "游戏生成升级选项时发生错误，指定升级选项已关闭。");
        return __exception;
    }

    private static void FailUpgradeMenu(UpgradeMenuScope scope, string message)
    {
        scope.Failed = true;
        scope.Owner._runtime.UpgradeRewardUnavailable(message);
        scope.Owner.Diagnostics["upgrade.lastGeneration"] = message;
    }

    private static WeaponType[] ReadUpgradeBaseTypes(WeaponData header)
    {
        if (!header.isEvolution || header.evolvesFrom is null) return Array.Empty<WeaponType>();
        if (header.evolvesFrom.Count > 64) throw new InvalidOperationException("进化基底列表异常，指定选项未生成。");
        var bases = new List<WeaponType>();
        foreach (var type in header.evolvesFrom)
            if (type != WeaponType.VOID && !bases.Contains(type)) bases.Add(type);
        return bases.ToArray();
    }

    private void ResetUpgradeTrackingForPlayer()
    {
        IntPtr player = _player?.Pointer ?? IntPtr.Zero, manager = _manager?.Pointer ?? IntPtr.Zero;
        if (player == _upgradeTrackingPlayer && manager == _upgradeTrackingManager) return;
        _upgradeOffers.Clear();
        _upgradeTrackingPlayer = player;
        _upgradeTrackingManager = manager;
    }

    private static void UpgradeSelectionPrefix(VSLevelUpPage __instance, WeaponType __0, LevelUpItemUI __1,
        out UpgradeSelectionObservation? __state)
    {
        __state = null;
        var owner = Active;
        if (owner is null) return;
        try
        {
            if (__instance is null || __1 is null || __instance._isBanishMode || __instance._hasSelected
                || !owner.IsLocal(__instance.GetCharacterControllingUi())) return;
            owner.ResetUpgradeTrackingForPlayer();
            if (!owner._upgradeOffers.TryGetValue(__instance.Pointer, out var offer) || offer.Selecting || offer.Completed) return;
            if (__0 != offer.Target || __1.Pointer != offer.Button || __1.GetWeaponType() != offer.Target)
            {
                // Choosing another normal option ends the offer; it must not
                // award the forced target or count as actual acquisition.
                owner._upgradeOffers.Remove(__instance.Pointer);
                return;
            }
            if (owner._player?.Pointer != offer.Player || owner._manager?.Pointer != offer.Manager) return;
            var before = owner.GetRewardEquipment(offer.Target);
            var baseBefore = new List<string>();
            foreach (var type in offer.BaseTypes)
                if (owner.GetRewardEquipment(type) is not null) baseBefore.Add(type.ToString());
            offer.Selecting = true;
            __state = new UpgradeSelectionObservation(owner, __instance.Pointer, offer,
                before?.Pointer ?? IntPtr.Zero, before?.Level ?? -1, baseBefore.ToArray());
        }
        catch (Exception error) { owner._runtime.UpgradeRewardUnavailable("无法观察指定选项的实际获得结果：" + error.Message); }
    }

    private static void UpgradeSelectionPostfix(UpgradeSelectionObservation? __state)
    {
        if (__state is null) return;
        var owner = __state.Owner;
        var offer = __state.Offer;
        try
        {
            if (owner != Active || owner._player?.Pointer != offer.Player || owner._manager?.Pointer != offer.Manager)
                throw new InvalidOperationException("选择期间角色或关卡已变化。");
            var after = owner.GetRewardEquipment(offer.Target);
            bool granted = after is not null && (__state.Before == IntPtr.Zero || after.Pointer != __state.Before || after.Level > __state.BeforeLevel);
            owner.Diagnostics["upgrade.selection.item"] = offer.TargetId;
            owner.Diagnostics["upgrade.selection.evolution"] = offer.Evolution.ToString();
            owner.Diagnostics["upgrade.selection.beforeLevel"] = __state.BeforeLevel.ToString(System.Globalization.CultureInfo.InvariantCulture);
            owner.Diagnostics["upgrade.selection.afterLevel"] = after?.Level.ToString(System.Globalization.CultureInfo.InvariantCulture) ?? "absent";
            owner.Diagnostics["upgrade.selection.basesBefore"] = string.Join(",", __state.BaseBefore);
            owner.Diagnostics["upgrade.selection.basesAfter"] = string.Join(",", offer.BaseTypes.Where(type => owner.GetRewardEquipment(type) is not null));
            owner.Diagnostics["upgrade.selection.status"] = granted ? "native acquisition/level change confirmed" : "native selection returned without target change";
            if (!granted)
                owner._runtime.UpgradeRewardUnavailable("所选物品未实际获得或升级，已关闭后续升级指定；不会额外补发。");
        }
        catch (Exception error) { owner._runtime.UpgradeRewardUnavailable("无法核对指定选项的实际获得结果：" + error.Message); }
        finally { owner.CompleteUpgradeSelection(__state.Page, offer); }
    }

    private static Exception? UpgradeSelectionFinalizer(Exception? __exception, UpgradeSelectionObservation? __state)
    {
        if (__state is not null)
        {
            __state.Offer.Selecting = false;
            if (__exception is not null)
            {
                __state.Owner.CompleteUpgradeSelection(__state.Page, __state.Offer);
                __state.Owner._runtime.UpgradeRewardUnavailable("游戏处理指定升级选择时发生错误，已关闭后续指定。");
            }
        }
        return __exception;
    }

    private void CompleteUpgradeSelection(IntPtr page, PendingUpgradeOffer offer)
    {
        offer.Completed = true;
        offer.Selecting = false;
        if (_upgradeOffers.TryGetValue(page, out var current) && ReferenceEquals(current, offer)) _upgradeOffers.Remove(page);
    }

    private sealed class UpgradeMenuBoundary(UpgradeMenuScope? previous)
    {
        internal UpgradeMenuScope? Previous { get; } = previous;
        internal UpgradeMenuScope? Scope { get; set; }
    }

    private sealed class UpgradePopulateBoundary(UpgradePopulateScope? previous)
    {
        internal UpgradePopulateScope? Previous { get; } = previous;
        internal UpgradePopulateScope? Scope { get; set; }
    }

    private sealed class UpgradePopulateScope(GameAdapter owner, VSLevelUpPage page, string targetId)
    {
        internal GameAdapter Owner { get; } = owner;
        internal VSLevelUpPage Page { get; } = page;
        internal string TargetId { get; } = targetId;
        internal bool Observed { get; set; }
    }

    private sealed class UpgradeMenuScope(GameAdapter owner, VSLevelUpPage page, string targetId, WeaponType target, string name, bool reroll)
    {
        internal GameAdapter Owner { get; } = owner;
        internal VSLevelUpPage Page { get; } = page;
        internal string TargetId { get; } = targetId;
        internal WeaponType Target { get; } = target;
        internal string Name { get; } = name;
        internal bool Reroll { get; } = reroll;
        internal bool Attempted { get; set; }
        internal bool Applied { get; set; }
        internal bool Failed { get; set; }
        internal int Rendered { get; set; }
        internal WeaponChoices? Choices { get; set; }
        internal IntPtr TargetData { get; set; }
        internal IntPtr TargetButton { get; set; }
        internal bool Evolution { get; set; }
        internal WeaponType[] BaseTypes { get; set; } = Array.Empty<WeaponType>();
    }

    private sealed class PendingUpgradeOffer(IntPtr button, IntPtr player, IntPtr manager,
        string targetId, WeaponType target, string name, bool evolution, WeaponType[] baseTypes)
    {
        internal IntPtr Button { get; } = button;
        internal IntPtr Player { get; } = player;
        internal IntPtr Manager { get; } = manager;
        internal string TargetId { get; } = targetId;
        internal WeaponType Target { get; } = target;
        internal string Name { get; } = name;
        internal bool Evolution { get; } = evolution;
        internal WeaponType[] BaseTypes { get; } = baseTypes;
        internal bool Selecting { get; set; }
        internal bool Completed { get; set; }
    }

    private sealed class UpgradeSelectionObservation(GameAdapter owner, IntPtr page, PendingUpgradeOffer offer,
        IntPtr before, int beforeLevel, string[] baseBefore)
    {
        internal GameAdapter Owner { get; } = owner;
        internal IntPtr Page { get; } = page;
        internal PendingUpgradeOffer Offer { get; } = offer;
        internal IntPtr Before { get; } = before;
        internal int BeforeLevel { get; } = beforeLevel;
        internal string[] BaseBefore { get; } = baseBefore;
    }
#endif
}
