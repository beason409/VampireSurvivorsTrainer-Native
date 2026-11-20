using System.Collections.Concurrent;
using System.Globalization;

namespace Trainer.Plugin;

// Native hooks update numbers in place. Text is materialized only when a
// diagnostic is read, and snapshots never expose the mutable backing store.
internal sealed class DiagnosticStore
{
    private readonly object _gate = new();
    private readonly Dictionary<string, Value> _values = new(StringComparer.Ordinal);
    private readonly List<string> _removedKeys = new();

    internal string this[string key]
    {
        get { lock (_gate) return _values[key].Text(); }
        set { lock (_gate) GetValue(key).SetText(value); }
    }

    internal bool TryGetValue(string key, out string value)
    {
        lock (_gate)
        {
            if (_values.TryGetValue(key, out var entry)) { value = entry.Text(); return true; }
            value = null!;
            return false;
        }
    }

    internal bool Remove(string key) { lock (_gate) return _values.Remove(key); }
    internal void SetInteger(string key, long value) { lock (_gate) GetValue(key).SetInteger(value); }
    internal void SetFloat(string key, float value) { lock (_gate) GetValue(key).SetNumber(value, true, "R"); }
    internal void SetDouble(string key, double value, string format = "R") { lock (_gate) GetValue(key).SetNumber(value, false, format); }
    internal void SetTimestamp(string key, DateTime value) { lock (_gate) GetValue(key).SetTimestamp(value); }
    internal void SetPointer(string key, IntPtr value) { lock (_gate) GetValue(key).SetPointer(value.ToInt64()); }
    internal void SetEnum<T>(string key, T value) where T : struct, Enum => this[key] = EnumNames<T>.Get(value);

    internal void Increment(string key)
    {
        lock (_gate)
        {
            var entry = GetValue(key);
            long current = entry.Kind == ValueKind.Integer ? entry.Integer
                : long.TryParse(entry.Text(), out var parsed) ? parsed : 0;
            entry.SetInteger(unchecked(current + 1));
        }
    }

    internal void AddDouble(string key, double value)
    {
        lock (_gate)
        {
            var entry = GetValue(key);
            double current = entry.Kind == ValueKind.Number ? entry.Number
                : double.TryParse(entry.Text(), NumberStyles.Float, CultureInfo.InvariantCulture, out var parsed) ? parsed : 0;
            entry.SetNumber(current + value, false, "R");
        }
    }

    internal void ClearPrefix(string prefix)
    {
        lock (_gate)
        {
            string childPrefix = prefix + ".";
            foreach (string key in _values.Keys)
                if (key == prefix || key.StartsWith(childPrefix, StringComparison.Ordinal)) _removedKeys.Add(key);
            foreach (string key in _removedKeys) _values.Remove(key);
            _removedKeys.Clear();
        }
    }

    internal Dictionary<string, string> Snapshot()
    {
        lock (_gate)
        {
            var result = new Dictionary<string, string>(_values.Count, StringComparer.Ordinal);
            foreach (var entry in _values) result.Add(entry.Key, entry.Value.Text());
            return result;
        }
    }

    private Value GetValue(string key)
    {
        if (!_values.TryGetValue(key, out var entry)) _values.Add(key, entry = new Value());
        return entry;
    }

    private static class EnumNames<T> where T : struct, Enum
    {
        private static readonly ConcurrentDictionary<T, string> Names = new();
        internal static string Get(T value) => Names.GetOrAdd(value, static item => item.ToString());
    }

    private enum ValueKind { Text, Integer, Number, Timestamp, Pointer }
    private sealed class Value
    {
        internal ValueKind Kind;
        internal long Integer;
        internal double Number;
        private bool _single;
        private string _format = "R";
        private DateTime _timestamp;
        private string? _text = "";

        internal string Text() => _text ??= Kind switch
        {
            ValueKind.Integer => Integer.ToString(CultureInfo.InvariantCulture),
            ValueKind.Number => _single ? ((float)Number).ToString(_format, CultureInfo.InvariantCulture)
                : Number.ToString(_format, CultureInfo.InvariantCulture),
            ValueKind.Timestamp => _timestamp.ToString("O", CultureInfo.InvariantCulture),
            ValueKind.Pointer => "0x" + Integer.ToString("X", CultureInfo.InvariantCulture),
            _ => ""
        };
        internal void SetText(string value) { Kind = ValueKind.Text; _text = value; }
        internal void SetInteger(long value)
        {
            if (Kind == ValueKind.Integer && Integer == value) return;
            Kind = ValueKind.Integer; Integer = value; _text = null;
        }
        internal void SetNumber(double value, bool single, string format)
        {
            if (Kind == ValueKind.Number && BitConverter.DoubleToInt64Bits(Number) == BitConverter.DoubleToInt64Bits(value)
                && _single == single && _format == format) return;
            Kind = ValueKind.Number; Number = value; _single = single; _format = format; _text = null;
        }
        internal void SetTimestamp(DateTime value)
        {
            if (Kind == ValueKind.Timestamp && _timestamp == value && _timestamp.Kind == value.Kind) return;
            Kind = ValueKind.Timestamp; _timestamp = value; _text = null;
        }
        internal void SetPointer(long value)
        {
            if (Kind == ValueKind.Pointer && Integer == value) return;
            Kind = ValueKind.Pointer; Integer = value; _text = null;
        }
    }
}
