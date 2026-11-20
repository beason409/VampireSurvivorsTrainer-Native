// Test substitutes for native/Unity dependencies only. TrainerRuntime itself is linked unchanged
// from the shipped plugin source, so session/queue behavior is exercised without running a game.
namespace BepInEx.Logging
{
    internal sealed class ManualLogSource
    {
        internal void LogWarning(object message) { }
        internal void LogError(object message) { }
    }
}

namespace Trainer.Plugin
{
    internal static class TrainerPlugin { internal const string Version = "runtime-fixture"; }
    internal static class BackupVerifier
    {
        internal static bool VerifyGame(out string error) { error = ""; return true; }
    }
    internal sealed class GameAdapter : IDisposable
    {
        internal static GameAdapter? LastCreated;
        private readonly TrainerRuntime runtime;
        internal GameAdapter(TrainerRuntime runtime, BepInEx.Logging.ManualLogSource log) { this.runtime = runtime; LastCreated = this; }
        internal DiagnosticStore Diagnostics { get; } = new();
        internal bool PlayerReady { get; set; } = true;
        internal bool SafeMode { get; set; } = true;
        internal string Scene => PlayerReady ? "fixture-run" : "fixture-menu";
        internal double? Gold { get; set; } = 100;
        internal int GoldWrites { get; private set; }
        internal bool RejectChestTarget { get; set; }
        internal Action? DuringChestValidation { get; set; }
        internal void Initialize() { foreach (var feature in Trainer.Shared.FeatureIds.All) runtime.SetCapability(feature, true); foreach(var stat in Trainer.Shared.StatCatalog.All) runtime.SetStatCapability(stat.Id,stat.Id!=Trainer.Shared.StatIds.Seal,"fixture unsupported"); runtime.SetChestCapability(true); runtime.SetUpgradeCapability(true); }
        internal IReadOnlyList<Trainer.Shared.RewardItemDefinition> GetRewardItemCatalog() => new[]
        {
            new Trainer.Shared.RewardItemDefinition { Id="TEST_WAND",Name="魔杖（测试）",EnglishName="Test Wand",Kind="weapon",MaxLevel=8,UpgradeAllowed=true },
            new Trainer.Shared.RewardItemDefinition { Id="TEST_SPINACH",Name="菠菜（测试）",EnglishName="Test Spinach",Kind="passive",MaxLevel=5,UpgradeAllowed=true },
            new Trainer.Shared.RewardItemDefinition { Id="TEST_EVOLUTION",Name="进化武器（测试）",EnglishName="Test Evolution",IsEvolution=true,Kind="weapon",MaxLevel=1,UpgradeAllowed=true },
            new Trainer.Shared.RewardItemDefinition { Id="TEST_UNSUPPORTED",Name="进化被动（测试）",EnglishName="Test Unsupported",IsEvolution=true,Kind="passive",MaxLevel=1,UpgradeAllowed=false,UpgradeUnsupportedReason="进化被动升级尚未适配" }
        };
        internal void EnableRewardIconSupport() { Diagnostics["chest.catalog.icons"] = "supported"; }
        internal List<string?> RewardIconRequests { get; } = new();
        internal Trainer.Shared.RewardItemIcon GetRewardItemIcon(string? id)
        {
            RewardIconRequests.Add(id);
            return new() { Id = id ?? "" };
        }
        internal void ValidateChestRewardTarget(string id) { DuringChestValidation?.Invoke(); if (RejectChestTarget) throw new InvalidOperationException("fixture target unavailable"); }
        internal void ValidateUpgradeRewardTarget(string id)
        {
            ValidateChestRewardTarget(id);
            if (id=="TEST_UNSUPPORTED") throw new InvalidOperationException("进化被动升级尚未适配，可仅用于宝箱。");
        }
        internal void Tick() { }
        internal void FeaturesChanged(string feature) { }
        internal void StatsChanged() { }
        internal void SetGold(double amount) { GoldWrites++; Gold = amount; }
        public void Dispose() { }
    }
}
