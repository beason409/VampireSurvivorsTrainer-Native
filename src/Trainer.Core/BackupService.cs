using System.Globalization;
using System.Text.RegularExpressions;
using Trainer.Shared;

namespace Trainer.Core;

public sealed class BackupService
{
    private readonly GamePaths paths;
    private readonly string storage;
    private readonly IProcessState processes;
    private string BackupRoot => Path.Combine(storage, "backups");

    public BackupService(GamePaths paths, string storageDirectory, IProcessState? guard = null)
    { this.paths = paths; storage = CoreFiles.Full(storageDirectory); processes = guard ?? new WindowsProcessState(); }

    public BackupResult CreateBackup(string gameDirectory)
    {
        CoreFiles.RequireStopped(processes);
        return CreateBackupCore(gameDirectory, "session");
    }

    private BackupResult CreateBackupCore(string gameDirectory, string purpose)
    {
        CoreFiles.RequireStopped(processes);
        var game = CoreFiles.Full(gameDirectory);
        var fingerprint = FingerprintService.Capture(game);
        var sourceFiles = paths.EnumerateSaveFiles(game).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        // Recovery must also work when the live save is missing/empty. Preserve that state in a
        // safety snapshot, but its proof will never authorize gold operations.
        if (purpose != "before-restore" && !sourceFiles.Any(p => paths.IsSteamSaveData(game, p) && new FileInfo(p).Length > 0)) throw new InvalidOperationException("未找到当前 Steam 账户的非空 SaveData，无法建立有效备份。");
        var now = DateTime.UtcNow;
        var id = purpose + "-" + now.ToString("yyyyMMdd-HHmmss-fffffff") + "-" + Guid.NewGuid().ToString("N")[..6];
        var directory = CoreFiles.SafeChild(BackupRoot, id);
        Directory.CreateDirectory(directory);
        var manifest = new BackupManifest { Id = id, CreatedUtc = now, GameDirectory = game, Fingerprint = fingerprint, Purpose = purpose, CompleteInventory = true };
        for (var index = 0; index < sourceFiles.Length; index++)
        {
            CoreFiles.RequireStopped(processes);
            var source = sourceFiles[index];
            if (!paths.IsAllowedSavePath(game, source)) throw new InvalidDataException("发现不属于此游戏的备份路径。");
            var relative = Path.Combine("files", index.ToString("D4"), Path.GetFileName(source));
            var digest = CoreFiles.Hash(source);
            var destination = CoreFiles.SafeChild(directory, relative);
            CoreFiles.CopyAtomic(source, destination, digest);
            manifest.Files.Add(new BackupEntry { OriginalPath = CoreFiles.Full(source), RelativeBackupPath = relative, Sha256 = digest, Length = new FileInfo(destination).Length });
        }
        // A save/cloud write during capture invalidates this snapshot; never emit an authorization proof.
        CoreFiles.RequireStopped(processes);
        foreach (var entry in manifest.Files)
            if (!File.Exists(entry.OriginalPath) || !string.Equals(CoreFiles.Hash(entry.OriginalPath), entry.Sha256, StringComparison.OrdinalIgnoreCase)) throw new IOException("备份过程中存档发生变化，请等待 Steam 同步完成后重试。");
        if (paths.EnumerateSaveFiles(game).Distinct(StringComparer.OrdinalIgnoreCase).Count() != sourceFiles.Length) throw new IOException("备份过程中存档文件列表发生变化，请重试。");
        var manifestPath = Path.Combine(directory, "manifest.json");
        CoreFiles.Write(manifestPath, manifest);
        var proofPath = Path.Combine(directory, "session-proof.json");
        CoreFiles.RequireStopped(processes);
        CoreFiles.Write(proofPath, new BackupProof
        {
            CreatedUtc = now, GameDirectory = game, GameAssemblySha256 = fingerprint.GameAssemblySha256, MetadataSha256 = fingerprint.MetadataSha256,
            Files = manifest.Files.Select(entry => new BackupProofFile { OriginalPath = entry.OriginalPath, BackupPath = CoreFiles.SafeChild(directory, entry.RelativeBackupPath), Sha256 = entry.Sha256 }).ToList()
        });
        return new BackupResult(id, directory, manifestPath, proofPath, now, manifest.Files.Count);
    }

    public IReadOnlyList<BackupResult> ListBackups()
    {
        if (!Directory.Exists(BackupRoot)) return Array.Empty<BackupResult>();
        var results = new List<BackupResult>();
        foreach (var directory in Directory.EnumerateDirectories(BackupRoot))
        {
            try
            {
                CoreFiles.RejectReparse(directory);
                var manifestPath = Path.Combine(directory, "manifest.json");
                var proofPath = Path.Combine(directory, "session-proof.json");
                if (!File.Exists(proofPath)) continue; // Incomplete backup cannot authorize or restore.
                if (!File.Exists(manifestPath)) manifestPath = proofPath; // Supports the initial verified baseline capture.
                var manifest = ReadManifest(manifestPath);
                results.Add(new BackupResult(manifest.Id, directory, manifestPath, proofPath, manifest.CreatedUtc, manifest.Files.Count));
            }
            catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException or System.Text.Json.JsonException or ArgumentException) { }
        }
        return results.OrderByDescending(r => r.CreatedUtc).ToArray();
    }

    public string? LatestSessionProofPath(string gameDirectory)
    {
        foreach (var backup in ListBackups())
        {
            try { ValidateProof(paths, storage, gameDirectory, backup.ProofPath); return backup.ProofPath; }
            catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException or InvalidOperationException or System.Text.Json.JsonException or ArgumentException) { }
        }
        return null;
    }

    public RestoreResult Restore(string gameDirectory, string manifestPath)
    {
        CoreFiles.RequireStopped(processes, steam: true);
        var manifest = ReadManifest(manifestPath);
        ValidateManifest(gameDirectory, manifestPath, manifest);
        var safety = CreateBackupCore(gameDirectory, "before-restore");
        var current = ReadManifest(safety.ManifestPath);
        try
        {
            var restored = ApplySnapshot(gameDirectory, manifestPath, manifest);
            return new RestoreResult(safety, restored);
        }
        catch (Exception original)
        {
            try { CoreFiles.RequireStopped(processes, steam: true); ApplySnapshot(gameDirectory, safety.ManifestPath, current); }
            catch (Exception rollback) { throw new AggregateException("恢复失败，自动回滚也未完成。请保留恢复前备份：" + safety.ManifestPath, original, rollback); }
            throw new IOException("恢复失败，已回滚到恢复前存档。", original);
        }
    }

    private int ApplySnapshot(string gameDirectory, string manifestPath, BackupManifest manifest)
    {
        ValidateManifest(gameDirectory, manifestPath, manifest);
        var directory = Path.GetDirectoryName(CoreFiles.Full(manifestPath))!;
        var destinations = manifest.Files.Select(f => CoreFiles.Full(f.OriginalPath)).ToHashSet(StringComparer.OrdinalIgnoreCase);
        // A proof-only baseline proves the listed bytes, not that every original save file was
        // inventoried. Overlay it without deleting files it never claimed to capture.
        var extras = manifest.CompleteInventory
            ? paths.EnumerateSaveFiles(gameDirectory).Where(p => !destinations.Contains(CoreFiles.Full(p)) && !IsCloudBookkeeping(gameDirectory, p)).ToArray()
            : Array.Empty<string>();
        var restored = 0;
        foreach (var entry in manifest.Files)
        {
            // Keep Steam's current sync history and account identity. Rewinding them can make
            // restored content appear unchanged locally and trigger a newer cloud download.
            if (IsCloudBookkeeping(gameDirectory, entry.OriginalPath)) continue;
            CoreFiles.RequireStopped(processes, steam: true);
            CoreFiles.CopyAtomic(CoreFiles.SafeChild(directory, entry.RelativeBackupPath), entry.OriginalPath, entry.Sha256);
            if (paths.IsSteamSaveData(gameDirectory, entry.OriginalPath)) MarkSaveDataChanged(gameDirectory, entry.OriginalPath);
            restored++;
        }
        foreach (var path in extras)
        {
            CoreFiles.RequireStopped(processes, steam: true);
            if (!paths.IsAllowedSavePath(gameDirectory, path)) throw new InvalidDataException("恢复文件超出了此游戏存档目录。");
            CoreFiles.RejectReparse(path);
            File.Delete(path); // Exact, allowlisted save files only; the safety snapshot retains their bytes.
        }
        return restored;
    }

    private bool IsCloudBookkeeping(string gameDirectory, string path) => paths.GetSaveLocations(gameDirectory).Any(location =>
        location.IsDirectory
            ? CoreFiles.Same(path, Path.Combine(location.Path, "steam_autocloud.vdf"))
            : Path.GetFileName(location.Path).Equals("remotecache.vdf", StringComparison.OrdinalIgnoreCase) && CoreFiles.Same(path, location.Path));

    private void MarkSaveDataChanged(string gameDirectory, string path)
    {
        var remote = paths.GetSaveLocations(gameDirectory).Single(location => location.IsDirectory &&
            location.Path.EndsWith(Path.Combine(GamePaths.AppId, "remote"), StringComparison.OrdinalIgnoreCase) && CoreFiles.Within(path, location.Path));
        var cache = Path.Combine(Path.GetDirectoryName(remote.Path)!, "remotecache.vdf");
        CoreFiles.RejectReparse(cache);
        long? cachedSecond = null;
        if (File.Exists(cache))
        {
            var save = Regex.Match(File.ReadAllText(cache), "\"SaveData\"\\s*\\{(?<fields>[^{}]*)\\}", RegexOptions.IgnoreCase);
            var localtime = Regex.Match(save.Groups["fields"].Value, "\"localtime\"\\s*\"(?<seconds>[0-9]+)\"", RegexOptions.IgnoreCase);
            if (long.TryParse(localtime.Groups["seconds"].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var seconds)) cachedSecond = seconds;
        }
        var now = DateTime.UtcNow;
        // Steam records localtime at whole-second precision. Wait at most the remainder of
        // this second when needed, so the new timestamp is real UTC now rather than a future date.
        while (cachedSecond == new DateTimeOffset(now).ToUnixTimeSeconds())
        {
            Thread.Sleep(20);
            CoreFiles.RequireStopped(processes, steam: true);
            now = DateTime.UtcNow;
        }
        CoreFiles.RequireStopped(processes, steam: true);
        File.SetLastWriteTimeUtc(path, now);
    }

    private BackupManifest ReadManifest(string manifestPath)
    {
        var full = CoreFiles.Full(manifestPath);
        if (!CoreFiles.Within(full, BackupRoot)) throw new InvalidDataException("请选择本修改器备份目录内的存档。");
        CoreFiles.RejectReparse(full);
        BackupManifest manifest;
        if (Path.GetFileName(full).Equals("session-proof.json", StringComparison.OrdinalIgnoreCase))
        {
            var proof = CoreFiles.Read<BackupProof>(full);
            var directory = Path.GetDirectoryName(full)!;
            if (proof.AppId != GamePaths.AppId || proof.Files == null) throw new InvalidDataException("备份证明无效。");
            manifest = new BackupManifest
            {
                Id = Path.GetFileName(directory), GameDirectory = proof.GameDirectory, CreatedUtc = proof.CreatedUtc,
                Fingerprint = new GameFingerprint { GameDirectory = proof.GameDirectory, GameAssemblySha256 = proof.GameAssemblySha256, MetadataSha256 = proof.MetadataSha256 },
                Files = proof.Files.Select(f => new BackupEntry { OriginalPath = f.OriginalPath, RelativeBackupPath = Path.GetRelativePath(directory, f.BackupPath), Sha256 = f.Sha256, Length = File.Exists(f.BackupPath) ? new FileInfo(f.BackupPath).Length : -1 }).ToList()
            };
        }
        else manifest = CoreFiles.Read<BackupManifest>(full);
        if (manifest.FormatVersion != 1 || manifest.Files == null || manifest.Files.Count == 0 && manifest.Purpose != "before-restore" || manifest.CreatedUtc == default || string.IsNullOrWhiteSpace(manifest.GameDirectory)) throw new InvalidDataException("备份记录无效或为空。");
        return manifest;
    }

    private void ValidateManifest(string gameDirectory, string manifestPath, BackupManifest manifest)
    {
        if (!CoreFiles.Same(gameDirectory, manifest.GameDirectory)) throw new InvalidDataException("此备份属于另一游戏目录。");
        var directory = Path.GetDirectoryName(CoreFiles.Full(manifestPath))!;
        if (manifest.Files.Select(f => CoreFiles.Full(f.OriginalPath)).Distinct(StringComparer.OrdinalIgnoreCase).Count() != manifest.Files.Count) throw new InvalidDataException("备份存在重复目标路径。");
        foreach (var entry in manifest.Files)
        {
            if (!paths.IsAllowedSavePath(gameDirectory, entry.OriginalPath)) throw new InvalidDataException("备份记录指向此游戏存档范围以外的文件。");
            CoreFiles.RejectReparse(entry.OriginalPath);
            var file = CoreFiles.SafeChild(directory, entry.RelativeBackupPath);
            if (!CoreFiles.Within(file, Path.Combine(directory, "files")) || entry.Sha256.Length != 64 || !File.Exists(file) || new FileInfo(file).Length != entry.Length || !string.Equals(CoreFiles.Hash(file), entry.Sha256, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("备份文件校验失败，未改动当前存档。");
        }
        if (manifest.Purpose != "before-restore" && !manifest.Files.Any(f => paths.IsSteamSaveData(gameDirectory, f.OriginalPath) && f.Length > 0)) throw new InvalidDataException("备份不包含有效的 Steam SaveData。");
    }

    public static BackupProof ValidateProof(GamePaths paths, string storageDirectory, string gameDirectory, string proofPath)
    {
        var backupRoot = Path.Combine(CoreFiles.Full(storageDirectory), "backups");
        var full = CoreFiles.Full(proofPath);
        if (!CoreFiles.Within(full, backupRoot) || !Path.GetFileName(full).Equals("session-proof.json", StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("备份证明路径无效。");
        CoreFiles.RejectReparse(full);
        var proof = CoreFiles.Read<BackupProof>(full);
        if (proof.AppId != GamePaths.AppId || !CoreFiles.Same(proof.GameDirectory, gameDirectory) || proof.CreatedUtc > DateTime.UtcNow.AddMinutes(1) || proof.CreatedUtc < DateTime.UtcNow.AddHours(-12) || proof.Files == null || proof.Files.Count == 0) throw new InvalidDataException("备份已过期或不属于当前游戏，请先退出游戏重新备份。");
        var fingerprint = FingerprintService.Capture(gameDirectory);
        if (!string.Equals(proof.GameAssemblySha256, fingerprint.GameAssemblySha256, StringComparison.OrdinalIgnoreCase) || !string.Equals(proof.MetadataSha256, fingerprint.MetadataSha256, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("备份建立后游戏版本已变化。");
        var directory = Path.GetDirectoryName(full)!;
        if (proof.Files.Select(f => CoreFiles.Full(f.OriginalPath)).Distinct(StringComparer.OrdinalIgnoreCase).Count() != proof.Files.Count) throw new InvalidDataException("备份证明存在重复文件。");
        foreach (var file in proof.Files)
        {
            if (!paths.IsAllowedSavePath(gameDirectory, file.OriginalPath) || !CoreFiles.Within(file.BackupPath, Path.Combine(directory, "files")) || file.Sha256.Length != 64) throw new InvalidDataException("备份证明包含非法文件路径。");
            CoreFiles.RejectReparse(file.OriginalPath); CoreFiles.RejectReparse(file.BackupPath);
            if (!File.Exists(file.BackupPath) || !string.Equals(CoreFiles.Hash(file.BackupPath), file.Sha256, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("备份文件校验失败。");
        }
        if (!proof.Files.Any(f => paths.IsSteamSaveData(gameDirectory, f.OriginalPath) && new FileInfo(f.BackupPath).Length > 0)) throw new InvalidDataException("备份证明缺少有效 Steam SaveData。");
        return proof;
    }
}

public sealed class BackupManifest
{
    public int FormatVersion { get; set; } = 1;
    public string Purpose { get; set; } = "session";
    public bool CompleteInventory { get; set; }
    public string Id { get; set; } = "";
    public string GameDirectory { get; set; } = "";
    public DateTime CreatedUtc { get; set; }
    public GameFingerprint Fingerprint { get; set; } = new();
    public List<BackupEntry> Files { get; set; } = new();
}
public sealed class BackupEntry
{
    public string OriginalPath { get; set; } = "";
    public string RelativeBackupPath { get; set; } = "";
    public string Sha256 { get; set; } = "";
    public long Length { get; set; }
}
