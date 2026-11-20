using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Trainer.Core;

public sealed class GameFingerprint
{
    public string AppId { get; set; } = "1794680";
    public string BuildId { get; set; } = "";
    public string UnityVersion { get; set; } = "";
    public string GameDirectory { get; set; } = "";
    public string GameAssemblySha256 { get; set; } = "";
    public string MetadataSha256 { get; set; } = "";
}

public sealed record VersionCheck(bool IsSupported, string Message, GameFingerprint? Actual);
public sealed record BackupResult(string Id, string DirectoryPath, string ManifestPath, string ProofPath, DateTime CreatedUtc, int FileCount);
public sealed record RestoreResult(BackupResult SafetyBackup, int RestoredFileCount);
public sealed record InstallResult(int FileCount, string ManifestPath);
public sealed record UninstallResult(int RemovedOrRestoredCount, IReadOnlyList<string> PreservedModifiedFiles);
public sealed record SaveLocation(string Path, bool IsDirectory);

public interface IProcessState
{
    bool IsGameRunning { get; }
    bool IsSteamRunning { get; }
}

public sealed class WindowsProcessState : IProcessState
{
    public bool IsGameRunning => IsRunning("VampireSurvivors");
    public bool IsSteamRunning => IsRunning("steam");
    private static bool IsRunning(string name)
    {
        var processes = Process.GetProcessesByName(name);
        try { return processes.Length > 0; }
        finally { foreach (var process in processes) process.Dispose(); }
    }
}

internal static class CoreFiles
{
    internal static readonly JsonSerializerOptions Json = new()
    {
        PropertyNameCaseInsensitive = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true
    };

    internal static string Full(string path) => System.IO.Path.TrimEndingDirectorySeparator(System.IO.Path.GetFullPath(path));
    internal static bool Same(string left, string right) => string.Equals(Full(left), Full(right), StringComparison.OrdinalIgnoreCase);
    internal static bool Within(string path, string root) => Same(path, root) || Full(path).StartsWith(Full(root) + System.IO.Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
    internal static string Hash(string path)
    {
        using var file = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(file));
    }
    internal static string Key(string path) => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(Full(path).ToUpperInvariant())))[..16];
    internal static T Read<T>(string path) => JsonSerializer.Deserialize<T>(File.ReadAllText(path), Json) ?? throw new InvalidDataException("文件内容无效：" + path);
    internal static void Write<T>(string path, T value)
    {
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(path)!);
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try { File.WriteAllText(temporary, JsonSerializer.Serialize(value, Json), new UTF8Encoding(false)); File.Move(temporary, path, true); }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
    internal static string SafeChild(string root, string relative)
    {
        if (string.IsNullOrWhiteSpace(relative) || System.IO.Path.IsPathRooted(relative)) throw new InvalidDataException("相对路径无效。");
        var result = Full(System.IO.Path.Combine(root, relative));
        if (!Within(result, root) || Same(result, root)) throw new InvalidDataException("文件路径超出了允许目录。");
        RejectReparse(result);
        return result;
    }
    internal static void RejectReparse(string path)
    {
        for (var current = Full(path); !string.IsNullOrEmpty(current); current = System.IO.Path.GetDirectoryName(current))
        {
            if ((File.Exists(current) || Directory.Exists(current)) && (File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("为保护存档，不能操作符号链接或目录联接：" + current);
        }
    }
    internal static void CopyAtomic(string source, string destination, string? expectedHash = null)
    {
        RejectReparse(source); RejectReparse(destination);
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(destination)!);
        var temporary = destination + ".trainer-" + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.Copy(source, temporary, false);
            if (expectedHash != null && !string.Equals(Hash(temporary), expectedHash, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("文件校验失败：" + source);
            File.Move(temporary, destination, true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
    internal static IEnumerable<string> EnumerateFiles(string root) => Directory.EnumerateFiles(root, "*", new EnumerationOptions { RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint, IgnoreInaccessible = false }).Order(StringComparer.OrdinalIgnoreCase);
    internal static void RequireStopped(IProcessState process, bool steam = false)
    {
        if (process.IsGameRunning) throw new InvalidOperationException("请先退出《吸血鬼幸存者》。");
        if (steam && process.IsSteamRunning) throw new InvalidOperationException("恢复存档前请先退出 Steam，防止云同步覆盖。");
    }
}
