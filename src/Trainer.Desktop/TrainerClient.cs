using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;
using Trainer.Shared;

namespace Trainer.Desktop;

internal sealed class TrainerClient : IAsyncDisposable
{
    private readonly SemaphoreSlim _exchange = new(1, 1);
    private NamedPipeClientStream? _pipe;
    private StreamReader? _reader;
    private BoundedResponseReader? _responses;
    private StreamWriter? _writer;
    public int? ProcessId { get; private set; }
    public bool IsConnected => _pipe?.IsConnected == true;

    public async Task<Response> ConnectAsync(int processId, string? backupToken, CancellationToken ct)
    {
        await _exchange.WaitAsync(ct);
        try
        {
            Close();
            _pipe = new NamedPipeClientStream(".", PipeProtocol.PipeName(processId), PipeDirection.InOut, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeout.CancelAfter(1800);
            await _pipe.ConnectAsync(timeout.Token);
            if (!GetNamedPipeServerProcessId(_pipe.SafePipeHandle, out uint serverPid) || serverPid != processId)
                throw new IOException("通信服务与当前游戏进程不匹配。");
            ProcessId = processId;
            _reader = new StreamReader(_pipe, new UTF8Encoding(false, true), false, 4096, true);
            _responses = new BoundedResponseReader(_reader, PipeProtocol.MaxMessageCharacters);
            _writer = new StreamWriter(_pipe, new UTF8Encoding(false), 4096, true) { AutoFlush = true, NewLine = "\n" };
            return await ExchangeAsync(new Request { Kind = "hello", BackupToken = backupToken }, timeout.Token);
        }
        catch { Close(); throw; }
        finally { _exchange.Release(); }
    }

    public async Task<Response> SendAsync(Request request, CancellationToken ct)
    {
        await _exchange.WaitAsync(ct);
        try
        {
            if (!IsConnected) throw new IOException("尚未连接游戏插件。");
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeout.CancelAfter(2000);
            return await ExchangeAsync(request, timeout.Token);
        }
        catch { Close(); throw; }
        finally { _exchange.Release(); }
    }

    private async Task<Response> ExchangeAsync(Request request, CancellationToken ct)
    {
        await _writer!.WriteLineAsync(JsonSerializer.Serialize(request, PipeProtocol.JsonOptions).AsMemory(), ct);
        string? line = await _responses!.ReadLineAsync(ct);
        if (line is null) throw new IOException("游戏插件已断开连接。");
        var response = JsonSerializer.Deserialize<Response>(line, PipeProtocol.JsonOptions) ?? throw new IOException("游戏插件返回了无效数据。");
        if (response.Id != request.Id) throw new IOException("通信响应编号不匹配。");
        return response;
    }

    private void Close()
    {
        ProcessId = null;
        _reader?.Dispose(); _writer?.Dispose(); _pipe?.Dispose();
        _reader = null; _responses = null; _writer = null; _pipe = null;
    }

    public async ValueTask DisposeAsync()
    {
        await _exchange.WaitAsync();
        try { Close(); } finally { _exchange.Release(); }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetNamedPipeServerProcessId(SafePipeHandle pipe, out uint serverProcessId);
}
