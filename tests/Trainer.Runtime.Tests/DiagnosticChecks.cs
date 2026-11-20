using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using Trainer.Plugin;
using Trainer.Shared;

internal static class DiagnosticChecks
{
    internal static readonly (string Name, Action Test)[] All =
    {
        ("deferred diagnostics retain float double fixed-point pointer enum and timestamp formats across cultures", Formats),
        ("diagnostic counters preserve text fallback and unchecked overflow", Counters),
        ("feature samples do not count as actual hits and keep separate latest values", FeatureSamples),
        ("damage observations retain actual loss sums and ignore nonfinite HP", DamageObservations),
        ("stat samples exclude game counts while omni reads count both sources", StatSamples),
        ("diagnostic prefix cleanup is ordinal and respects dotted boundaries", PrefixCleanup),
        ("concurrent diagnostic snapshots own their values while counters update", ConcurrentSnapshots),
        ("typed hook diagnostics match the previous event algorithm field for field", LegacyEquivalence)
    };

    private static readonly DateTime Timestamp = new(2026, 11, 16, 1, 2, 3, DateTimeKind.Utc);

    private static void Formats()
    {
        var prior = CultureInfo.CurrentCulture;
        CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("fr-FR");
        try
        {
            var store = new DiagnosticStore();
            store.SetFloat("float", 0.1f);
            store.SetDouble("double", 0.1d);
            store.SetDouble("fixed", 1234.56789d, "F3");
            store.SetPointer("pointer", new IntPtr(-1));
            store.SetTimestamp("timestamp", Timestamp);
            store.SetEnum("enum", DayOfWeek.Monday);
            store.SetEnum("unknownEnum", (DayOfWeek)12345);
            var snapshot = store.Snapshot();
            Check(snapshot["float"] == "0.1" && snapshot["double"] == "0.1", "Numeric output used a different precision or culture");
            Check(snapshot["fixed"] == "1234.568", "Fixed-point output changed");
            Check(snapshot["pointer"] == "0x" + (-1L).ToString("X", CultureInfo.InvariantCulture), "Pointer output changed");
            Check(snapshot["timestamp"] == Timestamp.ToString("O", CultureInfo.InvariantCulture), "UTC output changed");
            Check(snapshot["enum"] == "Monday" && snapshot["unknownEnum"] == "12345", "Enum output changed");
            foreach (double value in new[] { 0d, -0d, double.NaN, double.PositiveInfinity, double.NegativeInfinity, double.Epsilon })
            {
                store.SetDouble("double", value);
                Check(store["double"] == value.ToString("R", CultureInfo.InvariantCulture), "Double cache retained a previous representation");
            }
            foreach (float value in new[] { 0f, -0f, float.NaN, float.PositiveInfinity, float.Epsilon })
            {
                store.SetFloat("float", value);
                Check(store["float"] == value.ToString("R", CultureInfo.InvariantCulture), "Float cache retained a previous representation");
            }
            store.SetDouble("fixed", 1234.5, "F3");
            Check(store["fixed"] == "1234.500", "Deferred fixed-point cache failed to invalidate");
            Check(snapshot["fixed"] == "1234.568", "An earlier snapshot changed after an update");
        }
        finally { CultureInfo.CurrentCulture = prior; }
    }

    private static void Counters()
    {
        var store = new DiagnosticStore();
        store["count"] = long.MaxValue.ToString(CultureInfo.InvariantCulture);
        store.Increment("count");
        Check(store["count"] == long.MinValue.ToString(CultureInfo.InvariantCulture), "Counter overflow stopped matching unchecked long addition");
        store.Increment("count");
        Check(store["count"] == (long.MinValue + 1).ToString(CultureInfo.InvariantCulture), "Typed overflow counter did not continue");
        store["count"] = "invalid";
        store.Increment("count");
        Check(store["count"] == "1", "Invalid prior text did not reset the count");
        store["total"] = "1.25e2";
        store.AddDouble("total", 0.25);
        store.AddDouble("total", 0.5);
        Check(store["total"] == "125.75", "Typed sum did not preserve the existing invariant text total");
        store["total"] = "invalid";
        store.AddDouble("total", 0.5);
        Check(store["total"] == "0.5", "Invalid prior text did not reset the total");
        store.Remove("count"); store.Increment("count");
        Check(store["count"] == "1", "Removed counters were resurrected from a stale cache");
    }

    private static void FeatureSamples()
    {
        var store = new DiagnosticStore(); var hooks = new HookDiagnostics(store);
        hooks.Record(FeatureIds.Damage, 1, 10, true);
        Check(!store.TryGetValue("damage.hits", out _), "A sample counted as a game hit");
        hooks.Record(FeatureIds.Damage, 2, 20, false);
        hooks.Record(FeatureIds.Damage, 3, 30, false);
        hooks.Record(FeatureIds.Damage, 4, 40, true);
        var values = store.Snapshot();
        Check(values["damage.hits"] == "2" && values["damage.lastInput"] == "3" && values["damage.lastOutput"] == "30", "Latest actual hit or count was lost");
        Check(values["damage.sampleInput"] == "4" && values["damage.sampleOutput"] == "40", "Sample replaced actual hit values");
    }

    private static void DamageObservations()
    {
        var store = new DiagnosticStore(); var hooks = new HookDiagnostics(store);
        hooks.ObserveDamage("combat.enemy", float.NaN, 1);
        hooks.ObserveDamage("combat.enemy", 1, float.PositiveInfinity);
        Check(store.Snapshot().Count == 0, "Invalid HP created an observation");
        hooks.ObserveDamage("combat.enemy", 10, 7.5f);
        hooks.ObserveDamage("combat.enemy", 7.5f, 9);
        hooks.ObserveDamage("combat.enemy", 9, 8.5f);
        hooks.ObserveDamage("combat.player", 100, 99);
        var values = store.Snapshot();
        Check(values["combat.enemy.calls"] == "3" && values["combat.enemy.damagingCalls"] == "2", "Damage call counts changed");
        Check(values["combat.enemy.actualLossTotal"] == "3" && values["combat.enemy.lastActualLoss"] == "0.5", "Actual loss accumulation changed");
        Check(values["combat.enemy.lastHpBefore"] == "9" && values["combat.enemy.lastHpAfter"] == "8.5", "Latest HP observation changed");
        Check(values["combat.player.actualLossTotal"] == "1", "Enemy and player sums were mixed");
    }

    private static void StatSamples()
    {
        var store = new DiagnosticStore(); var hooks = new HookDiagnostics(store);
        hooks.RecordStatRead(StatIds.Might, 1.1, 2, StatIds.Omni, true, Timestamp);
        Check(!store.TryGetValue("stat.might.gameReads", out _) && !store.TryGetValue("stat.omni.gameReads", out _)
            && !store.TryGetValue("stat.might.lastGameReadUtc", out _), "A stat sample counted as a native game read");
        hooks.RecordStatRead(StatIds.Might, 1.2, 2, StatIds.Omni, false, Timestamp);
        hooks.RecordStatRead(StatIds.Might, 1.3, 3, StatIds.Might, false, Timestamp.AddTicks(1));
        hooks.RecordStatRead(StatIds.Might, 1.4, 4, StatIds.Might, true, Timestamp.AddTicks(2));
        var values = store.Snapshot();
        Check(values["stat.might.gameReads"] == "2" && values["stat.omni.gameReads"] == "1", "Stat/Omni game read counts changed");
        Check(values["stat.might.gameInput"] == "1.3" && values["stat.might.gameOutput"] == "3" && values["stat.might.source"] == StatIds.Might, "Latest stat game read changed");
        Check(values["stat.might.sampleInput"] == "1.4" && values["stat.might.sampleOutput"] == "4", "Latest stat sample changed");
        Check(values["stat.might.lastGameReadUtc"] == Timestamp.AddTicks(1).ToString("O", CultureInfo.InvariantCulture), "A sample overwrote the last actual read time");
    }

    private static void PrefixCleanup()
    {
        var store = new DiagnosticStore();
        foreach (string key in new[] { "raw", "raw.hp", "raw.hp.value", "rawX.hp", "Raw.hp", ".hidden" }) store[key] = key;
        store.SetDouble("raw.numeric", 1);
        store.ClearPrefix("raw");
        var first = store.Snapshot();
        Check(first.Count == 3 && first.ContainsKey("rawX.hp") && first.ContainsKey("Raw.hp") && first.ContainsKey(".hidden"), "Prefix cleanup crossed a dotted or ordinal boundary");
        store.SetDouble("raw.numeric", 2);
        Check(store["raw.numeric"] == "2", "Removed numeric entry could not be reused");
        store.ClearPrefix("missing"); store.ClearPrefix("raw"); store.ClearPrefix("");
        Check(store.Snapshot().Count == 2, "Empty prefix cleanup changed its old dotted-prefix behavior");
    }

    private static void ConcurrentSnapshots()
    {
        var store = new DiagnosticStore(); store.SetInteger("count", 0);
        using var start = new ManualResetEventSlim();
        using var finished = new CountdownEvent(4);
        var writers = Enumerable.Range(0, 4).Select(worker => Task.Run(() =>
        {
            start.Wait();
            try { for (int index = 0; index < 5000; index++) store.Increment("count"); }
            finally { finished.Signal(); }
        })).ToArray();
        var reader = Task.Run(() =>
        {
            start.Wait(); long previous = 0;
            do
            {
                var snapshot = store.Snapshot();
                long current = long.Parse(snapshot["count"], CultureInfo.InvariantCulture);
                Check(current >= previous, "A concurrent snapshot observed an older counter");
                previous = current;
                snapshot["count"] = "external mutation";
            } while (!finished.IsSet);
        });
        start.Set();
        Check(Task.WaitAll(writers.Append(reader).ToArray(), TimeSpan.FromSeconds(10)), "Concurrent diagnostic operations did not complete");
        Check(store["count"] == "20000", "Concurrent increments were lost or a snapshot leaked back into the store");
    }

    private static void LegacyEquivalence()
    {
        var store = new DiagnosticStore(); var hooks = new HookDiagnostics(store); var legacy = new LegacyDiagnostics();
        for (int index = 0; index < 257; index++)
        {
            bool sampling = index % 7 == 0;
            var time = Timestamp.AddTicks(index);
            float before = index + 0.125f, after = index % 5 == 0 ? before + 1 : before - 0.5f;
            string source = index % 2 == 0 ? StatIds.Omni : StatIds.Might;
            hooks.Record(FeatureIds.Damage, before, after, sampling); legacy.Record(FeatureIds.Damage, before, after, sampling);
            hooks.ObserveDamage("combat.enemy", before, after); legacy.ObserveDamage("combat.enemy", before, after);
            hooks.RecordStatRead(StatIds.Might, before, after, source, sampling, time); legacy.RecordStatRead(StatIds.Might, before, after, source, sampling, time);
            if (index % 11 == 0) Equal(legacy.Values, store.Snapshot());
        }
        Equal(legacy.Values, store.Snapshot());
    }

    internal static void Measure(string output)
    {
        const int events = 8192, snapshotEvery = 256, samples = 7;
        for (int index = 0; index < 2; index++) { RunLegacy(events, snapshotEvery); RunTyped(events, snapshotEvery); }
        var before = new List<object>(); var after = new List<object>();
        for (int sample = 0; sample < samples; sample++)
        {
            before.Add(Measure(() => RunLegacy(events, snapshotEvery)));
            after.Add(Measure(() => RunTyped(events, snapshotEvery)));
        }
        File.WriteAllText(output, JsonSerializer.Serialize(new { scope = "Offline managed hook diagnostics only; excludes native hooks, game FPS and UI", eventsPerSample = events,
            snapshotEvery, samples, before, after }, new JsonSerializerOptions { WriteIndented = true }));
    }

    private static object Measure(Action action)
    {
        long allocated = GC.GetAllocatedBytesForCurrentThread(); long start = Stopwatch.GetTimestamp();
        action();
        return new { allocatedBytes = GC.GetAllocatedBytesForCurrentThread() - allocated,
            elapsedMilliseconds = (Stopwatch.GetTimestamp() - start) * 1000d / Stopwatch.Frequency };
    }

    private static void RunLegacy(int events, int snapshotEvery)
    {
        var legacy = new LegacyDiagnostics();
        for (int index = 0; index < events; index++)
        {
            legacy.Record(FeatureIds.Damage, index, index * 10, false);
            legacy.ObserveDamage("combat.enemy", index + 0.5f, index);
            legacy.RecordStatRead(StatIds.Might, index, index + 1, StatIds.Omni, false, Timestamp.AddTicks(index));
            if (index % snapshotEvery == 0) GC.KeepAlive(new Dictionary<string, string>(legacy.Values));
        }
        GC.KeepAlive(legacy.Values);
    }

    private static void RunTyped(int events, int snapshotEvery)
    {
        var store = new DiagnosticStore(); var hooks = new HookDiagnostics(store);
        for (int index = 0; index < events; index++)
        {
            hooks.Record(FeatureIds.Damage, index, index * 10, false);
            hooks.ObserveDamage("combat.enemy", index + 0.5f, index);
            hooks.RecordStatRead(StatIds.Might, index, index + 1, StatIds.Omni, false, Timestamp.AddTicks(index));
            if (index % snapshotEvery == 0) GC.KeepAlive(store.Snapshot());
        }
        GC.KeepAlive(store);
    }

    private static void Equal(Dictionary<string, string> expected, Dictionary<string, string> actual)
    {
        Check(expected.Count == actual.Count, "Diagnostic field count changed");
        foreach (var pair in expected) Check(actual.TryGetValue(pair.Key, out string? value) && value == pair.Value, "Diagnostic mismatch: " + pair.Key);
    }
    private static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }

    // The previous hook arithmetic and formatting, retained only as an offline
    // behavior oracle and allocation baseline. It never references native code.
    private sealed class LegacyDiagnostics
    {
        internal Dictionary<string, string> Values { get; } = new();
        private static string F(float value) => value.ToString("R", CultureInfo.InvariantCulture);
        private void Increment(string key)
        {
            long count = Values.TryGetValue(key, out var text) && long.TryParse(text, out var old) ? old : 0;
            Values[key] = unchecked(count + 1).ToString(CultureInfo.InvariantCulture);
        }
        internal void Record(string feature, float before, float after, bool sampling)
        {
            string prefix = feature + (sampling ? ".sample" : ".last");
            Values[prefix + "Input"] = F(before); Values[prefix + "Output"] = F(after);
            if (!sampling) Increment(feature + ".hits");
        }
        internal void ObserveDamage(string prefix, float before, float after)
        {
            if (!float.IsFinite(before) || !float.IsFinite(after)) return;
            Increment(prefix + ".calls"); Values[prefix + ".lastHpBefore"] = F(before); Values[prefix + ".lastHpAfter"] = F(after);
            float loss = MathF.Max(0, before - after); Values[prefix + ".lastActualLoss"] = F(loss);
            if (loss > 0) Increment(prefix + ".damagingCalls");
            double total = Values.TryGetValue(prefix + ".actualLossTotal", out var text)
                && double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var old) ? old : 0;
            Values[prefix + ".actualLossTotal"] = (total + loss).ToString("R", CultureInfo.InvariantCulture);
        }
        internal void RecordStatRead(string id, double original, double current, string source, bool sampling, DateTime timestamp)
        {
            string suffix = sampling ? "sample" : "game";
            Values["stat." + id + "." + suffix + "Input"] = original.ToString("R", CultureInfo.InvariantCulture);
            Values["stat." + id + "." + suffix + "Output"] = current.ToString("R", CultureInfo.InvariantCulture);
            Values["stat." + id + ".source"] = source;
            if (sampling) return;
            Increment("stat." + id + ".gameReads"); Values["stat." + id + ".lastGameReadUtc"] = timestamp.ToString("O", CultureInfo.InvariantCulture);
            if (source == StatIds.Omni) Increment("stat." + StatIds.Omni + ".gameReads");
        }
    }
}
