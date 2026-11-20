using System.Diagnostics;
using System.Security.Cryptography;

namespace Trainer.Plugin;

internal static class BackupVerifier
{
    internal const string AssemblyHash = "BD5465840091C480AA97073092B4A2BB93608D0BF218125D535BE0D7252D48EE";
    internal const string MetadataHash = "EBC951FB2ADE1EE2822AB0565D07108667D1FD608F47512A05E94D7D9043CC2A";
    internal static string GameDirectory => Path.GetDirectoryName(Process.GetCurrentProcess().MainModule!.FileName)!;

    internal static bool VerifyGame(out string error)
    {
        try
        {
            var assembly = Path.Combine(GameDirectory, "GameAssembly.dll");
            var metadata = Path.Combine(GameDirectory, "VampireSurvivors_Data", "il2cpp_data", "Metadata", "global-metadata.dat");
            if (!HashEquals(assembly, AssemblyHash) || !HashEquals(metadata, MetadataHash))
            {
                error = "游戏版本与已适配版本不一致，请重新适配。";
                return false;
            }
            error = "";
            return true;
        }
        catch (Exception e) { error = "无法校验游戏版本：" + e.Message; return false; }
    }

    internal static (bool Valid, string? Error) Verify(string? proofPath)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(proofPath)) return (false, "金币功能需要本次启动前的有效备份。");
            var proof = PipePeerGuard.ReadLocalBackupProof(proofPath);
            if (proof == null || proof.AppId != "1794680" || proof.Files.Count == 0)
                return (false, "备份凭据无效。");
            var created = proof.CreatedUtc.Kind == DateTimeKind.Utc ? proof.CreatedUtc : proof.CreatedUtc.ToUniversalTime();
            var start = Process.GetCurrentProcess().StartTime.ToUniversalTime();
            if (created > start || created < DateTime.UtcNow.AddHours(-12))
                return (false, "备份必须在本次游戏启动前完成，并且不超过 12 小时。");
            if (!Path.GetFullPath(proof.GameDirectory).TrimEnd(Path.DirectorySeparatorChar)
                    .Equals(Path.GetFullPath(GameDirectory).TrimEnd(Path.DirectorySeparatorChar), StringComparison.OrdinalIgnoreCase)
                || !proof.GameAssemblySha256.Equals(AssemblyHash, StringComparison.OrdinalIgnoreCase)
                || !proof.MetadataSha256.Equals(MetadataHash, StringComparison.OrdinalIgnoreCase))
                return (false, "备份与当前游戏版本或安装目录不匹配。");
            bool hasSave = false;
            var originalPaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var backupFiles = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(proofPath)!, "files")) + Path.DirectorySeparatorChar;
            foreach (var file in proof.Files)
            {
                if (!Path.IsPathFullyQualified(file.OriginalPath) || !Path.IsPathFullyQualified(file.BackupPath)
                    || !originalPaths.Add(Path.GetFullPath(file.OriginalPath))
                    || !Path.GetFullPath(file.BackupPath).StartsWith(backupFiles, StringComparison.OrdinalIgnoreCase)
                    || !File.Exists(file.BackupPath) || !HashEquals(file.BackupPath, file.Sha256))
                    return (false, "备份文件缺失或校验失败。");
                var path = Path.GetFullPath(file.OriginalPath).Replace('/', '\\');
                hasSave |= path.Contains("\\userdata\\", StringComparison.OrdinalIgnoreCase)
                    && path.EndsWith("\\1794680\\remote\\SaveData", StringComparison.OrdinalIgnoreCase)
                    && new FileInfo(file.BackupPath).Length > 0;
            }
            return hasSave ? (true, null) : (false, "备份中没有游戏存档。");
        }
        catch (Exception e) { return (false, "备份校验失败：" + e.Message); }
    }

    private static bool HashEquals(string path, string expected)
    {
        if (expected.Length != 64 || !File.Exists(path)) return false;
        using var stream = File.OpenRead(path);
        using var sha = SHA256.Create();
        return Convert.ToHexString(sha.ComputeHash(stream)).Equals(expected, StringComparison.OrdinalIgnoreCase);
    }
}
