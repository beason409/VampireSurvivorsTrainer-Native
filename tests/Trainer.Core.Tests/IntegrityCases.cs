using System.Security.Cryptography;
using System.Text.Json;
using Trainer.Core;

internal static class IntegrityCases
{
    internal static readonly (string Name, Action<Fixture> Test)[] All =
    {
        ("trusted installer records catalog hashes and keeps legacy uninstall behavior", fixture =>
        {
            var backup = fixture.Backups.CreateBackup(fixture.Game);
            var installed = fixture.Installs.Install(fixture.Game, fixture.Payload, backup.ProofPath, fixture.Supported);
            var journal = JsonSerializer.Deserialize<InstallationManifest>(File.ReadAllText(installed.ManifestPath), Fixture.Json)!;
            Assert(journal.Completed && journal.Files.Count == 3 && fixture.Installs.IsInstalled(fixture.Game), "Trusted install was not completed");
            Assert(journal.Files.All(file => file.InstalledSha256.Equals(fixture.Catalog.Files[file.RelativePath.Replace('\\','/')].Sha256, StringComparison.OrdinalIgnoreCase)), "Journal trusted a new source digest");
            var removed = fixture.Installs.Uninstall(fixture.Game);
            Assert(removed.RemovedOrRestoredCount == 3 && removed.PreservedModifiedFiles.Count == 0, "Trusted install changed normal uninstall behavior");
        }),
        ("same-length payload replacement fails before any game or installation writes", fixture =>
        {
            var backup = fixture.Backups.CreateBackup(fixture.Game);
            File.WriteAllText(Path.Combine(fixture.Payload,"BepInEx","plugins","Trainer.Plugin.dll"), "trojan");
            AssertNoWrites(fixture, () => fixture.Installs.Install(fixture.Game, fixture.Payload, backup.ProofPath, fixture.Supported));
        }),
        ("default installer has no permissive loose-payload fallback", fixture =>
        {
            var backup = fixture.Backups.CreateBackup(fixture.Game);
            var defaults = new InstallationService(fixture.Paths,fixture.Storage,fixture.Processes);
            AssertNoWrites(fixture, () => defaults.Install(fixture.Game, fixture.Payload, backup.ProofPath, fixture.Supported));
        }),
        ("rewriting a loose release manifest cannot authorize a modified payload", fixture =>
        {
            var backup = fixture.Backups.CreateBackup(fixture.Game);
            var path = Path.Combine(fixture.Payload,"winhttp.dll");
            File.WriteAllText(path, "other!");
            File.WriteAllText(Path.Combine(fixture.Root,"release-manifest.json"), JsonSerializer.Serialize(new
            {
                formatVersion=1, files=new[] { new { path="payload/winhttp.dll", length=new FileInfo(path).Length, sha256=Hash(path) } }
            }));
            AssertNoWrites(fixture, () => fixture.Installs.Install(fixture.Game, fixture.Payload, backup.ProofPath, fixture.Supported));
        }),
        ("an extra loader-directory DLL fails before any installation writes", fixture =>
        {
            var backup = fixture.Backups.CreateBackup(fixture.Game);
            File.WriteAllText(Path.Combine(fixture.Payload,"BepInEx","plugins","Extra.dll"), "unexpected");
            AssertNoWrites(fixture, () => fixture.Installs.Install(fixture.Game, fixture.Payload, backup.ProofPath, fixture.Supported));
        }),
        ("a missing trusted payload file fails before any installation writes", fixture =>
        {
            var backup = fixture.Backups.CreateBackup(fixture.Game);
            File.Delete(Path.Combine(fixture.Payload,"doorstop_config.ini"));
            AssertNoWrites(fixture, () => fixture.Installs.Install(fixture.Game, fixture.Payload, backup.ProofPath, fixture.Supported));
        }),
        ("a modified supported-build document cannot approve a changed game", fixture =>
        {
            File.AppendAllText(Path.Combine(fixture.Game,"GameAssembly.dll"), "changed");
            fixture.WriteSupported();
            var backup = fixture.Backups.CreateBackup(fixture.Game);
            Assert(FingerprintService.Verify(fixture.Game, fixture.Supported).IsSupported, "Fixture no longer demonstrates a mutable config approving changed bytes");
            AssertNoWrites(fixture, () => fixture.Installs.Install(fixture.Game, fixture.Payload, backup.ProofPath, fixture.Supported));
        }),
        ("a source swapped after verification never becomes the expected install digest", fixture =>
        {
            PrepareOriginalCollisions(fixture);
            var backup = fixture.Backups.CreateBackup(fixture.Game);
            string originalFiles = FileSnapshot(fixture.Game);
            var first = fixture.Catalog.Files.Values.OrderBy(file => file.Path,StringComparer.OrdinalIgnoreCase).First();
            int checks = 0;
            fixture.Processes.BeforeGameCheck = () =>
            {
                if (++checks == 2) File.WriteAllBytes(Path.Combine(fixture.Payload,first.Path), Enumerable.Repeat((byte)'X',(int)first.Length).ToArray());
            };
            try { Reject(() => fixture.Installs.Install(fixture.Game, fixture.Payload, backup.ProofPath, fixture.Supported)); }
            finally { fixture.Processes.BeforeGameCheck = null; }
            Assert(checks >= 2 && FileSnapshot(fixture.Game) == originalFiles, "Post-verification replacement reached the game or damaged an original");
            Assert(!File.Exists(fixture.Installs.GetManifestPath(fixture.Game)), "Never-overwritten original left a stuck installation journal");
            AssertNoCopyTemps(fixture.Game);
        }),
        ("a late source swap rolls back earlier copies and leaves originals intact", fixture =>
        {
            PrepareOriginalCollisions(fixture);
            var backup = fixture.Backups.CreateBackup(fixture.Game);
            string originalFiles = FileSnapshot(fixture.Game);
            bool copiedBeforeSwap = false;
            int checks = 0;
            fixture.Processes.BeforeGameCheck = () =>
            {
                if (++checks != 3) return;
                copiedBeforeSwap = fixture.Catalog.Files.Values.Any(file => Hash(Path.Combine(fixture.Game,file.Path)).Equals(file.Sha256,StringComparison.OrdinalIgnoreCase));
                var pending = fixture.Catalog.Files.Values.First(file => !Hash(Path.Combine(fixture.Game,file.Path)).Equals(file.Sha256,StringComparison.OrdinalIgnoreCase));
                File.WriteAllBytes(Path.Combine(fixture.Payload,pending.Path), Enumerable.Repeat((byte)'Y',(int)pending.Length).ToArray());
            };
            try { Reject(() => fixture.Installs.Install(fixture.Game, fixture.Payload, backup.ProofPath, fixture.Supported)); }
            finally { fixture.Processes.BeforeGameCheck = null; }
            Assert(copiedBeforeSwap && FileSnapshot(fixture.Game) == originalFiles, "Partial install was not safely rolled back");
            Assert(!File.Exists(fixture.Installs.GetManifestPath(fixture.Game)), "Rollback retained a false modified-original conflict");
            AssertNoCopyTemps(fixture.Game);
        }),
        ("uninstall recognizes an already restored original without deleting it", fixture =>
        {
            string loader = Path.Combine(fixture.Game,"winhttp.dll");
            File.WriteAllText(loader,"original-loader");
            var backup = fixture.Backups.CreateBackup(fixture.Game);
            fixture.Installs.Install(fixture.Game, fixture.Payload, backup.ProofPath, fixture.Supported);
            File.WriteAllText(loader,"original-loader");
            var result = fixture.Installs.Uninstall(fixture.Game);
            Assert(result.PreservedModifiedFiles.Count == 0 && File.ReadAllText(loader) == "original-loader", "Original bytes were mistaken for a third-party modification");
            Assert(!File.Exists(fixture.Installs.GetManifestPath(fixture.Game)), "Already-restored original prevented journal cleanup");
        }),
        ("journal slash and case aliases cannot declare the same target twice", fixture =>
        {
            var backup = fixture.Backups.CreateBackup(fixture.Game);
            var installed = fixture.Installs.Install(fixture.Game, fixture.Payload, backup.ProofPath, fixture.Supported);
            var journal = JsonSerializer.Deserialize<InstallationManifest>(File.ReadAllText(installed.ManifestPath),Fixture.Json)!;
            var first = journal.Files.First();
            journal.Files.Add(new InstallationFile { RelativePath=first.RelativePath.Replace('\\','/').ToUpperInvariant(), InstalledSha256=first.InstalledSha256, OriginalBackupRelativePath=first.OriginalBackupRelativePath, OriginalSha256=first.OriginalSha256 });
            File.WriteAllText(installed.ManifestPath,JsonSerializer.Serialize(journal,Fixture.Json));
            string game=TreeSnapshot(fixture.Game), storage=TreeSnapshot(fixture.Storage);
            Reject(() => fixture.Installs.Uninstall(fixture.Game));
            Assert(TreeSnapshot(fixture.Game)==game && TreeSnapshot(fixture.Storage)==storage, "Invalid journal aliases changed files before rejection");
        })
    };

    private static void PrepareOriginalCollisions(Fixture fixture)
    {
        foreach (var file in fixture.Catalog.Files.Values)
        {
            string path = Path.Combine(fixture.Game,file.Path);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path,"original:" + file.Path);
        }
    }

    private static void AssertNoWrites(Fixture fixture, Action action)
    {
        string game = TreeSnapshot(fixture.Game), storage = TreeSnapshot(fixture.Storage);
        Reject(action);
        Assert(TreeSnapshot(fixture.Game) == game && TreeSnapshot(fixture.Storage) == storage, "Rejected input changed game files, storage bytes, directory inventory, or write timestamps");
        Assert(!File.Exists(fixture.Installs.GetManifestPath(fixture.Game)), "Rejected preflight created an installation journal");
    }

    private static string TreeSnapshot(string root)
    {
        if (!Directory.Exists(root)) return "absent";
        return string.Join("\n", Directory.EnumerateFileSystemEntries(root,"*",SearchOption.AllDirectories).Prepend(root).Order(StringComparer.OrdinalIgnoreCase).Select(path =>
            Directory.Exists(path) ? "D:"+Path.GetRelativePath(root,path)+":"+Directory.GetLastWriteTimeUtc(path).Ticks
                : "F:"+Path.GetRelativePath(root,path)+":"+File.GetLastWriteTimeUtc(path).Ticks+":"+Hash(path)));
    }

    private static string FileSnapshot(string root) => string.Join("\n", Directory.EnumerateFiles(root,"*",SearchOption.AllDirectories).Order(StringComparer.OrdinalIgnoreCase).Select(path => Path.GetRelativePath(root,path)+":"+Hash(path)));
    private static string Hash(string path) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));
    private static void AssertNoCopyTemps(string game) => Assert(!Directory.EnumerateFiles(game,"*.tmp",SearchOption.AllDirectories).Any(path => Path.GetFileName(path).Contains(".trainer-",StringComparison.Ordinal)), "Failed atomic copy left a staging file");
    private static void Assert(bool value,string message) { if (!value) throw new InvalidOperationException(message); }
    private static void Reject(Action action)
    {
        try { action(); }
        catch (Exception error) when (error is IOException or InvalidDataException or InvalidOperationException or UnauthorizedAccessException or ArgumentException) { return; }
        throw new Exception("Expected integrity failure");
    }
}
