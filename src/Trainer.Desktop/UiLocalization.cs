using System.Collections.Concurrent;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Trainer.Shared;

namespace Trainer.Desktop;

internal static class UiLocalization
{
    private static readonly Dictionary<string, string> English = LoadEnglish();
    private static readonly Template[] Templates = English.Where(pair => Regex.IsMatch(pair.Key, @"\{\d+\}"))
        .Select(pair => new Template(pair.Key, pair.Value)).OrderByDescending(item => item.LiteralLength).ToArray();
    private static readonly KeyValuePair<string, string>[] Fragments = English.Where(pair => !pair.Key.Contains('{') && pair.Key.Length >= 2)
        .OrderByDescending(pair => pair.Key.Length).ToArray();
    private static readonly ConcurrentDictionary<string, string> Cache = new(StringComparer.Ordinal);
    private static readonly ConditionalWeakTable<Control, ControlTree> Trees = new();
    private static readonly ConditionalWeakTable<Control, object> OwnedControls = new();
    private static readonly ConditionalWeakTable<Control, TextBinding> TextBindings = new();
    private static KeyValuePair<string, string>[] _itemNames = Array.Empty<KeyValuePair<string, string>>();
    internal static string Language { get; private set; } = "zh-CN";
    internal static bool IsEnglish => Language == "en";
    internal static event EventHandler? LanguageChanged;

    internal static string NormalizeLanguage(string? language) => language is not null &&
        (language.Equals("en", StringComparison.OrdinalIgnoreCase) || language.StartsWith("en-", StringComparison.OrdinalIgnoreCase)) ? "en" : "zh-CN";

    internal static void SetLanguage(string? language)
    {
        string normalized = NormalizeLanguage(language);
        if (Language == normalized) return;
        Language = normalized; Cache.Clear();
        LanguageChanged?.Invoke(null, EventArgs.Empty);
    }

    internal static string Translate(string? source)
    {
        if (string.IsNullOrEmpty(source) || !IsEnglish) return source ?? "";
        if (Cache.TryGetValue(source, out var cached)) return cached;
        string translated = TranslateCore(source, 0);
        if (Cache.Count > 4096) Cache.Clear();
        Cache.TryAdd(source, translated);
        return translated;
    }

    internal static void SetText(Control control, string? source)
    {
        // Keep the source separate from the displayed text. Status polling must
        // never put Chinese onto an English control, even for one message turn.
        TextBindings.GetValue(control, item => new TextBinding(item)).SetSource(source ?? "");
    }

    internal static string GetSource(Control control) => TextBindings.TryGetValue(control, out var binding) ? binding.Source : control.Text;

    internal static void RegisterItemNames(IEnumerable<RewardItemDefinition> items)
    {
        _itemNames = items.Where(item => !string.IsNullOrWhiteSpace(item.Name) && !string.IsNullOrWhiteSpace(item.EnglishName)
                && item.Name != item.EnglishName && HasChinese(item.Name))
            .GroupBy(item => item.Name, StringComparer.Ordinal).Select(group => new KeyValuePair<string, string>(group.Key, group.First().EnglishName!))
            .OrderByDescending(pair => pair.Key.Length).ToArray();
        Cache.Clear();
    }

    private static string TranslateCore(string source, int depth)
    {
        if (English.TryGetValue(source, out string? exact)) return exact;
        if (depth > 10 || !HasChinese(source)) return source;
        foreach (var pair in _itemNames) if (source == pair.Key) return pair.Value;
        foreach (var template in Templates)
        {
            if (!source.StartsWith(template.Prefix, StringComparison.Ordinal)) continue;
            Match match;
            try { match = template.Pattern.Match(source); }
            catch (RegexMatchTimeoutException) { continue; }
            if (!match.Success) continue;
            var arguments = Enumerable.Range(0, template.ArgumentCount)
                .Select(index => (object)TranslateCore(match.Groups["p" + index].Value, depth + 1)).ToArray();
            return string.Format(CultureInfo.InvariantCulture, template.English, arguments);
        }
        foreach (var pair in Fragments)
        {
            if (source.StartsWith(pair.Key, StringComparison.Ordinal))
                return pair.Value + TranslateCore(source[pair.Key.Length..], depth + 1);
            if (source.EndsWith(pair.Key, StringComparison.Ordinal))
                return TranslateCore(source[..^pair.Key.Length], depth + 1) + pair.Value;
        }
        // Combined labels, tooltips and native exception prefixes use the same
        // source catalogue, while paths, IDs and user input remain unchanged.
        string[] pieces = Regex.Split(source, @"(\r\n|\n|\r| · |：|；|。)");
        if (pieces.Length > 1)
            return string.Concat(pieces.Select(piece => piece switch { "：" => ": ", "；" => "; ", "。" => ".", _ => TranslateCore(piece, depth + 1) }));
        foreach (var pair in _itemNames) source = source.Replace(pair.Key, pair.Value, StringComparison.Ordinal);
        return source;
    }

    private static bool HasChinese(string value) => value.Any(character => character is >= '\u3400' and <= '\u9fff');
    private static Dictionary<string, string> LoadEnglish()
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        var assembly = typeof(UiLocalization).Assembly;
        foreach (string name in assembly.GetManifestResourceNames().Where(name => name.EndsWith(".en.json", StringComparison.Ordinal)))
        {
            using var stream = assembly.GetManifestResourceStream(name)!;
            foreach (var pair in JsonSerializer.Deserialize<Dictionary<string, string>>(stream)!)
                result[pair.Key] = pair.Value;
        }
        return result;
    }

    internal static void Attach(Control root)
    {
        if (!Trees.TryGetValue(root, out _)) Trees.Add(root, new ControlTree(root));
    }

    internal static void Refresh(Control root)
    {
        if (Trees.TryGetValue(root, out var tree)) tree.Apply();
    }

    private sealed class Template
    {
        internal readonly Regex Pattern;
        internal readonly string English, Prefix;
        internal readonly int ArgumentCount, LiteralLength;
        internal Template(string source, string english)
        {
            English = english;
            var placeholders = Regex.Matches(source, @"\{(\d+)\}");
            Prefix = source[..placeholders[0].Index];
            var expression = new StringBuilder("\\A");
            int start = 0;
            foreach (Match match in placeholders)
            {
                expression.Append(Regex.Escape(source[start..match.Index]));
                expression.Append("(?<p").Append(match.Groups[1].Value).Append(">[\\s\\S]*?)");
                ArgumentCount = Math.Max(ArgumentCount, int.Parse(match.Groups[1].Value, CultureInfo.InvariantCulture) + 1);
                LiteralLength += match.Index - start;
                start = match.Index + match.Length;
            }
            LiteralLength += source.Length - start;
            expression.Append(Regex.Escape(source[start..])).Append("\\z");
            Pattern = new Regex(expression.ToString(), RegexOptions.CultureInvariant, TimeSpan.FromMilliseconds(20));
        }
    }

    private sealed class ControlTree
    {
        private readonly Control _root;
        private readonly List<TextBinding> _texts = new();
        private readonly List<(TextBox TextBox, string Source)> _placeholders = new();
        private readonly List<(ColumnHeader Header, string Source, string Rendered)> _columns = new();
        private readonly List<ComboBox> _combos = new();
        private bool _queued, _applying;
        internal ControlTree(Control root)
        {
            _root = root; Bind(root); Apply(true);
            root.HandleCreated += (_, _) => Apply(true);
            LanguageChanged += OnLanguageChanged;
            root.Disposed += (_, _) => LanguageChanged -= OnLanguageChanged;
        }
        private void OnLanguageChanged(object? sender, EventArgs e) => Apply(true);
        private void Bind(Control control)
        {
            if (OwnedControls.TryGetValue(control, out _)) return;
            OwnedControls.Add(control, new object());
            if (control is TextBox input && !string.IsNullOrEmpty(input.PlaceholderText))
                _placeholders.Add((input, input.PlaceholderText));
            bool userText = control is TextBoxBase textBox && (!textBox.ReadOnly || control.Name is "GameDirectory" or "DataDirectory" or "ActivityLog");
            if (!userText && control is not NumericUpDown && control is not ComboBox)
            {
                var binding = TextBindings.GetValue(control, item => new TextBinding(item));
                _texts.Add(binding);
                control.TextChanged += (_, _) => { if (!binding.Updating) Queue(); };
            }
            if (control is ListControl options && options.Name != "LanguageSelector")
            {
                options.FormattingEnabled = true;
                options.Format += (_, e) => e.Value = Translate(e.ListItem?.ToString());
                if (options is ComboBox combo) _combos.Add(combo);
            }
            if (control is ListView list)
                foreach (ColumnHeader column in list.Columns) _columns.Add((column, column.Text, column.Text));
            control.ControlAdded += (_, e) => { if (e.Control is not null) { Bind(e.Control); Queue(); } };
            foreach (Control child in control.Controls) Bind(child);
        }
        private void Queue()
        {
            if (_applying || _queued || !_root.IsHandleCreated || _root.IsDisposed) return;
            _queued = true;
            _root.BeginInvoke(() => { _queued = false; if (!_root.IsDisposed) Apply(); });
        }
        internal void Apply(bool refreshChoices = false)
        {
            if (_applying || _root.IsDisposed) return;
            _applying = true; _root.SuspendLayout();
            try
            {
                foreach (var binding in _texts) binding.Apply();
                foreach (var item in _placeholders)
                {
                    string placeholder = Translate(item.Source);
                    if (item.TextBox.PlaceholderText != placeholder) item.TextBox.PlaceholderText = placeholder;
                }
                for (int index = 0; index < _columns.Count; index++)
                {
                    var (header, source, rendered) = _columns[index];
                    if (header.Text != rendered) source = header.Text;
                    rendered = Translate(source);
                    if (header.Text != rendered) header.Text = rendered;
                    _columns[index] = (header, source, rendered);
                }
                if (refreshChoices) foreach (var combo in _combos) combo.Refresh();
            }
            finally { _root.ResumeLayout(); _applying = false; }
        }
    }

    private sealed class TextBinding(Control control)
    {
        private string _source = control.Text, _rendered = control.Text;
        internal bool Updating { get; private set; }
        internal string Source => _source;
        internal void SetSource(string source)
        {
            _source = source;
            Render();
        }
        internal void Apply()
        {
            if (control.IsDisposed) return;
            if (control.Text != _rendered) _source = control.Text;
            Render();
        }
        private void Render()
        {
            if (control.IsDisposed) return;
            _rendered = Translate(_source);
            if (control.Text == _rendered) return;
            Updating = true;
            try { control.Text = _rendered; }
            finally { Updating = false; }
        }
    }
}
