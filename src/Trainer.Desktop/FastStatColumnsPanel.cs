using System.Windows.Forms.Layout;
using System.ComponentModel;

namespace Trainer.Desktop;

// Fixed stat rows need no content measurement or general-purpose table solver.
// Keep the table's attached row/column metadata for the existing UI builder.
internal sealed class FastStatColumnsPanel : TableLayoutPanel
{
    private bool _arranging;
    private Rectangle? _logicalBounds;
    private FixedRightRail? _rightRail;
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    internal ScrollableControl? Viewport { get; set; }
    internal bool NeedsViewportLayout { get; private set; }
    public override LayoutEngine LayoutEngine => StatColumnsLayout.Instance;

    public override Size GetPreferredSize(Size proposedSize) => new(0, Height);

    internal void AddFixedRightControl(Control control, int column)
    {
        if (column is < 2 or > 6) throw new ArgumentOutOfRangeException(nameof(column));
        if (_rightRail is null)
        {
            _rightRail = new FixedRightRail(this) { BackColor = BackColor, Dock = DockStyle.Fill, Margin = Padding.Empty, TabIndex = 2 };
            Controls.Add(_rightRail, 2, 0);
            SetColumnSpan(_rightRail, 5);
        }
        _rightRail.Add(control, column);
    }

    protected override void OnVisibleChanged(EventArgs e)
    {
        base.OnVisibleChanged(e);
        if (Visible) PerformLayout();
    }

    internal void LayoutIfInViewport()
    {
        if (NeedsViewportLayout && IsInViewport()) PerformLayout();
    }

    internal void SetLogicalBounds(Rectangle bounds)
    {
        _logicalBounds = bounds;
        if (Bounds != bounds) NeedsViewportLayout = true;
    }

    private bool IsInViewport()
    {
        if (!Visible) return false;
        if (Viewport is null) return true;
        int top = _logicalBounds?.Top ?? Top;
        Control? parent = Parent;
        while (parent is not null && parent != Viewport)
        {
            top += parent.Top;
            parent = parent.Parent;
        }
        // Detached rows are still arranged during their initial construction.
        return parent is null || (top < Viewport.ClientSize.Height && top + (_logicalBounds?.Height ?? Height) > 0);
    }

    private sealed class StatColumnsLayout : LayoutEngine
    {
        internal static readonly StatColumnsLayout Instance = new();

        public override bool Layout(object container, LayoutEventArgs layoutEventArgs)
        {
            var panel = (FastStatColumnsPanel)container;
            if (panel._arranging || panel.ColumnStyles.Count < 7) return false;
            if (!panel.IsInViewport()) { panel.NeedsViewportLayout = true; return false; }
            panel._arranging = true;
            try
            {
                Rectangle area = panel.DisplayRectangle;
                Span<int> widths = stackalloc int[7];
                Span<int> edges = stackalloc int[8];
                int fixedWidth = 0;
                for (int column = 0; column < 7; column++)
                {
                    if (column == 1) continue;
                    widths[column] = Math.Max(0, (int)Math.Round(panel.ColumnStyles[column].Width));
                    fixedWidth += widths[column];
                }
                widths[1] = Math.Max(0, area.Width - fixedWidth);
                edges[0] = area.Left;
                for (int column = 0; column < 7; column++) edges[column + 1] = edges[column] + widths[column];
                int firstHeight = panel.RowCount > 1 && panel.RowStyles.Count > 0
                    ? Math.Min(area.Height, Math.Max(0, (int)Math.Round(panel.RowStyles[0].Height))) : area.Height;

                foreach (Control child in panel.Controls)
                {
                    int column = panel.GetColumn(child), row = panel.GetRow(child);
                    if ((uint)column >= 7 || (uint)row >= 2) continue;
                    int end = Math.Min(7, column + Math.Max(1, panel.GetColumnSpan(child)));
                    var margin = child.Margin;
                    int top = row == 0 ? area.Top : area.Top + firstHeight;
                    int height = row == 0 ? firstHeight : area.Height - firstHeight;
                    var bounds = new Rectangle(edges[column] + margin.Left, top + margin.Top,
                        Math.Max(0, edges[end] - edges[column] - margin.Horizontal), Math.Max(0, height - margin.Vertical));
                    // The native numeric editor enforces this font-based height.
                    // Do not repeatedly propose a larger height that it rejects.
                    if (child is NumericUpDown numeric) bounds.Height = Math.Min(bounds.Height, numeric.PreferredHeight);
                    if (child.Bounds != bounds) child.SetBounds(bounds.X, bounds.Y, bounds.Width, bounds.Height, BoundsSpecified.All);
                }
                panel.NeedsViewportLayout = false;
            }
            finally { panel._arranging = false; }
            return false;
        }
    }

    private sealed class FixedRightRail(FastStatColumnsPanel owner) : Panel
    {
        private readonly FastStatColumnsPanel _owner = owner;
        private readonly Dictionary<int, Control> _cells = new();
        private bool _arranging;
        public override LayoutEngine LayoutEngine => RailLayout.Instance;

        internal void Add(Control control, int column)
        {
            _cells.Add(column, control);
            control.TabIndex = column - 2;
            Controls.Add(control);
            PerformLayout();
        }

        private sealed class RailLayout : LayoutEngine
        {
            internal static readonly RailLayout Instance = new();
            public override bool Layout(object container, LayoutEventArgs layoutEventArgs)
            {
                var rail = (FixedRightRail)container;
                if (rail._arranging) return false;
                rail._arranging = true;
                try
                {
                    var area = rail.DisplayRectangle;
                    int left = area.Left;
                    for (int column = 2; column <= 6; column++)
                    {
                        int width = Math.Max(0, (int)Math.Round(rail._owner.ColumnStyles[column].Width));
                        if (rail._cells.TryGetValue(column, out var child))
                        {
                            var margin = child.Margin;
                            var bounds = new Rectangle(left + margin.Left, area.Top + margin.Top,
                                Math.Max(0, width - margin.Horizontal), Math.Max(0, area.Height - margin.Vertical));
                            if (child is NumericUpDown numeric) bounds.Height = Math.Min(bounds.Height, numeric.PreferredHeight);
                            if (child.Bounds != bounds) child.SetBounds(bounds.X, bounds.Y, bounds.Width, bounds.Height, BoundsSpecified.All);
                        }
                        left += width;
                    }
                }
                finally { rail._arranging = false; }
                return false;
            }
        }
    }
}

// The scroll extent is supplied by StatEditorPanel. Moving the viewport must not
// resize every offscreen native row window just to keep that extent accurate.
internal sealed class FastStatRowsPanel : TableLayoutPanel
{
    private bool _arranging;
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    internal ScrollableControl? Viewport { get; set; }
    public override LayoutEngine LayoutEngine => StatRowsLayout.Instance;
    public override Size GetPreferredSize(Size proposedSize) => new(0, Height);

    protected override void OnVisibleChanged(EventArgs e)
    {
        base.OnVisibleChanged(e);
        if (Visible) PerformLayout();
    }

    private sealed class StatRowsLayout : LayoutEngine
    {
        internal static readonly StatRowsLayout Instance = new();

        public override bool Layout(object container, LayoutEventArgs layoutEventArgs)
        {
            var panel = (FastStatRowsPanel)container;
            if (panel._arranging) return false;
            panel._arranging = true;
            try
            {
                Rectangle area = panel.DisplayRectangle;
                Span<int> tops = stackalloc int[panel.RowStyles.Count + 1];
                tops[0] = area.Top;
                for (int index = 0; index < panel.RowStyles.Count; index++)
                    tops[index + 1] = tops[index] + Math.Max(0, (int)Math.Round(panel.RowStyles[index].Height));

                int viewportOffset = panel.Top;
                Control? parent = panel.Parent;
                while (parent is not null && parent != panel.Viewport)
                {
                    viewportOffset += parent.Top;
                    parent = parent.Parent;
                }
                foreach (Control child in panel.Controls)
                {
                    int row = panel.GetRow(child);
                    if ((uint)row >= (uint)panel.RowStyles.Count) continue;
                    var margin = child.Margin;
                    var bounds = new Rectangle(area.Left + margin.Left, tops[row] + margin.Top,
                        Math.Max(0, area.Width - margin.Horizontal), Math.Max(0, tops[row + 1] - tops[row] - margin.Vertical));
                    var columns = child as FastStatColumnsPanel;
                    columns?.SetLogicalBounds(bounds);

                    bool inViewport = panel.Visible && (panel.Viewport is null || parent is null
                        || (viewportOffset + bounds.Top < panel.Viewport.ClientSize.Height && viewportOffset + bounds.Bottom > 0));
                    if (bounds.Height <= 0) continue;
                    // Keep every matching row at its real vertical position for
                    // native focus/Tab scrolling; defer only offscreen width.
                    int width = inViewport ? bounds.Width : child.Width;
                    var placed = new Rectangle(bounds.X, bounds.Y, width, bounds.Height);
                    if (child.Bounds != placed) child.SetBounds(placed.X, placed.Y, placed.Width, placed.Height, BoundsSpecified.All);
                    if (inViewport) columns?.LayoutIfInViewport();
                }
            }
            finally { panel._arranging = false; }
            return false;
        }
    }
}
