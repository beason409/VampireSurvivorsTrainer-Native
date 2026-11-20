using System.Diagnostics;
using System.IO.Pipes;
using System.Reflection;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using BepInEx.Logging;
using Microsoft.Win32.SafeHandles;
using Trainer.Plugin;
using Trainer.Shared;

if (args is ["--peer", var peerName])
{
    using var client = new NamedPipeClientStream(".", peerName, PipeDirection.InOut, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
    await client.ConnectAsync(3000);
    Console.WriteLine("connected");
    await Console.In.ReadLineAsync();
    return;
}

var results = new List<object>();
string scratch = Path.Combine(Path.GetTempPath(), "Trainer-Pipe-Tests-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(scratch);
var log = new ManualLogSource();
var runtime = new TrainerRuntime();
using var server = new PipeServer(runtime, log);
try
{
    await Run("native current-user ACL, noninherited handle, first instance, actual PID/session and live process handle", async () =>
    {
        string name = "Trainer.Pipe.Test." + Guid.NewGuid().ToString("N");
        using var pipe = PipePeerGuard.CreateServer(name);
        using var identity = WindowsIdentity.GetCurrent();
        var rules = pipe.GetAccessControl().GetAccessRules(true, false, typeof(SecurityIdentifier)).Cast<PipeAccessRule>().ToArray();
        Assert(rules.Length == 1 && rules[0].AccessControlType == AccessControlType.Allow
            && rules[0].IdentityReference.Value == identity.User!.Value, "ACL admits another account");
        Assert(GetHandleInformation(pipe.SafePipeHandle, out uint flags) && (flags & 1) == 0, "Pipe handle is inheritable");
        Throws(() => { using var duplicate = PipePeerGuard.CreateServer(name); }, "First-instance guard allowed a duplicate server");
        var childStart = new ProcessStartInfo(Environment.ProcessPath!)
        {
            UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true
        };
        if (string.Equals(Path.GetFileNameWithoutExtension(Environment.ProcessPath), "dotnet", StringComparison.OrdinalIgnoreCase))
            childStart.ArgumentList.Add(Assembly.GetExecutingAssembly().Location);
        childStart.ArgumentList.Add("--peer"); childStart.ArgumentList.Add(name);
        using var child = Process.Start(childStart)!;
        try
        {
            try { await pipe.WaitForConnectionAsync().WaitAsync(TimeSpan.FromSeconds(4)); }
            catch (TimeoutException)
            {
                if (!child.HasExited) { child.Kill(); await child.WaitForExitAsync(); }
                throw new Exception("Child failed to connect: " + await child.StandardError.ReadToEndAsync() + await child.StandardOutput.ReadToEndAsync());
            }
            using var peer = PipePeerGuard.Verify(pipe);
            Assert(peer.ProcessId == child.Id && peer.SessionId == Process.GetCurrentProcess().SessionId, "OS peer identity mismatch");
            peer.EnsureAlive();
            var nativeHandle = (SafeProcessHandle)typeof(PipePeerGuard).GetField("_process", BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(peer)!;
            await child.StandardInput.WriteLineAsync("exit");
            await child.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(4));
            Assert(child.ExitCode == 0, "Child failed: " + await child.StandardError.ReadToEndAsync());
            await Until(() => { try { peer.EnsureAlive(); return false; } catch (IOException) { return true; } }, 1000);
            peer.Dispose();
            Assert(nativeHandle.IsClosed, "Peer process handle leaked on dispose");
        }
        finally { if (!child.HasExited) { child.Kill(); await child.WaitForExitAsync(); } }
    });

    server.Start(); server.Start();
    await Run("hello without backup remains connected and authorized commands follow; repeated hello closes", async () =>
    {
        await using var wire = await Wire.Connect();
        var hello = await wire.Exchange(new Request { Kind = "hello" });
        Assert(hello.Ok && hello.State?.BackupAuthorized == false, "Missing backup incorrectly blocked hello or authorized gold");
        foreach (string kind in new[] { "heartbeat", "getState", "getGold", "getItemCatalog", "getItemIcon", "disableAll", "disableStats", "setFeature", "setStat", "setChestReward", "setGold" })
            Assert((await wire.Exchange(new Request { Kind = kind })).Ok, "Whitelist rejected " + kind);
        Assert(!(await wire.Exchange(new Request { Kind = "hello" })).Ok, "Repeated hello succeeded");
        await wire.ExpectClosed();
    });
    await Run("command before hello and unknown command never reach runtime; disconnect clears effects", async () =>
    {
        int before = runtime.Requests.Count;
        await using (var wire = await Wire.Connect())
        {
            Assert(!(await wire.Exchange(new Request { Kind = "setFeature" })).Ok, "Pre-hello mutation accepted");
            await wire.ExpectClosed();
        }
        Assert(runtime.Requests.Count == before, "Pre-hello request submitted");
        await using (var wire = await Wire.Connect())
        {
            await wire.Exchange(new Request { Kind = "hello" });
            await wire.Exchange(new Request { Kind = "setFeature" });
            Assert(runtime.Effects, "Fixture mutation did not run");
            int submitted = runtime.Requests.Count;
            Assert(!(await wire.Exchange(new Request { Kind = "runShell", ItemId = "ignored" })).Ok, "Unknown command accepted");
            await wire.ExpectClosed();
            Assert(runtime.Requests.Count == submitted && !runtime.Effects, "Rejected command reached runtime or retained effects");
        }
    });
    await Run("native handshake deadline closes idle and slow-drip peers and permits reconnection", async () =>
    {
        foreach (bool drip in new[] { false, true })
        {
            await using var wire = await Wire.Connect();
            var watch = Stopwatch.StartNew();
            if (drip)
                for (int i = 0; i < 3; i++) { await wire.Raw(" "); await Task.Delay(400); }
            await wire.ExpectClosed(3500);
            Assert(watch.Elapsed.TotalSeconds is >= 1.5 and < 3.4, "Whole-line deadline reset by incoming bytes");
        }
        await using var again = await Wire.Connect();
        Assert((await again.Exchange(new Request { Kind = "hello" })).Ok, "Reconnect after timeout failed");
    });
    await Run("post-hello idle read deadline releases connection while independent watchdog keeps ticking", async () =>
    {
        await using var wire = await Wire.Connect();
        await wire.Exchange(new Request { Kind = "hello" });
        int checks = runtime.Checks;
        var watch = Stopwatch.StartNew();
        await wire.ExpectClosed(6500);
        Assert(watch.Elapsed.TotalSeconds is >= 4.5 and < 6.4 && runtime.Checks >= checks + 12, "Idle read blocked watchdog or deadline");
    });
    await Run("bad JSON, invalid IDs, malformed UTF-8, oversized and incomplete lines fail closed", async () =>
    {
        foreach (string value in new[] { "null", "{bad", "{\"kind\":\"hello\",\"id\":\"\"}", JsonSerializer.Serialize(new Request { Kind = "hello", Id = new string('X', 129) }) })
        {
            await using var wire = await Wire.Connect();
            await wire.Raw(value + "\n");
            Assert(!(await wire.Read()).Ok, "Malformed frame accepted");
            await wire.ExpectClosed();
        }
        await using (var wire = await Wire.Connect())
        {
            await wire.Pipe.WriteAsync(new byte[] { 0xc3, 0x28, 0x0a });
            await wire.ExpectClosed();
        }
        await using (var wire = await Wire.Connect())
        {
            try { await wire.Raw(new string('X', PipeProtocol.MaxMessageCharacters + 1) + "\n"); } catch (IOException) { }
            await wire.ExpectClosed();
        }
        var method = typeof(PipeServer).GetMethod("ReadBoundedLine", BindingFlags.NonPublic | BindingFlags.Static)!;
        using var partial = new StreamReader(new MemoryStream(Encoding.UTF8.GetBytes("{\"kind\":\"hello\"}")));
        var partialTask = (Task<string?>)method.Invoke(null, new object[] { partial, TimeSpan.FromSeconds(1), CancellationToken.None })!;
        await ThrowsAsync(() => partialTask, "Truncated frame was accepted");
    });
    await Run("native write deadline releases a peer that stops consuming responses", async () =>
    {
        runtime.Respond = request => Task.FromResult(new Response { Id = request.Id, Ok = true, Error = new string('A', 220000) });
        try
        {
            int ended = runtime.Ended;
            await using var wire = await Wire.Connect();
            await wire.Raw(JsonSerializer.Serialize(new Request { Kind = "hello" }, PipeProtocol.JsonOptions) + "\n");
            await Until(() => runtime.Ended > ended, 4000);
            Assert(log.Warnings.Any(w => w.Contains("写入超时")), "Write deadline was not exercised");
        }
        finally { runtime.Respond = null; }
    });
    await Run("watchdog survives one check exception; normal command stream stays alive beyond read timeout", async () =>
    {
        int checks = runtime.Checks;
        runtime.FailNextCheck = 1;
        await Until(() => runtime.Checks >= checks + 3, 1800);
        Assert(log.Warnings.Any(w => w.Contains("heartbeat check failed")), "Watchdog exception fixture not exercised");
        await using var wire = await Wire.Connect();
        await wire.Exchange(new Request { Kind = "hello" });
        for (int i = 0; i < 11; i++)
        { await Task.Delay(500); Assert((await wire.Exchange(new Request { Kind = i % 2 == 0 ? "heartbeat" : "getItemIcon" })).Ok, "Active stream disconnected"); }
    });
    await Run("real backup verification accepts local BOM proof; UNC/device/ADS/relative/oversized paths remain unauthorized", async () =>
    {
        Directory.CreateDirectory(Path.Combine(scratch, "files"));
        string backup = Path.Combine(scratch, "files", "save.backup"), proof = Path.Combine(scratch, "proof.json");
        File.WriteAllText(backup, "fixture-save-data");
        var valid = new BackupProof
        {
            CreatedUtc = Process.GetCurrentProcess().StartTime.ToUniversalTime().AddSeconds(-1),
            GameDirectory = BackupVerifier.GameDirectory,
            GameAssemblySha256 = BackupVerifier.AssemblyHash, MetadataSha256 = BackupVerifier.MetadataHash,
            Files = { new BackupProofFile
            {
                OriginalPath = Path.Combine(scratch, "userdata", "fixture", "1794680", "remote", "SaveData"),
                BackupPath = backup, Sha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(backup)))
            } }
        };
        File.WriteAllText(proof, JsonSerializer.Serialize(valid, PipeProtocol.JsonOptions), new UTF8Encoding(true));
        await using (var wire = await Wire.Connect())
        {
            var response = await wire.Exchange(new Request { Kind = "hello", BackupToken = proof });
            Assert(response.Ok && response.State?.BackupAuthorized == true, "Local proof/BOM rejected");
        }
        foreach (string path in new[] { @"\\untrusted.invalid\share\proof.json", @"\\?\C:\proof.json", proof + ":stream", "relative.json", "Z:\\absent\\proof.json" })
        {
            await using var wire = await Wire.Connect();
            var watch = Stopwatch.StartNew();
            var response = await wire.Exchange(new Request { Kind = "hello", BackupToken = path });
            Assert(response.Ok && response.State?.BackupAuthorized == false && watch.ElapsedMilliseconds < 1000, "Untrusted proof location blocked or was authorized");
        }
        File.WriteAllBytes(proof, new byte[1024 * 1024 + 1]);
        await using var last = await Wire.Connect();
        Assert((await last.Exchange(new Request { Kind = "hello", BackupToken = proof })).State?.BackupAuthorized == false, "Oversized proof was authorized");
        foreach (bool badOriginal in new[] { false, true })
        {
            var entry = valid.Files[0]; string original = entry.OriginalPath, backupPath = entry.BackupPath;
            if (badOriginal) entry.OriginalPath = @"\\untrusted.invalid\userdata\fixture\1794680\remote\SaveData";
            else entry.BackupPath = @"\\untrusted.invalid\share\backup";
            File.WriteAllText(proof, JsonSerializer.Serialize(valid, PipeProtocol.JsonOptions));
            var watch = Stopwatch.StartNew();
            Assert(!BackupVerifier.Verify(proof).Valid && watch.ElapsedMilliseconds < 1000, "Remote path in proof was read or authorized");
            entry.OriginalPath = original; entry.BackupPath = backupPath;
        }
        valid.Files = Enumerable.Repeat(valid.Files[0], 257).ToList();
        File.WriteAllText(proof, JsonSerializer.Serialize(valid, PipeProtocol.JsonOptions));
        Assert(!BackupVerifier.Verify(proof).Valid, "Proof accepted more than 256 files");
    });
    await Run("local junctions are rejected before probing existing or missing proof/backup target leaves", async () =>
    {
        string target = Path.Combine(scratch, "real"), link = Path.Combine(scratch, "junction");
        Directory.CreateDirectory(target);
        using (var mk = Process.Start(new ProcessStartInfo("cmd.exe")
        { ArgumentList = { "/d", "/c", "mklink", "/J", link, target }, UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true })!)
        { await mk.WaitForExitAsync(); Assert(mk.ExitCode == 0, "Could not create local test junction: " + await mk.StandardError.ReadToEndAsync()); }
        try
        {
            string backup = Path.Combine(target, "backup"), proof = Path.Combine(target, "proof.json");
            File.WriteAllText(backup, "fixture");
            File.WriteAllText(proof, JsonSerializer.Serialize(new BackupProof { Files = { new BackupProofFile { OriginalPath = backup, BackupPath = backup } } }, PipeProtocol.JsonOptions));
            Throws(() => PipePeerGuard.ReadLocalBackupProof(Path.Combine(link, "proof.json")), "Proof junction accepted");
            ThrowsReparse(() => PipePeerGuard.ReadLocalBackupProof(Path.Combine(link, "does-not-exist", "proof.json")), "Missing proof leaf was queried before rejecting ancestor junction");
            File.WriteAllText(proof, JsonSerializer.Serialize(new BackupProof { Files = { new BackupProofFile { OriginalPath = backup, BackupPath = Path.Combine(link, "backup") } } }, PipeProtocol.JsonOptions));
            Throws(() => PipePeerGuard.ReadLocalBackupProof(proof), "Backup file junction accepted");
            File.WriteAllText(proof, JsonSerializer.Serialize(new BackupProof { Files = { new BackupProofFile { OriginalPath = backup, BackupPath = Path.Combine(link, "does-not-exist", "backup") } } }, PipeProtocol.JsonOptions));
            ThrowsReparse(() => PipePeerGuard.ReadLocalBackupProof(proof), "Missing backup leaf was queried before rejecting ancestor junction");
        }
        finally { Directory.Delete(link); }
    });
    Console.WriteLine(JsonSerializer.Serialize(new { passed = results.Count, tests = results }, new JsonSerializerOptions { WriteIndented = true }));
}
finally
{
    server.Dispose();
    // This is a newly created test directory with known contents and no remaining
    // junctions. It contains no game/user save paths.
    string cleanup = Path.GetFullPath(scratch);
    string tempRoot = Path.GetFullPath(Path.GetTempPath()).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
    Assert(cleanup.StartsWith(tempRoot, StringComparison.OrdinalIgnoreCase)
        && Path.GetFileName(cleanup).StartsWith("Trainer-Pipe-Tests-", StringComparison.Ordinal)
        && (File.GetAttributes(cleanup) & FileAttributes.ReparsePoint) == 0, "Temporary cleanup target escaped expected root");
    Directory.Delete(cleanup, true);
}

async Task Run(string name, Func<Task> action)
{
    var timer = Stopwatch.StartNew(); await action();
    results.Add(new { name, milliseconds = Math.Round(timer.Elapsed.TotalMilliseconds, 1) });
    Console.WriteLine("PASS " + name);
}
static void Assert(bool value, string message) { if (!value) throw new Exception(message); }
static void Throws(Action action, string message) { try { action(); } catch { return; } throw new Exception(message); }
static void ThrowsReparse(Action action, string message)
{ try { action(); } catch (InvalidDataException e) when (e.Message.Contains("符号链接")) { return; } throw new Exception(message); }
static async Task ThrowsAsync(Func<Task> action, string message) { try { await action(); } catch { return; } throw new Exception(message); }
static async Task Until(Func<bool> predicate, int timeout)
{ var timer = Stopwatch.StartNew(); while (!predicate()) { if (timer.ElapsedMilliseconds > timeout) throw new TimeoutException("Condition did not occur"); await Task.Delay(20); } }

[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
[return: System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.Bool)]
static extern bool GetHandleInformation(SafePipeHandle handle, out uint flags);

sealed class Wire : IAsyncDisposable
{
    internal NamedPipeClientStream Pipe { get; }
    private readonly StreamReader reader;
    private Wire(NamedPipeClientStream pipe) { Pipe = pipe; reader = new StreamReader(pipe, new UTF8Encoding(false, true), false, 4096, true); }
    internal static async Task<Wire> Connect()
    {
        var pipe = new NamedPipeClientStream(".", PipeProtocol.PipeName(Environment.ProcessId), PipeDirection.InOut, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
        try { await pipe.ConnectAsync(4000); return new Wire(pipe); } catch { pipe.Dispose(); throw; }
    }
    internal async Task Raw(string value) => await Pipe.WriteAsync(Encoding.UTF8.GetBytes(value)).AsTask().WaitAsync(TimeSpan.FromSeconds(4));
    internal async Task<Response> Exchange(Request value) { await Raw(JsonSerializer.Serialize(value, PipeProtocol.JsonOptions) + "\n"); return await Read(); }
    internal async Task<Response> Read() => JsonSerializer.Deserialize<Response>(await reader.ReadLineAsync().WaitAsync(TimeSpan.FromSeconds(4)) ?? throw new IOException("Unexpected EOF"), PipeProtocol.JsonOptions)!;
    internal async Task ExpectClosed(int timeout = 1500)
    {
        try
        {
            string? line = await reader.ReadLineAsync().WaitAsync(TimeSpan.FromMilliseconds(timeout));
            if (line != null) throw new InvalidDataException("Unexpected response while expecting EOF");
        }
        catch (IOException) { }
    }
    public ValueTask DisposeAsync() { Pipe.Dispose(); reader.Dispose(); return ValueTask.CompletedTask; }
}
