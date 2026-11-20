using System.Globalization;
using System.Reflection;
using HarmonyLib;
using Trainer.Shared;
#if GAME_INTEROP
using VampireSurvivors.Objects;
using VampireSurvivors.Framework.NumberTypes;
using VSCharacter = VampireSurvivors.Objects.Characters.CharacterController;
#endif

namespace Trainer.Plugin;

internal sealed partial class GameAdapter
{
    [ThreadStatic] private static bool _readingOriginalStat;
    private readonly HashSet<string> _installedStats = new(StringComparer.Ordinal);
    private readonly Dictionary<string, double> _lastStatSettings = new(StringComparer.Ordinal);
    private IntPtr _statPlayer;
#if GAME_INTEROP
    private PlayerModifierStats? _resourceOwner;
    private TemporaryStatResource<EggFloat>? _temporaryRerolls;
    private TemporaryStatResource<EggFloat>? _temporarySkips;
    private TemporaryStatResource<EggFloat>? _temporaryBanishes;
    private TemporaryStatResource<EggDouble>? _temporaryRevivals;
    private TemporaryStatResource<int>? _temporaryUsedRevivals;
    private TemporaryStatResource<float>? _temporaryPreserve;
    private TemporaryStatResource<float>? _temporaryDefang;
    private int _resourceNativeDepth;
    private bool _resourceHooksReady;
    private MagnetZone? _magnetOwner;
    private TemporaryStatResource<EggFloat>? _temporaryMagnet;
    private int _magnetNativeDepth;
    private bool _magnetHooksReady;
#endif

#if GAME_INTEROP
    private static readonly Dictionary<string, string> CharacterStatMethods = new(StringComparer.Ordinal)
    {
        ["MaxHp"] = StatIds.MaxHealth,
        ["DisplayMaxHp"] = StatIds.MaxHealth,
        ["PArmor"] = StatIds.Armor,
        ["PRegen"] = StatIds.Recovery,
        ["PPower"] = StatIds.Might,
        ["PPowerFinal"] = StatIds.Might,
        ["PPowerWithoutSilentMight"] = StatIds.Might,
        ["PSpeed"] = StatIds.ProjectileSpeed,
        ["PDuration"] = StatIds.Duration,
        ["PArea"] = StatIds.Area,
        ["PCooldown"] = StatIds.Cooldown,
        ["PAmount"] = StatIds.Amount,
        ["PMoveSpeed"] = StatIds.MoveSpeed,
        ["PLuck"] = StatIds.Luck,
        ["PGrowth"] = StatIds.Growth,
        ["PGreed"] = StatIds.Greed,
        ["PCurse"] = StatIds.Curse
    };

    private static bool IsPercentageStat(string id) => id == StatIds.Might || id == StatIds.ProjectileSpeed
        || id == StatIds.Duration || id == StatIds.Area || id == StatIds.MoveSpeed
        || id == StatIds.Luck || id == StatIds.Growth || id == StatIds.Greed || id == StatIds.Curse;

    private static bool IsOmniStat(string id) => id == StatIds.Might || id == StatIds.ProjectileSpeed
        || id == StatIds.Duration || id == StatIds.Area;
#endif

    internal void InitializeStatHooks()
    {
#if GAME_INTEROP
        foreach (var group in CharacterStatMethods.Where(pair => pair.Value != StatIds.MoveSpeed).GroupBy(pair => pair.Value))
        {
            string id = group.Key;
            TryInstallStat(id, () =>
            {
                foreach (var entry in group)
                    Patch(typeof(VSCharacter), entry.Key, typeof(float), Type.EmptyTypes, null, nameof(CharacterStatPostfix));
                if (id == StatIds.Cooldown)
                    Patch(typeof(VSCharacter), "PCooldownFinal", typeof(float), new[] { typeof(float) }, null, nameof(CooldownStatFinalPostfix));
                if (id == StatIds.Area)
                    Patch(typeof(VSCharacter), "PAreaFinal", typeof(float), new[] { typeof(float) }, null, nameof(AreaStatFinalPostfix));
            }, string.Join("; ", group.Select(entry => "CharacterController." + entry.Key + "()")));
        }

        // Movement uses the already installed local-player hook. Its caller
        // applies this exact stat first, followed by the existing multiplier.
        bool movementReady = Diagnostics.TryGetValue(FeatureIds.Movement + ".patch", out string? movementPatch)
            && movementPatch == "installed";
        _runtime.SetStatCapability(StatIds.MoveSpeed, movementReady,
            movementReady ? null : "当前版本的角色移动速度读取入口未就绪。");
        if (movementReady) _installedStats.Add(StatIds.MoveSpeed);
        Diagnostics[StatKey(StatIds.MoveSpeed, "route")] = "CharacterController.PMoveSpeed; absolute stat then movement multiplier";

        // Native radius consumers inline EggFloat reads. Bind a separate runtime
        // object instead, preserving the real radius through normal upgrades.
        InitializeMagnetHooks();
        InitializeResourceHooks();

        TryInstallStat(StatIds.Charm, () =>
            Patch(typeof(PlayerModifierStats), "get_Charm", typeof(int), Type.EmptyTypes, null, nameof(CharmReadPostfix)),
            "PlayerModifierStats.get_Charm; local stats only; stage recalculated on change");

        UnsupportedStat(StatIds.Seal, "封印属于局外收藏选择，本局没有对应的可恢复角色属性；此处不会修改存档。");
        bool omniReady = new[] { StatIds.Might, StatIds.ProjectileSpeed, StatIds.Duration, StatIds.Area }
            .All(id => _installedStats.Contains(id));
        _runtime.SetStatCapability(StatIds.Omni, omniReady, omniReady ? null : "全能需要力量、速度、持续时间、范围四个读取入口全部可用。");
        if (omniReady) _installedStats.Add(StatIds.Omni);
        Diagnostics[StatKey(StatIds.Omni, "route")] = "Set the same target might/speed/duration/area percentage; individual override has priority";
        Diagnostics["stats.validation"] = "Typed metadata hooks installed; game-read counters exclude sampling; native inlining and gameplay effects require live validation.";
#else
        foreach (string id in new[] { StatIds.MaxHealth, StatIds.Armor, StatIds.Recovery, StatIds.Might,
            StatIds.ProjectileSpeed, StatIds.Duration, StatIds.Area, StatIds.Cooldown, StatIds.Amount,
            StatIds.MoveSpeed, StatIds.Magnet, StatIds.Luck, StatIds.Growth, StatIds.Greed, StatIds.Curse,
            StatIds.Revivals, StatIds.Rerolls, StatIds.Skips, StatIds.Banishes, StatIds.Charm, StatIds.Defang,
            StatIds.Preserve, StatIds.Omni, StatIds.Seal })
            _runtime.SetStatCapability(id, false, "需要重新编译当前游戏 interop 版本。");
#endif
    }

    internal void ApplyFloatStat(string id, ref float value)
    {
#if GAME_INTEROP
        if (_readingOriginalStat || !float.IsFinite(value)) return;
        double before = value;
        double edited;
        string? source = null;
        if (_runtime.StatEnabled(id))
        {
            double requested = _runtime.StatValue(id);
            edited = id == StatIds.Cooldown ? 1 - requested / 100d
                : IsPercentageStat(id) ? 1 + requested / 100d : requested;
            source = id;
        }
        else if (IsOmniStat(id) && _runtime.StatEnabled(StatIds.Omni))
        {
            edited = 1 + _runtime.StatValue(StatIds.Omni) / 100d;
            source = StatIds.Omni;
        }
        else return;
        if (!double.IsFinite(edited) || edited > float.MaxValue || edited < -float.MaxValue) return;
        value = (float)edited;
        RecordStatRead(id, before, value, source);
#endif
    }

    internal void SampleStats()
    {
#if GAME_INTEROP
        if (!PlayerReady || !SafeMode || _player == null)
        {
            foreach (string id in _installedStats) _runtime.SetStatSample(id, null, null);
            return;
        }
        SampleStat(StatIds.MaxHealth, () => _player.MaxHp());
        SampleStat(StatIds.Armor, () => _player.PArmor());
        SampleStat(StatIds.Recovery, () => _player.PRegen());
        SampleStat(StatIds.Might, () => _player.PPower());
        SampleStat(StatIds.ProjectileSpeed, () => _player.PSpeed());
        SampleStat(StatIds.Duration, () => _player.PDuration());
        SampleStat(StatIds.Area, () => _player.PArea());
        SampleStat(StatIds.Cooldown, () => _player.PCooldown());
        SampleStat(StatIds.Amount, () => _player.PAmount());
        SampleStat(StatIds.MoveSpeed, () => _player.PMoveSpeed());
        SampleStat(StatIds.Luck, () => _player.PLuck());
        SampleStat(StatIds.Growth, () => _player.PGrowth());
        SampleStat(StatIds.Greed, () => _player.PGreed());
        SampleStat(StatIds.Curse, () => _player.PCurse());
        var stats = _player.PlayerStats;
        if (stats != null)
        {
            SampleResourceStat(StatIds.Revivals, () => Math.Max(0, (double)stats._Revivals_k__BackingField - stats._UsedRevivals_k__BackingField));
            SampleResourceStat(StatIds.Rerolls, () => (float)stats._ReRolls_k__BackingField);
            SampleResourceStat(StatIds.Skips, () => (float)stats._Skips_k__BackingField);
            SampleResourceStat(StatIds.Banishes, () => (float)stats._Banish_k__BackingField);
            SampleResourceStat(StatIds.Preserve, () => stats._Recycle_k__BackingField * 100d);
            SampleResourceStat(StatIds.Defang, () => stats._Defang_k__BackingField * 100d);
            SampleStat(StatIds.Charm, () => stats.Charm);
        }
        else
            foreach (string id in new[] { StatIds.Revivals, StatIds.Rerolls, StatIds.Skips, StatIds.Banishes, StatIds.Preserve, StatIds.Defang, StatIds.Charm })
                _runtime.SetStatSample(id, null, null);
        SampleMagnet();
        _runtime.SetStatSample(StatIds.Omni, null, null);
#endif
    }

    internal void TickStats()
    {
#if GAME_INTEROP
        if (_player == null || !PlayerReady || !SafeMode)
        {
            RestoreStatOverrides();
            _statPlayer = IntPtr.Zero;
            _lastStatSettings.Clear();
            return;
        }
        bool playerChanged = _statPlayer != _player.Pointer;
        if (playerChanged) { RestoreStatOverrides(); _statPlayer = _player.Pointer; _lastStatSettings.Clear(); }
        TickResourceOverrides();
        TickMagnetOverride();
        bool curseChanged = StatSettingChanged(StatIds.Curse, playerChanged);
        bool charmChanged = StatSettingChanged(StatIds.Charm, playerChanged);
        if (curseChanged || charmChanged)
            if (!RefreshTransientStat("stage", () => _manager?.Stage?.RecalculateCurseAndCharm()))
            {
                _lastStatSettings.Remove(StatIds.Curse);
                _lastStatSettings.Remove(StatIds.Charm);
            }

        // Reducing or disabling max health must not leave current HP above the
        // newly restored native maximum. This never heals the player on enable.
        if (StatSettingChanged(StatIds.MaxHealth, playerChanged))
        {
            bool priorSampling = _sampling;
            _sampling = true;
            try
            {
                float max = _player.MaxHp();
                float hp = _player.CurrentHp;
                if (float.IsFinite(max) && max > 0 && float.IsFinite(hp) && hp > max)
                {
                    _player.CurrentHp = max;
                    Diagnostics[StatKey(StatIds.MaxHealth, "hpClamp")] = F(hp) + " -> " + F(max);
                }
            }
            catch (Exception e)
            {
                Diagnostics[StatKey(StatIds.MaxHealth, "refreshError")] = e.Message;
                _lastStatSettings.Remove(StatIds.MaxHealth);
            }
            finally { _sampling = priorSampling; }
        }
#endif
    }

    private static string StatKey(string id, string suffix) => "stat." + id + "." + suffix;

    internal void RestoreStatOverrides()
    {
#if GAME_INTEROP
        bool restored = RestoreMagnetOverride();
        void Restore<T>(TemporaryStatResource<T>? resource)
        {
            try { resource?.Restore(); }
            catch (Exception e) { restored = false; Diagnostics["stats.resourceRestoreError"] = e.Message; }
        }
        Restore(_temporaryRerolls);
        Restore(_temporarySkips);
        Restore(_temporaryBanishes);
        Restore(_temporaryRevivals);
        Restore(_temporaryUsedRevivals);
        Restore(_temporaryPreserve);
        Restore(_temporaryDefang);
        if (!restored) return; // Retain owner and references for a later retry.
        _resourceOwner = null;
        _temporaryRerolls = null;
        _temporarySkips = null;
        _temporaryBanishes = null;
        _temporaryRevivals = null;
        _temporaryUsedRevivals = null;
        _temporaryPreserve = null;
        _temporaryDefang = null;
        _resourceNativeDepth = 0;
        Diagnostics.Remove("stats.resourceRestoreError");
#endif
    }

#if GAME_INTEROP
    private static readonly string[] ResourceStatIds = { StatIds.Rerolls, StatIds.Skips, StatIds.Banishes, StatIds.Revivals, StatIds.Preserve, StatIds.Defang };

    private void InitializeResourceHooks()
    {
        try
        {
            // All native normal-stat mutations run against the preserved objects.
            // The finalizer resumes temporary objects on success and exceptions.
            foreach (var entry in new[]
            {
                (Name: "Upgrade", Parameters: new[] { typeof(ModifierStats), typeof(bool) }),
                (Name: "Set", Parameters: new[] { typeof(ModifierStats) }),
                (Name: "ResetStats", Parameters: Type.EmptyTypes)
            })
            {
                var method = typeof(PlayerModifierStats).GetMethod(entry.Name, entry.Parameters)
                    ?? throw new MissingMethodException(typeof(PlayerModifierStats).FullName, entry.Name);
                if (method.ReturnType != typeof(void) || method.IsStatic) throw new MissingMethodException(entry.Name + " signature changed.");
                HarmonyMethod Hook(string name) => new(typeof(GameAdapter).GetMethod(name, BindingFlags.NonPublic | BindingFlags.Static)!);
                _native.DescribeMethod(method, "beforePatch");
                _harmony.Patch(method, prefix: Hook(nameof(ResourceMutationPrefix)), finalizer: Hook(nameof(ResourceMutationFinalizer)));
                _native.DescribeMethod(method, "afterPatch");
            }
            foreach (string name in new[] { "get_ReRolls", "get_Skips", "get_Banish" })
                Patch(typeof(PlayerModifierStats), name, typeof(EggFloat), Type.EmptyTypes, null, nameof(ResourceReadPostfix));
            Patch(typeof(VSCharacter), "PRevivals", typeof(EggDouble), Type.EmptyTypes, null, nameof(RevivalReadPostfix));
            _resourceHooksReady = true;
            foreach (string id in ResourceStatIds)
            {
                _installedStats.Add(id);
                _runtime.SetStatCapability(id, true);
                Diagnostics[StatKey(id, "route")] = "Local runtime Egg-object budget; original references isolated; native Upgrade/Set/ResetStats suspension + finalizer";
                Diagnostics[StatKey(id, "patch")] = "installed; independent temporary consumable budget; live validation pending";
            }
            Diagnostics[StatKey(StatIds.Preserve, "route")] = "Local PlayerModifierStats.<Recycle>k__BackingField; chance percent / 100; native OnReRollLevelUp/OnLevelUpBanish/OnLevelUpSkipped compare UnityRandom.value; original float isolated across Upgrade/Set/ResetStats";
            Diagnostics[StatKey(StatIds.Defang, "route")] = "Local PlayerModifierStats.<Defang>k__BackingField; chance percent / 100; native EnemyController.OnRecycleEnemy compares with precomputed shuffled 0..0.999 sample before DoDefang; affects subsequent enemy generation; original float isolated across Upgrade/Set/ResetStats";
        }
        catch (Exception e)
        {
            _resourceHooksReady = false;
            foreach (string id in ResourceStatIds) UnsupportedStat(id, "本局次数隔离与恢复入口不完整：" + e.Message);
        }
    }

    private void TickResourceOverrides()
    {
        if (!_resourceHooksReady || _resourceNativeDepth != 0) return;
        var owner = _player?.PlayerStats;
        if (owner == null) { RestoreStatOverrides(); return; }
        if (_resourceOwner == null || _resourceOwner.Pointer != owner.Pointer)
        {
            RestoreStatOverrides();
            if (_resourceOwner != null) return; // Failed restoration must retain its old owner.
            _resourceOwner = owner;
            _temporaryRerolls = new(() => owner._ReRolls_k__BackingField, value => owner._ReRolls_k__BackingField = value,
                target => new EggFloat((float)target, 0));
            _temporarySkips = new(() => owner._Skips_k__BackingField, value => owner._Skips_k__BackingField = value,
                target => new EggFloat((float)target, 0));
            _temporaryBanishes = new(() => owner._Banish_k__BackingField, value => owner._Banish_k__BackingField = value,
                target => new EggFloat((float)target, 0));
            _temporaryRevivals = new(() => owner._Revivals_k__BackingField, value => owner._Revivals_k__BackingField = value,
                target => new EggDouble(target, 0));
            _temporaryUsedRevivals = new(() => owner._UsedRevivals_k__BackingField, value => owner._UsedRevivals_k__BackingField = value,
                _ => 0);
            _temporaryPreserve = new(() => owner._Recycle_k__BackingField, value => owner._Recycle_k__BackingField = value,
                target => (float)(target / 100d));
            _temporaryDefang = new(() => owner._Defang_k__BackingField, value => owner._Defang_k__BackingField = value,
                target => (float)(target / 100d));
        }
        ConfigureResource(StatIds.Rerolls, _temporaryRerolls!);
        ConfigureResource(StatIds.Skips, _temporarySkips!);
        ConfigureResource(StatIds.Banishes, _temporaryBanishes!);
        ConfigureResource(StatIds.Preserve, _temporaryPreserve!);
        ConfigureResource(StatIds.Defang, _temporaryDefang!);
        try
        {
            if (_runtime.StatEnabled(StatIds.Revivals))
            {
                double requested = _runtime.StatValue(StatIds.Revivals);
                bool resetUsed = !_temporaryRevivals!.Active || !_temporaryRevivals.Target.Equals(requested);
                _temporaryRevivals.Configure(requested);
                _temporaryUsedRevivals!.Configure(0, resetUsed);
            }
            else { _temporaryRevivals!.Restore(); _temporaryUsedRevivals!.Restore(); }
        }
        catch (Exception e)
        {
            UnsupportedStat(StatIds.Revivals, "临时复活次数无法安全绑定：" + e.Message);
            RestoreStatOverrides();
        }
    }

    private void ConfigureResource<T>(string id, TemporaryStatResource<T> resource)
    {
        try
        {
            if (_runtime.StatEnabled(id)) resource.Configure(_runtime.StatValue(id));
            else resource.Restore();
        }
        catch (Exception e)
        {
            UnsupportedStat(id, "临时次数无法安全绑定：" + e.Message);
            try { resource.Restore(); }
            catch (Exception restoreError) { Diagnostics["stats.resourceRestoreError"] = restoreError.Message; }
        }
    }

    private void EnterResourceNativeScope()
    {
        if (_resourceNativeDepth++ != 0) return;
        try
        {
            _temporaryRerolls?.Suspend();
            _temporarySkips?.Suspend();
            _temporaryBanishes?.Suspend();
            _temporaryRevivals?.Suspend();
            _temporaryUsedRevivals?.Suspend();
            _temporaryPreserve?.Suspend();
            _temporaryDefang?.Suspend();
        }
        catch
        {
            RestoreStatOverrides();
            throw;
        }
    }

    private void ExitResourceNativeScope()
    {
        if (_resourceNativeDepth <= 0 || --_resourceNativeDepth != 0) return;
        try
        {
            _temporaryRerolls?.Resume();
            _temporarySkips?.Resume();
            _temporaryBanishes?.Resume();
            _temporaryRevivals?.Resume();
            _temporaryUsedRevivals?.Resume();
            _temporaryPreserve?.Resume();
            _temporaryDefang?.Resume();
        }
        catch (Exception e)
        {
            foreach (string id in ResourceStatIds) UnsupportedStat(id, "正常升级后临时次数无法安全恢复：" + e.Message);
            RestoreStatOverrides();
        }
    }


    private static void ResourceMutationPrefix(PlayerModifierStats __instance, out GameAdapter? __state)
    {
        __state = null;
        var adapter = Active;
        if (adapter?._resourceOwner == null || adapter._resourceOwner.Pointer != __instance.Pointer) return;
        __state = adapter;
        try { adapter.EnterResourceNativeScope(); }
        catch (Exception e)
        {
            foreach (string id in ResourceStatIds) adapter.UnsupportedStat(id, "正常升级前无法隔离临时次数：" + e.Message);
        }
    }

    private static Exception? ResourceMutationFinalizer(Exception? __exception, GameAdapter? __state)
    {
        __state?.ExitResourceNativeScope();
        return __exception;
    }

    private static void ResourceReadPostfix(PlayerModifierStats __instance, MethodBase __originalMethod, EggFloat? __result)
    {
        var adapter = Active;
        if (adapter == null || _sampling || adapter._resourceNativeDepth != 0 || __result == null
            || adapter._resourceOwner?.Pointer != __instance.Pointer) return;
        string? id = __originalMethod.Name switch
        { "get_ReRolls" => StatIds.Rerolls, "get_Skips" => StatIds.Skips, "get_Banish" => StatIds.Banishes, _ => null };
        if (id == null || !adapter._runtime.StatEnabled(id)) return;
        adapter.Increment(StatKey(id, "gameReads"));
        adapter.Diagnostics[StatKey(id, "gameRemaining")] = F(__result);
    }

    private static void RevivalReadPostfix(VSCharacter __instance, EggDouble? __result)
    {
        var adapter = Active;
        if (adapter == null || _sampling || adapter._resourceNativeDepth != 0 || __result == null
            || !adapter.IsLocal(__instance) || !adapter._runtime.StatEnabled(StatIds.Revivals)) return;
        adapter.Increment(StatKey(StatIds.Revivals, "gameReads"));
        adapter.Diagnostics[StatKey(StatIds.Revivals, "gameTotal")] = ((double)__result).ToString("R", CultureInfo.InvariantCulture);
        adapter.Diagnostics[StatKey(StatIds.Revivals, "gameUsed")] = (__instance.PlayerStats?._UsedRevivals_k__BackingField ?? 0)
            .ToString(CultureInfo.InvariantCulture);
    }

    private void SampleResourceStat(string id, Func<double> read)
    {
        if (!_installedStats.Contains(id)) return;
        bool priorSampling = _sampling;
        _sampling = true;
        try
        {
            double original;
            EnterResourceNativeScope();
            try { original = read(); }
            finally { ExitResourceNativeScope(); }
            double current = read();
            _runtime.SetStatSample(id, double.IsFinite(original) ? original : null, double.IsFinite(current) ? current : null);
            Diagnostics.Remove(StatKey(id, "sampleError"));
        }
        catch (Exception e)
        {
            _runtime.SetStatSample(id, null, null);
            Diagnostics[StatKey(id, "sampleError")] = e.Message;
        }
        finally { _sampling = priorSampling; }
    }

    private void TryInstallStat(string id, Action install, string route)
    {
        try
        {
            install();
            _installedStats.Add(id);
            _runtime.SetStatCapability(id, true);
            Diagnostics[StatKey(id, "route")] = route;
            Diagnostics[StatKey(id, "patch")] = "installed; gameplay reads not yet observed";
        }
        catch (Exception e)
        {
            UnsupportedStat(id, "当前版本的属性读取入口不可用：" + e.Message);
            _log.LogWarning("Stat hook " + id + " unavailable: " + e);
        }
    }

    private void UnsupportedStat(string id, string reason)
    {
        _runtime.SetStatCapability(id, false, reason);
        _runtime.SetStatSample(id, null, null);
        Diagnostics[StatKey(id, "patch")] = "unsupported: " + reason;
    }

    private void InitializeMagnetHooks()
    {
        try
        {
            // PlayerStatsUpgrade mutates Radius AFTER PlayerModifierStats.Upgrade
            // returns. Its complete scope must run against the original radius.
            var method = typeof(VSCharacter).GetMethod("PlayerStatsUpgrade", new[] { typeof(ModifierStats), typeof(bool) })
                ?? throw new MissingMethodException(typeof(VSCharacter).FullName, "PlayerStatsUpgrade");
            if (method.IsStatic || method.ReturnType != typeof(void)) throw new MissingMethodException("PlayerStatsUpgrade signature changed.");
            HarmonyMethod Hook(string name) => new(typeof(GameAdapter).GetMethod(name, BindingFlags.NonPublic | BindingFlags.Static)!);
            _native.DescribeMethod(method, "beforePatch");
            _harmony.Patch(method, prefix: Hook(nameof(MagnetMutationPrefix)), finalizer: Hook(nameof(MagnetMutationFinalizer)));
            _native.DescribeMethod(method, "afterPatch");
            _magnetHooksReady = true;
            _installedStats.Add(StatIds.Magnet);
            _runtime.SetStatCapability(StatIds.Magnet, true);
            Diagnostics[StatKey(StatIds.Magnet, "route")] = "Local MagnetZone.Radius temporary EggFloat; native RefreshSize; full CharacterController.PlayerStatsUpgrade suspension/finalizer; absolute radius units";
            Diagnostics[StatKey(StatIds.Magnet, "patch")] = "installed; direct native radius object; live validation pending";
        }
        catch (Exception e)
        {
            _magnetHooksReady = false;
            UnsupportedStat(StatIds.Magnet, "临时拾取范围无法安全恢复：" + e.Message);
        }
    }

    private void TickMagnetOverride()
    {
        if (!_magnetHooksReady || _magnetNativeDepth != 0) return;
        var zone = _player?.Magnet;
        if (zone == null) { RestoreMagnetOverride(); return; }
        bool ownerChanged = _magnetOwner == null || _magnetOwner.Pointer != zone.Pointer;
        if (ownerChanged)
        {
            if (!RestoreMagnetOverride()) return;
            _magnetOwner = zone;
            _temporaryMagnet = new(() => zone.Radius, value => zone.Radius = value,
                target => new EggFloat((float)target, 0));
        }
        try
        {
            if (_runtime.StatEnabled(StatIds.Magnet)) _temporaryMagnet!.Configure(_runtime.StatValue(StatIds.Magnet));
            else _temporaryMagnet!.Restore();
            bool settingChanged = StatSettingChanged(StatIds.Magnet, ownerChanged);
            if ((settingChanged || ownerChanged && _runtime.StatEnabled(StatIds.Magnet))
                && !RefreshTransientStat("magnet", () => zone.RefreshSize()))
                _lastStatSettings.Remove(StatIds.Magnet);
        }
        catch (Exception e)
        {
            UnsupportedStat(StatIds.Magnet, "临时拾取范围对象不可用：" + e.Message);
            RestoreMagnetOverride();
        }
    }

    private bool RestoreMagnetOverride()
    {
        try
        {
            bool changed = _temporaryMagnet?.Active == true;
            _temporaryMagnet?.Restore();
            if (changed && _magnetOwner != null) _magnetOwner.RefreshSize();
            _temporaryMagnet = null;
            _magnetOwner = null;
            _magnetNativeDepth = 0;
            Diagnostics.Remove("stats.magnetRestoreError");
            return true;
        }
        catch (Exception e) { Diagnostics["stats.magnetRestoreError"] = e.Message; return false; }
    }

    private void SampleMagnet()
    {
        if (!_installedStats.Contains(StatIds.Magnet)) return;
        bool priorSampling = _sampling;
        _sampling = true;
        try
        {
            var zone = _player?.Magnet;
            if (zone == null) { _runtime.SetStatSample(StatIds.Magnet, null, null); return; }
            _temporaryMagnet?.Suspend();
            double original;
            try { original = (float)zone.Radius; }
            finally { _temporaryMagnet?.Resume(); }
            double current = (float)zone.Radius;
            _runtime.SetStatSample(StatIds.Magnet, original, current);
            Diagnostics[StatKey(StatIds.Magnet, "originalRadius")] = original.ToString("R", CultureInfo.InvariantCulture);
            Diagnostics[StatKey(StatIds.Magnet, "boundRadius")] = current.ToString("R", CultureInfo.InvariantCulture);
            Diagnostics.Remove(StatKey(StatIds.Magnet, "sampleError"));
        }
        catch (Exception e)
        {
            _runtime.SetStatSample(StatIds.Magnet, null, null);
            Diagnostics[StatKey(StatIds.Magnet, "sampleError")] = e.Message;
        }
        finally { _sampling = priorSampling; }
    }

    private static void MagnetMutationPrefix(VSCharacter __instance, out GameAdapter? __state)
    {
        __state = null;
        var adapter = Active;
        if (adapter == null || !adapter.IsLocal(__instance) || adapter._temporaryMagnet?.Active != true
            || adapter._magnetOwner?.Pointer != __instance.Magnet?.Pointer) return;
        __state = adapter;
        try { if (adapter._magnetNativeDepth++ == 0) adapter._temporaryMagnet.Suspend(); }
        catch (Exception e)
        {
            adapter.UnsupportedStat(StatIds.Magnet, "正常升级前无法隔离拾取范围：" + e.Message);
            adapter.RestoreMagnetOverride();
        }
    }

    private static Exception? MagnetMutationFinalizer(Exception? __exception, GameAdapter? __state)
    {
        if (__state == null || __state._magnetNativeDepth <= 0 || --__state._magnetNativeDepth != 0) return __exception;
        try
        {
            __state._temporaryMagnet?.Resume();
            __state._magnetOwner?.RefreshSize();
        }
        catch (Exception e)
        {
            __state.UnsupportedStat(StatIds.Magnet, "正常升级后无法恢复临时拾取范围：" + e.Message);
            __state.RestoreMagnetOverride();
        }
        return __exception;
    }

    private static void CharacterStatPostfix(VSCharacter __instance, MethodBase __originalMethod, ref float __result)
    {
        var adapter = Active;
        if (adapter == null || !adapter.IsLocal(__instance) || _readingOriginalStat) return;
        if (CharacterStatMethods.TryGetValue(__originalMethod.Name, out string? id))
            adapter.ApplyFloatStat(id, ref __result);
    }

    private static void CooldownStatFinalPostfix(VSCharacter __instance, float __0, ref float __result)
    {
        var adapter = Active;
        if (adapter == null || _readingOriginalStat || !adapter.IsLocal(__instance)
            || !adapter._runtime.StatEnabled(StatIds.Cooldown) || !float.IsFinite(__result) || !float.IsFinite(__0)) return;
        float before = __result;
        __result = MathF.Max(__0, (float)(1 - adapter._runtime.StatValue(StatIds.Cooldown) / 100d));
        adapter.RecordStatRead(StatIds.Cooldown, before, __result, StatIds.Cooldown);
    }

    private static void AreaStatFinalPostfix(VSCharacter __instance, float __0, ref float __result)
    {
        var adapter = Active;
        if (adapter == null || _readingOriginalStat || !adapter.IsLocal(__instance)
            || !float.IsFinite(__0) || !float.IsFinite(__result)) return;
        string? source = adapter._runtime.StatEnabled(StatIds.Area) ? StatIds.Area
            : adapter._runtime.StatEnabled(StatIds.Omni) ? StatIds.Omni : null;
        if (source == null) return;
        double target = (1 + adapter._runtime.StatValue(source) / 100d) * __0;
        if (!double.IsFinite(target) || target > float.MaxValue || target < -float.MaxValue) return;
        float before = __result;
        __result = (float)target;
        adapter.RecordStatRead(StatIds.Area, before, __result, source);
        // The supplied per-weapon multiplier is retained. No claim is made
        // about additional native clamps that are not present in the metadata.
    }

    private static void CharmReadPostfix(PlayerModifierStats __instance, ref int __result)
    {
        var adapter = Active;
        if (_readingOriginalStat || adapter == null || !adapter.PlayerReady || !adapter.SafeMode
            || !adapter._runtime.StatEnabled(StatIds.Charm)) return;
        var local = adapter._player?.PlayerStats;
        if (local == null || local.Pointer != __instance.Pointer) return;
        int before = __result;
        __result = checked((int)adapter._runtime.StatValue(StatIds.Charm));
        adapter.RecordStatRead(StatIds.Charm, before, __result, StatIds.Charm);
    }

    private void RecordStatRead(string id, double original, double current, string source)
        => _hookDiagnostics.RecordStatRead(id, original, current, source, _sampling, _sampling ? default : DateTime.UtcNow);

    private void SampleStat(string id, Func<double> read)
    {
        if (!_installedStats.Contains(id)) return;
        bool priorSampling = _sampling;
        bool priorOriginal = _readingOriginalStat;
        _sampling = true;
        try
        {
            _readingOriginalStat = true;
            double original = read();
            _readingOriginalStat = false;
            double current = read();
            double Display(double value) => id == StatIds.Cooldown ? (1 - value) * 100
                : IsPercentageStat(id) ? (value - 1) * 100 : value;
            _runtime.SetStatSample(id, double.IsFinite(original) ? Display(original) : null,
                double.IsFinite(current) ? Display(current) : null);
            Diagnostics.Remove(StatKey(id, "sampleError"));
        }
        catch (Exception e)
        {
            _runtime.SetStatSample(id, null, null);
            Diagnostics[StatKey(id, "sampleError")] = e.Message;
        }
        finally { _readingOriginalStat = priorOriginal; _sampling = priorSampling; }
    }

    private bool StatSettingChanged(string id, bool playerChanged)
    {
        double setting = _runtime.StatEnabled(id) ? _runtime.StatValue(id) : double.NaN;
        bool changed = !_lastStatSettings.TryGetValue(id, out double last) || !setting.Equals(last);
        _lastStatSettings[id] = setting;
        return changed && (!playerChanged || !double.IsNaN(setting));
    }

    private bool RefreshTransientStat(string name, Action refresh)
    {
        bool priorSampling = _sampling;
        _sampling = true;
        try { refresh(); Diagnostics.Remove("stats." + name + "RefreshError"); return true; }
        catch (Exception e) { Diagnostics["stats." + name + "RefreshError"] = e.Message; return false; }
        finally { _sampling = priorSampling; }
    }
#endif
}

// Keeps the original resource independent from a consumable temporary budget.
// Delegates bind to one runtime owner, which remains rooted until restoration.
internal sealed class TemporaryStatResource<T>
{
    private readonly Func<T> _read;
    private readonly Action<T> _write;
    private readonly Func<double, T> _create;
    private T _original = default!;
    private T _temporary = default!;
    internal bool Active { get; private set; }
    internal bool Suspended { get; private set; }
    internal double Target { get; private set; }

    internal TemporaryStatResource(Func<T> read, Action<T> write, Func<double, T> create)
    { _read = read; _write = write; _create = create; }

    internal void Configure(double target, bool force = false)
    {
        if (Active && Target.Equals(target) && !force) return;
        T temporary = _create(target);
        if (!Active)
        {
            _original = _read();
            if (_original is null) throw new InvalidOperationException("原次数对象尚未初始化。");
            Active = true;
        }
        _temporary = temporary;
        Target = target;
        if (!Suspended) _write(_temporary);
    }

    internal void Suspend()
    {
        if (!Active || Suspended) return;
        _temporary = _read(); // Native operators may have assigned a new object.
        _write(_original);
        Suspended = true;
    }

    internal void Resume()
    {
        if (!Active || !Suspended) return;
        _original = _read(); // Keep normal native upgrades, resets and exceptions.
        _write(_temporary);
        Suspended = false;
    }

    internal void Restore()
    {
        if (!Active) return;
        if (!Suspended) _write(_original);
        // While suspended, the updated original is already bound to the owner.
        Active = false;
        Suspended = false;
        _original = default!;
        _temporary = default!;
    }

}
