using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using BepInEx.Logging;
using Trainer.Shared;

namespace Trainer.Plugin;

internal sealed class PipeServer : IDisposable
{
    private readonly TrainerRuntime _runtime;
    private readonly ManualLogSource _log;
    private readonly CancellationTokenSource _stop = new();
    private static readonly UTF8Encoding WireEncoding = new(false, true);
    private int _started;
    internal PipeServer(TrainerRuntime runtime, ManualLogSource log) { _runtime = runtime; _log = log; }
    internal void Start()
    {
        if (Interlocked.Exchange(ref _started, 1) != 0) return;
        var stop = _stop.Token;
        _ = Task.Run(() => Listen(stop));
        _ = Task.Run(async () =>
        {
            using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(250));
            try
            {
                while (await timer.WaitForNextTickAsync(stop))
                {
                    try { _runtime.CheckHeartbeat(); }
                    catch (Exception e) { _log.LogWarning("Trainer heartbeat check failed: " + e.Message); }
                }
            }
            catch (OperationCanceledException) when (stop.IsCancellationRequested) { }
        });
    }

    private async Task Listen(CancellationToken stop)
    {
        while (!stop.IsCancellationRequested)
        {
            int session = 0;
            try
            {
                using var pipe = PipePeerGuard.CreateServer(PipeProtocol.PipeName(Environment.ProcessId));
                await pipe.WaitForConnectionAsync(stop);
                using var peer = PipePeerGuard.Verify(pipe);
                using var reader = new StreamReader(pipe, WireEncoding, false, 4096, leaveOpen: true);
                bool handshaken = false;
                while (pipe.IsConnected && !stop.IsCancellationRequested)
                {
                    var line = await ReadBoundedLine(reader, handshaken ? TimeSpan.FromSeconds(5) : TimeSpan.FromSeconds(2), stop);
                    if (line == null) break;
                    peer.EnsureAlive();
                    Response response;
                    Request? request = null;
                    bool closeAfterResponse = false;
                    try
                    {
                        request = JsonSerializer.Deserialize<Request>(line, PipeProtocol.JsonOptions)
                            ?? throw new InvalidDataException("空请求。");
                        if (string.IsNullOrWhiteSpace(request.Id) || request.Id.Length > 128)
                            throw new InvalidDataException("请求 ID 无效。");
                        if ((!handshaken && request.Kind != "hello") || (handshaken && request.Kind == "hello"))
                            throw new InvalidDataException("请先完成通信握手，重复握手需要重新连接。");
                        if (!IsAllowedCommand(request.Kind)) throw new InvalidDataException("未知指令。");
                        bool? backupValid = null;
                        string? backupError = null;
                        if (request.Kind == "hello")
                        {
                            var result = BackupVerifier.Verify(request.BackupToken);
                            backupValid = result.Valid;
                            backupError = result.Error;
                        }
                        // Local backup verification can outlast a heartbeat interval.
                        // No runtime session or enabled effects exist until it finishes.
                        stop.ThrowIfCancellationRequested();
                        peer.EnsureAlive();
                        if (session == 0) session = _runtime.BeginSession();
                        _runtime.Heartbeat(session);
                        response = await _runtime.Submit(request, session, backupValid, backupError)
                            .WaitAsync(TimeSpan.FromSeconds(3), stop);
                        if (!handshaken)
                        {
                            handshaken = response.Ok;
                            closeAfterResponse = !handshaken;
                        }
                    }
                    catch (Exception e) when (e is not OperationCanceledException)
                    {
                        if (session != 0) _runtime.EndSession(session);
                        closeAfterResponse = true;
                        response = new Response { Id = request?.Id ?? "", Ok = false, Error = e.Message, State = _runtime.Snapshot() };
                    }
                    await WriteResponse(pipe, response, stop);
                    if (closeAfterResponse) break;
                }
            }
            catch (OperationCanceledException) when (stop.IsCancellationRequested) { break; }
            catch (Exception e) { _log.LogWarning("Trainer pipe disconnected: " + e.Message); }
            finally { if (session != 0) _runtime.EndSession(session); }
            try { if (!stop.IsCancellationRequested) await Task.Delay(200, stop); }
            catch (OperationCanceledException) when (stop.IsCancellationRequested) { break; }
        }
    }

    private static bool IsAllowedCommand(string? kind) => kind is "hello" or "heartbeat" or "getState" or "getGold"
        or "getItemCatalog" or "getItemIcon" or "disableAll" or "disableStats"
        or "setFeature" or "setStat" or "setChestReward" or "setGold";

    private static async Task WriteResponse(NamedPipeServerStream pipe, Response response, CancellationToken stop)
    {
        string json = JsonSerializer.Serialize(response, PipeProtocol.JsonOptions);
        if (json.Length > PipeProtocol.MaxMessageCharacters) throw new InvalidDataException("响应过长。");
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(stop);
        deadline.CancelAfter(TimeSpan.FromSeconds(2));
        try { await pipe.WriteAsync(WireEncoding.GetBytes(json + "\n"), deadline.Token); }
        catch (OperationCanceledException) when (!stop.IsCancellationRequested)
        { throw new TimeoutException("通信响应写入超时。"); }
    }

    private static async Task<string?> ReadBoundedLine(StreamReader reader, TimeSpan timeout, CancellationToken stop)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(stop);
        deadline.CancelAfter(timeout);
        var result = new StringBuilder();
        var character = new char[1];
        try
        {
            while (true)
            {
                deadline.Token.ThrowIfCancellationRequested();
                int read = await reader.ReadAsync(character.AsMemory(0, 1), deadline.Token);
                if (read == 0)
                {
                    if (result.Length != 0) throw new InvalidDataException("请求未完整结束。");
                    return null;
                }
                if (character[0] == '\n') return result.ToString().TrimEnd('\r');
                if (result.Length >= PipeProtocol.MaxMessageCharacters) throw new InvalidDataException("请求过长。");
                result.Append(character[0]);
            }
        }
        catch (OperationCanceledException) when (!stop.IsCancellationRequested)
        { throw new TimeoutException("通信请求读取超时。"); }
    }

    public void Dispose() { _runtime.DisableAll(); _stop.Cancel(); }
}
