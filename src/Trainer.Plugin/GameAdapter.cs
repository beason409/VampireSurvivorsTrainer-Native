using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using BepInEx.Logging;
using HarmonyLib;
using Trainer.Shared;
#if GAME_INTEROP
using VampireSurvivors.Data;
using VampireSurvivors.Objects.Characters;
using VSCharacter = VampireSurvivors.Objects.Characters.CharacterController;
using VSEnemy = VampireSurvivors.Objects.Characters.EnemyController;
using VSWeapon = VampireSurvivors.Objects.Weapons.Weapon;
using VSOptions = VampireSurvivors.Objects.PlayerOptions;
using VSManager = VampireSurvivors.Framework.GameManager;
using VSMenu = VampireSurvivors.UI.MainMenuPage;
#endif

namespace Trainer.Plugin;

internal sealed partial class GameAdapter : IDisposable
{
    private readonly TrainerRuntime _runtime;
    private readonly ManualLogSource _log;
    private readonly Harmony _harmony = new(TrainerPlugin.PluginGuid);
    private readonly NativeTelemetry _native;
    private readonly HookDiagnostics _hookDiagnostics;
    internal DiagnosticStore Diagnostics { get; } = new();
    internal bool PlayerReady { get; private set; }
    internal bool SafeMode { get; private set; }
    internal string Scene { get; private set; } = "初始化游戏接口";
    internal double? Gold { get; private set; }
    private long _nextPoll;
    private bool _lastCooldown;
    private bool _writingGold;
    private static GameAdapter? Active;
    private readonly Dictionary<IntPtr, long> _lastAttackTimes = new();
    [ThreadStatic] private static bool _sampling;
#if GAME_INTEROP
    private VSManager? _manager;
    private VSCharacter? _player;
    private VSOptions? _options;
    private readonly HashSet<WeaponType> _ownedWeaponTypes = new();
    private static readonly HashSet<string> FastCooldownWeapons = new(StringComparer.Ordinal)
    { "WHIP", "MAGIC_MISSILE", "KNIFE", "AXE", "CROSS", "BIBLE", "FIREBALL", "HOLY_WATER", "LIGHTNING" };
    private static readonly ConcurrentDictionary<WeaponType, AttackKeys> AttackDiagnosticKeys = new();
    private sealed class AttackKeys(WeaponType type)
    {
        internal string Count { get; } = "attack." + type + ".count";
        internal string Pointer { get; } = "attack." + type + ".weaponPointer";
        internal string Interval { get; } = "attack." + type + ".lastIntervalMs";
    }
#endif

    internal GameAdapter(TrainerRuntime runtime, ManualLogSource log)
    { _runtime = runtime; _log = log; _native = new NativeTelemetry(Diagnostics); _hookDiagnostics = new HookDiagnostics(Diagnostics); }

    internal void Initialize()
    {
        Active = this;
#if GAME_INTEROP
        Install(FeatureIds.Health, () => Patch(typeof(VSCharacter), "TakeDamage", typeof(void), new[] { typeof(float) }, nameof(HealthPrefix), nameof(HealthPostfix)));
        Install(FeatureIds.Experience, () => Patch(typeof(VSCharacter), "AddXp", typeof(void),
            new[] { typeof(float), typeof(XPMultiplierMode) }, nameof(ExperiencePrefix), nameof(ExperiencePostfix)));
        Install(FeatureIds.Movement, () => Patch(typeof(VSCharacter), "PMoveSpeed", typeof(float), Type.EmptyTypes, null, nameof(MovementPostfix)));
        Install(FeatureIds.Cooldown, () => Patch(typeof(VSWeapon), "PInterval", typeof(float), Type.EmptyTypes, null, nameof(CooldownPostfix)));
        try
        {
            Patch(typeof(VSEnemy), "GetDamaged", typeof(void), new[] { typeof(float), typeof(HitVfxType), typeof(float), typeof(WeaponType), typeof(bool) },
                nameof(EnemyDamagePrefix), nameof(EnemyDamagePostfix));
            foreach (var id in new[] { FeatureIds.Damage, FeatureIds.InstantKill })
            {
                _runtime.SetCapability(id, true);
                Diagnostics[id + ".patch"] = "EnemyController.GetDamaged; one shared prefix";
            }
        }
        catch (Exception e)
        {
            foreach (var id in new[] { FeatureIds.Damage, FeatureIds.InstantKill })
            {
                _runtime.SetCapability(id, false, "当前版本伤害入口无法验证：" + e.Message);
                Diagnostics[id + ".patch"] = e.Message;
            }
            _log.LogError("Enemy damage entry unavailable: " + e);
        }
        Diagnostics["damage.scope"] = "Positive ordinary enemy hits from local active/hidden equipment WeaponType only; unknown/environment sources unchanged; instantKill has priority.";
        Install(FeatureIds.NoGoldSpend, () =>
        {
            Patch(typeof(VSOptions), "RemoveCoins", typeof(void), new[] { typeof(float), typeof(bool) }, nameof(RemoveCoinsFloatPrefix));
            Patch(typeof(VSOptions), "RemoveCoins", typeof(void), new[] { typeof(int), typeof(bool) }, nameof(RemoveCoinsIntPrefix));
            Patch(typeof(VSOptions), "RemoveCoins", typeof(void), new[] { typeof(int), typeof(bool), typeof(PlayerOptionsData) }, nameof(RemoveCoinsConfigPrefix));
            Patch(typeof(VSOptions), "RemoveCoinsFlat", typeof(float), new[] { typeof(float) }, nameof(RemoveCoinsFlatPrefix));
        });
        try
        {
            Patch(typeof(VSCharacter), "OnWeaponFired", typeof(void), new[] { typeof(VSWeapon) }, null, nameof(WeaponFiredPostfix));
            Diagnostics["attack.observationHook"] = "CharacterController.OnWeaponFired; read-only";
        }
        catch (Exception e) { Diagnostics["attack.observationHook"] = "unavailable: " + e.Message; }
        _log.LogInfo("Exact typed game hooks initialized. No feature enabled.");
        InitializeStatHooks();
        InitializeChestHooks();
        InitializeUpgradeRewardHooks();
#else
        foreach (var id in FeatureIds.All) _runtime.SetCapability(id, false, "需要重新编译 interop 版本。");
        _runtime.SetChestCapability(false, "需要重新编译 interop 版本。");
        _runtime.SetUpgradeCapability(false, "需要重新编译 interop 版本。");
#endif
    }

    private void Install(string feature, Action install)
    {
        try { install(); _runtime.SetCapability(feature, true); Diagnostics[feature + ".patch"] = "installed"; }
        catch (Exception e)
        {
            _runtime.SetCapability(feature, false, "当前版本接口无法验证：" + e.Message);
            Diagnostics[feature + ".patch"] = e.Message;
            _log.LogError($"Feature {feature} is unavailable: {e}");
        }
    }

    private void Patch(Type type, string name, Type returnType, Type[] parameters, string? prefix = null, string? postfix = null, bool isStatic = false, string? finalizer = null)
    {
        var method = type.GetMethod(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly,
            null, parameters, null);
        if (method == null || method.ReturnType != returnType || method.IsStatic != isStatic) throw new MissingMethodException(type.FullName, name);
        _native.DescribeMethod(method, "beforePatch");
        HarmonyMethod? Find(string? hook) => hook == null ? null : new HarmonyMethod(typeof(GameAdapter).GetMethod(hook, BindingFlags.NonPublic | BindingFlags.Static)!);
        _harmony.Patch(method, Find(prefix), Find(postfix), finalizer: Find(finalizer));
        _native.DescribeMethod(method, "afterPatch");
    }

    internal void Tick()
    {
#if GAME_INTEROP
        if (Stopwatch.GetTimestamp() < _nextPoll) return;
        _nextPoll = Stopwatch.GetTimestamp() + Stopwatch.Frequency / 4;
        Scene = UnityEngine.SceneManagement.SceneManager.GetActiveScene().name ?? "未知场景";
        _native.ClearValues("menu");
        _native.ClearValues("game");
        _native.ClearValues("weapon");
        if (_manager == null) _manager = UnityEngine.Object.FindObjectOfType<VSManager>();
        if (_manager != null)
        {
            Diagnostics["menu.lookup"] = "not used: gameplay manager is present";
            SafeMode = !_manager.IsMultiplayer && !_manager.StartedAsOnlineMultiplayerRun;
            if (!SafeMode) _runtime.DisableAll();
            _options = _manager.PlayerOptions;
            _player = _manager.Player;
            PlayerReady = SafeMode && _player != null && !_player.IsDead && !_manager.InGameOverState;
        }
        else
        {
            _player = null;
            PlayerReady = false;
            // MainMenuPage becomes inactive when character/stage/power-up pages
            // are open. Its injected services still belong to this menu scene.
            var menu = Scene == "MainMenu" ? UnityEngine.Object.FindObjectOfType<VSMenu>(true) : null;
            if (menu != null) _options = menu.PlayerOptions;
            var multiplayer = menu?._multiplayerManager;
            Diagnostics["menu.found"] = (menu != null).ToString();
            Diagnostics["menu.includeInactive"] = "True; only in MainMenu scene";
            Diagnostics["menu.active"] = (menu != null && menu.isActiveAndEnabled).ToString();
            Diagnostics["menu.multiplayerPresent"] = (multiplayer != null).ToString();
            if (multiplayer != null)
            {
                Diagnostics["menu.isMultiplayer"] = multiplayer.IsMultiplayer.ToString();
                Diagnostics["menu.isLocalMultiplayer"] = multiplayer.IsLocalMultiplayer.ToString();
                Diagnostics["menu.isOnlineMultiplayer"] = multiplayer.IsOnlineMultiplayer.ToString();
                Diagnostics["menu.playerCount"] = multiplayer.GetPlayerCount().ToString(CultureInfo.InvariantCulture);
                Diagnostics["menu.localPlayerCount"] = multiplayer.GetLocalPlayerCount().ToString(CultureInfo.InvariantCulture);
            }
            SafeMode = menu != null && _options != null && multiplayer != null
                && !multiplayer.IsMultiplayer && !multiplayer.IsOnlineMultiplayer;
        }
        bool optionsReady = _options != null && _options.IsInitialized && _options.IsConfigReady;
        SafeMode &= optionsReady;
        PlayerReady &= SafeMode;
        ReadRunState();
        // Reading the local permanent wallet is independent of eligibility to
        // mutate gameplay. The write paths retain solo + backup authorization.
        Gold = optionsReady ? _options?.MainGameConfig?.Coins : null;
        Diagnostics["gold.readReady"] = (Gold != null).ToString();
        Diagnostics["session.offline"] = SafeMode.ToString();
        Diagnostics["session.reason"] = SafeMode ? "单人模式" : "尚未就绪或首版不支持当前多人模式";
        Diagnostics["manager.present"] = (_manager != null).ToString();
        if (_manager != null)
        {
            Diagnostics["manager.isMultiplayer"] = _manager.IsMultiplayer.ToString();
            Diagnostics["manager.startedAsOnline"] = _manager.StartedAsOnlineMultiplayerRun.ToString();
        }
        Diagnostics["options.initialized"] = (_options?.IsInitialized ?? false).ToString();
        Diagnostics["options.configReady"] = (_options?.IsConfigReady ?? false).ToString();
        if (optionsReady && _options != null)
        {
            Diagnostics["options.pointer"] = _options.Pointer.ToString("X");
            Diagnostics["options.configPointer"] = _options.Config?.Pointer.ToString("X") ?? "null";
            Diagnostics["options.mainPointer"] = _options.MainGameConfig?.Pointer.ToString("X") ?? "null";
            var mainConfig = _options.MainGameConfig;
            Diagnostics["options.mainCoins"] = mainConfig == null ? "null" : F(mainConfig.Coins);
            Diagnostics["options.configCoins"] = _options.Config == null ? "null" : F(_options.Config.Coins);
        }
        Diagnostics["gold.interface"] = _options == null ? "waiting" : "PlayerOptions.MainGameConfig.Coins + Save(true,true)";
        Diagnostics["cooldown.units"] = "milliseconds; basic weapon allowlist; cap=100";
        Diagnostics["raw.sampleUtc"] = DateTime.UtcNow.ToString("O", CultureInfo.InvariantCulture);
        Diagnostics["raw.processId"] = Environment.ProcessId.ToString(CultureInfo.InvariantCulture);
        if (optionsReady && _options?.MainGameConfig != null)
            _native.SampleFloat("raw.gold", _options.MainGameConfig.Pointer, "<Coins>k__BackingField");
        else _native.ClearValues("raw.gold");
        if (PlayerReady && _player != null)
        {
            RefreshOwnedWeaponTypes();
            Diagnostics["raw.player.pointer"] = NativeTelemetry.Hex(_player.Pointer);
            Diagnostics["raw.player.nativeType"] = _player.GetIl2CppType().FullName;
            _native.SampleFloat("raw.hp", _player.Pointer, "_currentHp");
            _native.SampleFloat("raw.xp", _player.Pointer, "_xp");
            var body = _player.body;
            if (body != null)
            {
                Diagnostics["raw.body.pointer"] = NativeTelemetry.Hex(body.Pointer);
                _native.SampleFloat2("raw.position", body.Pointer, "_position");
                _native.SampleFloat2("raw.velocity", body.Pointer, "_velocity");
            }
            else { _native.ClearValues("raw.body"); _native.ClearValues("raw.position"); _native.ClearValues("raw.velocity"); }
            _sampling = true;
            try
            {
                Diagnostics["player.hp"] = F(_player.CurrentHp);
                Diagnostics["player.maxHp"] = F(_player.MaxHp());
                Diagnostics["player.xp"] = F(_player.Xp);
                Diagnostics["player.level"] = _player.Level.ToString(CultureInfo.InvariantCulture);
                Diagnostics["movement.effective"] = F(_player.PMoveSpeed());
                Diagnostics["movement.actualSpeed"] = F(_player.Speed);
                Diagnostics["movement.velocity"] = $"{F(_player.Velocity.x)},{F(_player.Velocity.y)}";
                Diagnostics["player.currentPosition"] = $"{F(_player.CurrentPos.x)},{F(_player.CurrentPos.y)}";
                var weapon = FirstWeapon();
                if (weapon != null)
                {
                    Diagnostics["weapon.type"] = weapon.Type.ToString();
                    Diagnostics["weapon.level"] = weapon.Level.ToString(CultureInfo.InvariantCulture);
                    Diagnostics["weapon.reportedCumulativeDamage"] = F(weapon.CalculateTotalDamage());
                    Diagnostics["cooldown.effectiveMs"] = F(weapon.PInterval());
                    Diagnostics["cooldown.baseIntervalMs"] = F(weapon.Interval);
                }
            }
            finally { _sampling = false; }
        }
        else
        {
            foreach (var prefix in new[] { "raw.player", "raw.hp", "raw.xp", "raw.body", "raw.position", "raw.velocity" }) _native.ClearValues(prefix);
            _lastAttackTimes.Clear();
            _ownedWeaponTypes.Clear();
        }
        bool cooldown = _runtime.Enabled(FeatureIds.Cooldown);
        if (cooldown != _lastCooldown) { _lastCooldown = cooldown; RefreshWeaponTimers(); }
        TickStats();
        SampleStats();
        RefreshStatButtons();
#endif
    }

    internal void FeaturesChanged(string feature)
    {
#if GAME_INTEROP
        if (feature == FeatureIds.Cooldown) RefreshWeaponTimers();
#endif
    }

    internal void StatsChanged() { TickStats(); SampleStats(); RefreshStatButtons(true); }

    internal void SetGold(double value)
    {
#if GAME_INTEROP
        if (!SafeMode || _options == null || _options.MainGameConfig == null)
            throw new InvalidOperationException("金币接口尚未准备好；请回到单人主菜单。");
        if (_manager != null && _player != null) throw new InvalidOperationException("永久金币请在单人主菜单修改。");
        var config = _options.MainGameConfig;
        if (_options.Config == null || _options.Config.Pointer != config.Pointer)
            throw new InvalidOperationException("第一版永久金币仅支持普通模式主菜单。");
        float desired = (float)value;
        if ((double)desired != value)
            throw new InvalidOperationException($"游戏无法精确保存该金币数值，请使用可精确表示的值，例如 {desired.ToString("0", CultureInfo.InvariantCulture)}。");
        float before = config.Coins;
        _writingGold = true;
        try
        {
            if (desired > before) _options.AddCoinsFlat(desired - before);
            else if (desired < before) _options.RemoveCoins(before - desired, false);
            _options.Save(true, true);
            Gold = config.Coins;
            Diagnostics["gold.lastBefore"] = F(before);
            Diagnostics["gold.lastAfter"] = F(config.Coins);
            Diagnostics["gold.save"] = "PlayerOptions.Save(commitImmediately:true, createBackup:true) returned";
            if (config.Coins != desired) throw new InvalidOperationException("游戏对金币数值进行了调整，当前值：" + F(config.Coins));
        }
        finally { _writingGold = false; }
#else
        throw new InvalidOperationException("游戏接口尚未初始化。");
#endif
    }

#if GAME_INTEROP
    private void ReadRunState()
    {
        // The global PauseSystem class and each GameManager getter below were
        // verified in this build's generated interop. Read only: never call
        // Pause/Resume or assign a native state. Missing manager means unknown.
        if (_manager == null) return;
        bool pauseSystem = global::PauseSystem.Paused;
        bool managerPaused = _manager.IsPaused;
        bool pauseMenu = _manager.IsInPauseGameState;
        bool upgrade = _manager.IsLevelUpPageOpen;
        bool freezing = _manager.FreezingFrame;
        bool gameOver = _manager.InGameOverState;
        bool restarting = _manager.RestartingGameScene;
        Diagnostics["game.pauseSystemPaused"] = pauseSystem.ToString();
        Diagnostics["game.managerPaused"] = managerPaused.ToString();
        Diagnostics["game.pauseMenuOpen"] = pauseMenu.ToString();
        Diagnostics["game.upgradeOpen"] = upgrade.ToString();
        Diagnostics["game.freezingFrame"] = freezing.ToString();
        Diagnostics["game.gameOver"] = gameOver.ToString();
        Diagnostics["game.restarting"] = restarting.ToString();
        Diagnostics["game.paused"] = (pauseSystem || managerPaused || pauseMenu || upgrade || freezing || gameOver || restarting).ToString();
        Diagnostics["game.pausedSource"] = "PauseSystem.Paused OR GameManager(IsPaused,IsInPauseGameState,IsLevelUpPageOpen,FreezingFrame,InGameOverState,RestartingGameScene)";
        Diagnostics["game.stateSampleUtc"] = DateTime.UtcNow.ToString("O", CultureInfo.InvariantCulture);
        float elapsed = _manager.SurvivedSeconds;
        if (float.IsFinite(elapsed)) Diagnostics["game.elapsedSeconds"] = F(elapsed);
        Diagnostics["game.pauseSystemDeltaTime"] = F(global::PauseSystem.DeltaTime);
        var stage = _manager.Stage;
        if (stage != null) Diagnostics["game.stage"] = stage.StageType.ToString();
        if (_player != null) Diagnostics["game.character"] = _player.CharacterType.ToString();
    }

    private bool IsLocal(VSCharacter? player) => SafeMode && PlayerReady && player != null && _player != null && player.Pointer == _player.Pointer;
    private bool IsOwnWeapon(VSWeapon? weapon) => weapon != null && IsLocal(weapon.Owner);
    private void RefreshOwnedWeaponTypes()
    {
        _ownedWeaponTypes.Clear();
        var manager = _player?.WeaponsManager;
        if (manager == null) return;
        foreach (bool hidden in new[] { false, true })
        {
            var list = hidden ? manager.HiddenEquipment : manager.ActiveEquipment;
            if (list == null) continue;
            for (int i = 0; i < list.Count; i++)
            {
                var equipment = list[i];
                if (equipment != null && IsLocal(equipment.Owner)) _ownedWeaponTypes.Add(equipment.Type);
            }
        }
        Diagnostics["damage.ownedWeaponTypes"] = string.Join(",", _ownedWeaponTypes.Select(type => type.ToString()).OrderBy(name => name));
    }
    private VSWeapon? FirstWeapon()
    {
        var list = _player?.WeaponsManager?.ActiveEquipment;
        if (list == null) return null;
        for (int index = 0; index < list.Count; index++)
        {
            var weapon = list[index]?.TryCast<VSWeapon>();
            if (weapon != null && IsOwnWeapon(weapon)) return weapon;
        }
        return null;
    }
    private void RefreshWeaponTimers()
    {
        if (!PlayerReady || !SafeMode) return;
        var list = _player?.WeaponsManager?.ActiveEquipment;
        if (list == null) return;
        for (int i = 0; i < list.Count; i++)
        {
            var weapon = list[i]?.TryCast<VSWeapon>();
            if (weapon != null && IsOwnWeapon(weapon) && FastCooldownWeapons.Contains(weapon.Type.ToString())) weapon.ResetFiringTimer();
        }
    }
    private static bool HealthPrefix(VSCharacter __instance, ref float __0, out float __state)
    {
        var a = Active;
        __state = a != null && a.IsLocal(__instance) ? __instance.CurrentHp : float.NaN;
        if (a == null || !a._runtime.Enabled(FeatureIds.Health) || !a.IsLocal(__instance) || !float.IsFinite(__0) || __0 <= 0) return true;
        a.Record("health", __0, 0);
        // The verified native implementation subtracts damage, then performs its
        // normal hit feedback/invulnerability flow even for zero damage. Keep
        // that flow; skipping TakeDamage causes collision callbacks to flood.
        __0 = 0;
        return true;
    }
    private static void HealthPostfix(VSCharacter __instance, float __state)
    {
        var a = Active;
        if (a != null && a.IsLocal(__instance) && float.IsFinite(__state))
            a.ObserveDamage("combat.player", __state, __instance.CurrentHp);
    }
    private struct XpObservation { internal bool Valid; internal float Before; internal int Level; internal float Input; internal float Effective; }
    private static void ExperiencePrefix(VSCharacter __instance, ref float __0, out XpObservation __state)
    {
        var a = Active;
        __state = default;
        if (a != null && a.IsLocal(__instance) && float.IsFinite(__0) && __0 > 0)
            __state = new XpObservation { Valid = true, Before = __instance.Xp, Level = __instance.Level, Input = __0, Effective = __0 };
        if (a == null || !a.IsLocal(__instance) || !a._runtime.Enabled(FeatureIds.Experience) || __0 <= 0 || !float.IsFinite(__0)) return;
        float before = __0;
        float result = (float)(__0 * a._runtime.Multiplier(FeatureIds.Experience));
        if (!float.IsFinite(result)) return;
        __0 = result;
        __state.Effective = result;
        a.Record("experience", before, result);
    }
    private static void ExperiencePostfix(VSCharacter __instance, XpObservation __state)
    {
        var a = Active;
        if (a == null || !__state.Valid || !a.IsLocal(__instance)) return;
        a.Increment("experience.actualCalls");
        a.Diagnostics.SetFloat("experience.actualInput", __state.Input);
        a.Diagnostics.SetFloat("experience.actualHookInput", __state.Effective);
        a.Diagnostics.SetFloat("experience.actualXpBefore", __state.Before);
        a.Diagnostics.SetFloat("experience.actualXpAfter", __instance.Xp);
        a.Diagnostics.SetInteger("experience.actualLevelBefore", __state.Level);
        a.Diagnostics.SetInteger("experience.actualLevelAfter", __instance.Level);
    }
    private static void MovementPostfix(VSCharacter __instance, ref float __result)
    {
        var a = Active;
        if (a == null || !a.IsLocal(__instance) || !float.IsFinite(__result)) return;
        a.ApplyFloatStat(StatIds.MoveSpeed, ref __result);
        if (!a._runtime.Enabled(FeatureIds.Movement)) return;
        float before = __result;
        __result = (float)(__result * a._runtime.Multiplier(FeatureIds.Movement));
        a.Record("movement", before, __result);
    }
    private static void CooldownPostfix(VSWeapon __instance, ref float __result)
    {
        var a = Active;
        if (a == null || !a.IsOwnWeapon(__instance) || !a._runtime.Enabled(FeatureIds.Cooldown)
            || !FastCooldownWeapons.Contains(__instance.Type.ToString()) || !float.IsFinite(__result) || __result <= 0) return;
        float before = __result;
        __result = MathF.Min(__result, 100f);
        a.Record("cooldown", before, __result);
    }
    private static void EnemyDamagePrefix(VSEnemy __instance, ref float __0, WeaponType __3, out float __state)
    {
        var a = Active;
        __state = a != null && a.SafeMode && a.PlayerReady && __instance != null && !__instance.IsDead && !__instance.IsBullet()
            ? __instance.Hp : float.NaN;
        if (a == null || __instance == null || !float.IsFinite(__state) || !float.IsFinite(__0) || __0 <= 0) return;
        a.Diagnostics.SetEnum("combat.enemy.lastWeaponType", __3);
        a.Diagnostics.SetFloat("combat.enemy.lastInput", __0);
        a.ModifyEnemyDamage(__instance, ref __0, __3);
        a.Diagnostics.SetFloat("combat.enemy.lastHookInput", __0);
    }
    private void ModifyEnemyDamage(VSEnemy enemy, ref float damage, WeaponType source)
    {
        if (_runtime.Enabled(FeatureIds.Damage) && _ownedWeaponTypes.Contains(source))
        {
            float before = damage;
            float scaled = (float)(damage * _runtime.Multiplier(FeatureIds.Damage));
            if (float.IsFinite(scaled)) { damage = scaled; Record("damage", before, damage); }
        }
        if (!_runtime.Enabled(FeatureIds.InstantKill)) return;
        float health = enemy.Hp;
        if (!float.IsFinite(health) || health <= 0) return;
        float previousDamage = damage;
        // Apply one rule to every enemy type. Compute in double precision and
        // saturate to a finite float instead of excluding high-HP enemies.
        float lethalDamage = (float)Math.Min(float.MaxValue, (double)health * 100d);
        damage = MathF.Max(damage, lethalDamage);
        Record("instantKill", previousDamage, damage);
        // The game's native damage processing still runs.
    }
    private static void EnemyDamagePostfix(VSEnemy __instance, float __state)
    {
        var a = Active;
        if (a == null || !a.SafeMode || !a.PlayerReady || !float.IsFinite(__state) || __instance == null) return;
        a.ObserveDamage("combat.enemy", __state, __instance.Hp);
        a.Diagnostics.SetEnum("combat.enemy.lastType", __instance.EnemyType);
        a.Diagnostics.SetPointer("combat.enemy.lastPointer", __instance.Pointer);
    }
    private static void WeaponFiredPostfix(VSCharacter __instance, VSWeapon __0)
    {
        var a = Active;
        if (a == null || !a.IsLocal(__instance) || !a.IsOwnWeapon(__0)) return;
        var keys = AttackDiagnosticKeys.GetOrAdd(__0.Type, static type => new AttackKeys(type));
        a.Increment(keys.Count);
        a.Diagnostics.SetPointer(keys.Pointer, __0.Pointer);
        long now = Stopwatch.GetTimestamp();
        if (a._lastAttackTimes.TryGetValue(__0.Pointer, out long previous))
            a.Diagnostics.SetDouble(keys.Interval, (now - previous) * 1000.0 / Stopwatch.Frequency, "F3");
        a._lastAttackTimes[__0.Pointer] = now;
    }
    private static bool PreserveCoins(VSOptions options, float amount)
    {
        var a = Active;
        if (a == null || a._writingGold || !a.SafeMode || !a._runtime.Enabled(FeatureIds.NoGoldSpend)
            || a._options == null || options.Pointer != a._options.Pointer || !float.IsFinite(amount) || amount <= 0
            || options.Config == null || options.MainGameConfig == null || options.Config.Pointer != options.MainGameConfig.Pointer) return false;
        a.Record("noGoldSpend", amount, 0);
        return true;
    }
    private static void RemoveCoinsFloatPrefix(VSOptions __instance, ref float __0) { if (PreserveCoins(__instance, __0)) __0 = 0; }
    private static void RemoveCoinsIntPrefix(VSOptions __instance, ref int __0) { if (PreserveCoins(__instance, __0)) __0 = 0; }
    private static void RemoveCoinsConfigPrefix(VSOptions __instance, ref int __0, PlayerOptionsData __2)
    {
        var a = Active;
        if (a?._options?.MainGameConfig == null || __2 == null || __2.Pointer != a._options.MainGameConfig.Pointer) return;
        if (PreserveCoins(__instance, __0)) __0 = 0;
    }
    private static void RemoveCoinsFlatPrefix(VSOptions __instance, ref float __0)
    {
        if (PreserveCoins(__instance, __0)) __0 = 0;
    }
#endif
    private void ObserveDamage(string prefix, float before, float after)
        => _hookDiagnostics.ObserveDamage(prefix, before, after);
    private void Increment(string key) => Diagnostics.Increment(key);
    private void Record(string feature, float before, float after)
        => _hookDiagnostics.Record(feature, before, after, _sampling);
    private static string F(float value) => value.ToString("R", CultureInfo.InvariantCulture);
    public void Dispose()
    {
        _runtime.DisableAll();
        TickStats();
        RestoreStatOverrides();
        if (Active == this) Active = null;
        _harmony.UnpatchSelf();
    }
}
