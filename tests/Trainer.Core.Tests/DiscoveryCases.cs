using Trainer.Core;
using System.Diagnostics.CodeAnalysis;

static class DiscoveryCases
{
    public static readonly (string Name, Action<Fixture> Test)[] All =
    {
        ("discovery reads a manifest installdir in an additional Steam library", fixture =>
        {
            var steam = Path.Combine(fixture.Root, "Client");
            var library = Path.Combine(fixture.Root, "Alternate Library");
            var installed = CreateGame(Path.Combine(library, "steamapps", "common", "Custom Install Name"));
            Write(Path.Combine(steam, "steamapps", "libraryfolders.vdf"), "\"libraryfolders\" { \"1\" { \"path\" \"" + Vdf(library) + "\" } }");
            WriteManifest(library, "Custom Install Name");
            var result = Paths(fixture, new FakeGameProcessProvider(), steam).DiscoverGame();
            Check(result.Selected?.Directory == installed, "Did not use manifest's actual installation folder");
            Check(result.Candidates.Count == 1 && result.Selected.Sources.Contains(GameDiscoverySource.SteamManifest), "Manifest source was not retained");
            Check(!result.IsGameRunning, "Synthetic empty process list reported running");
        }),
        ("discovery supports older Steam library layout and conventional fallback", fixture =>
        {
            var steam = Path.Combine(fixture.Root, "Client");
            var library = Path.Combine(fixture.Root, "Legacy Library");
            var installed = CreateGame(Path.Combine(library, "steamapps", "common", "Vampire Survivors"));
            Write(Path.Combine(steam, "steamapps", "libraryfolders.vdf"), "\"libraryfolders\" { \"1\" \"" + Vdf(library) + "\" }");
            var paths = Paths(fixture, new FakeGameProcessProvider(), steam);
            Check(paths.DiscoverGameDirectory() == installed, "Compatibility discovery failed on legacy library");
            Check(paths.DiscoverGame().Selected?.Sources.Contains(GameDiscoverySource.SteamLibrary) == true, "Conventional fallback source missing");
        }),
        ("discovery leaves two installed copies for an explicit choice", fixture =>
        {
            var first = Path.Combine(fixture.Root, "First Library");
            var second = Path.Combine(fixture.Root, "Second Library");
            var firstGame = CreateGame(Path.Combine(first, "steamapps", "common", "First Copy"));
            var secondGame = CreateGame(Path.Combine(second, "steamapps", "common", "Second Copy"));
            WriteManifest(first, "First Copy"); WriteManifest(second, "Second Copy");
            var paths = Paths(fixture, new FakeGameProcessProvider(), first, second);
            var result = paths.DiscoverGame();
            Check(result.RequiresSelection && result.Selected is null && result.Candidates.Count == 2, "Discovery silently picked one of multiple installations");
            Check(paths.DiscoverGameDirectory() is null, "Legacy method silently picked an ambiguous installation");
            result = paths.DiscoverGame(secondGame);
            Check(result.Selected?.Directory == secondGame && !result.RequiresSelection, "Valid saved choice was not respected");
            Check(result.Selected.Sources.Contains(GameDiscoverySource.SavedDirectory), "Saved choice provenance lost");
        }),
        ("discovery prioritizes the active game and merges matching sources", fixture =>
        {
            var other = CreateGame(Path.Combine(fixture.Root, "Other Copy"));
            var provider = new FakeGameProcessProvider(new RunningGameProcess(42, Path.Combine(fixture.Game, "VampireSurvivors.exe")), new RunningGameProcess(43, Path.Combine(fixture.Game, "VampireSurvivors.exe")));
            var result = Paths(fixture, provider, Path.Combine(fixture.Root, "Steam")).DiscoverGame(other);
            Check(result.Selected?.Directory == fixture.Game && result.Selected.IsRunning, "Saved inactive copy overrode active game");
            Check(result.Selected.ProcessIds.SequenceEqual(new[] { 42, 43 }), "Process IDs for the same installation were not merged");
            Check(result.Selected.Sources.Contains(GameDiscoverySource.RunningProcess) && result.Selected.Sources.Contains(GameDiscoverySource.SteamLibrary), "Provenance for matching Steam/process paths not merged");
        }),
        ("discovery refuses to choose between active games from different directories", fixture =>
        {
            var other = CreateGame(Path.Combine(fixture.Root, "Second Active Copy"));
            var provider = new FakeGameProcessProvider(new RunningGameProcess(42, Path.Combine(fixture.Game, "VampireSurvivors.exe")), new RunningGameProcess(43, Path.Combine(other, "VampireSurvivors.exe")));
            var result = Paths(fixture, provider).DiscoverGame(fixture.Game);
            Check(result.RequiresSelection && result.Selected is null && result.Candidates.Count == 2, "Saved directory hid multiple active installations");
            Check(result.IsGameRunning && result.RunningProcesses.Count == 2, "Multiple processes disappeared");
        }),
        ("discovery retains unreadable process identity while Steam supplies its directory", fixture =>
        {
            var provider = new FakeGameProcessProvider(new RunningGameProcess(42, null));
            var result = Paths(fixture, provider, Path.Combine(fixture.Root, "Steam")).DiscoverGame();
            Check(result.IsGameRunning && result.RunningProcesses.Single().ProcessId == 42, "Unreadable image path was mistaken for a stopped game");
            Check(result.Selected?.Directory == fixture.Game && result.Warnings.Count > 0, "Steam fallback did not survive unreadable image path");
            Check(!result.Selected.IsRunning, "Directory ownership was invented for an unknown process path");
        }),
        ("discovery handles denied process enumeration without losing Steam fallback", fixture =>
        {
            var provider = new FakeGameProcessProvider { Fail = true };
            var result = Paths(fixture, provider, Path.Combine(fixture.Root, "Steam")).DiscoverGame();
            Check(result.ProcessScanFailed && result.Warnings.Count > 0, "Process enumeration failure was hidden");
            Check(result.Selected?.Directory == fixture.Game, "Process access failure blocked installation discovery");
        }),
        ("discovery skips unreadable Steam metadata and keeps other libraries", fixture =>
        {
            var blocked = Path.Combine(fixture.Root, "Unavailable Client");
            // A directory at the expected file path deterministically causes a read access failure, without changing ACLs.
            Directory.CreateDirectory(Path.Combine(blocked, "steamapps", "libraryfolders.vdf"));
            Directory.CreateDirectory(Path.Combine(blocked, "steamapps", "appmanifest_1794680.acf"));
            var result = Paths(fixture, new FakeGameProcessProvider(), blocked, Path.Combine(fixture.Root, "Steam")).DiscoverGame();
            Check(result.Selected?.Directory == fixture.Game && result.Warnings.Count > 0, "Unreadable metadata blocked other known libraries");
        }),
        ("discovery rejects incomplete folders and misleading process executable names", fixture =>
        {
            var incomplete = Path.Combine(fixture.Root, "Incomplete");
            Write(Path.Combine(incomplete, "VampireSurvivors.exe"), "synthetic-exe-only");
            var provider = new FakeGameProcessProvider(new RunningGameProcess(42, Path.Combine(fixture.Game, "SomeOtherGame.exe")));
            var result = Paths(fixture, provider).DiscoverGame(incomplete);
            Check(result.Selected is null && result.Candidates.Count == 0, "Executable name alone or incomplete directory was trusted");
            Check(!GamePaths.IsGameDirectory(incomplete) && GamePaths.IsGameDirectory(fixture.Game), "Directory validation did not require game assembly and metadata");
        }),
        ("discovery rejects manifest traversal and mismatched app IDs", fixture =>
        {
            var malicious = Path.Combine(fixture.Root, "Malformed Library");
            var outside = CreateGame(Path.Combine(malicious, "steamapps", "Outside"));
            WriteManifest(malicious, "../Outside");
            var paths = Paths(fixture, new FakeGameProcessProvider(), malicious);
            Check(paths.DiscoverGame().Candidates.Count == 0, "Manifest installdir escaped common folder");
            CreateGame(Path.Combine(malicious, "steamapps", "common", "Vampire Survivors"));
            Write(Path.Combine(malicious, "steamapps", "appmanifest_1794680.acf"), "\"appid\" \"999\" \"installdir\" \"Vampire Survivors\"");
            Check(paths.DiscoverGame().Candidates.Count == 0, "Mismatched app manifest was treated as this game's installation");
        })
    };

    private static GamePaths Paths(Fixture fixture, IGameProcessProvider provider, params string[] roots) =>
        new(fixture.Paths.ProfileDirectory, roots, provider);
    private static string CreateGame(string directory)
    {
        Write(Path.Combine(directory, "VampireSurvivors.exe"), "synthetic-exe");
        Write(Path.Combine(directory, "GameAssembly.dll"), "synthetic-assembly");
        Write(Path.Combine(directory, "VampireSurvivors_Data", "il2cpp_data", "Metadata", "global-metadata.dat"), "synthetic-metadata");
        return Path.GetFullPath(directory);
    }
    private static void WriteManifest(string library, string installDirectory) =>
        Write(Path.Combine(library, "steamapps", "appmanifest_1794680.acf"), "\"AppState\" { \"appid\" \"1794680\" \"installdir\" \"" + Vdf(installDirectory) + "\" }");
    private static string Vdf(string value) => value.Replace("\\", "\\\\", StringComparison.Ordinal).Replace("\"", "\\\"", StringComparison.Ordinal);
    private static void Write(string path, string value) { Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllText(path, value); }
    private static void Check([DoesNotReturnIf(false)] bool condition, string message) { if (!condition) throw new Exception(message); }

    private sealed class FakeGameProcessProvider(params RunningGameProcess[] processes) : IGameProcessProvider
    {
        public bool Fail { get; init; }
        public IReadOnlyList<RunningGameProcess> GetRunningGameProcesses() => Fail ? throw new UnauthorizedAccessException("Synthetic process access denied") : processes;
    }
}
