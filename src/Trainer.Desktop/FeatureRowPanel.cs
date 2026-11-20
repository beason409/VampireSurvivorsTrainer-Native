using System.Windows.Forms.Layout;

namespace Trainer.Desktop;

// These four columns have a fixed shape. Arrange them directly rather than
// asking the general table solver to measure nested text containers on resize.
internal sealed class FeatureRowPanel : TableLayoutPanel
{
    private bool _arranging;
    private TableLayoutPanel? _rightRail;
    internal FeatureRowPanel() => DoubleBuffered = true;
    public override LayoutEngine LayoutEngine => FeatureColumnsLayout.Instance;
    public override Size GetPreferredSize(Size proposedSize) => new(0, Height);

    internal void AddRightControl(Control child, int column)
    {
        if (_rightRail is null)
        {
            _rightRail = new TableLayoutPanel { ColumnCount = 2, RowCount = 1, Margin = Padding.Empty, Dock = DockStyle.Fill };
            _rightRail.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, ColumnStyles[2].Width));
            _rightRail.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, ColumnStyles[3].Width));
            _rightRail.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            Controls.Add(_rightRail, 2, 0);
            SetColumnSpan(_rightRail, 2);
        }
        _rightRail.Controls.Add(child, column - 2, 0);
    }

    private sealed class FeatureColumnsLayout : LayoutEngine
    {
        internal static readonly FeatureColumnsLayout Instance = new();
        public override bool Layout(object container, LayoutEventArgs layoutEventArgs)
        {
            var panel = (FeatureRowPanel)container;
            if (panel._arranging || panel.ColumnStyles.Count < 4) return false;
            panel._arranging = true;
            try
            {
                Rectangle area = panel.DisplayRectangle;
                Span<int> edges = stackalloc int[5];
                int hotkey = (int)Math.Round(panel.ColumnStyles[0].Width);
                int multiplier = (int)Math.Round(panel.ColumnStyles[2].Width);
                int toggle = (int)Math.Round(panel.ColumnStyles[3].Width);
                edges[0] = area.Left;
                edges[1] = edges[0] + hotkey;
                edges[2] = edges[1] + Math.Max(0, area.Width - hotkey - multiplier - toggle);
                edges[3] = edges[2] + multiplier;
                edges[4] = edges[3] + toggle;
                foreach (Control child in panel.Controls)
                {
                    int column = panel.GetColumn(child);
                    if ((uint)column >= 4) continue;
                    int end = Math.Min(4, column + Math.Max(1, panel.GetColumnSpan(child)));
                    var margin = child.Margin;
                    var bounds = new Rectangle(edges[column] + margin.Left, area.Top + margin.Top,
                        Math.Max(0, edges[end] - edges[column] - margin.Horizontal),
                        Math.Max(0, area.Height - margin.Vertical));
                    if (child.Bounds != bounds) child.Bounds = bounds;
                }
            }
            finally { panel._arranging = false; }
            return false;
        }
    }
}

// One paint surface holds the title and changing status, keeping both lines
// localized without three separate native windows following every size step.
internal sealed class FeatureCaption : Control
{
    private readonly string _title;
    private readonly Font _titleFont;
    private readonly Color _titleColor;
    internal FeatureCaption(string title, Font titleFont, Color titleColor)
    {
        _title = title; _titleFont = titleFont; _titleColor = titleColor;
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
            ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        TabStop = false;
        AccessibleRole = AccessibleRole.StaticText;
        AccessibleName = UiLocalization.Translate(_title);
        UiLocalization.LanguageChanged += OnLanguageChanged;
    }

    private void OnLanguageChanged(object? sender, EventArgs e)
    {
        AccessibleName = UiLocalization.Translate(_title);
        Invalidate();
    }
    protected override void OnTextChanged(EventArgs e) { base.OnTextChanged(e); AccessibleDescription = Text; Invalidate(); }
    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        int firstHeight = (int)Math.Round(Height * .52);
        const TextFormatFlags flags = TextFormatFlags.Left | TextFormatFlags.VerticalCenter |
            TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix;
        string title = UiLocalization.Translate(_title);
        TextRenderer.DrawText(e.Graphics, title, _titleFont, new Rectangle(0, 0, Width, firstHeight), _titleColor, flags);
        TextRenderer.DrawText(e.Graphics, Text, Font, new Rectangle(0, firstHeight, Width, Height - firstHeight), ForeColor, flags);
    }
    protected override void Dispose(bool disposing)
    {
        if (disposing) { UiLocalization.LanguageChanged -= OnLanguageChanged; _titleFont.Dispose(); }
        base.Dispose(disposing);
    }
}
