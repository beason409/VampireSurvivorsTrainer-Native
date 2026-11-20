using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using Trainer.Probe;
using Trainer.Shared;

try
{
    var options = ProbeArguments.Parse(args);
    if (options.ContainsKey("help"))
    {
        Console.WriteLine("Trainer.Probe --pid <game-pid> [--duration <seconds>] [--token <proof-path>] [--output <jsonl>] [--inbox <append-only-jsonl>] [--commandsfile <jsonl>] [--steps <wall-time-json-array> | --gameplay-steps <unpaused-time-json-array>] [--abrupt]");
        Console.WriteLine("Default: hello + read-only state/heartbeat requests. Inbox records are committed by a newline; incomplete tails wait for the next append.");
        Console.WriteLine("Gameplay plans wait for playerReady=true and diagnostics game.paused=false; missing pause data waits. --duration is the wall-time test limit; final disableAll cleanup follows.");
        return 0;
    }

    // Validate finite inputs before connecting to a running game.
    var commands = options.TryGetValue("commandsfile", out var commandPath) ? CommandFiles.ReadReplay(commandPath) : [];
    bool gameplayMode = options.TryGetValue("gameplay-steps", out var gameplayStepPath);
    var steps = gameplayMode ? StepPlans.Read(gameplayStepPath!)
        : options.TryGetValue("steps", out var stepPath) ? StepPlans.Read(stepPath) : new List<ProbeStep>();
    int duration = options.TryGetValue("duration", out var durationText) && int.TryParse(durationText, out int seconds) && seconds >= 0
        ? seconds : options.ContainsKey("duration") ? throw new ArgumentException("--duration must be a non-negative integer.") : 5;
    int pid;
    if (options.TryGetValue("pid", out var pidText))
        pid = int.TryParse(pidText, out int parsedPid) && parsedPid > 0 ? parsedPid : throw new ArgumentException("--pid must be a positive integer.");
    else
    {
        var processes = Process.GetProcessesByName("VampireSurvivors");
        try { pid = processes.Length == 1 ? processes[0].Id : throw new InvalidOperationException("Use --pid when exactly one VampireSurvivors process cannot be found."); }
        finally { foreach (var process in processes) process.Dispose(); }
    }

    using var pipe = new NamedPipeClientStream(".", PipeProtocol.PipeName(pid), PipeDirection.InOut, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
    await pipe.ConnectAsync(10000);
    using var reader = new StreamReader(pipe, new UTF8Encoding(false), false, 4096, true);
    using var writer = new StreamWriter(pipe, new UTF8Encoding(false), 4096, true) { AutoFlush = true, NewLine = "\n" };
    using var output = options.TryGetValue("output", out var outputPath) ? new StreamWriter(outputPath, append: false, new UTF8Encoding(false)) { AutoFlush = true } : null;
    bool mutationRequested = false;
    Request? pendingResponse = null;
    string? cleanupFailure = null;

    async Task<Response> Send(Request request, CancellationToken cancellation = default, bool drainInterruptedResponse = false)
    {
        if (!CommandFiles.IsReadOnly(request.Kind)) mutationRequested = true;
        Request? interrupted = drainInterruptedResponse ? pendingResponse : null;
        pendingResponse = request;
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        timeout.CancelAfter(TimeSpan.FromSeconds(5));
        long started = Stopwatch.GetTimestamp();
        await writer.WriteLineAsync(JsonSerializer.Serialize(request, PipeProtocol.JsonOptions).AsMemory(), timeout.Token);
        while (true)
        {
            var line = await reader.ReadLineAsync(timeout.Token) ?? throw new IOException("Disconnected from game plugin.");
            if (line.Length > PipeProtocol.MaxMessageCharacters) throw new InvalidDataException("Plugin response exceeds protocol limit.");
            var response = JsonSerializer.Deserialize<Response>(line, PipeProtocol.JsonOptions) ?? throw new InvalidDataException("Plugin returned an empty response.");
            if (response.Id != request.Id)
            {
                // A wall deadline may interrupt an outstanding read. Cleanup is still written
                // immediately, then its response is read after that exact earlier response.
                if (interrupted is not null && response.Id == interrupted.Id)
                { await Record(interrupted, response); interrupted = null; continue; }
                throw new InvalidDataException("Plugin response ID does not match its request.");
            }
            pendingResponse = null;
            await Record(request, response, Stopwatch.GetElapsedTime(started).TotalMilliseconds);
            return response;
        }
    }

    async Task Record(Request request, Response response, double? requestMs = null)
    {
        // IPC round trip plus decoding, not the game's frame or hook CPU time.
        var record = JsonSerializer.Serialize(new { utc = DateTime.UtcNow, requestMs, request, response }, PipeProtocol.JsonOptions);
        Console.WriteLine(record);
        if (output != null) await output.WriteLineAsync(record);
    }

    try
    {
        var hello = await Send(new Request { Kind = "hello", BackupToken = options.GetValueOrDefault("token") });
        if (!hello.Ok) throw new InvalidOperationException(hello.Error ?? "Plugin refused the probe session.");
        bool exitRequested = false;
        foreach (var command in commands)
        {
            if (command.Kind == "exit") { exitRequested = true; break; }
            await Send(command);
        }
        var clock = Stopwatch.StartNew();
        using var gameplayDeadline = new CancellationTokenSource();
        if (gameplayMode) gameplayDeadline.CancelAfter(TimeSpan.FromSeconds(duration));
        var loopCancellation = gameplayMode ? gameplayDeadline.Token : CancellationToken.None;
        var gameplayClock = new GameplayClock();
        string? lastGameplayStatus = null;
        var nextState = TimeSpan.Zero;
        var inbox = new CommandInbox();
        int stepIndex = 0;

        async Task LogPlan(string phase)
        {
            string record = JsonSerializer.Serialize(new { utc = DateTime.UtcNow, probeEvent = "gameplayPlan", phase,
                wallMs = (long)clock.Elapsed.TotalMilliseconds, gameplayMs = (long)gameplayClock.Elapsed.TotalMilliseconds,
                status = gameplayClock.Status, completedSteps = stepIndex, totalSteps = steps.Count,
                nextAtMs = stepIndex < steps.Count ? (int?)steps[stepIndex].AtMs : null }, PipeProtocol.JsonOptions);
            Console.WriteLine(record);
            if (output is not null) await output.WriteLineAsync(record);
        }

        try
        {
            while (!exitRequested && clock.Elapsed < TimeSpan.FromSeconds(duration))
            {
                if (options.TryGetValue("inbox", out var inboxPath) && File.Exists(inboxPath))
                {
                    string snapshot;
                    using (var input = new FileStream(inboxPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                    using (var inputReader = new StreamReader(input))
                        snapshot = await inputReader.ReadToEndAsync(loopCancellation);
                    foreach (var request in inbox.Read(snapshot, warning => Console.Error.WriteLine("Ignored probe inbox line: " + warning)))
                    {
                        if (request.Kind == "exit") { exitRequested = true; break; }
                        await Send(request, loopCancellation);
                    }
                }
                if (exitRequested) break;
                if (gameplayMode)
                {
                    var state = await Send(new Request { Kind = clock.Elapsed >= nextState ? "getState" : "heartbeat" }, loopCancellation);
                    if (!state.Ok) throw new InvalidOperationException(state.Error ?? "Probe session is no longer accepted by plugin.");
                    gameplayClock.Observe(clock.Elapsed, state.State);
                    if (lastGameplayStatus != gameplayClock.Status)
                    { await LogPlan("status"); lastGameplayStatus = gameplayClock.Status; }
                }
                while (stepIndex < steps.Count
                    && (!gameplayMode || gameplayClock.IsRunning)
                    && (gameplayMode ? gameplayClock.Elapsed : clock.Elapsed).TotalMilliseconds >= steps[stepIndex].AtMs)
                {
                    if (gameplayMode) loopCancellation.ThrowIfCancellationRequested();
                    var step = steps[stepIndex];
                    if (step.Kind == "exit") { stepIndex++; exitRequested = true; break; }
                    var response = await Send(step.ToRequest(), loopCancellation);
                    if (gameplayMode && !response.Ok) throw new InvalidOperationException(response.Error ?? "Gameplay step was rejected by plugin.");
                    stepIndex++;
                    if (gameplayMode)
                    {
                        gameplayClock.Observe(clock.Elapsed, response.State);
                        await LogPlan("step");
                        lastGameplayStatus = gameplayClock.Status;
                    }
                }
                if (exitRequested) break;
                if (!gameplayMode)
                {
                    var state = await Send(new Request { Kind = clock.Elapsed >= nextState ? "getState" : "heartbeat" });
                    if (!state.Ok) throw new InvalidOperationException(state.Error ?? "Probe session is no longer accepted by plugin.");
                }
                if (clock.Elapsed >= nextState) nextState = clock.Elapsed.Add(TimeSpan.FromSeconds(1));
                await Task.Delay(400, loopCancellation);
            }
        }
        catch (OperationCanceledException) when (gameplayMode && gameplayDeadline.IsCancellationRequested) { }
        finally { if (gameplayMode) await LogPlan(exitRequested ? "exit" : "end"); }
        if (gameplayMode && !exitRequested && stepIndex < steps.Count)
            throw new TimeoutException($"Gameplay plan incomplete at wall-time limit: completed {stepIndex}/{steps.Count} steps, gameplay {(long)gameplayClock.Elapsed.TotalMilliseconds} ms, status {gameplayClock.Status}.");
    }
    finally
    {
        // Observation-only probes never issue cleanup mutations. Explicit mutation tests clean up
        // their persistent effects unless --abrupt was selected to exercise the plugin watchdog.
        if ((gameplayMode || mutationRequested) && !options.ContainsKey("abrupt") && pipe.IsConnected)
        {
            try
            {
                var cleanup = await Send(new Request { Kind = "disableAll" }, drainInterruptedResponse: true);
                if (!cleanup.Ok) cleanupFailure = "Probe cleanup was rejected: " + cleanup.Error;
            }
            catch (Exception error) { cleanupFailure = "Probe cleanup failed; plugin watchdog must disable effects: " + error.Message; }
            if (cleanupFailure is not null) Console.Error.WriteLine(cleanupFailure);
        }
    }
    if (gameplayMode && cleanupFailure is not null) throw new IOException(cleanupFailure);
    return 0;
}
catch (Exception error)
{
    Console.Error.WriteLine("Probe failed: " + error.Message);
    return 1;
}
