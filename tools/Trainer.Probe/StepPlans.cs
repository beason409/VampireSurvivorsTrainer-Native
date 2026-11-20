using System.Text.Json;
using Trainer.Shared;

namespace Trainer.Probe;

internal sealed class ProbeStep
{
    public int AtMs { get; set; }
    public string Kind { get; set; } = "setFeature";
    public string? Feature { get; set; }
    public string? Stat { get; set; }
    public string? ItemId { get; set; }
    public bool? Enabled { get; set; }
    public bool? UpgradeEnabled { get; set; }
    public double? Value { get; set; }

    public Request ToRequest() => new() { Kind = Kind, Feature = Feature, Stat=Stat, ItemId=ItemId, Enabled = Enabled, UpgradeEnabled=UpgradeEnabled, Value = Value };
}

internal static class StepPlans
{
    public static List<ProbeStep> Read(string path)
    {
        var steps = JsonSerializer.Deserialize<List<ProbeStep>>(File.ReadAllText(path), PipeProtocol.JsonOptions)
            ?? throw new InvalidDataException("The steps file must contain a JSON array.");
        if (steps.Any(step => step is null || step.AtMs < 0 || string.IsNullOrWhiteSpace(step.Kind)))
            throw new InvalidDataException("Step timestamps must be non-negative and command kinds must be non-empty.");
        return steps.OrderBy(step => step.AtMs).ToList();
    }
}

// Use monotonic wall time between consecutive eligible snapshots. A gap that starts or ends
// paused, unknown, or without a player is excluded rather than assigned to gameplay.
internal sealed class GameplayClock
{
    private TimeSpan? _previousObservation;
    private bool _previousEligible;
    public TimeSpan Elapsed { get; private set; }
    public bool IsRunning { get; private set; }
    public string Status { get; private set; } = "waiting-state";

    public void Observe(TimeSpan wallTime, TrainerState? state)
    {
        if (wallTime < TimeSpan.Zero || (_previousObservation is TimeSpan previous && wallTime < previous))
            throw new ArgumentOutOfRangeException(nameof(wallTime), "Observation time must be monotonic.");
        Status = state is null ? "waiting-state"
            : !state.PlayerReady ? "waiting-player"
            : state.Diagnostics is null || !state.Diagnostics.TryGetValue("game.paused", out string? value) || !bool.TryParse(value, out bool paused)
                ? "waiting-pause-diagnostic"
                : paused ? "paused" : "running";
        IsRunning = Status == "running";
        if (IsRunning && _previousEligible && _previousObservation is TimeSpan last)
            Elapsed += wallTime - last;
        _previousObservation = wallTime;
        _previousEligible = IsRunning;
    }
}
