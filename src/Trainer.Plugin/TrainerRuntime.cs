using System.Collections.Concurrent;
using System.Diagnostics;
using BepInEx.Logging;
using Trainer.Shared;

namespace Trainer.Plugin;

internal sealed class TrainerRuntime : IDisposable
{
    private readonly ConcurrentQueue<Action> _queue = new();
    private readonly object _gate = new();
    private readonly ManualLogSource _log;
    private readonly Dictionary<string, FeatureState> _features;
    private readonly Dictionary<string, StatState> _stats;
    private GameAdapter? _adapter;
    private long _lastHeartbeat;
    private int _session;
    private bool _backupAuthorized;
    private bool _started;
    private bool _disposed;
    private volatile bool _pumpReady;
    private string _versionError = "正在校验游戏版本。";
    private bool _gameVerified;
    private string _scene = "正在初始化";
    private bool _playerReady;
    private double? _gold;
    private bool _safeMode;
    private DiagnosticStore _diagnostics = new();
    private readonly ChestRewardState _chestReward = new() { Message = "箱子奖励接口正在初始化。" };

    internal TrainerRuntime(ManualLogSource log)
    {
        _log = log;
        _features = FeatureIds.All.ToDictionary(id => id, id => new FeatureState
        { Id = id, Supported = false, Enabled = false, Value = 1, Reason = "正在初始化。" });
        _stats = StatCatalog.All.ToDictionary(d => d.Id, d => new StatState
        { Id=d.Id, Value=d.DefaultValue, Supported=false, Reason="正在初始化。" });
        _ = Task.Run(() =>
        {
            bool valid = BackupVerifier.VerifyGame(out var error);
            _queue.Enqueue(() => { _gameVerified = valid; _versionError = error; });
        });
    }

    internal bool PumpReady => _pumpReady;
    internal int BeginSession()
    {
        lock (_gate)
        {
            DisableAllLocked();
            _backupAuthorized = false;
            _lastHeartbeat = Stopwatch.GetTimestamp();
            _chestReward.ProcessedCount = 0;
            _chestReward.LastRewardItemId = null;
            _chestReward.LastRewardName = null;
            _chestReward.UpgradeProcessedCount = 0;
            _chestReward.LastUpgradeItemId = null;
            _chestReward.LastUpgradeName = null;
            return ++_session;
        }
    }

    internal void Heartbeat(int session)
    {
        lock (_gate) if (session == _session) _lastHeartbeat = Stopwatch.GetTimestamp();
    }

    internal void EndSession(int session)
    {
        lock (_gate)
        {
            if (session != _session) return;
            ++_session;
            DisableAllLocked();
            _backupAuthorized = false;
        }
    }

    internal void CheckHeartbeat()
    {
        lock (_gate)
        {
            if (_lastHeartbeat != 0 && (Stopwatch.GetTimestamp() - _lastHeartbeat) / (double)Stopwatch.Frequency > 3)
            {
                DisableAllLocked();
                _backupAuthorized = false;
                ++_session;
                _lastHeartbeat = 0;
            }
        }
    }

    internal bool Enabled(string id)
    {
        lock (_gate) return _features.TryGetValue(id, out var feature) && feature.Supported && feature.Enabled && _safeMode
            && (id == FeatureIds.NoGoldSpend ? _backupAuthorized : _playerReady);
    }

    internal double Multiplier(string id)
    {
        lock (_gate) return _features.TryGetValue(id, out var feature) && feature.Enabled && _playerReady ? feature.Value : 1;
    }

    internal void SetChestCapability(bool supported, string? reason = null)
    {
        lock (_gate)
        {
            _chestReward.Supported = supported;
            if (!supported) _chestReward.Enabled = false;
            _chestReward.Message = reason ?? (supported ? "选择物品后开启；后续拾取的箱子保证一个指定奖励。" : "当前版本的箱子奖励入口不可用。");
        }
    }

    internal string? ChestTarget()
    {
        lock (_gate) return _chestReward.Enabled && _chestReward.Supported && _gameVerified && _safeMode && _playerReady
            ? _chestReward.TargetItemId : null;
    }

    internal void RecordChestReward(string itemId, string itemName, string? message = null)
    {
        lock (_gate)
        {
            if (_chestReward.ProcessedCount < int.MaxValue) _chestReward.ProcessedCount++;
            _chestReward.LastRewardItemId = itemId;
            _chestReward.LastRewardName = itemName;
            _chestReward.Message = message ?? ("最近已获得：" + itemName);
        }
    }

    internal void ChestRewardUnavailable(string message)
    {
        lock (_gate) { _chestReward.Enabled = false; _chestReward.Message = message; }
    }

    internal void SetUpgradeCapability(bool supported, string? reason = null)
    {
        lock (_gate)
        {
            _chestReward.UpgradeSupported = supported;
            if (!supported) _chestReward.UpgradeEnabled = false;
            _chestReward.UpgradeMessage = reason ?? (supported ? "开启后，升级时保证一个指定选项；由玩家选择是否拿取。" : "当前版本的升级奖励入口不可用。");
        }
    }

    internal string? UpgradeTarget()
    {
        lock (_gate) return _chestReward.UpgradeEnabled && _chestReward.UpgradeSupported && _gameVerified && _safeMode && _playerReady
            ? _chestReward.TargetItemId : null;
    }

    internal void RecordUpgradeReward(string itemId, string itemName, string? message = null)
    {
        lock (_gate)
        {
            if (_chestReward.UpgradeProcessedCount < int.MaxValue) _chestReward.UpgradeProcessedCount++;
            _chestReward.LastUpgradeItemId = itemId;
            _chestReward.LastUpgradeName = itemName;
            _chestReward.UpgradeMessage = message ?? ("已提供升级选项：" + itemName + "；选择后才会获得。");
        }
    }

    internal void UpgradeRewardUnavailable(string message)
    {
        lock (_gate) { _chestReward.UpgradeEnabled = false; _chestReward.UpgradeMessage = message; }
    }

    internal bool StatEnabled(string id)
    {
        lock (_gate) return _stats.TryGetValue(id,out var stat) && stat.Supported && stat.Enabled && _safeMode && _playerReady;
    }
    internal double StatValue(string id)
    {
        lock (_gate) return _stats.TryGetValue(id,out var stat) ? stat.Value : 0;
    }
    internal void SetStatCapability(string id,bool supported,string? reason=null)
    {
        lock (_gate)
        {
            if(!_stats.TryGetValue(id,out var stat)) return;
            stat.Supported=supported; stat.Reason=supported ? null : reason ?? "当前版本属性不可用。";
            if(!supported) stat.Enabled=false;
        }
    }
    internal void SetStatSample(string id,double? original,double? current)
    {
        lock (_gate)
        {
            if(!_stats.TryGetValue(id,out var stat)) return;
            stat.OriginalValue=original.HasValue && double.IsFinite(original.Value) ? original : null;
            stat.CurrentValue=current.HasValue && double.IsFinite(current.Value) ? current : null;
        }
    }

    internal void SetCapability(string id, bool supported, string? reason = null)
    {
        lock (_gate)
        {
            var feature = _features[id];
            feature.Supported = supported;
            feature.Reason = supported ? null : reason ?? "当前版本尚未验证此功能。";
            if (!supported) feature.Enabled = false;
        }
    }

    internal Task<Response> Submit(Request request, int session, bool? backupValid = null, string? backupError = null)
    {
        var completion = new TaskCompletionSource<Response>(TaskCreationOptions.RunContinuationsAsynchronously);
        // Desktop times out at two seconds. Expire queued mutations before its
        // timeout so a resumed Unity frame cannot apply a reported failed write.
        var deadline = Stopwatch.GetTimestamp() + (long)(Stopwatch.Frequency * 1.5);
        if (!_pumpReady)
        {
            completion.TrySetResult(new Response { Id = request.Id, Ok = false, Error = "插件正在初始化或需要重新编译 interop 版本。", State = Snapshot() });
            return completion.Task;
        }
        _queue.Enqueue(() =>
        {
            try
            {
                List<RewardItemDefinition>? catalogPage = null;
                int? catalogTotal = null;
                RewardItemIcon? itemIcon = null;
                lock (_gate)
                {
                    if (session != _session || Stopwatch.GetTimestamp() > deadline) throw new InvalidOperationException("连接或指令已失效，请重新连接。");
                    if (backupValid.HasValue) _backupAuthorized = backupValid.Value;
                }
                switch (request.Kind)
                {
                    case "hello":
                    case "heartbeat":
                    case "getState":
                    case "getGold": break;
                    case "getItemCatalog":
                        if (request.CatalogOffset < 0 || request.CatalogLimit is < 1 or > 200)
                            throw new InvalidOperationException("物品列表分页范围无效。");
                        if (_adapter is null || !_safeMode) throw new InvalidOperationException("请等待单人游戏菜单或关卡就绪后读取物品列表。");
                        var catalog = _adapter.GetRewardItemCatalog();
                        _adapter.EnableRewardIconSupport();
                        lock (_gate)
                            if (_adapter.Diagnostics.TryGetValue("chest.catalog.icons", out var iconCapability))
                                _diagnostics["chest.catalog.icons"] = iconCapability;
                        catalogTotal = catalog.Count;
                        catalogPage = catalog.Skip(request.CatalogOffset).Take(request.CatalogLimit).Select(item => new RewardItemDefinition
                        {
                            Id = item.Id, Name = item.Name, EnglishName = item.EnglishName,
                            IsEvolution = item.IsEvolution,
                            Kind = item.Kind, MaxLevel = item.MaxLevel,
                            UpgradeAllowed = item.UpgradeAllowed, UpgradeUnsupportedReason = item.UpgradeUnsupportedReason,
                            Description = null
                        }).ToList();
                        break;
                    case "getItemIcon":
                        itemIcon = _adapter?.GetRewardItemIcon(request.ItemId) ?? new RewardItemIcon();
                        break;
                    case "disableAll": DisableAll(); _adapter?.StatsChanged(); break;
                    case "disableStats": DisableStats(); _adapter?.StatsChanged(); break;
                    case "setFeature": SetFeature(request); break;
                    case "setStat": SetStat(request); break;
                    case "setChestReward": SetChestReward(request, session, deadline); break;
                    case "setGold":
                        lock (_gate) if (!_backupAuthorized) throw new InvalidOperationException(backupError ?? "需要有效的本次启动前存档备份。");
                        if (!request.Value.HasValue || !double.IsFinite(request.Value.Value)
                            || request.Value.Value < 0 || request.Value.Value > 999_999_999
                            || request.Value.Value != Math.Truncate(request.Value.Value))
                            throw new InvalidOperationException("金币必须是 0 到 999999999 的整数。");
                        if (_adapter == null) throw new InvalidOperationException("游戏接口尚未初始化。");
                        _adapter.SetGold(request.Value.Value);
                        _gold = _adapter.Gold;
                        break;
                    default: throw new InvalidOperationException("未知指令。");
                }
                completion.TrySetResult(new Response { Id = request.Id, Ok = true, State = Snapshot(), ItemCatalog = catalogPage, ItemCatalogTotal = catalogTotal, ItemIcon = itemIcon });
            }
            catch (Exception e)
            {
                _log.LogWarning($"Command {request.Kind} rejected: {e.Message}");
                completion.TrySetResult(new Response { Id = request.Id, Ok = false, Error = e.Message,
                    ErrorScope = (e as RewardSelectionException)?.Scope, State = Snapshot() });
            }
        });
        return completion.Task;
    }

    private void SetChestReward(Request request, int session, long deadline)
    {
        bool enable, upgrade;
        string? itemId;
        lock (_gate)
        {
            enable = request.Enabled ?? _chestReward.Enabled;
            upgrade = request.UpgradeEnabled ?? _chestReward.UpgradeEnabled;
            itemId = request.ItemId ?? _chestReward.TargetItemId;
            // Turning one route off must remain possible even if the other
            // route's target has since become ineligible (for example, maxed).
            if (request.ItemId is null && request.Enabled != true && request.UpgradeEnabled != true)
            {
                _chestReward.Enabled = enable;
                _chestReward.UpgradeEnabled = upgrade;
                if (!enable) _chestReward.Message = "已恢复后续箱子的正常随机奖励。";
                if (!upgrade) _chestReward.UpgradeMessage = "已恢复后续升级的正常随机选项。";
                return;
            }
            if (!enable && !upgrade)
            {
                _chestReward.Enabled = false;
                _chestReward.UpgradeEnabled = false;
                _chestReward.Message = "已恢复后续箱子的正常随机奖励。";
                _chestReward.UpgradeMessage = "已恢复后续升级的正常随机选项。";
                return;
            }
            if (enable && !_chestReward.Supported) throw new RewardSelectionException("chest", _chestReward.Message ?? "箱子奖励接口不可用。");
            if (upgrade && !_chestReward.UpgradeSupported) throw new RewardSelectionException("upgrade", _chestReward.UpgradeMessage ?? "升级奖励接口不可用。");
            if (!_gameVerified || !_safeMode || !_playerReady) throw new RewardSelectionException("rewardTarget", "请进入单人关卡后开启指定奖励。");
        }
        if (_adapter is null || string.IsNullOrWhiteSpace(itemId) || itemId.Length > 128)
            throw new RewardSelectionException("rewardTarget", "请从当前游戏的物品列表选择有效武器或被动装备。");
        RewardItemDefinition? target;
        try { target = _adapter.GetRewardItemCatalog().FirstOrDefault(item => string.Equals(item.Id, itemId, StringComparison.Ordinal)); }
        catch (Exception error) { throw new RewardSelectionException("rewardTarget", error.Message, error); }
        if (target is null) throw new RewardSelectionException("rewardTarget", "所选物品在当前游戏中不可用，请刷新物品列表。");
        if (enable)
        {
            try { _adapter.ValidateChestRewardTarget(target.Id); }
            catch (Exception error) { throw new RewardSelectionException("chest", error.Message, error); }
        }
        if (upgrade)
        {
            try { _adapter.ValidateUpgradeRewardTarget(target.Id); }
            catch (Exception error) { throw new RewardSelectionException("upgrade", error.Message, error); }
        }
        lock (_gate)
        {
            if (session != _session || Stopwatch.GetTimestamp() > deadline || !_safeMode || !_playerReady)
                throw new RewardSelectionException("rewardTarget", "连接或关卡状态已变化，指定奖励未开启。");
            _chestReward.TargetItemId = target.Id;
            _chestReward.TargetItemName = target.Name;
            _chestReward.Enabled = enable;
            _chestReward.UpgradeEnabled = upgrade;
            _chestReward.Message = enable ? "后续拾取箱子时，保证一个奖励为：" + target.Name : "已恢复后续箱子的正常随机奖励。";
            _chestReward.UpgradeMessage = upgrade ? "后续升级时，保证一个可选项为：" + target.Name : "已恢复后续升级的正常随机选项。";
        }
    }

    private sealed class RewardSelectionException : InvalidOperationException
    {
        internal string Scope { get; }
        internal RewardSelectionException(string scope, string message, Exception? inner = null) : base(message, inner) => Scope = scope;
    }

    private void SetFeature(Request request)
    {
        lock (_gate)
        {
            if (request.Feature == null || !_features.TryGetValue(request.Feature, out var feature))
                throw new InvalidOperationException("未知功能。");
            if (request.Enabled != false && !feature.Supported)
                throw new InvalidOperationException(feature.Reason ?? "此功能在当前版本不可用。");
            if (request.Enabled == true && !_safeMode)
                throw new InvalidOperationException("首版仅支持单人模式；请等待单人主菜单或单人关卡就绪。");
            if (request.Feature == FeatureIds.NoGoldSpend && request.Enabled == true && !_backupAuthorized)
                throw new InvalidOperationException("金币功能需要有效的本次启动前存档备份。");
            if (request.Value.HasValue)
            {
                var value = request.Value.Value;
                var range = request.Feature switch
                {
                    FeatureIds.Experience => (1.0, 1000.0),
                    FeatureIds.Damage => (1.0, 1000.0),
                    FeatureIds.Movement => (0.25, 10.0),
                    _ => (1.0, 1.0)
                };
                if (!double.IsFinite(value) || value < range.Item1 || value > range.Item2)
                    throw new InvalidOperationException($"倍率必须在 {range.Item1} 到 {range.Item2} 之间。");
                feature.Value = value;
            }
            feature.Enabled = request.Enabled ?? feature.Enabled;
        }
        _adapter?.FeaturesChanged(request.Feature!);
    }

    private void SetStat(Request request)
    {
        lock (_gate)
        {
            var definition=StatCatalog.Find(request.Stat) ?? throw new InvalidOperationException("未知属性。");
            var stat=_stats[definition.Id];
            bool enabling=request.Enabled ?? stat.Enabled;
            if(enabling && !stat.Supported) throw new InvalidOperationException(stat.Reason ?? "当前版本属性不可用。");
            if(enabling && (!_safeMode || !_playerReady)) throw new InvalidOperationException("请进入单人关卡后再开启属性编辑。");
            if(request.Value.HasValue) { StatCatalog.Validate(definition,request.Value.Value); stat.Value=request.Value.Value; }
            stat.Enabled=enabling;
        }
        _adapter?.StatsChanged();
    }

    internal void DisableStats() { lock (_gate) DisableStatsLocked(); }
    private void DisableStatsLocked() { foreach(var stat in _stats.Values) stat.Enabled=false; }

    internal void Tick()
    {
        if (_disposed) return;
        _pumpReady = true;
        for (int count = 0; count < 32 && _queue.TryDequeue(out var action); count++) action();
        if (!_started && (_gameVerified || !string.IsNullOrEmpty(_versionError) && _versionError != "正在校验游戏版本。"))
        {
            _started = true;
            if (_gameVerified)
            {
                try { _adapter = new GameAdapter(this, _log); _adapter.Initialize(); }
                catch (Exception e) { _log.LogError(e); DisableAll(); _versionError = "游戏接口初始化失败：" + e.Message; }
            }
            else
            {
                foreach (var id in FeatureIds.All) SetCapability(id, false, _versionError);
                foreach (var definition in StatCatalog.All) SetStatCapability(definition.Id,false,_versionError);
                SetChestCapability(false, _versionError);
                SetUpgradeCapability(false, _versionError);
            }
        }
        if (_adapter != null)
        {
            try
            {
                _adapter.Tick();
                lock (_gate)
                {
                    if (_playerReady && !_adapter.PlayerReady)
                    {
                        DisableStatsLocked();
                        _chestReward.Enabled = false;
                        _chestReward.Message = "本局已结束，指定箱子奖励已关闭。";
                        _chestReward.UpgradeEnabled = false;
                        _chestReward.UpgradeMessage = "本局已结束，指定升级选项已关闭。";
                    }
                    _scene = _adapter.Scene;
                    _playerReady = _adapter.PlayerReady;
                    _gold = _adapter.Gold;
                    _safeMode = _adapter.SafeMode;
                    _diagnostics = _adapter.Diagnostics;
                }
            }
            catch (Exception e)
            {
                DisableAll();
                lock (_gate) _playerReady = false;
                _log.LogWarning("Game adapter tick failed; all runtime features disabled: " + e.Message);
            }
        }
    }

    internal TrainerState Snapshot()
    {
        lock (_gate) return new TrainerState
        {
            GameBuild = "25984009", PluginVersion = TrainerPlugin.Version, Scene = _scene,
            PlayerReady = _playerReady, Gold = _gold, BackupAuthorized = _backupAuthorized,
            Diagnostics = _diagnostics.Snapshot(),
            ChestReward = new ChestRewardState
            {
                Supported = _chestReward.Supported, Enabled = _chestReward.Enabled,
                TargetItemId = _chestReward.TargetItemId, TargetItemName = _chestReward.TargetItemName,
                Message = _chestReward.Message, ProcessedCount = _chestReward.ProcessedCount,
                LastRewardItemId = _chestReward.LastRewardItemId, LastRewardName = _chestReward.LastRewardName,
                UpgradeSupported = _chestReward.UpgradeSupported, UpgradeEnabled = _chestReward.UpgradeEnabled,
                UpgradeMessage = _chestReward.UpgradeMessage, UpgradeProcessedCount = _chestReward.UpgradeProcessedCount,
                LastUpgradeItemId = _chestReward.LastUpgradeItemId, LastUpgradeName = _chestReward.LastUpgradeName
            },
            Features = _features.Values.Select(f => new FeatureState
                { Id = f.Id, Supported = f.Supported, Enabled = f.Enabled, Value = f.Value, Reason = f.Reason }).ToList(),
            Stats = _stats.Values.Select(s => new StatState
                { Id=s.Id, Supported=s.Supported, Enabled=s.Enabled, Value=s.Value, OriginalValue=s.OriginalValue, CurrentValue=s.CurrentValue, Reason=s.Reason }).ToList()
        };
    }

    internal void DisableAll() { lock (_gate) DisableAllLocked(); }
    private void DisableAllLocked()
    {
        foreach (var feature in _features.Values) feature.Enabled = false;
        DisableStatsLocked();
        _chestReward.Enabled = false;
        _chestReward.UpgradeEnabled = false;
        if (_chestReward.Supported) _chestReward.Message = "指定奖励已关闭；后续箱子使用正常随机奖励。";
        if (_chestReward.UpgradeSupported) _chestReward.UpgradeMessage = "指定奖励已关闭；后续升级使用正常随机选项。";
    }
    public void Dispose() { _disposed = true; DisableAll(); _adapter?.Dispose(); }
}
