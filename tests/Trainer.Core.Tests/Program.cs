using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using Trainer.Core;
using Trainer.Shared;

var cases = new (string Name, Action<Fixture> Test)[]
{
    ("version fingerprints reject changed binary and Steam build", fixture =>
    {
        Assert(FingerprintService.Verify(fixture.Game, fixture.Supported).IsSupported, "Expected current version accepted");
        File.AppendAllText(Path.Combine(fixture.Game, "GameAssembly.dll"), "changed");
        Assert(!FingerprintService.Verify(fixture.Game, fixture.Supported).IsSupported, "Modified binary must reject");
        fixture.WriteSupported();
        File.WriteAllText(fixture.Manifest, "\"buildid\" \"different\"\n\"LastOwner\" \"76561197960265729\"");
        Assert(!FingerprintService.Verify(fixture.Game, fixture.Supported).IsSupported, "Changed Steam build must reject");
    }),
    ("backup captures only game files and emits verified proof", fixture =>
    {
        var backup = fixture.Backups.CreateBackup(fixture.Game);
        var proof = BackupService.ValidateProof(fixture.Paths, fixture.Storage, fixture.Game, backup.ProofPath);
        Assert(proof.Files.Any(p => p.OriginalPath == fixture.Save), "Steam SaveData missing");
        Assert(proof.Files.All(p => !p.OriginalPath.Contains("unrelated", StringComparison.OrdinalIgnoreCase)), "Backed up unrelated user data");
        Assert(fixture.Backups.LatestSessionProofPath(fixture.Game) == backup.ProofPath, "Proof lookup failed");
        Assert(fixture.Backups.ListBackups().Count == 1, "Backup listing wrong");
    }),
    ("backup while game running and proof after binary update reject", fixture =>
    {
        fixture.Processes.IsGameRunning = true;
        Reject(() => fixture.Backups.CreateBackup(fixture.Game));
        fixture.Processes.IsGameRunning = false;
        var backup = fixture.Backups.CreateBackup(fixture.Game);
        File.AppendAllText(Path.Combine(fixture.Game, "GameAssembly.dll"), "updated");
        Assert(fixture.Backups.LatestSessionProofPath(fixture.Game) == null, "Old proof accepted after game update");
        Reject(() => BackupService.ValidateProof(fixture.Paths, fixture.Storage, fixture.Game, backup.ProofPath));
    }),
    ("tampered backup cannot authorize or overwrite live save", fixture =>
    {
        var backup = fixture.Backups.CreateBackup(fixture.Game);
        var proof = JsonSerializer.Deserialize<BackupProof>(File.ReadAllText(backup.ProofPath), Fixture.Json)!;
        File.WriteAllText(proof.Files.Single(p => p.OriginalPath == fixture.Save).BackupPath, "tampered");
        Reject(() => BackupService.ValidateProof(fixture.Paths, fixture.Storage, fixture.Game, backup.ProofPath));
        Reject(() => fixture.Backups.Restore(fixture.Game, backup.ManifestPath));
        Assert(File.ReadAllText(fixture.Save) == "original-save", "Tampered backup altered save");
    }),
    ("manifest path traversal and outside restore destinations reject", fixture =>
    {
        var backup = fixture.Backups.CreateBackup(fixture.Game);
        var json = JsonNode.Parse(File.ReadAllText(backup.ManifestPath))!;
        json["files"]![0]!["relativeBackupPath"] = @"..\..\outside.dat";
        File.WriteAllText(backup.ManifestPath, json.ToJsonString());
        Reject(() => fixture.Backups.Restore(fixture.Game, backup.ManifestPath));
        Assert(File.ReadAllText(fixture.Save) == "original-save", "Traversal changed live save");
        var second = fixture.Backups.CreateBackup(fixture.Game);
        json = JsonNode.Parse(File.ReadAllText(second.ManifestPath))!;
        json["files"]![0]!["originalPath"] = Path.Combine(fixture.Root, "unrelated.txt");
        File.WriteAllText(second.ManifestPath, json.ToJsonString());
        Reject(() => fixture.Backups.Restore(fixture.Game, second.ManifestPath));
        Assert(File.ReadAllText(Path.Combine(fixture.Root, "unrelated.txt")) == "do-not-touch", "Outside file changed");
    }),
    ("restore requires Steam stopped and keeps safety snapshot", fixture =>
    {
        var backup = fixture.Backups.CreateBackup(fixture.Game);
        File.WriteAllText(fixture.Save, "new-progress");
        var newFile = Path.Combine(Path.GetDirectoryName(fixture.Save)!, "new-sidecar");
        File.WriteAllText(newFile, "new-file");
        fixture.Processes.IsSteamRunning = true;
        Reject(() => fixture.Backups.Restore(fixture.Game, backup.ManifestPath));
        Assert(File.ReadAllText(fixture.Save) == "new-progress", "Steam-running restore touched save");
        fixture.Processes.IsSteamRunning = false;
        var result = fixture.Backups.Restore(fixture.Game, backup.ManifestPath);
        Assert(File.ReadAllText(fixture.Save) == "original-save", "Restore did not recover original bytes");
        Assert(!File.Exists(newFile), "Snapshot restore retained untracked sidecar");
        var proof = JsonSerializer.Deserialize<BackupProof>(File.ReadAllText(result.SafetyBackup.ProofPath), Fixture.Json)!;
        Assert(File.ReadAllText(proof.Files.Single(p => p.OriginalPath == fixture.Save).BackupPath) == "new-progress", "Safety copy lost progress");
        Assert(proof.Files.Any(p => p.OriginalPath == newFile), "Removed sidecar not backed up");
    }),
    ("proof-only bootstrap backup can be listed and restored", fixture =>
    {
        var backup = fixture.Backups.CreateBackup(fixture.Game);
        File.Delete(backup.ManifestPath);
        File.WriteAllText(fixture.Save, "changed");
        var listed = fixture.Backups.ListBackups().Single();
        Assert(listed.ManifestPath == backup.ProofPath, "Proof-only baseline not discovered");
        fixture.Backups.Restore(fixture.Game, listed.ManifestPath);
        Assert(File.ReadAllText(fixture.Save) == "original-save", "Proof-only baseline restore failed");
    }),
    ("restore preserves current cloud bookkeeping captured in the manifest", fixture =>
    {
        var remote = Path.GetDirectoryName(fixture.Save)!;
        var cache = Path.Combine(Path.GetDirectoryName(remote)!, "remotecache.vdf");
        var identity = Path.Combine(fixture.Paths.ProfileDirectory, "AppData", "Roaming", "Vampire_Survivors", "saves", "steam_autocloud.vdf");
        Directory.CreateDirectory(Path.GetDirectoryName(identity)!);
        // Similar names elsewhere are game content, not the exact Steam bookkeeping paths.
        var contentCache = Path.Combine(remote, "remotecache.vdf");
        var contentIdentity = Path.Combine(remote, "nested", "steam_autocloud.vdf");
        Directory.CreateDirectory(Path.GetDirectoryName(contentIdentity)!);
        File.WriteAllText(cache, "old-cloud-cache"); File.WriteAllText(identity, "old-account-identity");
        File.WriteAllText(contentCache, "old-content-cache"); File.WriteAllText(contentIdentity, "old-content-identity");
        var backup = fixture.Backups.CreateBackup(fixture.Game);
        File.WriteAllText(fixture.Save, "new-progress");
        File.WriteAllText(cache, "current-cloud-cache"); File.WriteAllText(identity, "current-account-identity");
        File.WriteAllText(contentCache, "new-content-cache"); File.WriteAllText(contentIdentity, "new-content-identity");
        var cacheTime = File.GetLastWriteTimeUtc(cache);
        var identityTime = File.GetLastWriteTimeUtc(identity);
        var result = fixture.Backups.Restore(fixture.Game, backup.ManifestPath);
        Assert(File.ReadAllText(fixture.Save) == "original-save", "Cloud preservation prevented content restore");
        Assert(File.ReadAllText(cache) == "current-cloud-cache" && File.GetLastWriteTimeUtc(cache) == cacheTime, "Restore rewound current cloud cache");
        Assert(File.ReadAllText(identity) == "current-account-identity" && File.GetLastWriteTimeUtc(identity) == identityTime, "Restore rewound account identity");
        Assert(File.ReadAllText(contentCache) == "old-content-cache" && File.ReadAllText(contentIdentity) == "old-content-identity", "Bookkeeping exclusion matched unrelated paths");
        Assert(result.RestoredFileCount == backup.FileCount - 2, "Restore count included preserved bookkeeping");
        var safety = JsonSerializer.Deserialize<BackupProof>(File.ReadAllText(result.SafetyBackup.ProofPath), Fixture.Json)!;
        Assert(File.ReadAllText(safety.Files.Single(f => f.OriginalPath == cache).BackupPath) == "current-cloud-cache", "Safety snapshot omitted current cache");
    }),
    ("complete restore preserves cloud bookkeeping absent from the manifest", fixture =>
    {
        var backup = fixture.Backups.CreateBackup(fixture.Game);
        var remote = Path.GetDirectoryName(fixture.Save)!;
        var cache = Path.Combine(Path.GetDirectoryName(remote)!, "remotecache.vdf");
        var identity = Path.Combine(fixture.Paths.ProfileDirectory, "AppData", "Roaming", "Vampire_Survivors", "saves", "steam_autocloud.vdf");
        Directory.CreateDirectory(Path.GetDirectoryName(identity)!);
        File.WriteAllText(cache, "current-cloud-cache"); File.WriteAllText(identity, "current-account-identity");
        var sidecar = Path.Combine(remote, "new-sidecar");
        File.WriteAllText(sidecar, "new-content");
        File.WriteAllText(fixture.Save, "new-progress");
        var result = fixture.Backups.Restore(fixture.Game, backup.ManifestPath);
        Assert(File.ReadAllText(cache) == "current-cloud-cache", "Extra-file cleanup deleted current cache");
        Assert(File.ReadAllText(identity) == "current-account-identity", "Extra-file cleanup deleted account identity");
        Assert(!File.Exists(sidecar), "Cloud preservation disabled content inventory cleanup");
        Assert(File.ReadAllText(fixture.Save) == "original-save" && result.RestoredFileCount == backup.FileCount, "Absent bookkeeping changed restored content count");
    }),
    ("restore marks SaveData with fresh UTC time distinct from cached whole second", fixture =>
    {
        var oldTime = DateTime.UtcNow.AddDays(-10);
        File.SetLastWriteTimeUtc(fixture.Save, oldTime);
        var backup = fixture.Backups.CreateBackup(fixture.Game);
        File.WriteAllText(fixture.Save, "new-progress");
        var cachedSecond = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
        var cache = Path.Combine(Path.GetDirectoryName(Path.GetDirectoryName(fixture.Save)!)!, "remotecache.vdf");
        var cacheText = "\"1794680\"\n{\n\"ChangeNumber\" \"50\"\n\"SaveData\"\n{\n\"localtime\" \"" + cachedSecond + "\"\n}\n}";
        File.WriteAllText(cache, cacheText);
        var started = DateTime.UtcNow;
        fixture.Backups.Restore(fixture.Game, backup.ManifestPath);
        var finished = DateTime.UtcNow;
        var restoredTime = File.GetLastWriteTimeUtc(fixture.Save);
        Assert(File.ReadAllText(fixture.Save) == "original-save", "Fresh timestamp changed restored bytes");
        Assert(restoredTime >= started && restoredTime <= finished && restoredTime > oldTime, "Restore retained backup timestamp or wrote a future timestamp");
        Assert(new DateTimeOffset(restoredTime).ToUnixTimeSeconds() != cachedSecond, "Restore timestamp equals cached whole second");
        Assert(File.ReadAllText(cache) == cacheText, "Timestamp refresh changed Steam cache metadata");
    }),
    ("recovery restores missing or empty live save without authorizing an empty safety snapshot", fixture =>
    {
        var backup = fixture.Backups.CreateBackup(fixture.Game);
        File.Delete(fixture.Save);
        var recovered = fixture.Backups.Restore(fixture.Game, backup.ManifestPath);
        Assert(File.ReadAllText(fixture.Save) == "original-save", "Missing live save prevented recovery");
        Reject(() => BackupService.ValidateProof(fixture.Paths, fixture.Storage, fixture.Game, recovered.SafetyBackup.ProofPath));
        File.WriteAllText(fixture.Save, "");
        recovered = fixture.Backups.Restore(fixture.Game, backup.ManifestPath);
        Assert(File.ReadAllText(fixture.Save) == "original-save", "Empty live save prevented recovery");
        Reject(() => BackupService.ValidateProof(fixture.Paths, fixture.Storage, fixture.Game, recovered.SafetyBackup.ProofPath));
        Assert(fixture.Backups.ListBackups().Count == 3, "Recovery safety snapshots not listed");
    }),
    ("proof-only partial baseline overlays and preserves omitted original files", fixture =>
    {
        var remoteCache = Path.Combine(Path.GetDirectoryName(Path.GetDirectoryName(fixture.Save)!)!, "remotecache.vdf");
        var prefsBackup = Path.Combine(fixture.Root, "Profile", "AppData", "LocalLow", "poncle", "Vampire Survivors", "prefs.bin.bak");
        Directory.CreateDirectory(Path.GetDirectoryName(prefsBackup)!);
        File.WriteAllText(remoteCache, "original-cache"); File.WriteAllText(prefsBackup, "original-prefs-backup");
        var backup = fixture.Backups.CreateBackup(fixture.Game);
        var proof = JsonSerializer.Deserialize<BackupProof>(File.ReadAllText(backup.ProofPath), Fixture.Json)!;
        proof.Files.RemoveAll(file => file.OriginalPath == remoteCache || file.OriginalPath == prefsBackup);
        File.WriteAllText(backup.ProofPath, JsonSerializer.Serialize(proof, Fixture.Json));
        File.Delete(backup.ManifestPath);
        File.WriteAllText(fixture.Save, "new-progress");
        fixture.Backups.Restore(fixture.Game, backup.ProofPath);
        Assert(File.ReadAllText(fixture.Save) == "original-save", "Partial proof did not overlay captured file");
        Assert(File.ReadAllText(remoteCache) == "original-cache", "Partial baseline deleted omitted remotecache");
        Assert(File.ReadAllText(prefsBackup) == "original-prefs-backup", "Partial baseline deleted omitted prefs backup");
    }),
    ("installer requires proof and restores collisions without broad delete", fixture =>
    {
        File.WriteAllText(Path.Combine(fixture.Game, "winhttp.dll"), "existing-loader");
        var unrelated = Path.Combine(fixture.Game, "other-mod.txt");
        File.WriteAllText(unrelated, "other-mod");
        Reject(() => fixture.Installs.Install(fixture.Game, fixture.Payload, Path.Combine(fixture.Storage, "missing.json"), fixture.Supported));
        var backup = fixture.Backups.CreateBackup(fixture.Game);
        var installed = fixture.Installs.Install(fixture.Game, fixture.Payload, backup.ProofPath, fixture.Supported);
        Assert(installed.FileCount == 3 && fixture.Installs.IsInstalled(fixture.Game), "Install tracking failed");
        var uninstalled = fixture.Installs.Uninstall(fixture.Game);
        Assert(uninstalled.PreservedModifiedFiles.Count == 0, "Unmodified install unexpectedly preserved");
        Assert(File.ReadAllText(Path.Combine(fixture.Game, "winhttp.dll")) == "existing-loader", "Collision original not restored");
        Assert(!File.Exists(Path.Combine(fixture.Game, "BepInEx", "plugins", "Trainer.Plugin.dll")), "Plugin not removed");
        Assert(File.ReadAllText(unrelated) == "other-mod", "Unrelated file changed");
        Assert(!File.Exists(installed.ManifestPath), "Completed uninstall left active manifest");
    }),
    ("uninstaller preserves files modified after install", fixture =>
    {
        var backup = fixture.Backups.CreateBackup(fixture.Game);
        fixture.Installs.Install(fixture.Game, fixture.Payload, backup.ProofPath, fixture.Supported);
        var changed = Path.Combine(fixture.Game, "winhttp.dll");
        File.WriteAllText(changed, "different-mod");
        var result = fixture.Installs.Uninstall(fixture.Game);
        Assert(result.PreservedModifiedFiles.Contains("winhttp.dll"), "Modified installed file not reported");
        Assert(File.ReadAllText(changed) == "different-mod", "Modified installed file deleted");
        Assert(!fixture.Installs.IsInstalled(fixture.Game), "Partial uninstall still reports installed");
    }),
    ("installer and malicious journal cannot target original game binaries", fixture =>
    {
        var backup = fixture.Backups.CreateBackup(fixture.Game);
        File.WriteAllText(Path.Combine(fixture.Payload, "GameAssembly.dll"), "wrong");
        Reject(() => fixture.Installs.Install(fixture.Game, fixture.Payload, backup.ProofPath, fixture.Supported));
        Assert(File.ReadAllText(Path.Combine(fixture.Game, "GameAssembly.dll")) == "assembly", "Installer overwrote game binary");
        File.Delete(Path.Combine(fixture.Payload, "GameAssembly.dll"));
        var install = fixture.Installs.Install(fixture.Game, fixture.Payload, backup.ProofPath, fixture.Supported);
        var json = JsonNode.Parse(File.ReadAllText(install.ManifestPath))!;
        json["files"]![0]!["relativePath"] = @"..\outside.txt";
        File.WriteAllText(install.ManifestPath, json.ToJsonString());
        Reject(() => fixture.Installs.Uninstall(fixture.Game));
        Assert(File.ReadAllText(Path.Combine(fixture.Root, "unrelated.txt")) == "do-not-touch", "Malicious journal changed outside file");
    })
};

cases = cases.Concat(DiscoveryCases.All).ToArray();
cases = cases.Concat(IntegrityCases.All).ToArray();
var failures = 0;
foreach (var test in cases)
{
    var fixture = new Fixture();
    try { test.Test(fixture); Console.WriteLine("PASS " + test.Name); }
    catch (Exception error) { failures++; Console.Error.WriteLine("FAIL " + test.Name + "\n" + error); }
}
Console.WriteLine($"{cases.Length - failures}/{cases.Length} core tests passed. Fixture files retained under tests/Trainer.Core.Tests/.artifacts.");
return failures == 0 ? 0 : 1;

static void Assert(bool condition, string message) { if (!condition) throw new Exception(message); }
static void Reject(Action action)
{
    try { action(); }
    catch (Exception e) when (e is IOException or InvalidDataException or InvalidOperationException or UnauthorizedAccessException or ArgumentException) { return; }
    throw new Exception("Expected operation rejected");
}

sealed class FakeProcesses : IProcessState
{
    private bool gameRunning;
    public Action? BeforeGameCheck { get; set; }
    public bool IsGameRunning { get { BeforeGameCheck?.Invoke(); return gameRunning; } set => gameRunning = value; }
    public bool IsSteamRunning { get; set; }
}

sealed class Fixture
{
    public static JsonSerializerOptions Json { get; } = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase, PropertyNameCaseInsensitive = true, WriteIndented = true };
    public string Root { get; }
    public string Game { get; }
    public string Storage { get; }
    public string Save { get; }
    public string Manifest { get; }
    public string Supported { get; }
    public string Payload { get; }
    public GamePaths Paths { get; }
    public FakeProcesses Processes { get; } = new();
    public BackupService Backups { get; }
    public InstallationService Installs { get; }
    public TrustedPayloadCatalog Catalog { get; }
    public Fixture()
    {
        // Fixtures only ever write below this project; no live game paths or real processes are used.
        var project = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", ".."));
        Root = Path.Combine(project, ".artifacts", Guid.NewGuid().ToString("N"));
        var steam = Path.Combine(Root, "Steam");
        Game = Path.Combine(steam, "steamapps", "common", "Vampire Survivors");
        Storage = Path.Combine(Root, "storage");
        Payload = Path.Combine(Root, "payload");
        Directory.CreateDirectory(Game);
        Write(Path.Combine(Game, "GameAssembly.dll"), "assembly");
        Write(Path.Combine(Game, "UnityPlayer.dll"), "not-a-real-PE");
        Write(Path.Combine(Game, "VampireSurvivors.exe"), "exe");
        Write(Path.Combine(Game, "VampireSurvivors_Data", "il2cpp_data", "Metadata", "global-metadata.dat"), "metadata");
        Manifest = Path.Combine(steam, "steamapps", "appmanifest_1794680.acf");
        Write(Manifest, "\"buildid\" \"25984009\"\n\"LastOwner\" \"76561197960265729\"");
        Save = Path.Combine(steam, "userdata", "1", "1794680", "remote", "SaveData");
        Write(Save, "original-save");
        Write(Path.Combine(steam, "userdata", "1", "999", "remote", "unrelated-data"), "other-app");
        Write(Path.Combine(Root, "unrelated.txt"), "do-not-touch");
        Paths = new GamePaths(Path.Combine(Root, "Profile"), new[] { steam });
        Backups = new BackupService(Paths, Storage, Processes);
        Supported = Path.Combine(Root, "supported.json");
        WriteSupported();
        Write(Path.Combine(Payload, "winhttp.dll"), "loader");
        Write(Path.Combine(Payload, "doorstop_config.ini"), "config");
        Write(Path.Combine(Payload, "BepInEx", "plugins", "Trainer.Plugin.dll"), "plugin");
        Catalog = CreateCatalog();
        Installs = new InstallationService(Paths, Storage, Processes, Catalog);
    }
    public void WriteSupported() => Write(Supported, JsonSerializer.Serialize(FingerprintService.Capture(Game), Json));
    public TrustedPayloadCatalog CreateCatalog()
    {
        var entries = Directory.EnumerateFiles(Payload, "*", SearchOption.AllDirectories).Order(StringComparer.OrdinalIgnoreCase).Select(path => new
        {
            path = Path.GetRelativePath(Payload, path).Replace('\\', '/'),
            length = new FileInfo(path).Length,
            sha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)))
        }).ToArray();
        using var bytes = new MemoryStream(JsonSerializer.SerializeToUtf8Bytes(new
        {
            formatVersion = 1,
            files = entries,
            supportedGame = new { length = new FileInfo(Supported).Length, sha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(Supported))) }
        }, Json));
        return TrustedPayloadCatalog.Parse(bytes);
    }
    private static void Write(string path, string value) { Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllText(path, value); }
}
