using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace Trainer.Core;

public sealed partial class GamePaths
{
    public const string AppId = "1794680";
    public static readonly string DefaultGameDirectory = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Steam", "steamapps", "common", "Vampire Survivors");
    public string ProfileDirectory { get; }
    public IReadOnlyList<string> SteamDirectories { get; }
    private readonly IGameProcessProvider _gameProcesses;
    private readonly bool _useConventionalDefault;

    public GamePaths(string? profileDirectory = null, IEnumerable<string>? steamRoots = null, IGameProcessProvider? gameProcesses = null)
    {
        ProfileDirectory = CoreFiles.Full(profileDirectory ?? Environment.GetFolderPath(Environment.SpecialFolder.UserProfile));
        SteamDirectories = NormalizeSteamRoots(steamRoots ?? FindSteamDirectories());
        _gameProcesses = gameProcesses ?? new WindowsGameProcessProvider();
        _useConventionalDefault = steamRoots is null;
    }

    public string? DiscoverGameDirectory() => DiscoverGame().Selected?.Directory;

    public IReadOnlyList<SaveLocation> GetSaveLocations(string gameDirectory)
    {
        var result = new List<SaveLocation>();
        var manifest = FindAppManifest(gameDirectory);
        var owner = manifest != null ? ReadManifestValue(manifest, "LastOwner") : null;
        string? account = null;
        if (ulong.TryParse(owner, out var steamId) && steamId >= 76561197960265728UL)
            account = (steamId - 76561197960265728UL).ToString(System.Globalization.CultureInfo.InvariantCulture);
        var appRoots = new List<string>();
        foreach (var steam in SteamDirectories)
        {
            var userdata = Path.Combine(steam, "userdata");
            if (account != null) { appRoots.Add(Path.Combine(userdata, account, AppId)); continue; }
            if (Directory.Exists(userdata))
                appRoots.AddRange(Directory.EnumerateDirectories(userdata).Where(p => ulong.TryParse(Path.GetFileName(p), out _)).Select(p => Path.Combine(p, AppId)).Where(Directory.Exists));
        }
        if (account == null && appRoots.Count(p => Directory.Exists(p)) > 1) throw new InvalidOperationException("发现多个 Steam 存档账户，无法确定当前账户；请先通过 Steam 启动并退出一次游戏。");
        foreach (var app in appRoots.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            result.Add(new SaveLocation(Path.Combine(app, "remote"), true));
            result.Add(new SaveLocation(Path.Combine(app, "remotecache.vdf"), false));
        }
        result.Add(new SaveLocation(Path.Combine(ProfileDirectory, "AppData", "Roaming", "Vampire_Survivors", "saves"), true));
        result.Add(new SaveLocation(Path.Combine(ProfileDirectory, "AppData", "Roaming", "Vampire_Survivors_Data", "saves"), true));
        if (account != null) result.Add(new SaveLocation(Path.Combine(ProfileDirectory, "AppData", "Roaming", "Vampire_Survivors_" + account, "saves"), true));
        var local = Path.Combine(ProfileDirectory, "AppData", "LocalLow", "poncle", "Vampire Survivors");
        result.Add(new SaveLocation(Path.Combine(local, "prefs.bin"), false));
        result.Add(new SaveLocation(Path.Combine(local, "prefs.bin.bak"), false));
        return result;
    }

    public IEnumerable<string> EnumerateSaveFiles(string gameDirectory)
    {
        foreach (var location in GetSaveLocations(gameDirectory))
        {
            CoreFiles.RejectReparse(location.Path);
            if (location.IsDirectory && Directory.Exists(location.Path))
            { foreach (var file in CoreFiles.EnumerateFiles(location.Path)) yield return file; }
            else if (!location.IsDirectory && File.Exists(location.Path)) yield return location.Path;
        }
    }

    public bool IsAllowedSavePath(string gameDirectory, string path) => GetSaveLocations(gameDirectory).Any(location => location.IsDirectory ? CoreFiles.Within(path, location.Path) && !CoreFiles.Same(path, location.Path) : CoreFiles.Same(path, location.Path));
    public bool IsSteamSaveData(string gameDirectory, string path) => string.Equals(Path.GetFileName(path), "SaveData", StringComparison.OrdinalIgnoreCase) && GetSaveLocations(gameDirectory).Any(location => location.IsDirectory && location.Path.EndsWith(Path.Combine(AppId, "remote"), StringComparison.OrdinalIgnoreCase) && CoreFiles.Within(path, location.Path));

    public static string? FindAppManifest(string gameDirectory)
    {
        var common = Directory.GetParent(CoreFiles.Full(gameDirectory));
        var steamapps = common?.Parent;
        if (steamapps == null || !steamapps.Name.Equals("steamapps", StringComparison.OrdinalIgnoreCase)) return null;
        var path = Path.Combine(steamapps.FullName, "appmanifest_" + AppId + ".acf");
        return File.Exists(path) ? path : null;
    }
    public static string? ReadManifestValue(string path, string key)
    {
        var match = Regex.Match(File.ReadAllText(path), "\"" + Regex.Escape(key) + "\"\\s+\"([^\"]*)\"", RegexOptions.IgnoreCase);
        return match.Success ? match.Groups[1].Value : null;
    }
    private static IEnumerable<string> FindSteamDirectories()
    {
        var roots = new List<string>();
        if (OperatingSystem.IsWindows())
        {
            try
            {
                using var key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam");
                if (key?.GetValue("SteamPath") is string steamPath) roots.Add(steamPath);
            }
            catch (Exception error) when (error is System.Security.SecurityException or UnauthorizedAccessException or System.IO.IOException)
            {
                // Restricted desktop sessions may deny HKCU reads; conventional Steam paths still work.
            }
            foreach (var view in new[] { RegistryView.Registry32, RegistryView.Registry64 })
            {
                try
                {
                    using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view);
                    using var key = machine.OpenSubKey(@"Software\Valve\Steam");
                    if (key?.GetValue("InstallPath") is string installPath) roots.Add(installPath);
                }
                catch (Exception error) when (error is System.Security.SecurityException or UnauthorizedAccessException or System.IO.IOException)
                {
                    // Steam discovery never requests elevation for a denied registry view.
                }
            }
        }
        var program = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);
        if (program.Length > 0) roots.Add(Path.Combine(program, "Steam"));
        return roots;
    }
}
