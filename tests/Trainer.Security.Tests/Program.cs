using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Trainer.Core;
using Trainer.Desktop;

var results = new List<object>();
int failed = 0;
async Task Check(string name, Func<Task> action)
{
    try { await action(); results.Add(new { name, passed = true }); Console.WriteLine("PASS " + name); }
    catch (Exception error) { failed++; results.Add(new { name, passed = false, error = error.ToString() }); Console.Error.WriteLine("FAIL " + name + ": " + error); }
}
Task Sync(Action action) { action(); return Task.CompletedTask; }
void Require(bool value, string reason) { if (!value) throw new Exception(reason); }
void Reject(Action action)
{
    try { action(); }
    catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or ArgumentException) { return; }
    throw new Exception("Operation should have been rejected.");
}
async Task RejectRead(BoundedResponseReader reader, CancellationToken ct = default)
{
    try { await reader.ReadLineAsync(ct); }
    catch (Exception error) when (error is IOException or DecoderFallbackException or OperationCanceledException) { return; }
    throw new Exception("Response should have been rejected.");
}

await Check("embedded inventory is required rather than falling back to an external manifest", () => Sync(() =>
{
    // Source-only builds may have no generated resource; a packaged build must
    // load its compiled resource. Both branches are explicit and fail closed.
    using var embedded = typeof(TrustedPayloadCatalog).Assembly.GetManifestResourceStream(TrustedPayloadCatalog.ResourceName);
    if (embedded is null) Reject(() => TrustedPayloadCatalog.LoadEmbedded());
    else Require(TrustedPayloadCatalog.LoadEmbedded().Files.Count > 0, "Embedded resource was ignored.");
}));
await Check("original complete payload and compatibility bytes pass", () => Sync(() =>
{
    var fixture = new Fixture(); fixture.Catalog.VerifyPayload(fixture.Payload);
    Require(fixture.Catalog.ReadSupportedBuild(fixture.Supported).BuildId == "25984009", "Wrong trusted build.");
}));
await Check("same-length byte substitution is rejected even when the loose manifest is changed", () => Sync(() =>
{
    var fixture = new Fixture(); File.WriteAllText(Path.Combine(fixture.Payload, "winhttp.dll"), "evil!!");
    File.WriteAllText(Path.Combine(fixture.Root, "release-manifest.json"), "{\"files\":[]}");
    Reject(() => fixture.Catalog.VerifyPayload(fixture.Payload));
}));
await Check("missing payload file is rejected", () => Sync(() =>
{
    var fixture = new Fixture(); File.Delete(Path.Combine(fixture.Payload, "winhttp.dll"));
    Reject(() => fixture.Catalog.VerifyPayload(fixture.Payload));
}));
await Check("extra plugin DLL is rejected and preserved for the user", () => Sync(() =>
{
    var fixture = new Fixture(); string extra = Path.Combine(fixture.Payload, "BepInEx", "plugins", "extra.dll");
    File.WriteAllText(extra, "extra"); Reject(() => fixture.Catalog.VerifyPayload(fixture.Payload));
    Require(File.ReadAllText(extra) == "extra", "Check changed the extra file.");
}));
await Check("hidden extra files are also checked", () => Sync(() =>
{
    var fixture = new Fixture(); string extra = Path.Combine(fixture.Payload, "hidden.dll");
    File.WriteAllText(extra, "hidden"); File.SetAttributes(extra, FileAttributes.Hidden);
    Reject(() => fixture.Catalog.VerifyPayload(fixture.Payload));
}));
await Check("modified compatibility file is rejected", () => Sync(() =>
{
    var fixture = new Fixture(); File.AppendAllText(fixture.Supported, " ");
    Reject(() => fixture.Catalog.ReadSupportedBuild(fixture.Supported));
}));
await Check("verified compatibility is parsed from the same byte snapshot", () => Sync(() =>
{
    var fixture = new Fixture(); var supported = fixture.Catalog.ReadSupportedBuild(fixture.Supported);
    File.WriteAllText(fixture.Supported, "{}");
    Require(supported.BuildId == "25984009", "Returned object was replaced by a later read.");
    Reject(() => fixture.Catalog.ReadSupportedBuild(fixture.Supported));
}));
foreach (string badPath in new[] { "../outside.dll", "BepInEx/../outside.dll", "BepInEx\\bad.dll", "/rooted.dll", "x:alternate", "BepInEx//bad.dll", "BepInEx/NUL.dll", "bad.dll.", "bad.dll ", "bad?.dll" })
    await Check("catalog rejects unsafe path " + badPath, () => Sync(() =>
    {
        var fixture = new Fixture(); var document = JsonNode.Parse(fixture.Json)!;
        document["files"]![0]!["path"] = badPath;
        Reject(() => Fixture.Parse(document.ToJsonString()));
    }));
await Check("case-aliased duplicate paths are rejected", () => Sync(() =>
{
    var fixture = new Fixture(); var document = JsonNode.Parse(fixture.Json)!;
    document["files"]!.AsArray().Add(JsonNode.Parse(document["files"]![0]!.ToJsonString()));
    document["files"]![2]!["path"] = "WINHTTP.DLL";
    Reject(() => Fixture.Parse(document.ToJsonString()));
}));
await Check("invalid and oversized inventories are rejected", () => Sync(() =>
{
    foreach (string json in new[] { "{}", "null", "[]", "{\"formatVersion\":2}", "{", new string(' ', 1024 * 1024 + 1) }) Reject(() => Fixture.Parse(json));
    var fixture = new Fixture(); var document = JsonNode.Parse(fixture.Json)!;
    document["files"]![0]!["length"] = -1; Reject(() => Fixture.Parse(document.ToJsonString()));
    document = JsonNode.Parse(fixture.Json)!; document["files"]![0]!["sha256"] = "bad";
    Reject(() => Fixture.Parse(document.ToJsonString()));
}));
await Check("compiled expectations cannot be changed through the public dictionary", () => Sync(() =>
{
    var fixture = new Fixture(); var dictionary = (IDictionary<string, TrustedPayloadFile>)fixture.Catalog.Files;
    try { dictionary.Clear(); throw new Exception("Dictionary was mutable."); } catch (NotSupportedException) { }
    Require(fixture.Catalog.Files.Count == 2, "Expected files changed.");
}));
await Check("installed files allow generated cache but reject extra plugin or patcher content", () => Sync(() =>
{
    var fixture = new Fixture(); string cache = Path.Combine(fixture.Payload, "BepInEx", "cache", "generated.dll");
    Directory.CreateDirectory(Path.GetDirectoryName(cache)!); File.WriteAllText(cache, "generated");
    fixture.Catalog.VerifyInstalledPayload(fixture.Payload);
    string patcher = Path.Combine(fixture.Payload, "BepInEx", "patchers", "extra.dll");
    Directory.CreateDirectory(Path.GetDirectoryName(patcher)!); File.WriteAllText(patcher, "extra");
    Reject(() => fixture.Catalog.VerifyInstalledPayload(fixture.Payload));
}));

await Check("response reader preserves multiple buffered lines and Unicode", async () =>
{
    using var stream = new MemoryStream(Encoding.UTF8.GetBytes("你好\r\nnext\n"));
    using var source = new StreamReader(stream, new UTF8Encoding(false, true));
    var reader = new BoundedResponseReader(source, 20);
    Require(await reader.ReadLineAsync(default) == "你好", "Unicode/CRLF changed.");
    Require(await reader.ReadLineAsync(default) == "next", "Buffered next line lost.");
    Require(await reader.ReadLineAsync(default) is null, "Clean EOF was not returned.");
});
await Check("exact length limit and a large chunked response are accepted", async () =>
{
    string text = new('x', 8192);
    using var stream = new CountingStream(Encoding.UTF8.GetBytes(text + "\n"), 1000);
    using var source = new StreamReader(stream, new UTF8Encoding(false, true));
    var reader = new BoundedResponseReader(source, text.Length);
    Require(await reader.ReadLineAsync(default) == text, "Chunked response changed.");
    Require(stream.Reads < 30, "Reader regressed to a read per character.");
});
await Check("oversized response stops reading before the whole response is allocated", async () =>
{
    using var stream = new CountingStream(Encoding.UTF8.GetBytes(new string('x', 30000) + "\n"), 1000);
    using var source = new StreamReader(stream, new UTF8Encoding(false, true));
    await RejectRead(new BoundedResponseReader(source, 64));
    Require(stream.BytesRead < 30000, "Oversized response was fully consumed.");
});
await Check("truncated and invalid-UTF8 responses are rejected", async () =>
{
    using var truncated = new StreamReader(new MemoryStream(Encoding.UTF8.GetBytes("partial")), new UTF8Encoding(false, true));
    await RejectRead(new BoundedResponseReader(truncated, 64));
    using var invalid = new StreamReader(new MemoryStream(new byte[] { 0xff, 0x0a }), new UTF8Encoding(false, true));
    await RejectRead(new BoundedResponseReader(invalid, 64));
});
await Check("canceled response read does not consume input", async () =>
{
    using var stream = new CountingStream(Encoding.UTF8.GetBytes("data\n"), 1000);
    using var source = new StreamReader(stream, new UTF8Encoding(false, true));
    using var cancellation = new CancellationTokenSource(); cancellation.Cancel();
    await RejectRead(new BoundedResponseReader(source, 64), cancellation.Token);
    Require(stream.BytesRead == 0, "Canceled read consumed data.");
});

string output = Path.GetFullPath(args.FirstOrDefault() ?? "artifacts/security-hardening/catalog-regression.json");
Directory.CreateDirectory(Path.GetDirectoryName(output)!);
File.WriteAllText(output, JsonSerializer.Serialize(new { passed = results.Count - failed, failed, gameConnected = false, userDataTouched = false, checks = results }, new JsonSerializerOptions { WriteIndented = true }));
Console.WriteLine($"{results.Count - failed}/{results.Count} security checks passed.");
return failed == 0 ? 0 : 1;

sealed class Fixture
{
    internal string Root { get; } = Path.GetFullPath(Path.Combine("tests", "Trainer.Security.Tests", ".artifacts", Guid.NewGuid().ToString("N")));
    internal string Payload => Path.Combine(Root, "payload");
    internal string Supported => Path.Combine(Root, "supported-game.json");
    internal string Json { get; }
    internal TrustedPayloadCatalog Catalog { get; }
    internal Fixture()
    {
        Directory.CreateDirectory(Path.Combine(Payload, "BepInEx", "plugins"));
        File.WriteAllText(Path.Combine(Payload, "winhttp.dll"), "loader");
        File.WriteAllText(Path.Combine(Payload, "BepInEx", "plugins", "Trainer.Plugin.dll"), "plugin");
        File.WriteAllText(Supported, "{\"buildId\":\"25984009\"}");
        Json = JsonSerializer.Serialize(new { formatVersion = 1, files = Directory.GetFiles(Payload, "*", SearchOption.AllDirectories).OrderBy(path => Path.GetRelativePath(Payload, path), StringComparer.OrdinalIgnoreCase).Select(path => new { path = Path.GetRelativePath(Payload, path).Replace('\\', '/'), length = new FileInfo(path).Length, sha256 = Hash(path) }), supportedGame = new { length = new FileInfo(Supported).Length, sha256 = Hash(Supported) } });
        Catalog = Parse(Json);
    }
    internal static TrustedPayloadCatalog Parse(string json) { using var input = new MemoryStream(Encoding.UTF8.GetBytes(json)); return TrustedPayloadCatalog.Parse(input); }
    private static string Hash(string path) { using var stream = File.OpenRead(path); return Convert.ToHexString(SHA256.HashData(stream)); }
}

sealed class CountingStream(byte[] bytes, int chunk) : Stream
{
    private readonly MemoryStream _inner = new(bytes);
    internal int Reads { get; private set; }
    internal long BytesRead => _inner.Position;
    public override bool CanRead => true;
    public override bool CanSeek => false;
    public override bool CanWrite => false;
    public override long Length => _inner.Length;
    public override long Position { get => _inner.Position; set => throw new NotSupportedException(); }
    public override int Read(byte[] buffer, int offset, int count) { Reads++; return _inner.Read(buffer, offset, Math.Min(count, chunk)); }
    public override ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken ct = default) { ct.ThrowIfCancellationRequested(); Reads++; return _inner.ReadAsync(buffer[..Math.Min(buffer.Length, chunk)], ct); }
    protected override void Dispose(bool disposing) { if (disposing) _inner.Dispose(); base.Dispose(disposing); }
    public override void Flush() { }
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
}
