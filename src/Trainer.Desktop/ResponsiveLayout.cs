using System.Reflection;
using System.Runtime.CompilerServices;

namespace Trainer.Desktop;

// Buffer each managed container's own paint surface. Native child windows keep
// their normal live behavior; no WS_EX_COMPOSITED or captured-window overlay.
internal class BufferedTableLayoutPanel : TableLayoutPanel
{
    internal BufferedTableLayoutPanel() => DoubleBuffered = true;
}

internal class BufferedPanel : Panel
{
    internal BufferedPanel() => DoubleBuffered = true;
}

internal class BufferedFlowLayoutPanel : FlowLayoutPanel
{
    internal BufferedFlowLayoutPanel() => DoubleBuffered = true;
}

// TabControl updates its native display rectangle several times while applying
// one bounds change. Arrange the selected page once after that synchronous
// operation, with its final rectangle; this never spans a message or a frame.
internal sealed class ResponsiveTabControl : TabControl
{
    private bool _batchingBounds;

    protected override void SetBoundsCore(int x, int y, int width, int height, BoundsSpecified specified)
    {
        var selected = SelectedTab;
        if (_batchingBounds || IsDisposed || Disposing || selected is null || selected.IsDisposed
            || (width == Width && height == Height))
        {
            base.SetBoundsCore(x, y, width, height, specified);
            return;
        }
        _batchingBounds = true;
        selected.SuspendLayout();
        try { base.SetBoundsCore(x, y, width, height, specified); }
        finally
        {
            // A selection or parent change can occur inside native processing.
            // Release only our own count; the hidden-page policy keeps its
            // separate suspension if this page has become inactive.
            try { if (!selected.IsDisposed) selected.ResumeLayout(true); }
            finally { _batchingBounds = false; }
        }
    }
}

internal static class ResponsiveLayout
{
    private static readonly ConditionalWeakTable<Control, Attachment> Attachments = new();
    private static readonly PropertyInfo DoubleBufferedProperty = typeof(Control).GetProperty("DoubleBuffered", BindingFlags.Instance | BindingFlags.NonPublic)!;

    // May be called after BuildLayout on a Form or a panel. Added controls are
    // configured too. The attachment releases its own layout suspensions and
    // event subscriptions when the root is disposed.
    internal static void Attach(Control root, bool suspendHiddenTabs = true)
    {
        if (root.IsDisposed || Attachments.TryGetValue(root, out _)) return;
        Attachments.Add(root, new Attachment(root, suspendHiddenTabs));
    }

    private sealed class Attachment : IDisposable
    {
        private readonly Control _root;
        private readonly bool _suspendHiddenTabs;
        private readonly HashSet<Control> _controls = new();
        private readonly List<TabLayout> _tabs = new();
        private bool _disposed;

        internal Attachment(Control root, bool suspendHiddenTabs)
        {
            _root = root; _suspendHiddenTabs = suspendHiddenTabs;
            Configure(root);
            root.Disposed += OnDisposed;
        }

        private void Configure(Control control)
        {
            if (control.IsDisposed || !_controls.Add(control)) return;
            if (control is Panel or UserControl)
                DoubleBufferedProperty.SetValue(control, true);
            if (_suspendHiddenTabs && control is TabControl tabs) _tabs.Add(new TabLayout(tabs));
            control.ControlAdded += OnControlAdded;
            foreach (Control child in control.Controls) Configure(child);
        }

        private void OnControlAdded(object? sender, ControlEventArgs e)
        {
            if (!_disposed && e.Control is not null) Configure(e.Control);
        }
        private void OnDisposed(object? sender, EventArgs e) => Dispose();
        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            _root.Disposed -= OnDisposed;
            foreach (var tabs in _tabs) tabs.Dispose();
            foreach (var control in _controls) control.ControlAdded -= OnControlAdded;
            _tabs.Clear(); _controls.Clear();
        }
    }

    private sealed class TabLayout : IDisposable
    {
        private readonly TabControl _tabs;
        private readonly HashSet<TabPage> _suspended = new();
        private bool _disposed;

        internal TabLayout(TabControl tabs)
        {
            _tabs = tabs;
            tabs.SelectedIndexChanged += OnSelected;
            tabs.ControlAdded += OnPageAdded;
            tabs.ControlRemoved += OnPageRemoved;
            tabs.HandleCreated += OnHandleCreated;
            tabs.Disposed += OnDisposed;
            Synchronize();
        }

        private void OnSelected(object? sender, EventArgs e) => Synchronize();
        private void OnHandleCreated(object? sender, EventArgs e)
        {
            // Native items and the saved SelectedIndex are installed after
            // TabControl raises HandleCreated. Pre-handle selection changes do
            // not raise SelectedIndexChanged, so synchronize after that setup.
            _tabs.BeginInvoke(() => Synchronize());
        }
        private void OnPageAdded(object? sender, ControlEventArgs e) => Synchronize();
        private void OnPageRemoved(object? sender, ControlEventArgs e)
        {
            if (e.Control is TabPage page && _suspended.Remove(page) && !page.IsDisposed)
                page.ResumeLayout(false);
            Synchronize();
        }
        private void Synchronize()
        {
            if (_disposed || _tabs.IsDisposed || _tabs.Disposing) return;
            var selected = _tabs.SelectedTab;
            foreach (TabPage page in _tabs.TabPages)
            {
                // TabPages can still enumerate a removed page while handling
                // ControlRemoved. Its Parent is already null at that point.
                if (page.IsDisposed || page.Parent != _tabs) continue;
                if (page == selected)
                {
                    // A page receives one normal layout with its latest bounds
                    // when selected. Inputs, localization and virtual lists all
                    // remain the original controls throughout the interaction.
                    if (_suspended.Remove(page)) page.ResumeLayout(true);
                }
                else if (_suspended.Add(page)) page.SuspendLayout();
            }
        }
        private void OnDisposed(object? sender, EventArgs e) => Dispose();
        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            _tabs.SelectedIndexChanged -= OnSelected;
            _tabs.ControlAdded -= OnPageAdded;
            _tabs.ControlRemoved -= OnPageRemoved;
            _tabs.HandleCreated -= OnHandleCreated;
            _tabs.Disposed -= OnDisposed;
            foreach (var page in _suspended) if (!page.IsDisposed) page.ResumeLayout(false);
            _suspended.Clear();
        }
    }
}
