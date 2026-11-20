using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Win32.SafeHandles;

namespace Trainer.Core;

public enum GameDiscoverySource
{
    RunningProcess,
    SavedDirectory,
    SteamManifest,
    SteamLibrary,
    DefaultDirectory
}

public sealed record RunningGameProcess(int ProcessId, string? ExecutablePath)
{
    public string? Directory
    {
        get
        {
            try { return string.IsNullOrWhiteSpace(ExecutablePath) ? null : Path.GetDirectoryName(ExecutablePath); }
            catch (ArgumentException) { return null; }
        }
    }
}

public sealed record GameInstallationCandidate(string Directory, IReadOnlyList<GameDiscoverySource> Sources, IReadOnlyList<int> ProcessIds)
{
    public bool IsRunning => ProcessIds.Count > 0;
}

public sealed record GameDiscoveryResult(
    GameInstallationCandidate? Selected,
    IReadOnlyList<GameInstallationCandidate> Candidates,
    IReadOnlyList<RunningGameProcess> RunningProcesses,
    IReadOnlyList<string> Warnings,
    bool ProcessScanFailed = false)
{
    public bool RequiresSelection => Selected is null && Candidates.Count > 1;
    public bool IsGameRunning => RunningProcesses.Count > 0;
}

public interface IGameProcessProvider
{
    IReadOnlyList<RunningGameProcess> GetRunningGameProcesses();
}

/// <summary>Reads process identity/path only. It neither attaches a debugger nor changes a process.</summary>
public sealed class WindowsGameProcessProvider : IGameProcessProvider
{
    public IReadOnlyList<RunningGameProcess> GetRunningGameProcesses()
    {
        if (!OperatingSystem.IsWindows()) return Array.Empty<RunningGameProcess>();
        var processes = Process.GetProcessesByName("VampireSurvivors");
        var found = new List<RunningGameProcess>();
        try
        {
            foreach (var process in processes)
            {
                int processId;
                try { processId = process.Id; }
                catch (Exception error) when (error is Win32Exception or InvalidOperationException or NotSupportedException)
                {
                    // A process that disappeared must not discard the remaining scan results.
                    continue;
                }
                string? executable = null;
                try { executable = ReadLimitedProcessImage(processId) ?? process.MainModule?.FileName; }
                catch (Exception error) when (error is Win32Exception or InvalidOperationException or NotSupportedException or UnauthorizedAccessException or SecurityException)
                {
                    // The process remains visible even when its image path is inaccessible.
                }
                found.Add(new RunningGameProcess(processId, executable));
            }
        }
        finally { foreach (var process in processes) process.Dispose(); }
        return found.OrderBy(process => process.ProcessId).ToArray();
    }

    private static string? ReadLimitedProcessImage(int processId)
    {
        // Querying the image path needs only PROCESS_QUERY_LIMITED_INFORMATION, not VM_READ/debug access.
        using var process = OpenProcess(0x1000, false, processId);
        if (process.IsInvalid) return null;
        var buffer = new StringBuilder(32768);
        uint length = (uint)buffer.Capacity;
        return QueryFullProcessImageName(process, 0, buffer, ref length) ? buffer.ToString() : null;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern SafeProcessHandle OpenProcess(uint access, [MarshalAs(UnmanagedType.Bool)] bool inheritHandle, int processId);
    [DllImport("kernel32.dll", EntryPoint = "QueryFullProcessImageNameW", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool QueryFullProcessImageName(SafeProcessHandle process, uint flags, StringBuilder path, ref uint length);
}

public sealed partial class GamePaths
{
    /// <summary>Discovers known installations without scanning drives or reading saves.</summary>
    public GameDiscoveryResult DiscoverGame(string? savedDirectory = null)
    {
        var warnings = new List<string>();
        IReadOnlyList<RunningGameProcess> processes;
        bool processScanFailed = false;
        try { processes = _gameProcesses.GetRunningGameProcesses(); }
        catch (Exception error) when (IsDiscoveryError(error))
        {
            processes = Array.Empty<RunningGameProcess>();
            processScanFailed = true;
            warnings.Add("暂时无法读取游戏进程列表；仍可通过 Steam 安装信息识别游戏。");
        }
        var candidates = new Dictionary<string, CandidateBuilder>(StringComparer.OrdinalIgnoreCase);
        void Add(string? directory, GameDiscoverySource source, int? processId = null)
        {
            if (!TryGetGameDirectory(directory, out var full)) return;
            if (!candidates.TryGetValue(full, out var candidate))
                candidates[full] = candidate = new CandidateBuilder(full);
            candidate.Sources.Add(source);
            if (processId.HasValue) candidate.ProcessIds.Add(processId.Value);
        }

        foreach (var process in processes)
        {
            if (!TryGetProcessDirectory(process.ExecutablePath, out var directory)) continue;
            Add(directory, GameDiscoverySource.RunningProcess, process.ProcessId);
        }
        if (processes.Any(process => string.IsNullOrWhiteSpace(process.ExecutablePath)))
            warnings.Add("已发现游戏进程，但部分进程的安装路径不可读取；可先连接游戏，或使用 Steam 识别到的目录。");
        Add(savedDirectory, GameDiscoverySource.SavedDirectory);

        var libraries = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var steam in SteamDirectories)
        {
            libraries.Add(steam);
            var folderFile = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
            if (!TryReadDiscoveryText(folderFile, out var folderText, out bool unreadable))
            {
                if (unreadable) AddWarning(warnings, "部分 Steam 游戏库信息不可读取；已跳过并继续识别其他来源。");
                continue;
            }
            foreach (var path in ReadLibraryPaths(folderText)) libraries.Add(path);
        }
        foreach (var library in libraries)
        {
            string steamapps = Path.Combine(library, "steamapps");
            var manifest = Path.Combine(steamapps, "appmanifest_" + AppId + ".acf");
            bool manifestRead = TryReadDiscoveryText(manifest, out var manifestText, out bool unreadable);
            if (unreadable) AddWarning(warnings, "部分 Steam 游戏安装信息不可读取；已跳过并继续识别其他来源。");
            if (manifestRead)
            {
                string? appId = ReadVdfValue(manifestText, "appid");
                // A mismatched manifest cannot establish a path even if named like this game's manifest.
                if (appId is not null && appId != AppId) continue;
                string? installDirectory = ReadVdfValue(manifestText, "installdir");
                if (TryResolveInstallDirectory(steamapps, installDirectory, out var installed))
                    Add(installed, GameDiscoverySource.SteamManifest);
            }
            // Older manifests can lack installdir. This bounded conventional path is a fallback only.
            Add(Path.Combine(steamapps, "common", "Vampire Survivors"), GameDiscoverySource.SteamLibrary);
        }
        if (_useConventionalDefault) Add(DefaultGameDirectory, GameDiscoverySource.DefaultDirectory);

        var installations = candidates.Values.Select(candidate => new GameInstallationCandidate(
                candidate.Directory, candidate.Sources.Order().ToArray(), candidate.ProcessIds.Order().ToArray()))
            .OrderByDescending(candidate => candidate.IsRunning)
            .ThenBy(candidate => candidate.Sources.Min())
            .ThenBy(candidate => candidate.Directory, StringComparer.OrdinalIgnoreCase).ToArray();
        var active = installations.Where(candidate => candidate.IsRunning).ToArray();
        GameInstallationCandidate? selected = active.Length == 1 ? active[0] : null;
        if (active.Length == 0)
        {
            selected = installations.FirstOrDefault(candidate => candidate.Sources.Contains(GameDiscoverySource.SavedDirectory));
            if (selected is null && installations.Length == 1) selected = installations[0];
        }
        if (active.Length > 1)
            warnings.Add("发现来自多个安装目录的游戏进程，请明确选择本次使用的游戏目录。");
        else if (selected is null && installations.Length > 1)
            warnings.Add("发现多个有效游戏安装，请选择本次使用的目录；选择后会记住。");
        return new GameDiscoveryResult(selected, installations, processes, warnings, processScanFailed);
    }

    public static bool IsGameDirectory(string? directory) => TryGetGameDirectory(directory, out _);

    private static bool TryGetGameDirectory(string? directory, out string full)
    {
        full = "";
        if (string.IsNullOrWhiteSpace(directory) || !Path.IsPathFullyQualified(directory)) return false;
        try
        {
            full = CoreFiles.Full(directory);
            return File.Exists(Path.Combine(full, "VampireSurvivors.exe")) &&
                File.Exists(Path.Combine(full, "GameAssembly.dll")) &&
                File.Exists(Path.Combine(full, "VampireSurvivors_Data", "il2cpp_data", "Metadata", "global-metadata.dat"));
        }
        catch (Exception error) when (IsDiscoveryError(error)) { full = ""; return false; }
    }

    private static bool TryGetProcessDirectory(string? executable, out string? directory)
    {
        directory = null;
        if (string.IsNullOrWhiteSpace(executable)) return false;
        try
        {
            if (!Path.IsPathFullyQualified(executable) || !Path.GetFileName(executable).Equals("VampireSurvivors.exe", StringComparison.OrdinalIgnoreCase)) return false;
            directory = Path.GetDirectoryName(executable);
            return directory is not null;
        }
        catch (Exception error) when (IsDiscoveryError(error)) { return false; }
    }

    private static bool TryResolveInstallDirectory(string steamapps, string? installDirectory, out string? directory)
    {
        directory = null;
        if (string.IsNullOrWhiteSpace(installDirectory)) return false;
        try
        {
            // Steam installdir is one folder name, not an arbitrary absolute path or traversal.
            if (installDirectory is "." or ".." || installDirectory.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || installDirectory.Contains('/') || installDirectory.Contains('\\') || installDirectory.Contains(':')) return false;
            directory = Path.Combine(steamapps, "common", installDirectory);
            return true;
        }
        catch (Exception error) when (IsDiscoveryError(error)) { return false; }
    }

    private static bool TryReadDiscoveryText(string path, out string text, out bool unreadable)
    {
        text = ""; unreadable = false;
        try
        {
            // A malformed or unexpectedly large local metadata file is skipped, never fully scanned.
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            if (stream.Length > 4 * 1024 * 1024) { unreadable = true; return false; }
            using var reader = new StreamReader(stream, Encoding.UTF8, true);
            text = reader.ReadToEnd();
            return true;
        }
        catch (FileNotFoundException) { return false; }
        catch (DirectoryNotFoundException) { return false; }
        catch (Exception error) when (IsDiscoveryError(error)) { unreadable = true; return false; }
    }

    private static IEnumerable<string> ReadLibraryPaths(string text)
    {
        // Both current libraryfolders path entries and the older flat numbered format are supported.
        Match[] matches;
        try { matches = Regex.Matches(text, "\"(?:path|[0-9]+)\"\\s+\"((?:\\\\.|[^\"\\\\])*)\"", RegexOptions.IgnoreCase, TimeSpan.FromSeconds(1)).Cast<Match>().ToArray(); }
        catch (RegexMatchTimeoutException) { yield break; }
        foreach (Match match in matches)
        {
            var value = DecodeVdfValue(match.Groups[1].Value);
            string? full = null;
            try { if (Path.IsPathFullyQualified(value)) full = CoreFiles.Full(value); }
            catch (Exception error) when (IsDiscoveryError(error)) { }
            if (full is not null) yield return full;
        }
    }

    private static string? ReadVdfValue(string text, string key)
    {
        try
        {
            var match = Regex.Match(text, "\"" + Regex.Escape(key) + "\"\\s+\"((?:\\\\.|[^\"\\\\])*)\"", RegexOptions.IgnoreCase, TimeSpan.FromSeconds(1));
            return match.Success ? DecodeVdfValue(match.Groups[1].Value) : null;
        }
        catch (RegexMatchTimeoutException) { return null; }
    }

    private static string[] NormalizeSteamRoots(IEnumerable<string> roots)
    {
        var valid = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var root in roots)
        {
            try { if (!string.IsNullOrWhiteSpace(root) && Path.IsPathFullyQualified(root)) valid.Add(CoreFiles.Full(root)); }
            catch (Exception error) when (IsDiscoveryError(error)) { }
        }
        return valid.ToArray();
    }

    private static string DecodeVdfValue(string value) => value.Replace("\\\"", "\"", StringComparison.Ordinal).Replace("\\\\", "\\", StringComparison.Ordinal);
    private static bool IsDiscoveryError(Exception error) => error is IOException or UnauthorizedAccessException or SecurityException or ArgumentException or InvalidOperationException or NotSupportedException or Win32Exception or RegexMatchTimeoutException;
    private static void AddWarning(List<string> warnings, string message) { if (!warnings.Contains(message)) warnings.Add(message); }

    private sealed class CandidateBuilder(string directory)
    {
        public string Directory { get; } = directory;
        public HashSet<GameDiscoverySource> Sources { get; } = new();
        public HashSet<int> ProcessIds { get; } = new();
    }
}
