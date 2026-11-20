using System.Globalization;
using Trainer.Shared;

namespace Trainer.Desktop;

internal sealed class StatEditorPanel : UserControl
{
    private static readonly Color Background = Color.FromArgb(18, 22, 28);
    private static readonly Color Surface = Color.FromArgb(28, 34, 43);
    private static readonly Color Raised = Color.FromArgb(39, 47, 58);
    private static readonly Color Primary = Color.FromArgb(233, 238, 246);
    private static readonly Color Secondary = Color.FromArgb(155, 169, 189);
    private static readonly Color Accent = Color.FromArgb(91, 211, 170);
    private static readonly Color Warning = Color.FromArgb(240, 190, 101);
    private readonly Func<Request, string?, Task> _send;
    private readonly Action<string>? _beginHotkeyCapture;
    private readonly Func<string, string>? _hotkeyText;
    private readonly Dictionary<string, StatRow> _rows = new();
    private readonly Dictionary<string, StatState> _states = new();
    private readonly HashSet<string> _pending = new();
    private readonly TextBox _search = new();
    private readonly Button _disableStats = new DarkButton();
    private readonly Label _summary = new();
    private readonly TableLayoutPanel _table = new FastStatRowsPanel();
    private TableLayoutPanel _headings = null!;
    private Panel _scroll = null!;
    private float _rowHeight = 69;
    private int _rowHeightDpi = 96;
    private readonly LocalizedToolTip _tips = new() { AutoPopDelay = 15000, InitialDelay = 400, ReshowDelay = 100 };
    private bool _connected, _playerReady, _blocked, _updating, _clearing;

    public bool HasEnabledStats => _rows.Keys.Any(id => _states.TryGetValue(id, out var state) && state.Enabled);

    public StatEditorPanel(Func<Request, string?, Task> send, Action<string>? beginHotkeyCapture = null, Func<string, string>? hotkeyText = null)
    {
        _send = send; _beginHotkeyCapture = beginHotkeyCapture; _hotkeyText = hotkeyText;
        Dock = DockStyle.Fill; BackColor = Background; ForeColor = Primary; Margin = Padding.Empty;
        BuildLayout();
        RefreshHotkeyLabels();
        SetDisconnected();
        UiLocalization.LanguageChanged += OnLanguageChanged;
        DpiChangedAfterParent += OnDpiChangedAfterParent;
        UiLocalization.Attach(this);
    }

    private void OnDpiChangedAfterParent(object? sender, EventArgs e)
    {
        if (IsDisposed) return;
        RememberRowHeight();
        RefreshHotkeyLabels();
        FilterRows();
    }

    private void RememberRowHeight()
    {
        float measured = _table.RowStyles.Cast<RowStyle>().Select(style => style.Height).FirstOrDefault(height => height > 0);
        if (measured > 0) _rowHeight = measured;
        else if (DeviceDpi != _rowHeightDpi) _rowHeight *= DeviceDpi / (float)_rowHeightDpi;
        _rowHeightDpi = DeviceDpi;
    }

    private void OnLanguageChanged(object? sender, EventArgs e)
    {
        if (IsDisposed) return;
        bool wasUpdating = _updating;
        _updating = true;
        try { RefreshRows(); RefreshHotkeyLabels(); FilterRows(); UiLocalization.Refresh(this); }
        finally { _updating = wasUpdating; }
    }

    public void ApplyState(TrainerState state, bool blocked)
    {
        _connected = true; _playerReady = state.PlayerReady; _blocked = blocked;
        _states.Clear();
        foreach (var stat in state.Stats) _states[stat.Id] = stat;
        RefreshRows();
    }

    public void SetBlocked(bool blocked) { _blocked = blocked; RefreshRows(); }

    public void SetDisconnected()
    {
        _connected = false; _playerReady = false; _states.Clear();
        RefreshRows();
    }

    internal void RefreshHotkeyLabels()
    {
        int width = 92;
        foreach (var row in _rows.Values)
        {
            string? text = _hotkeyText?.Invoke(row.Definition.Id);
            string caption = string.IsNullOrWhiteSpace(text) ? "未绑定" : text;
            UiLocalization.SetText(row.HotkeyButton, caption);
            _tips.SetToolTip(row.HotkeyButton, caption + "\n点击设置属性快捷键；快捷键仅在游戏前台生效。");
            width = Math.Max(width, TextRenderer.MeasureText(UiLocalization.Translate(caption), row.HotkeyButton.Font).Width
                + row.HotkeyButton.Margin.Horizontal + row.HotkeyButton.Padding.Horizontal + 12);
        }
        if (_headings.ColumnStyles[0].Width != width) _headings.ColumnStyles[0].Width = width;
        foreach (var row in _rows.Values)
            if (row.Panel.ColumnStyles[0].Width != width) row.Panel.ColumnStyles[0].Width = width;
    }

    internal async Task ToggleHotkeyAsync(string statId)
    {
        if (statId == StatIds.Seal || !_rows.TryGetValue(statId, out var row) || !_connected || !_playerReady
            || _blocked || _clearing || _pending.Contains(statId) || !row.Toggle.Enabled
            || !_states.TryGetValue(statId, out var state) || !state.Supported) return;
        await SetStatAsync(row, !state.Enabled);
    }

    private void BuildLayout()
    {
        var root = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 4, Margin = Padding.Empty, Padding = new Padding(8, 6, 8, 4) };
        root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 39));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 28));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        Controls.Add(root);
        var note = TextLabel("仅覆盖本局属性。关闭后按角色、永久强化和局内道具重新计算。\n不改永久强化购买记录；正常关卡进度仍由游戏保存。", Secondary);
        note.Font = new Font(Font.FontFamily, 8.5F); root.Controls.Add(note, 0, 0);

        var toolbar = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, RowCount = 1, Margin = Padding.Empty };
        toolbar.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 245));
        toolbar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        toolbar.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 146));
        _search.PlaceholderText = "搜索属性、分类或单位"; _search.BackColor = Raised; _search.ForeColor = Primary;
        _search.BorderStyle = BorderStyle.FixedSingle; _search.Dock = DockStyle.Fill; _search.Margin = new Padding(0, 5, 12, 5);
        _search.TextChanged += (_, _) => FilterRows(); toolbar.Controls.Add(_search, 0, 0);
        _summary.ForeColor = Secondary; _summary.Dock = DockStyle.Fill; _summary.TextAlign = ContentAlignment.MiddleLeft; _summary.AutoEllipsis = true;
        toolbar.Controls.Add(_summary, 1, 0);
        _disableStats.Text = "关闭全部属性"; StyleButton(_disableStats); _disableStats.Dock = DockStyle.Fill; _disableStats.Margin = new Padding(8, 2, 0, 3);
        _disableStats.Click += async (_, _) =>
        {
            if (!_disableStats.Enabled || _clearing) return;
            _clearing = true; RefreshRows();
            try { await _send(new Request { Kind = "disableStats" }, "已关闭全部属性覆盖，恢复正常计算。"); }
            finally { _clearing = false; RefreshRows(); }
        };
        toolbar.Controls.Add(_disableStats, 2, 0); root.Controls.Add(toolbar, 0, 1);

        var headings = _headings = CreateColumns(); headings.Padding = new Padding(8, 0, 8 + SystemInformation.VerticalScrollBarWidth, 0);
        string[] headers = ["快捷键", "属性 / 单位", "正常值", "当前生效值", "目标值", "提交修改", "本局开关"];
        for (int i = 0; i < headers.Length; i++)
        {
            var label = TextLabel(headers[i], Secondary); label.Font = new Font(Font.FontFamily, 8);
            if (i != 1) label.TextAlign = ContentAlignment.MiddleCenter;
            headings.Controls.Add(label, i, 0);
        }
        root.Controls.Add(headings, 0, 2);
        var scroll = _scroll = new Panel { Dock = DockStyle.Fill, AutoScroll = true, BackColor = Background, Margin = Padding.Empty };
        scroll.Layout += (_, _) => { UpdateHeadingPadding(); LayoutVisibleRows(); };
        scroll.Scroll += (_, _) => LayoutVisibleRows();
        scroll.SizeChanged += (_, _) => LayoutVisibleRows();
        _table.ColumnCount = 1; _table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        ((FastStatRowsPanel)_table).Viewport = scroll;
        _table.Dock = DockStyle.Top; _table.AutoSize = false; _table.Margin = Padding.Empty; _table.Padding = Padding.Empty;
        _table.LocationChanged += (_, _) => LayoutVisibleRows();
        scroll.Controls.Add(_table); root.Controls.Add(scroll, 0, 3);
        _table.SuspendLayout();
        foreach (var definition in StatCatalog.All.Where(definition => definition.Id != StatIds.Seal))
        {
            var row = new StatRow(definition);
            row.Panel = CreateColumns(); row.Panel.BackColor = Surface; row.Panel.Padding = new Padding(8, 4, 8, 4); row.Panel.Margin = new Padding(0, 0, 0, 4);
            ((FastStatColumnsPanel)row.Panel).Viewport = scroll;
            row.Panel.RowCount = 2; row.Panel.RowStyles.Clear();
            row.Panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 34)); row.Panel.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            StyleButton(row.HotkeyButton); row.HotkeyButton.Dock = DockStyle.Fill; row.HotkeyButton.Margin = new Padding(0, 2, 8, 2);
            row.HotkeyButton.Enabled = _beginHotkeyCapture is not null;
            row.HotkeyButton.Click += (_, _) => _beginHotkeyCapture?.Invoke(definition.Id);
            _tips.SetToolTip(row.HotkeyButton, "点击设置属性快捷键；快捷键仅在游戏前台生效。");
            row.Panel.Controls.Add(row.HotkeyButton, 0, 0);
            var title = TextLabel(definition.Name + " · " + definition.Unit, Primary); title.Font = new Font(Font.FontFamily, 9, FontStyle.Bold);
            _tips.SetToolTip(title, definition.Group + " · " + definition.Description); row.Panel.Controls.Add(title, 1, 0);
            foreach (var valueLabel in new[] { row.Original, row.Current })
            {
                valueLabel.Dock = DockStyle.Fill; valueLabel.TextAlign = ContentAlignment.MiddleCenter; valueLabel.ForeColor = Secondary;
            }
            ((FastStatColumnsPanel)row.Panel).AddFixedRightControl(row.Original, 2);
            ((FastStatColumnsPanel)row.Panel).AddFixedRightControl(row.Current, 3);
            row.Target.Minimum = (decimal)definition.Min; row.Target.Maximum = (decimal)definition.Max;
            row.Target.DecimalPlaces = definition.DecimalPlaces; row.Target.Increment = definition.DecimalPlaces == 0 ? 1 : .1m;
            row.Target.Value = (decimal)definition.DefaultValue; row.Target.ThousandsSeparator = true;
            row.Target.BackColor = Raised; row.Target.ForeColor = Primary; row.Target.BorderStyle = BorderStyle.FixedSingle;
            row.Target.Dock = DockStyle.Fill; row.Target.Margin = new Padding(4, 4, 8, 0);
            _tips.SetToolTip(row.Target, $"{definition.Description}\n范围 {definition.Min}～{definition.Max} {definition.Unit}；输入后点击“应用”，或开启本行。");
            row.Target.ValueChanged += (_, _) => { if (!_updating) { row.Dirty = true; RefreshRows(); } };
            ((FastStatColumnsPanel)row.Panel).AddFixedRightControl(row.Target, 4);
            row.Apply.Text = "应用"; StyleButton(row.Apply); row.Apply.Dock = DockStyle.Fill; row.Apply.Margin = new Padding(2, 2, 6, 2);
            row.Apply.Click += async (_, _) => { if (row.Apply.Enabled) await SetStatAsync(row, true); };
            ((FastStatColumnsPanel)row.Panel).AddFixedRightControl(row.Apply, 5);
            StyleButton(row.Toggle); row.Toggle.Dock = DockStyle.Fill; row.Toggle.Margin = new Padding(0, 2, 0, 2);
            row.Toggle.Click += async (_, _) =>
            {
                if (row.Toggle.Enabled && _states.TryGetValue(definition.Id, out var state)) await SetStatAsync(row, !state.Enabled);
            };
            ((FastStatColumnsPanel)row.Panel).AddFixedRightControl(row.Toggle, 6);
            row.Reason.Dock = DockStyle.Fill; row.Reason.TextAlign = ContentAlignment.MiddleLeft; row.Reason.AutoEllipsis = true;
            row.Reason.Font = new Font(Font.FontFamily, 8); row.Reason.Margin = Padding.Empty;
            row.Panel.Controls.Add(row.Reason, 0, 1); row.Panel.SetColumnSpan(row.Reason, 7);
            foreach (var input in new Control[] { row.HotkeyButton, row.Target, row.Apply, row.Toggle })
                input.Enter += (_, _) => { scroll.ScrollControlIntoView(row.Panel); LayoutVisibleRows(); };
            _rows.Add(definition.Id, row);
            _table.RowStyles.Add(new RowStyle(SizeType.Absolute, 69)); _table.Controls.Add(row.Panel, 0, _table.Controls.Count);
        }
        RememberRowHeight();
        _table.Height = _rows.Count * (int)Math.Round(_rowHeight) + _table.Padding.Vertical;
        _table.ResumeLayout(true);
    }

    private static TableLayoutPanel CreateColumns()
    {
        var panel = new FastStatColumnsPanel { Dock = DockStyle.Fill, ColumnCount = 7, RowCount = 1, Margin = Padding.Empty };
        panel.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 92));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        foreach (int width in new[] { 113, 113, 142, 68, 94 }) panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, width));
        return panel;
    }

    private void UpdateHeadingPadding()
    {
        var padding = _headings.Padding;
        int right = padding.Left + (_scroll.VerticalScroll.Visible ? Math.Max(0, _scroll.Width - _scroll.ClientSize.Width) : 0);
        if (padding.Right != right) _headings.Padding = new Padding(padding.Left, padding.Top, right, padding.Bottom);
    }

    private void LayoutVisibleRows()
    {
        _table.PerformLayout();
    }

    private void RefreshRows()
    {
        _disableStats.Enabled = _connected && !_blocked && !_clearing && _pending.Count == 0 && HasEnabledStats;
        int supported = 0, enabled = 0;
        foreach (var row in _rows.Values)
        {
            _states.TryGetValue(row.Definition.Id, out var state);
            bool supportedRow = state?.Supported == true;
            bool isEnabled = supportedRow && state?.Enabled == true;
            bool pending = _pending.Contains(row.Definition.Id);
            bool ready = _connected && _playerReady && supportedRow && !_blocked && !_clearing && !pending;
            if (supportedRow) supported++;
            if (isEnabled) enabled++;
            UiLocalization.SetText(row.Original, FormatValue(state?.OriginalValue, row.Definition));
            UiLocalization.SetText(row.Current, FormatValue(state?.CurrentValue, row.Definition));
            row.Current.ForeColor = isEnabled ? Accent : Primary;
            row.Target.Enabled = ready;
            row.Apply.Enabled = ready && isEnabled && row.Dirty;
            row.Toggle.Enabled = ready;
            UiLocalization.SetText(row.Toggle, !_connected ? "未连接" : !supportedRow ? "不可用" : isEnabled ? "关闭" : "开启");
            row.Toggle.BackColor = isEnabled ? Color.FromArgb(34, 97, 79) : Raised;
            row.Toggle.ForeColor = isEnabled ? Accent : Primary;
            string reason = !_connected ? "未连接 · " + row.Definition.Group
                : state is null ? "插件未提供此属性；请更新并安装配套插件。"
                : !supportedRow ? state.Reason ?? "当前游戏版本未支持此属性。"
                : pending ? "等待插件确认…"
                : !_playerReady ? "进入关卡后可用。"
                : !string.IsNullOrWhiteSpace(state.Reason) ? state.Reason
                : isEnabled ? row.Dirty ? "已开启 · 输入有修改，点击“应用”后生效。" : "已开启 · 关闭后恢复正常计算。"
                : "已关闭 · " + row.Definition.Description;
            UiLocalization.SetText(row.Reason, reason); row.Reason.ForeColor = _connected && !supportedRow ? Warning : isEnabled ? Accent : Secondary;
            _tips.SetToolTip(row.Reason, reason + "\n" + row.Definition.Description);
            if (!_updating && !row.Dirty && !pending && !row.Target.ContainsFocus && isEnabled && double.IsFinite(state!.Value))
            {
                _updating = true;
                try { row.Target.Value = (decimal)Math.Clamp(state.Value, row.Definition.Min, row.Definition.Max); }
                finally { _updating = false; }
            }
        }
        string summary = !_connected ? "未连接 · 所有属性关闭" : _states.Count == 0 ? "插件未提供属性编辑，请更新配套插件" : $"可用 {supported}/{_rows.Count} 项 · 已开启 {enabled} 项";
        UiLocalization.SetText(_summary, summary);
        _tips.SetToolTip(_summary, summary);
    }

    private async Task SetStatAsync(StatRow row, bool enabled)
    {
        if (!_pending.Add(row.Definition.Id)) return;
        double target = (double)row.Target.Value;
        RefreshRows();
        try
        {
            await _send(new Request { Kind = "setStat", Stat = row.Definition.Id, Enabled = enabled, Value = enabled ? target : null }, enabled ? $"已提交属性：{row.Definition.Name} = {target} {row.Definition.Unit}" : $"已关闭属性：{row.Definition.Name}");
            if (_connected && _states.TryGetValue(row.Definition.Id, out var state) && state.Enabled == enabled && (!enabled || Math.Abs(state.Value - target) < .00001))
                row.Dirty = false;
        }
        finally { _pending.Remove(row.Definition.Id); RefreshRows(); }
    }

    private void FilterRows()
    {
        string query = _search.Text.Trim();
        RememberRowHeight();
        _table.SuspendLayout();
        int index = 0;
        int visibleRows = 0;
        bool changed = false;
        foreach (var row in _rows.Values)
        {
            string original = row.Definition.Name + " " + row.Definition.Group + " " + row.Definition.Unit + " " + row.Definition.Id;
            string translated = UiLocalization.Translate(row.Definition.Name) + " " + UiLocalization.Translate(row.Definition.Group) + " " + UiLocalization.Translate(row.Definition.Unit);
            bool visible = query.Length == 0 || original.Contains(query, StringComparison.OrdinalIgnoreCase) || translated.Contains(query, StringComparison.OrdinalIgnoreCase);
            if (visible) visibleRows++;
            var style = _table.RowStyles[index++];
            float cellHeight = visible ? _rowHeight : 0;
            if (row.MatchesFilter == visible && style.Height == cellHeight) continue;
            changed = true;
            if (row.MatchesFilter != visible) { row.MatchesFilter = visible; row.Panel.Visible = visible; }
            if (style.Height != cellHeight) style.Height = cellHeight;
        }
        int height = visibleRows * (int)Math.Round(_rowHeight) + _table.Padding.Vertical;
        if (_table.Height != height) { _table.Height = height; changed = true; }
        _table.ResumeLayout(changed);
        UpdateHeadingPadding();
        LayoutVisibleRows();
    }

    private static string FormatValue(double? value, StatDefinition definition) => value.HasValue && double.IsFinite(value.Value)
        ? value.Value.ToString("N" + definition.DecimalPlaces, CultureInfo.CurrentCulture) : "—";
    private static Label TextLabel(string text, Color color) => new() { Text = text, ForeColor = color, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft, Margin = Padding.Empty, AutoEllipsis = true };
    private static void StyleButton(Button button)
    {
        button.FlatStyle = FlatStyle.Flat; button.FlatAppearance.BorderSize = 1; button.FlatAppearance.BorderColor = Color.FromArgb(60, 72, 88);
        button.FlatAppearance.MouseOverBackColor = Color.FromArgb(52, 65, 80); button.FlatAppearance.MouseDownBackColor = Color.FromArgb(35, 84, 70);
        button.BackColor = Raised; button.ForeColor = Primary; button.Cursor = Cursors.Hand; button.UseVisualStyleBackColor = false;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            UiLocalization.LanguageChanged -= OnLanguageChanged;
            DpiChangedAfterParent -= OnDpiChangedAfterParent;
            _tips.Dispose();
        }
        base.Dispose(disposing);
    }

    private sealed class StatRow(StatDefinition definition)
    {
        public StatDefinition Definition { get; } = definition;
        public TableLayoutPanel Panel { get; set; } = null!;
        public Label Original { get; } = new();
        public Label Current { get; } = new();
        public Label Reason { get; } = new();
        public Button HotkeyButton { get; } = new DarkButton();
        public NumericUpDown Target { get; } = new();
        public Button Apply { get; } = new DarkButton();
        public Button Toggle { get; } = new DarkButton();
        public bool Dirty { get; set; }
        public bool MatchesFilter { get; set; } = true;
    }
}
