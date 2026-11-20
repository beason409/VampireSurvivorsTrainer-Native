using System.Collections.Concurrent;
using Trainer.Shared;

namespace BepInEx.Logging
{
    internal sealed class ManualLogSource
    {
        internal ConcurrentQueue<string> Warnings { get; } = new();
        internal void LogWarning(object value) => Warnings.Enqueue(value.ToString() ?? "");
    }
}

namespace Trainer.Plugin
{
    // Only the Unity/runtime endpoint is substituted. Pipe creation, peer checks,
    // handshake parsing, framing, timeouts and watchdog scheduling are production code.
    internal sealed class TrainerRuntime
    {
        internal ConcurrentQueue<Request> Requests { get; } = new();
        internal int Sessions, Ended, Checks, Heartbeats, ActiveSession;
        internal bool Effects, BackupValid;
        internal int FailNextCheck;
        internal Func<Request, Task<Response>>? Respond;
        internal int BeginSession() { Effects = false; BackupValid = false; return ActiveSession = Interlocked.Increment(ref Sessions); }
        internal void Heartbeat(int session) { if (session == ActiveSession) Interlocked.Increment(ref Heartbeats); }
        internal void EndSession(int session)
        {
            if (Interlocked.CompareExchange(ref ActiveSession, 0, session) != session) return;
            Effects = false; BackupValid = false; Interlocked.Increment(ref Ended);
        }
        internal void DisableAll() { Effects = false; BackupValid = false; }
        internal void CheckHeartbeat()
        {
            Interlocked.Increment(ref Checks);
            if (Interlocked.Exchange(ref FailNextCheck, 0) != 0) throw new InvalidOperationException("fixture check failed once");
        }
        internal TrainerState Snapshot() => new() { BackupAuthorized = BackupValid };
        internal Task<Response> Submit(Request request, int session, bool? backupValid, string? backupError)
        {
            Requests.Enqueue(request);
            if (backupValid.HasValue) BackupValid = backupValid.Value;
            if (request.Kind == "setFeature") Effects = true;
            if (Respond != null) return Respond(request);
            return Task.FromResult(new Response { Id = request.Id, Ok = true, State = Snapshot() });
        }
    }
}
