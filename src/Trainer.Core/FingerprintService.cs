using System.Diagnostics;
using System.Text.RegularExpressions;

namespace Trainer.Core;

public static class FingerprintService
{
    public static GameFingerprint Capture(string gameDirectory)
    {
        var directory = CoreFiles.Full(gameDirectory);
        var assembly = Path.Combine(directory, "GameAssembly.dll");
        var metadata = Path.Combine(directory, "VampireSurvivors_Data", "il2cpp_data", "Metadata", "global-metadata.dat");
        var exe = Path.Combine(directory, "VampireSurvivors.exe");
        if (!File.Exists(exe) || !File.Exists(assembly) || !File.Exists(metadata)) throw new DirectoryNotFoundException("目录中缺少此游戏的 EXE、GameAssembly 或 IL2CPP 元数据。");
        CoreFiles.RejectReparse(assembly); CoreFiles.RejectReparse(metadata);
        var manifest = GamePaths.FindAppManifest(directory);
        var build = manifest != null ? GamePaths.ReadManifestValue(manifest, "buildid") ?? "" : "";
        return new GameFingerprint
        {
            GameDirectory = directory,
            BuildId = build,
            UnityVersion = FileVersionInfo.GetVersionInfo(Path.Combine(directory, "UnityPlayer.dll")).FileVersion ?? "",
            GameAssemblySha256 = CoreFiles.Hash(assembly),
            MetadataSha256 = CoreFiles.Hash(metadata)
        };
    }

    public static VersionCheck Verify(string gameDirectory, string supportedJsonPath)
    {
        try
        {
            var supported = CoreFiles.Read<GameFingerprint>(supportedJsonPath);
            return Verify(gameDirectory, supported);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or ArgumentException)
        {
            return new VersionCheck(false, "无法验证游戏版本：" + e.Message, null);
        }
    }

    public static VersionCheck Verify(string gameDirectory, GameFingerprint supported)
    {
        try
        {
            var actual = Capture(gameDirectory);
            var valid = supported.AppId == "1794680" && supported.GameAssemblySha256.Length == 64 && supported.MetadataSha256.Length == 64 &&
                string.Equals(actual.GameAssemblySha256, supported.GameAssemblySha256, StringComparison.OrdinalIgnoreCase) &&
                string.Equals(actual.MetadataSha256, supported.MetadataSha256, StringComparison.OrdinalIgnoreCase) &&
                !string.IsNullOrEmpty(actual.BuildId) && actual.BuildId == supported.BuildId;
            return new VersionCheck(valid, valid ? "游戏版本已验证" : "游戏文件或 Steam Build 已变化，需要重新适配；已停止修改。", actual);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or ArgumentException)
        {
            return new VersionCheck(false, "无法验证游戏版本：" + e.Message, null);
        }
    }
}
