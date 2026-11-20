using System.Text.Json;

namespace Trainer.Shared;

public static class FeatureIds
{
    public const string Health = "health";
    public const string InstantKill = "instantKill";
    public const string Experience = "experience";
    public const string Damage = "damage";
    public const string Movement = "movement";
    public const string Cooldown = "cooldown";
    public const string NoGoldSpend = "noGoldSpend";
    public static readonly string[] All = { Health, InstantKill, Experience, Damage, Movement, Cooldown, NoGoldSpend };
}

public static class PipeProtocol
{
    public const int Version = 1;
    public const int MaxMessageCharacters = 262144;
    public static string PipeName(int processId) => $"VampireSurvivors.Trainer.v1.{processId}";
    public static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true
    };
}

public sealed class Request
{
    public string Id { get; set; } = Guid.NewGuid().ToString("N");
    public string Kind { get; set; } = "getState";
    public string? Feature { get; set; }
    public string? Stat { get; set; }
    public string? ItemId { get; set; }
    public int CatalogOffset { get; set; }
    public int CatalogLimit { get; set; } = 200;
    public bool? Enabled { get; set; }
    public bool? UpgradeEnabled { get; set; }
    public double? Value { get; set; }
    public string? BackupToken { get; set; }
}

public sealed class Response
{
    public string Id { get; set; } = "";
    public bool Ok { get; set; }
    public string? Error { get; set; }
    public string? ErrorScope { get; set; }
    public TrainerState? State { get; set; }
    public List<RewardItemDefinition>? ItemCatalog { get; set; }
    public int? ItemCatalogTotal { get; set; }
    public bool ItemCatalogLoading { get; set; }
    public bool ItemIconsSupported { get; set; }
    public RewardItemIcon? ItemIcon { get; set; }
}

public sealed class RewardItemIcon
{
    public string Id { get; set; } = "";
    public byte[] Png { get; set; } = Array.Empty<byte>();
    public string? RgbaHex { get; set; }
    public int Width { get; set; }
    public int Height { get; set; }
    public bool Pending { get; set; }
}

public sealed class TrainerState
{
    public string GameBuild { get; set; } = "";
    public string PluginVersion { get; set; } = "";
    public string Scene { get; set; } = "";
    public bool PlayerReady { get; set; }
    public double? Gold { get; set; }
    // Optional native capabilities: an older backend leaves these unknown.
    public bool? WalletReady { get; set; }
    public bool? GoldWriteSupported { get; set; }
    public bool? GoldWriteReady { get; set; }
    public bool BackupAuthorized { get; set; }
    public List<FeatureState> Features { get; set; } = new();
    public List<StatState> Stats { get; set; } = new();
    public ChestRewardState ChestReward { get; set; } = new();
    public Dictionary<string, string> Diagnostics { get; set; } = new();
}

public sealed class RewardItemDefinition
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public string? EnglishName { get; set; }
    public bool? IsEvolution { get; set; }
    public string Kind { get; set; } = "weapon";
    public int MaxLevel { get; set; }
    public string? Description { get; set; }
    public bool? UpgradeAllowed { get; set; }
    public string? UpgradeUnsupportedReason { get; set; }
}

public sealed class ChestRewardState
{
    public bool Supported { get; set; }
    public bool Enabled { get; set; }
    public string? TargetItemId { get; set; }
    public string? TargetItemName { get; set; }
    public string? Message { get; set; }
    public int ProcessedCount { get; set; }
    public string? LastRewardItemId { get; set; }
    public string? LastRewardName { get; set; }
    public bool UpgradeSupported { get; set; }
    public bool UpgradeEnabled { get; set; }
    public string? UpgradeMessage { get; set; }
    public int UpgradeProcessedCount { get; set; }
    public string? LastUpgradeItemId { get; set; }
    public string? LastUpgradeName { get; set; }
    // Offered choices and confirmed native grants are separate observations.
    public int UpgradeGrantedCount { get; set; }
    public string? LastUpgradeGrantedItemId { get; set; }
    public string? LastUpgradeGrantedName { get; set; }
}

public sealed class FeatureState
{
    public string Id { get; set; } = "";
    public bool Supported { get; set; }
    public bool Enabled { get; set; }
    public double Value { get; set; } = 1;
    public string? Reason { get; set; }
}

public sealed class StatState
{
    public string Id { get; set; } = "";
    public bool Supported { get; set; }
    public bool Enabled { get; set; }
    public double Value { get; set; }
    public double? OriginalValue { get; set; }
    public double? CurrentValue { get; set; }
    // Active binding sample, including a consumed temporary budget. This is
    // not evidence that a natural gameplay getter has read the bound value.
    public double? BoundValue { get; set; }
    public string? Reason { get; set; }
}

public sealed class BackupProof
{
    public string AppId { get; set; } = "1794680";
    public DateTime CreatedUtc { get; set; }
    public string GameDirectory { get; set; } = "";
    public string GameAssemblySha256 { get; set; } = "";
    public string MetadataSha256 { get; set; } = "";
    public List<BackupProofFile> Files { get; set; } = new();
}

public sealed class BackupProofFile
{
    public string OriginalPath { get; set; } = "";
    public string BackupPath { get; set; } = "";
    public string Sha256 { get; set; } = "";
}
