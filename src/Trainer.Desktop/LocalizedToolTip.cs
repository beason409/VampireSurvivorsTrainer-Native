namespace Trainer.Desktop;

internal sealed class LocalizedToolTip : ToolTip
{
    private readonly Dictionary<Control, string?> _sources = new();
    private readonly Dictionary<Control, string> _rendered = new();
    internal LocalizedToolTip() => UiLocalization.LanguageChanged += ChangeLanguage;
    public new void SetToolTip(Control control, string? caption)
    {
        _sources[control] = caption;
        Apply(control, caption);
    }
    private void ChangeLanguage(object? sender, EventArgs e)
    {
        foreach (var pair in _sources)
            if (!pair.Key.IsDisposed) Apply(pair.Key, pair.Value);
    }
    private void Apply(Control control, string? source)
    {
        string translated = UiLocalization.Translate(source);
        if (_rendered.TryGetValue(control, out string? previous) && previous == translated) return;
        _rendered[control] = translated;
        base.SetToolTip(control, translated);
    }
    protected override void Dispose(bool disposing)
    {
        if (disposing) { UiLocalization.LanguageChanged -= ChangeLanguage; _sources.Clear(); _rendered.Clear(); }
        base.Dispose(disposing);
    }
}
