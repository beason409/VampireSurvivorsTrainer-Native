using System.Collections.ObjectModel;
using System.Security.Cryptography;
using System.Text.Json;

namespace Trainer.Core;

public sealed record TrustedPayloadFile(string Path, long Length, string Sha256);

// This inventory is compiled into the application, not read from the mutable
// release-manifest.json beside it. Its trust boundary is the original program
// obtained from the official release; replacing that program can replace this
// check too. No encryption, private key, or Windows certificate is involved.
public sealed class TrustedPayloadCatalog
{
    public const string ResourceName = "Trainer.Core.TrustedPayloadCatalog.json";
    private static readonly Lazy<TrustedPayloadCatalog> Embedded = new(() =>
    {
        using var stream = typeof(TrustedPayloadCatalog).Assembly.GetManifestResourceStream(ResourceName)
            ?? throw new InvalidDataException("程序缺少内置配套文件指纹。请完整下载官方发行包；源码构建请先生成配套文件目录。");
        return Parse(stream);
    });
    private readonly TrustedPayloadFile _supported;
    public IReadOnlyDictionary<string, TrustedPayloadFile> Files { get; }

    private TrustedPayloadCatalog(Dictionary<string, TrustedPayloadFile> files, TrustedPayloadFile supported)
    {
        Files = new ReadOnlyDictionary<string, TrustedPayloadFile>(files);
        _supported = supported;
    }

    public static TrustedPayloadCatalog LoadEmbedded() => Embedded.Value;

    public static TrustedPayloadCatalog Parse(Stream stream)
    {
        ArgumentNullException.ThrowIfNull(stream);
        try { return ParseCore(stream); }
        catch (Exception error) when (error is JsonException or KeyNotFoundException or InvalidOperationException or FormatException or OverflowException)
        { throw new InvalidDataException("配套文件指纹内容无效。", error); }
    }

    private static TrustedPayloadCatalog ParseCore(Stream stream)
    {
        // The cap also applies to non-seekable input and prevents unbounded
        // allocation when this public parser is used by source-level tooling.
        using var bytes = new MemoryStream();
        byte[] buffer = new byte[8192];
        for (int count; (count = stream.Read(buffer, 0, buffer.Length)) != 0;)
        {
            if (bytes.Length + count > 1024 * 1024) throw new InvalidDataException("配套文件指纹清单过大。");
            bytes.Write(buffer, 0, count);
        }
        using var document = JsonDocument.Parse(bytes.ToArray(), new JsonDocumentOptions { MaxDepth = 16 });
        var root = document.RootElement;
        if (root.GetProperty("formatVersion").GetInt32() != 1) throw new InvalidDataException("配套文件指纹格式不支持。");
        var entries = root.GetProperty("files");
        if (entries.ValueKind != JsonValueKind.Array || entries.GetArrayLength() is < 1 or > 4096)
            throw new InvalidDataException("配套文件指纹集合无效。");
        var files = new Dictionary<string, TrustedPayloadFile>(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in entries.EnumerateArray())
        {
            string path = entry.GetProperty("path").GetString() ?? "";
            ValidateRelative(path);
            var file = ReadEntry(path, entry);
            if (!files.TryAdd(path, file)) throw new InvalidDataException("配套文件指纹包含重复路径。");
        }
        if (!files.ContainsKey("winhttp.dll")) throw new InvalidDataException("配套文件指纹缺少加载器。");
        var supported = ReadEntry("supported-game.json", root.GetProperty("supportedGame"));
        if (supported.Length is < 1 or > 65536) throw new InvalidDataException("游戏适配文件长度无效。");
        return new TrustedPayloadCatalog(files, supported);
    }

    private static TrustedPayloadFile ReadEntry(string path, JsonElement entry)
    {
        long length = entry.GetProperty("length").GetInt64();
        string hash = entry.GetProperty("sha256").GetString() ?? "";
        if (length < 0 || hash.Length != 64 || !hash.All(Uri.IsHexDigit)) throw new InvalidDataException("配套文件指纹内容无效。");
        return new(path, length, hash.ToUpperInvariant());
    }

    private static void ValidateRelative(string path)
    {
        if (path.Length is < 1 or > 4096 || path.Contains('\\') || Path.IsPathRooted(path)) throw new InvalidDataException("配套文件指纹路径无效。");
        foreach (string part in path.Split('/'))
        {
            if (part.Length is < 1 or > 255 || part is "." or ".." || part.EndsWith('.') || part.EndsWith(' ')
                || part.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || part.Any(c => c is '<' or '>' or ':' or '"' or '\\' or '|' or '?' or '*' or < ' '))
                throw new InvalidDataException("配套文件指纹路径无效。");
            string device = part.Split('.')[0].ToUpperInvariant();
            if (device is "CON" or "PRN" or "AUX" or "NUL" || (device.Length == 4 && device[3] is >= '1' and <= '9' && (device.StartsWith("COM") || device.StartsWith("LPT"))))
                throw new InvalidDataException("配套文件指纹路径无效。");
        }
    }

    public void VerifyPayload(string payloadDirectory)
    {
        string root = CoreFiles.Full(payloadDirectory);
        CoreFiles.RejectReparse(root);
        if (!Directory.Exists(root)) throw new DirectoryNotFoundException("找不到完整的配套文件目录。");
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (string path in EnumerateUnlinkedFiles(root))
        {
            string relative = Path.GetRelativePath(root, path).Replace('\\', '/');
            if (!seen.Add(relative) || !Files.TryGetValue(relative, out var expected))
                throw new InvalidDataException("配套目录存在未识别文件，已停止安装：" + relative);
            VerifyFile(path, expected);
        }
        if (seen.Count != Files.Count) throw new InvalidDataException("配套文件缺失，已停止安装。请完整解压官方发行包。");
    }

    public GameFingerprint ReadSupportedBuild(string supportedPath)
    {
        using var file = OpenVerifiedFile(supportedPath, _supported);
        byte[] bytes = new byte[checked((int)_supported.Length)];
        file.ReadExactly(bytes);
        if (!Convert.ToHexString(SHA256.HashData(bytes)).Equals(_supported.Sha256, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("游戏适配文件与内置指纹不一致，已停止操作。");
        return JsonSerializer.Deserialize<GameFingerprint>(bytes, CoreFiles.Json)
            ?? throw new InvalidDataException("游戏适配文件无效。");
    }

    // Installed caches, interop and logs belong to the loader/game. Fixed
    // distribution files and executable plugin/patcher directories are checked.
    public void VerifyInstalledPayload(string gameDirectory)
    {
        string game = CoreFiles.Full(gameDirectory);
        CoreFiles.RejectReparse(game);
        foreach (var expected in Files.Values) VerifyFile(CoreFiles.SafeChild(game, expected.Path), expected);
        foreach (string relativeRoot in new[] { "BepInEx/plugins", "BepInEx/patchers" })
        {
            string directory = CoreFiles.SafeChild(game, relativeRoot);
            if (!Directory.Exists(directory)) continue;
            foreach (string path in EnumerateUnlinkedFiles(directory))
            {
                string relative = Path.GetRelativePath(game, path).Replace('\\', '/');
                if (!Files.ContainsKey(relative)) throw new InvalidDataException("插件目录存在非本发行包的文件，已停止连接或启动：" + relative);
            }
        }
    }

    private static void VerifyFile(string path, TrustedPayloadFile expected)
    {
        using var file = OpenVerifiedFile(path, expected);
        if (!Convert.ToHexString(SHA256.HashData(file)).Equals(expected.Sha256, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("配套文件与内置指纹不一致，已停止操作：" + expected.Path);
    }

    private static FileStream OpenVerifiedFile(string path, TrustedPayloadFile expected)
    {
        CoreFiles.RejectReparse(path);
        // Deny writers/deletion while verifying this file. Installation also
        // checks the copied bytes against the same immutable expected hash.
        var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        if (file.Length == expected.Length) return file;
        file.Dispose();
        throw new InvalidDataException("配套文件长度与内置指纹不一致，已停止操作：" + expected.Path);
    }

    private static IEnumerable<string> EnumerateUnlinkedFiles(string root)
    {
        var pending = new Stack<string>();
        pending.Push(root);
        while (pending.TryPop(out string? directory))
        {
            foreach (string entry in Directory.EnumerateFileSystemEntries(directory))
            {
                var attributes = File.GetAttributes(entry);
                if ((attributes & FileAttributes.ReparsePoint) != 0) throw new InvalidDataException("配套目录包含链接，已停止操作。");
                if ((attributes & FileAttributes.Directory) != 0) pending.Push(entry);
                else yield return entry;
            }
        }
    }
}
