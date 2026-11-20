using System.Text.Json;
using Trainer.Shared;

namespace Trainer.Desktop;

internal sealed class DesktopSettings
{
    private static readonly JsonSerializerOptions SaveOptions = new(PipeProtocol.JsonOptions) { WriteIndented = true };
    public string GameDirectory { get; set; } = "";
    public string Language { get; set; } = "zh-CN";
    public Dictionary<string, int> Hotkeys { get; set; } = HotkeyService.CreateDefaults();
    public Dictionary<string, decimal> Values { get; set; } = new()
    { [FeatureIds.Experience] = 10, [FeatureIds.Damage] = 10, [FeatureIds.Movement] = 3 };

    public static DesktopSettings Load(string path)
    {
        try
        {
            var settings = File.Exists(path) ? JsonSerializer.Deserialize<DesktopSettings>(File.ReadAllText(path), PipeProtocol.JsonOptions) : null;
            if (settings is null) return new();
            settings.Language = UiLocalization.NormalizeLanguage(settings.Language);
            var defaults = new DesktopSettings();
            if (string.IsNullOrWhiteSpace(settings.GameDirectory)) settings.GameDirectory = defaults.GameDirectory;
            settings.Hotkeys ??= new(); settings.Values ??= new();
            var used = new HashSet<int>();
            var normalized = new Dictionary<string, int>();
            foreach (string id in HotkeyService.Actions)
            {
                int key = settings.Hotkeys.TryGetValue(id, out int stored) ? stored : defaults.Hotkeys[id];
                if (key != 0 && (!HotkeyService.IsValid((Keys)key) || used.Contains(key)))
                {
                    key = FeatureIds.All.Contains(id)
                        ? Enumerable.Range((int)Keys.F1, 24).FirstOrDefault(candidate => !used.Contains(candidate)) : 0;
                }
                normalized[id] = key;
                if (key != 0) used.Add(key);
            }
            settings.Hotkeys = normalized;
            foreach (var pair in defaults.Values)
            {
                decimal min = pair.Key == FeatureIds.Movement ? .25m : 1m;
                decimal max = pair.Key == FeatureIds.Movement ? 10m : 1000m;
                settings.Values[pair.Key] = settings.Values.TryGetValue(pair.Key, out decimal value) ? Math.Clamp(value, min, max) : pair.Value;
            }
            return settings;
        }
        catch { return new(); }
    }

    public void Save(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        string temp = path + ".tmp";
        File.WriteAllText(temp, JsonSerializer.Serialize(this, SaveOptions));
        File.Move(temp, path, true);
    }
}

internal static class RuntimePaths
{
    public static string BaseDirectory { get; } = AppContext.BaseDirectory;
    public static string DataDirectory { get; } = FindDataDirectory();
    public static string SettingsPath => Path.Combine(DataDirectory, "desktop-settings.json");
    public static string PayloadDirectory => FindAsset("payload", true);
    public static string SupportedBuildPath => FindAsset("supported-game.json", false);

    private static string FindDataDirectory()
    {
        string? overridePath = Environment.GetEnvironmentVariable("VS_TRAINER_DATA");
        if (!string.IsNullOrWhiteSpace(overridePath)) return Path.GetFullPath(overridePath);
        var parent = new DirectoryInfo(BaseDirectory);
        for (int i = 0; parent is not null && i < 8; i++, parent = parent.Parent)
            if (Directory.Exists(Path.Combine(parent.FullName, "src", "Trainer.Desktop"))) return parent.FullName;
        return Path.Combine(BaseDirectory, "data");
    }

    private static string FindAsset(string name, bool isDirectory)
    {
        var parent = new DirectoryInfo(BaseDirectory);
        for (int i = 0; parent is not null && i < 8; i++, parent = parent.Parent)
        {
            foreach (string candidate in new[] { Path.Combine(parent.FullName, name), Path.Combine(parent.FullName, "artifacts", name), Path.Combine(parent.FullName, "data", name) })
                if (isDirectory ? Directory.Exists(candidate) : File.Exists(candidate)) return candidate;
        }
        return Path.Combine(BaseDirectory, name);
    }
}
