using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using Trainer.Probe;
using Trainer.Shared;

string root = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "../../../../../"));
string probe = Path.Combine(root, "tools", "Trainer.Probe", "bin", "Release", "net10.0", "Trainer.Probe.dll");
string fixture = Path.Combine(root, "tests", "Trainer.Probe.Tests", "artifacts", DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N")[..6]);
Directory.CreateDirectory(fixture);
var tests = new (string Name, Func<Task> Run)[]
{
    ("native nullable wallet capabilities and bound samples survive probe recording", () =>
    {
        const string wire = """
            {"id":"native-fixture","ok":true,"itemCatalogLoading":true,"state":{"walletReady":true,"goldWriteSupported":true,"goldWriteReady":false,
              "stats":[{"id":"rerolls","supported":true,"enabled":true,"value":100,"originalValue":2,"currentValue":null,"boundValue":0}],
              "chestReward":{"upgradeProcessedCount":3,"upgradeGrantedCount":1,"lastUpgradeGrantedItemId":"WHIP","lastUpgradeGrantedName":"Whip"}}}
            """;
        var response = JsonSerializer.Deserialize<Response>(wire, PipeProtocol.JsonOptions)!;
        // Use the same shared model/options and record envelope as Probe.Record.
        string record = JsonSerializer.Serialize(new { utc = DateTime.UtcNow, request = new Request { Kind = "getState" }, response }, PipeProtocol.JsonOptions);
        using var document = JsonDocument.Parse(record);
        Assert(document.RootElement.GetProperty("response").GetProperty("itemCatalogLoading").GetBoolean(), "Probe lost a pending incremental catalog response");
        var state = document.RootElement.GetProperty("response").GetProperty("state");
        Assert(state.GetProperty("walletReady").GetBoolean() && state.GetProperty("goldWriteSupported").GetBoolean()
            && !state.GetProperty("goldWriteReady").GetBoolean(), "Native wallet capability state was lost or false became unknown");
        var stat = state.GetProperty("stats")[0];
        Assert(stat.GetProperty("boundValue").GetDouble() == 0 && stat.GetProperty("currentValue").ValueKind == JsonValueKind.Null,
            "A consumed binding was lost or converted into natural-getter evidence");
        var reward = state.GetProperty("chestReward");
        Assert(reward.GetProperty("upgradeProcessedCount").GetInt32() == 3 && reward.GetProperty("upgradeGrantedCount").GetInt32() == 1
            && reward.GetProperty("lastUpgradeGrantedItemId").GetString() == "WHIP", "Probe confused offered choices with confirmed acquisition");
        var legacy = JsonSerializer.Deserialize<Response>("{\"ok\":true,\"state\":{\"stats\":[{\"id\":\"rerolls\"}]}}", PipeProtocol.JsonOptions)!.State!;
        Assert(legacy.WalletReady is null && legacy.GoldWriteSupported is null && legacy.GoldWriteReady is null
            && legacy.Stats[0].BoundValue is null, "Missing old-backend fields were guessed instead of left unknown");
        return Task.CompletedTask;
    }),
    ("reward plans preserve independent toggles and omitted controls", () =>
    {
        string path=Path.Combine(fixture,"reward-plan.json");
        File.WriteAllText(path,"[{\"atMs\":0,\"kind\":\"setChestReward\",\"itemId\":\"TEST_WAND\",\"upgradeEnabled\":true},{\"atMs\":1,\"kind\":\"setChestReward\",\"enabled\":false}]");
        var plan=StepPlans.Read(path);
        var upgrade=plan[0].ToRequest(); var chestOff=plan[1].ToRequest();
        Assert(upgrade.ItemId=="TEST_WAND" && upgrade.UpgradeEnabled==true && upgrade.Enabled is null,"Upgrade plan lost independent state");
        Assert(chestOff.Enabled==false && chestOff.UpgradeEnabled is null && chestOff.ItemId is null,"Partial-off plan introduced unrelated control");
        Assert(CommandFiles.IsReadOnly("getItemCatalog") && !CommandFiles.IsReadOnly("setChestReward"),"Reward mutation cleanup misclassified");
        return Task.CompletedTask;
    }),
    ("whitespace-only Set-Content inbox remains valid and consumes appends once", () =>
    {
        var inbox = new CommandInbox();
        const string blank = "\r\n \t\r\n";
        Assert(inbox.Read(blank).Count == 0, "Blank file produced commands");
        string text = blank + " {\"kind\":\"getGold\"} \r\n";
        Assert(inbox.Read(text).Single().Kind == "getGold", "Append was not read");
        Assert(inbox.Read(text).Count == 0, "Append replayed twice");
        return Task.CompletedTask;
    }),
    ("unfinished tail waits for newline and survives intermediate snapshots", () =>
    {
        var inbox = new CommandInbox();
        string prefix = "{\"kind\":\"getState\"}\n";
        Assert(inbox.Read(prefix + "{\"kind\":\"get").Single().Kind == "getState", "Complete prefix lost");
        Assert(inbox.Read(prefix + "{\"kind\":\"getGold\"}").Count == 0, "Uncommitted tail executed");
        Assert(inbox.Read(prefix + "{\"kind\":\"getGold\"}\n").Single().Kind == "getGold", "Committed tail was lost");
        return Task.CompletedTask;
    }),
    ("bad committed line does not hide following good command", () =>
    {
        var inbox = new CommandInbox(); var warnings = new List<string>();
        var commands = inbox.Read("not-json\n{\"kind\":\"getGold\"}\n", warnings.Add);
        Assert(warnings.Count == 1 && commands.Single().Kind == "getGold", "Bad record stopped inbox");
        Assert(inbox.Read("not-json\n{\"kind\":\"getGold\"}\n", warnings.Add).Count == 0 && warnings.Count == 1, "Bad line was retried forever");
        return Task.CompletedTask;
    }),
    ("clear and replacement restart inbox without stale index", () =>
    {
        var inbox = new CommandInbox();
        inbox.Read("{\"kind\":\"getState\"}\n"); inbox.Read("\r\n");
        Assert(inbox.Read("\r\n{\"kind\":\"getGold\"}\n").Single().Kind == "getGold", "Clear preserved old cursor");
        Assert(inbox.Read("{\"kind\":\"heartbeat\"}\n").Single().Kind == "heartbeat", "Replacement was ignored");
        return Task.CompletedTask;
    }),
    ("replay accepts CRLF, blanks, equals in paths, and final line without newline", () =>
    {
        string path = Path.Combine(fixture, "replay = sample.jsonl");
        File.WriteAllText(path, "\r\n {\"kind\":\"getGold\"} \r\n\t\n{\"kind\":\"getState\"}");
        Assert(CommandFiles.ReadReplay(path).Select(command => command.Kind).SequenceEqual(new[] { "getGold", "getState" }), "Replay order changed");
        var options = ProbeArguments.Parse(["--commandsfile", path, "--pid=123", "--abrupt"]);
        Assert(options["commandsfile"] == path && options["pid"] == "123" && options.ContainsKey("abrupt"), "CLI forms not accepted");
        return Task.CompletedTask;
    }),
    ("finite replay rejects incomplete JSON before use", () =>
    {
        string path = Path.Combine(fixture, "incomplete.jsonl"); File.WriteAllText(path, "\n{\"kind\":");
        try { CommandFiles.ReadReplay(path); throw new Exception("Bad replay accepted"); }
        catch (InvalidDataException error) { Assert(error.Message.Contains("line 2"), "Missing bad-line context"); }
        return Task.CompletedTask;
    }),
    ("gameplay plan accepts existing request fields and sorts stable timestamps", () =>
    {
        string path = Path.Combine(fixture, "gameplay-steps.json");
        File.WriteAllText(path, "[{\"atMs\":800,\"feature\":\"experience\",\"enabled\":true,\"value\":2},{\"atMs\":0,\"kind\":\"disableAll\"},{\"atMs\":800,\"kind\":\"exit\"}]");
        var plan = StepPlans.Read(path);
        Assert(plan.Select(step => step.Kind).SequenceEqual(new[] { "disableAll", "setFeature", "exit" }), "Plan order/default changed");
        var request = plan[1].ToRequest();
        Assert(request.Feature == "experience" && request.Enabled == true && request.Value == 2, "Request payload changed");
        var options = ProbeArguments.Parse(["--gameplay-steps", path, "--duration=300"]);
        Assert(options["gameplay-steps"] == path, "New option not accepted");
        return Task.CompletedTask;
    }),
    ("invalid plans and unsafe mixed timing modes fail before a connection", () =>
    {
        foreach (string contents in new[] { "null", "[null]", "[{\"atMs\":-1}]", "[{\"kind\":\" \"}]" })
        {
            string path = Path.Combine(fixture, "invalid-plan.json"); File.WriteAllText(path, contents);
            try { StepPlans.Read(path); throw new Exception("Invalid plan accepted"); }
            catch (InvalidDataException) { }
        }
        foreach (string[] arguments in new[] { new[] { "--steps=x", "--gameplay-steps=y" }, new[] { "--gameplay-steps=x", "--abrupt" } })
        {
            try { ProbeArguments.Parse(arguments); throw new Exception("Incompatible timing/cleanup accepted"); }
            catch (ArgumentException) { }
        }
        return Task.CompletedTask;
    }),
    ("gameplay time waits for a player and an explicit unpaused diagnostic", () =>
    {
        var clock = new GameplayClock();
        clock.Observe(TimeSpan.Zero, null);
        clock.Observe(TimeSpan.FromSeconds(1), new TrainerState());
        Assert(clock.Status == "waiting-player" && !clock.IsRunning, "No-player state counted");
        clock.Observe(TimeSpan.FromSeconds(2), new TrainerState { PlayerReady = true });
        Assert(clock.Status == "waiting-pause-diagnostic" && !clock.IsRunning, "Missing pause state guessed running");
        clock.Observe(TimeSpan.FromSeconds(2), new TrainerState { PlayerReady = true, Diagnostics = null! });
        Assert(clock.Status == "waiting-pause-diagnostic", "Null diagnostics did not wait");
        foreach (string unknown in new[] { "", "0", "unknown" })
        {
            clock.Observe(TimeSpan.FromSeconds(3), State(unknown));
            Assert(!clock.IsRunning && clock.Elapsed == TimeSpan.Zero, "Unparseable pause state counted");
        }
        clock.Observe(TimeSpan.FromSeconds(4), State("False"));
        Assert(clock.IsRunning && clock.Elapsed == TimeSpan.Zero, "Entry boundary was counted");
        clock.Observe(TimeSpan.FromMilliseconds(4400), State("false"));
        Assert(clock.Elapsed == TimeSpan.FromMilliseconds(400), "Eligible interval lost");
        return Task.CompletedTask;
    }),
    ("pause and upgrade waits exclude crossing intervals and resume without catch-up", () =>
    {
        var clock = new GameplayClock();
        clock.Observe(TimeSpan.Zero, State("False"));
        clock.Observe(TimeSpan.FromSeconds(1), State("False"));
        clock.Observe(TimeSpan.FromSeconds(2), State("True"));
        Assert(clock.Status == "paused" && !clock.IsRunning, "Pause did not gate steps");
        clock.Observe(TimeSpan.FromSeconds(90), State("True"));
        clock.Observe(TimeSpan.FromSeconds(91), State("False"));
        Assert(clock.Elapsed == TimeSpan.FromSeconds(1), "Pause/resume caught up wall time");
        clock.Observe(TimeSpan.FromSeconds(92), State("False"));
        clock.Observe(TimeSpan.FromSeconds(93), new TrainerState { PlayerReady = true });
        clock.Observe(TimeSpan.FromSeconds(100), State("False"));
        clock.Observe(TimeSpan.FromSeconds(101), State("False"));
        Assert(clock.Elapsed == TimeSpan.FromSeconds(3), "Unknown gap leaked into gameplay time");
        try { clock.Observe(TimeSpan.Zero, State("False")); throw new Exception("Clock accepted a backwards observation"); }
        catch (ArgumentOutOfRangeException) { }
        return Task.CompletedTask;
    }),
    ("actual default CLI only sends observation requests", async () =>
    {
        var result = await RunProbe(["--duration", "1"]);
        Assert(result.ExitCode == 0 && result.Requests.Count >= 3, result.Error);
        Assert(result.Requests.All(request => CommandFiles.IsReadOnly(request.Kind)), "Default CLI sent a mutation");
    }),
    ("actual CLI replays explicit commands in order and cleans up mutation", async () =>
    {
        string path = Path.Combine(fixture, "commands.jsonl");
        File.WriteAllText(path, "\r\n{\"kind\":\"getGold\"}\n{\"kind\":\"setFeature\",\"feature\":\"health\",\"enabled\":true}\n{\"kind\":\"getState\"}\n{\"kind\":\"exit\"}");
        var result = await RunProbe(["--commandsfile", path, "--duration=0"]);
        Assert(result.ExitCode == 0, result.Error);
        Assert(result.Requests.Select(request => request.Kind).SequenceEqual(new[] { "hello", "getGold", "setFeature", "getState", "disableAll" }), "Replay/cleanup order differs");
    }),
    ("actual CLI abrupt mutation skips cleanup", async () =>
    {
        string path = Path.Combine(fixture, "abrupt.jsonl");
        File.WriteAllText(path, "{\"kind\":\"setFeature\",\"feature\":\"health\",\"enabled\":true}\n");
        var result = await RunProbe(["--commandsfile=" + path, "--duration=0", "--abrupt"]);
        Assert(result.ExitCode == 0 && result.Requests.Select(request => request.Kind).SequenceEqual(new[] { "hello", "setFeature" }), result.Error);
    }),
    ("actual monitored inbox handles blank file and two-part append", async () =>
    {
        string path = Path.Combine(fixture, "inbox.jsonl"); File.WriteAllText(path, "\r\n \t\r\n");
        var result = await RunProbe(["--inbox", path, "--duration=4"], during: async process =>
        {
            await File.AppendAllTextAsync(path, "{\"kind\":\"getGo");
            await Task.Delay(650);
            Assert(!process.HasExited, "Probe crashed on unfinished JSON tail");
            await File.AppendAllTextAsync(path, "ld\"}\n");
            await Task.Delay(650);
            await File.AppendAllTextAsync(path, "{\"kind\":\"exit\"}\n");
        });
        Assert(result.ExitCode == 0 && result.Requests.Count(request => request.Kind == "getGold") == 1, result.Error);
        Assert(result.Requests.All(request => CommandFiles.IsReadOnly(request.Kind)), "Observation inbox caused cleanup mutation");
    }),
    ("actual CLI stops on rejected state even when error includes a snapshot", async () =>
    {
        var result = await RunProbe(["--duration=1"], respond: request => new Response { Id = request.Id, Ok = request.Kind == "hello", Error = "expired fixture", State = new TrainerState() });
        Assert(result.ExitCode == 1 && result.Error.Contains("expired fixture"), "Rejected state looked successful");
    }),
    ("actual CLI rejects mismatched response IDs", async () =>
    {
        var result = await RunProbe(["--duration=0"], respond: request => new Response { Id = "wrong", Ok = true, State = new TrainerState() });
        Assert(result.ExitCode == 1 && result.Error.Contains("ID"), "Wrong response accepted");
    })
};
int failed = 0;
var selectedTests = args.Contains("--offline-only") ? tests.Where(test => !test.Name.StartsWith("actual ", StringComparison.Ordinal)).ToArray() : tests;
foreach (var test in selectedTests)
{
    try { await test.Run(); Console.WriteLine("PASS " + test.Name); }
    catch (Exception error) { failed++; Console.Error.WriteLine("FAIL " + test.Name + ": " + error.Message); }
}
Console.WriteLine($"{selectedTests.Length - failed}/{selectedTests.Length} actual-source probe fixtures passed. No game connection was used.");
return failed == 0 ? 0 : 1;

async Task<ProbeResult> RunProbe(string[] arguments, Func<Request, Response>? respond = null, Func<Process, Task>? during = null)
{
    Assert(File.Exists(probe), "Build Trainer.Probe in Release first");
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
    var requests = new List<Request>();
    var helloReceived = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
    using var server = new NamedPipeServerStream(PipeProtocol.PipeName(Environment.ProcessId), PipeDirection.InOut, 1,
        PipeTransmissionMode.Byte, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
    var serve = Task.Run(async () =>
    {
        await server.WaitForConnectionAsync(timeout.Token);
        using var reader = new StreamReader(server, new UTF8Encoding(false), false, 4096, true);
        using var writer = new StreamWriter(server, new UTF8Encoding(false), 4096, true) { AutoFlush = true, NewLine = "\n" };
        while (await reader.ReadLineAsync(timeout.Token) is string line)
        {
            var request = JsonSerializer.Deserialize<Request>(line, PipeProtocol.JsonOptions)!;
            requests.Add(request);
            var response = respond?.Invoke(request) ?? new Response { Id = request.Id, Ok = true, State = new TrainerState { GameBuild = "offline-fixture" } };
            await writer.WriteLineAsync(JsonSerializer.Serialize(response, PipeProtocol.JsonOptions).AsMemory(), timeout.Token);
            if (request.Kind == "hello") helloReceived.TrySetResult();
        }
    }, timeout.Token);
    var start = new ProcessStartInfo("dotnet") { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
    start.ArgumentList.Add(probe); start.ArgumentList.Add("--pid=" + Environment.ProcessId);
    foreach (string argument in arguments) start.ArgumentList.Add(argument);
    using var process = Process.Start(start) ?? throw new Exception("Could not launch fixture probe");
    var output = process.StandardOutput.ReadToEndAsync(); var error = process.StandardError.ReadToEndAsync();
    try
    {
        if (during is not null) { await helloReceived.Task.WaitAsync(timeout.Token); await during(process); }
        await process.WaitForExitAsync(timeout.Token);
        await serve;
        return new ProbeResult(process.ExitCode, requests, await output, await error);
    }
    finally { if (!process.HasExited) process.Kill(); timeout.Cancel(); }
}

static void Assert(bool condition, string message) { if (!condition) throw new Exception(message); }
static TrainerState State(string paused) => new() { PlayerReady = true, Diagnostics = new() { ["game.paused"] = paused } };
sealed record ProbeResult(int ExitCode, List<Request> Requests, string Output, string Error);
