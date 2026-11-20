namespace Trainer.Core;

public sealed class InstallationService
{
    private readonly GamePaths paths;
    private readonly string storage;
    private readonly IProcessState processes;
    private readonly TrustedPayloadCatalog? trustedCatalog;

    public InstallationService(GamePaths paths, string storageDirectory, IProcessState? guard = null, TrustedPayloadCatalog? catalog = null)
    { this.paths = paths; storage = CoreFiles.Full(storageDirectory); processes = guard ?? new WindowsProcessState(); trustedCatalog = catalog; }

    public string GetManifestPath(string gameDirectory) => Path.Combine(storage, "installations", CoreFiles.Key(gameDirectory), "manifest.json");
    public bool IsInstalled(string gameDirectory)
    {
        var path = GetManifestPath(gameDirectory);
        if (!File.Exists(path)) return false;
        var journal = ReadJournal(gameDirectory);
        return journal.Completed && journal.Files.Count > 0 && journal.Files.All(file =>
        {
            var installed = CoreFiles.SafeChild(gameDirectory, file.RelativePath);
            return File.Exists(installed) && string.Equals(CoreFiles.Hash(installed), file.InstalledSha256, StringComparison.OrdinalIgnoreCase);
        });
    }

    public InstallResult Install(string gameDirectory, string payloadDirectory, string proofPath, string supportedJsonPath)
    {
        CoreFiles.RequireStopped(processes);
        var game = CoreFiles.Full(gameDirectory);
        var payload = CoreFiles.Full(payloadDirectory);
        if (!Directory.Exists(payload)) throw new DirectoryNotFoundException("找不到插件安装包：" + payload);
        if (CoreFiles.Within(payload, game) || CoreFiles.Within(game, payload)) throw new InvalidOperationException("安装包目录不能与游戏目录相互包含。");
        CoreFiles.RejectReparse(game); CoreFiles.RejectReparse(payload);
        var catalog = trustedCatalog ?? TrustedPayloadCatalog.LoadEmbedded();
        catalog.VerifyPayload(payload);
        var supported = catalog.ReadSupportedBuild(supportedJsonPath);
        var verification = FingerprintService.Verify(game, supported);
        if (!verification.IsSupported) throw new InvalidOperationException(verification.Message);
        BackupService.ValidateProof(paths, storage, game, proofPath);
        // Freeze the trusted copy plan; later source changes never become new expectations.
        var files = catalog.Files.Values.OrderBy(file => file.Path, StringComparer.OrdinalIgnoreCase).Select(file =>
        {
            var source = CoreFiles.SafeChild(payload, file.Path);
            var relative = Path.GetRelativePath(payload, source);
            ValidatePayloadRelative(relative);
            return (Source: source, Relative: relative, Expected: file);
        }).ToArray();
        if (files.Length == 0 || !files.Any(file => file.Relative.Equals("winhttp.dll", StringComparison.OrdinalIgnoreCase)))
            throw new InvalidDataException("安装包缺少 BepInEx 加载文件。");
        var journalPath = GetManifestPath(game);
        CoreFiles.RejectReparse(journalPath);
        var journalDirectory = Path.GetDirectoryName(journalPath)!;
        var journal = File.Exists(journalPath) ? ReadJournal(game) : new InstallationManifest { GameDirectory = game, InstalledUtc = DateTime.UtcNow };
        if (File.Exists(journalPath) && !journal.Completed) throw new InvalidOperationException("上次安装未完成，请先卸载或恢复该次安装。");
        foreach (var file in journal.Files)
        {
            var installed = CoreFiles.SafeChild(game, file.RelativePath);
            if (File.Exists(installed) && !string.Equals(CoreFiles.Hash(installed), file.InstalledSha256, StringComparison.OrdinalIgnoreCase))
                throw new IOException("已安装文件被其他程序修改，请先处理后重试：" + file.RelativePath);
        }
        foreach (var file in files)
        {
            var prior = journal.Files.FirstOrDefault(f => SameRelativePath(f.RelativePath, file.Relative));
            if (prior != null && !string.Equals(prior.InstalledSha256, file.Expected.Sha256, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("安装包版本与已安装版本不同，请先卸载旧版：" + file.Relative);
        }
        journal.Completed = false;
        CoreFiles.Write(journalPath, journal);
        try
        {
            foreach (var file in files)
            {
                CoreFiles.RequireStopped(processes);
                var source = file.Source;
                var relative = file.Relative;
                var destination = CoreFiles.SafeChild(game, relative);
                var digest = file.Expected.Sha256;
                if (new FileInfo(source).Length != file.Expected.Length) throw new InvalidDataException("文件校验失败：" + source);
                var record = journal.Files.FirstOrDefault(f => SameRelativePath(f.RelativePath, relative));
                if (record == null)
                {
                    record = new InstallationFile { RelativePath = relative, InstalledSha256 = digest };
                    if (File.Exists(destination))
                    {
                        record.OriginalBackupRelativePath = Path.Combine("originals", relative);
                        record.OriginalSha256 = CoreFiles.Hash(destination);
                        CoreFiles.CopyAtomic(destination, CoreFiles.SafeChild(journalDirectory, record.OriginalBackupRelativePath), record.OriginalSha256);
                    }
                    journal.Files.Add(record);
                }
                record.InstalledSha256 = digest;
                CoreFiles.Write(journalPath, journal); // Record before replacement, so interrupted installs remain reversible.
                CoreFiles.CopyAtomic(source, destination, digest);
            }
            CoreFiles.RequireStopped(processes);
            journal.Completed = true;
            CoreFiles.Write(journalPath, journal);
            return new InstallResult(files.Length, journalPath);
        }
        catch
        {
            if (!processes.IsGameRunning) Uninstall(game);
            throw;
        }
    }

    public UninstallResult Uninstall(string gameDirectory)
    {
        CoreFiles.RequireStopped(processes);
        var journalPath = GetManifestPath(gameDirectory);
        CoreFiles.RejectReparse(journalPath);
        if (!File.Exists(journalPath)) return new UninstallResult(0, Array.Empty<string>());
        var journal = ReadJournal(gameDirectory);
        var journalDirectory = Path.GetDirectoryName(journalPath)!;
        // Validate the whole journal and original bytes before touching any installed file.
        foreach (var file in journal.Files)
        {
            ValidatePayloadRelative(file.RelativePath);
            CoreFiles.SafeChild(gameDirectory, file.RelativePath);
            if (file.OriginalBackupRelativePath != null)
            {
                var original = CoreFiles.SafeChild(journalDirectory, file.OriginalBackupRelativePath);
                if (!CoreFiles.Within(original, Path.Combine(journalDirectory, "originals")) || !File.Exists(original) || file.OriginalSha256 == null || !string.Equals(CoreFiles.Hash(original), file.OriginalSha256, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("原始文件备份损坏，已停止卸载：" + file.RelativePath);
            }
        }
        var preserved = new List<string>();
        var remaining = new List<InstallationFile>();
        var count = 0;
        foreach (var file in journal.Files.AsEnumerable().Reverse().ToArray())
        {
            CoreFiles.RequireStopped(processes);
            var installed = CoreFiles.SafeChild(gameDirectory, file.RelativePath);
            if (File.Exists(installed))
            {
                string actualHash = CoreFiles.Hash(installed);
                if (!string.Equals(actualHash, file.InstalledSha256, StringComparison.OrdinalIgnoreCase))
                {
                    if (file.OriginalSha256 is not null && string.Equals(actualHash, file.OriginalSha256, StringComparison.OrdinalIgnoreCase))
                    {
                        // A failed atomic copy may never have replaced this original.
                        journal.Files.Remove(file); CoreFiles.Write(journalPath, journal); continue;
                    }
                    preserved.Add(file.RelativePath); remaining.Add(file); continue;
                }
            }
            if (file.OriginalBackupRelativePath != null)
                CoreFiles.CopyAtomic(CoreFiles.SafeChild(journalDirectory, file.OriginalBackupRelativePath), installed, file.OriginalSha256);
            else if (File.Exists(installed)) File.Delete(installed);
            count++;
            journal.Files.Remove(file);
            CoreFiles.Write(journalPath, journal);
        }
        journal.Completed = false;
        CoreFiles.Write(journalPath, journal);
        if (journal.Files.Count == 0)
        {
            File.Move(journalPath, Path.Combine(journalDirectory, "uninstalled-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fffffff") + ".json"));
            // Original backup bytes and generated game caches remain available; no directory-wide delete.
        }
        return new UninstallResult(count, preserved);
    }

    private InstallationManifest ReadJournal(string gameDirectory)
    {
        string path = GetManifestPath(gameDirectory);
        CoreFiles.RejectReparse(path);
        var manifest = CoreFiles.Read<InstallationManifest>(path);
        if (manifest.FormatVersion != 1 || !CoreFiles.Same(manifest.GameDirectory, gameDirectory) || manifest.Files == null || manifest.Files.Select(f => f.RelativePath.Replace('\\', '/')).Distinct(StringComparer.OrdinalIgnoreCase).Count() != manifest.Files.Count)
            throw new InvalidDataException("安装记录与所选游戏目录不一致。");
        foreach (var file in manifest.Files)
        {
            ValidatePayloadRelative(file.RelativePath);
            if (string.IsNullOrEmpty(file.InstalledSha256) || file.InstalledSha256.Length != 64 || !file.InstalledSha256.All(Uri.IsHexDigit))
                throw new InvalidDataException("安装记录中的文件校验值无效。");
        }
        return manifest;
    }

    private static bool SameRelativePath(string left, string right) => string.Equals(left.Replace('\\', '/'), right.Replace('\\', '/'), StringComparison.OrdinalIgnoreCase);

    private static void ValidatePayloadRelative(string relative)
    {
        if (Path.IsPathRooted(relative) || relative.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar).Any(p => p is ".." or "." or "")) throw new InvalidDataException("安装记录包含无效路径。");
        var first = relative.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)[0];
        var allowed = first.Equals("BepInEx", StringComparison.OrdinalIgnoreCase) || first.Equals("dotnet", StringComparison.OrdinalIgnoreCase) ||
            new[] { "winhttp.dll", "doorstop_config.ini", ".doorstop_version", "changelog.txt" }.Contains(relative, StringComparer.OrdinalIgnoreCase);
        if (!allowed) throw new InvalidDataException("安装包包含不属于加载器的文件：" + relative);
    }
}

public sealed class InstallationManifest
{
    public int FormatVersion { get; set; } = 1;
    public string GameDirectory { get; set; } = "";
    public DateTime InstalledUtc { get; set; }
    public bool Completed { get; set; }
    public List<InstallationFile> Files { get; set; } = new();
}
public sealed class InstallationFile
{
    public string RelativePath { get; set; } = "";
    public string InstalledSha256 { get; set; } = "";
    public string? OriginalBackupRelativePath { get; set; }
    public string? OriginalSha256 { get; set; }
}
