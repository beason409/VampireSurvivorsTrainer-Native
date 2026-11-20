using System.Reflection;
using System.Reflection.PortableExecutable;
using System.Security.Cryptography;
using System.Text.Json;

namespace Trainer.Desktop;

internal static class ReleaseInfo
{
    internal const string Author = "Beason9527";

    internal static string Version => (typeof(ReleaseInfo).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion
        ?? typeof(ReleaseInfo).Assembly.GetName().Version?.ToString(3) ?? "未知版本").Split('+')[0];

    internal static Uri? RepositoryUri
    {
        get
        {
            try
            {
                string path = Path.Combine(AppContext.BaseDirectory, "project-info.json");
                if (!File.Exists(path)) return null;
                using var document = JsonDocument.Parse(File.ReadAllText(path));
                string? value = document.RootElement.GetProperty("repositoryUrl").GetString();
                return Uri.TryCreate(value, UriKind.Absolute, out var uri) && uri.Scheme == Uri.UriSchemeHttps
                    && uri.Host.Equals("github.com", StringComparison.OrdinalIgnoreCase)
                    && string.IsNullOrEmpty(uri.UserInfo) && string.IsNullOrEmpty(uri.Query) && string.IsNullOrEmpty(uri.Fragment) ? uri : null;
            }
            catch { return null; }
        }
    }

    internal static string SignatureDescription()
    {
        try
        {
            string? executable = Environment.ProcessPath;
            if (executable is null) return "无法定位程序文件，未检查签名。";
            if (!Path.GetFileNameWithoutExtension(executable).Equals(typeof(ReleaseInfo).Assembly.GetName().Name, StringComparison.OrdinalIgnoreCase))
                return "当前通过其他启动程序运行，未检查应用程序的发布者签名。";
            using var file = File.OpenRead(executable);
            using var pe = new PEReader(file, PEStreamOptions.LeaveOpen);
            var certificate = pe.PEHeaders.PEHeader?.CertificateTableDirectory;
            if (certificate is null || certificate.Value.Size == 0)
                return "未进行 Windows 代码签名。作者署名与 Windows 发布者证书分别显示；可结合下载来源、SHA-256 校验值和公开源码核对文件。";
            // The PE security directory points to file offsets rather than RVAs.
            // Presence is deliberately not a Windows trust/Authenticode verdict.
            if (certificate.Value.RelativeVirtualAddress < 0 || certificate.Value.Size < 8
                || (long)certificate.Value.RelativeVirtualAddress + certificate.Value.Size > file.Length)
                return "程序的签名数据范围无效；请从项目发行页重新下载并核对校验值。";
            return "检测到嵌入式签名数据，尚未验证发布者或签名有效性。\n请在文件属性的“数字签名”页核对发布者及 Windows 验证结果。";
        }
        catch (Exception) { return "无法读取发布者签名。请在文件属性中核对数字签名与发布者。"; }
    }

    internal static bool PayloadMatchesGame(string payload, string game)
    {
        var files = Directory.EnumerateFiles(payload, "*", new EnumerationOptions
        { RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint }).ToArray();
        if (files.Length == 0) return false;
        foreach (string source in files)
        {
            string destination = Path.Combine(game, Path.GetRelativePath(payload, source));
            if (!File.Exists(destination) || new FileInfo(source).Length != new FileInfo(destination).Length) return false;
            if (!HashFile(source).AsSpan().SequenceEqual(HashFile(destination))) return false;
        }
        return true;
    }

    private static byte[] HashFile(string path)
    {
        using var stream = File.OpenRead(path);
        return SHA256.HashData(stream);
    }

    internal static string VerifyPackage(string directory)
    {
        directory = Path.GetFullPath(directory);
        string manifestPath = Path.Combine(directory, "release-manifest.json");
        if (!File.Exists(manifestPath))
            throw new FileNotFoundException("此目录没有发行文件清单。开发构建可正常使用；正式发行包请完整解压后再校验。");
        using var document = JsonDocument.Parse(File.ReadAllText(manifestPath));
        var root = document.RootElement;
        if (root.GetProperty("formatVersion").GetInt32() != 1) throw new InvalidDataException("不支持的发行清单版本。");
        var files = root.GetProperty("files");
        if (files.ValueKind != JsonValueKind.Array || files.GetArrayLength() == 0 || files.GetArrayLength() > 10000)
            throw new InvalidDataException("发行清单中的文件集合无效。");
        var expected = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var item in files.EnumerateArray())
        {
            string relative = item.GetProperty("path").GetString() ?? "";
            string[] parts = relative.Split('/');
            if (relative.Contains('\\') || parts.Any(part => string.IsNullOrWhiteSpace(part) || part is "." or ".." || part.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
                || Path.IsPathRooted(relative) || parts[0].Equals("data", StringComparison.OrdinalIgnoreCase)
                || relative.Equals("release-manifest.json", StringComparison.OrdinalIgnoreCase) || !expected.Add(relative))
                throw new InvalidDataException("发行清单包含重复或不允许的路径。");
            string path = directory;
            foreach (string part in parts)
            {
                path = Path.Combine(path, part);
                if ((File.Exists(path) || Directory.Exists(path)) && (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
                    throw new InvalidDataException("不能校验链接文件或目录：" + relative);
            }
            string digest = item.GetProperty("sha256").GetString() ?? "";
            long length = item.GetProperty("length").GetInt64();
            if (length < 0 || digest.Length != 64 || !digest.All(Uri.IsHexDigit))
                throw new InvalidDataException("发行清单中的校验值无效：" + relative);
            if (!File.Exists(path)) throw new InvalidDataException("缺少发行文件：" + relative);
            if (new FileInfo(path).Length != length || !Convert.ToHexString(HashFile(path)).Equals(digest, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("文件与发行清单不一致：" + relative);
        }
        VerifyInventory(directory, directory, expected);
        return $"已核对 {expected.Count} 个发行文件，长度与 SHA-256 均匹配。\n清单用于发现缺失、改动和额外文件；它不是发布者签名，仍需核对下载来源及发行页校验值。";
    }

    private static void VerifyInventory(string directory, string current, HashSet<string> expected)
    {
        foreach (string entry in Directory.EnumerateFileSystemEntries(current))
        {
            string relative = Path.GetRelativePath(directory, entry).Replace('\\', '/');
            if (relative.Equals("data", StringComparison.OrdinalIgnoreCase)) continue;
            if ((File.GetAttributes(entry) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("发行目录包含链接：" + relative);
            if (Directory.Exists(entry)) VerifyInventory(directory, entry, expected);
            else if (!relative.Equals("release-manifest.json", StringComparison.OrdinalIgnoreCase) && !expected.Contains(relative))
                throw new InvalidDataException("发行目录中发现清单外文件：" + relative);
        }
    }
}
