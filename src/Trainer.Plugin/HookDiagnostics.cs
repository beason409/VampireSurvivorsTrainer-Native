using Trainer.Shared;

namespace Trainer.Plugin;

// Keys are fixed when the adapter starts; game callbacks only update values.
internal sealed class HookDiagnostics
{
    private readonly DiagnosticStore _values;
    private readonly Dictionary<string, FeatureKeys> _features = FeatureIds.All.ToDictionary(id => id, id => new FeatureKeys(id), StringComparer.Ordinal);
    private readonly Dictionary<string, StatKeys> _stats = StatCatalog.All.ToDictionary(stat => stat.Id, stat => new StatKeys(stat.Id), StringComparer.Ordinal);
    private readonly DamageKeys _playerDamage = new("combat.player");
    private readonly DamageKeys _enemyDamage = new("combat.enemy");

    internal HookDiagnostics(DiagnosticStore values) { _values = values; }

    internal void Record(string feature, float before, float after, bool sampling)
    {
        var keys = _features[feature];
        _values.SetFloat(sampling ? keys.SampleInput : keys.Input, before);
        _values.SetFloat(sampling ? keys.SampleOutput : keys.Output, after);
        if (!sampling) _values.Increment(keys.Hits);
    }

    internal void ObserveDamage(string prefix, float before, float after)
    {
        if (!float.IsFinite(before) || !float.IsFinite(after)) return;
        var keys = prefix == "combat.player" ? _playerDamage : _enemyDamage;
        _values.Increment(keys.Calls);
        _values.SetFloat(keys.Before, before);
        _values.SetFloat(keys.After, after);
        float loss = MathF.Max(0, before - after);
        _values.SetFloat(keys.Loss, loss);
        if (loss > 0) _values.Increment(keys.DamagingCalls);
        _values.AddDouble(keys.Total, loss);
    }

    internal void RecordStatRead(string id, double original, double current, string source, bool sampling, DateTime timestamp)
    {
        var keys = _stats[id];
        _values.SetDouble(sampling ? keys.SampleInput : keys.Input, original);
        _values.SetDouble(sampling ? keys.SampleOutput : keys.Output, current);
        _values[keys.Source] = source;
        if (sampling) return;
        _values.Increment(keys.Reads);
        _values.SetTimestamp(keys.Timestamp, timestamp);
        if (source == StatIds.Omni) _values.Increment(_stats[StatIds.Omni].Reads);
    }

    private sealed class FeatureKeys(string feature)
    {
        internal readonly string Input = feature + ".lastInput", Output = feature + ".lastOutput",
            SampleInput = feature + ".sampleInput", SampleOutput = feature + ".sampleOutput", Hits = feature + ".hits";
    }
    private sealed class DamageKeys(string prefix)
    {
        internal readonly string Calls = prefix + ".calls", Before = prefix + ".lastHpBefore", After = prefix + ".lastHpAfter",
            Loss = prefix + ".lastActualLoss", DamagingCalls = prefix + ".damagingCalls", Total = prefix + ".actualLossTotal";
    }
    private sealed class StatKeys(string id)
    {
        internal readonly string Input = "stat." + id + ".gameInput", Output = "stat." + id + ".gameOutput",
            SampleInput = "stat." + id + ".sampleInput", SampleOutput = "stat." + id + ".sampleOutput",
            Source = "stat." + id + ".source", Reads = "stat." + id + ".gameReads", Timestamp = "stat." + id + ".lastGameReadUtc";
    }
}
