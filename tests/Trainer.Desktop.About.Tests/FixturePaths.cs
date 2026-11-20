namespace Trainer.Desktop;

// ReleaseInfoDialog's real verification path is linked, but it may only read
// this synthetic fixture. No production RuntimePaths/settings code is linked.
internal static class RuntimePaths
{
    internal static string Root { get; set; } = "";
    internal static string PayloadDirectory => Path.Combine(Root, "payload");
    internal static string SupportedBuildPath => Path.Combine(Root, "supported-game.json");
}
