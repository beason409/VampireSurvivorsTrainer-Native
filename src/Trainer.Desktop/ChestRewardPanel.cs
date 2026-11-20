using Trainer.Shared;

namespace Trainer.Desktop;

internal sealed partial class ChestRewardPanel : UserControl
{
    private const int PageSize = 200;
    private const int CatalogCap = 4096;
    private static readonly Color Background = Color.FromArgb(18, 22, 28);
    private static readonly Color Surface = Color.FromArgb(28, 34, 43);
    private static readonly Color Raised = Color.FromArgb(39, 47, 58);
    private static readonly Color Primary = Color.FromArgb(233, 238, 246);
    private static readonly Color Secondary = Color.FromArgb(155, 169, 189);
    private static readonly Color Accent = Color.FromArgb(91, 211, 170);
    private static readonly Color Warning = Color.FromArgb(240, 190, 101);
    private readonly Func<Request, string?, Task> _send;
    private readonly List<RewardItemDefinition> _catalog = new();
    private readonly Dictionary<string, RewardItemDefinition> _catalogById = new(StringComparer.Ordinal);
    private RewardItemDefinition[] _filteredItems = Array.Empty<RewardItemDefinition>();
    private readonly Dictionary<string, ListViewItem> _rowCache = new(StringComparer.Ordinal);
    private TableLayoutPanel _layoutRoot = null!;
    private int _columnLayoutWidth = -1;
    private bool _columnLayoutEnglish, _updatingColumns;
    private readonly TextBox _search = new();
    private readonly ComboBox _kind = new();
    private readonly ListView _items = new();
    private readonly Label _summary = new();
    private readonly Label _selection = new();
    private readonly Label _activeTarget = new();
    private readonly Label _operationHint = new();
    private readonly Label _status = new();
    private readonly Label _lastReward = new();
    private readonly Label _upgradeStatus = new();
    private readonly Label _lastUpgrade = new();
    private readonly Button _refresh = new DarkButton();
    private readonly Button _enable = new DarkButton();
    private readonly Button _upgrade = new DarkButton();
    private readonly Button _applyTarget = new DarkButton();
    private readonly Button _chestOnly = new DarkButton();
    private readonly Button _disable = new DarkButton();
    private readonly LocalizedToolTip _tips = new() { AutoPopDelay = 15000, InitialDelay = 400 };
    private ChestRewardState _state = new();
    private RewardItemDefinition? _selected;
    private bool _connected, _playerReady, _blocked, _loading, _pending, _catalogLoaded, _filtering, _previewMode;
    private bool _pageReceived, _catalogFailed;
    private int _pageCount, _catalogTotal;
    private string? _catalogMessage, _operationError, _operationErrorScope, _observedTargetId;

    internal bool HasEnabledReward => _connected && (_state.Enabled || _state.UpgradeEnabled);
    private bool CatalogSupported => _state.Supported || _state.UpgradeSupported;

    internal ChestRewardPanel(Func<Request, string?, Task> send, Func<string, Task<RewardItemIcon?>>? loadIcon = null)
    {
        _send = send; _loadIcon = loadIcon;
        Dock = DockStyle.Fill; BackColor = Background; ForeColor = Primary; Margin = Padding.Empty;
        BuildLayout();
        _iconTimer.Tick += async (_, _) => await LoadVisibleIconsAsync();
        VisibleChanged += (_, _) =>
        {
            if (Visible && _connected && _catalog.Count == 0 && !_loading)
            { _catalogLoaded = false; _catalogFailed = false; _catalogMessage = null; }
            QueueInitialCatalog();
            UpdateIconTimer();
        };
        SetDisconnected();
        UiLocalization.Attach(this);
        UiLocalization.LanguageChanged += OnLanguageChanged;
        OnLanguageChanged(this, EventArgs.Empty);
    }

    private void OnLanguageChanged(object? sender, EventArgs e)
    {
        if (IsDisposed || Disposing) return;
        _layoutRoot.RowStyles[0].Height = UiLocalization.IsEnglish ? 62 : 40;
        string? selectedId = _selected?.Id;
        int topIndex = _items.IsHandleCreated ? _items.TopItem?.Index ?? -1 : -1;
        // The catalog and icon cache are language-independent. Rebuild only the
        // virtual row presentation; switching language sends no game command.
        _rowCache.Clear();
        FilterItems();
        if (selectedId is null && _selected is not null)
        {
            _filtering = true;
            try { _items.SelectedIndices.Clear(); _selected = null; }
            finally { _filtering = false; }
        }
        if (_items.IsHandleCreated && topIndex >= 0 && topIndex < _filteredItems.Length)
            _items.TopItem = _items.Items[topIndex];
        RefreshStatus();
        UiLocalization.Refresh(this);
        _items.Columns[1].Text = UiLocalization.IsEnglish ? "Game name" : "英文名称";
        UpdateColumns();
        _items.Invalidate();
    }

    internal void ApplyState(TrainerState state, bool blocked)
    {
        bool enteredRun = !_playerReady && state.PlayerReady;
        _connected = true; _playerReady = state.PlayerReady; _blocked = blocked;
        _state = state.ChestReward ?? new ChestRewardState();
        _iconsSupported = state.Diagnostics.TryGetValue("chest.catalog.icons", out string? support) && support == "supported";
        UpdateIconTimer();
        if (!string.Equals(_observedTargetId, _state.TargetItemId, StringComparison.Ordinal))
            _operationError = _operationErrorScope = null;
        _observedTargetId = _state.TargetItemId;
        if (enteredRun && _catalog.Count == 0 && !_loading)
        { _catalogLoaded = false; _catalogFailed = false; _catalogMessage = null; }
        RefreshStatus();
        QueueInitialCatalog();
    }

    internal void SetBlocked(bool blocked) { _blocked = blocked; RefreshStatus(); }

    internal void SetDisconnected()
    {
        _connected = false; _playerReady = false; _state = new(); _iconsSupported = false; _iconTimer.Stop();
        ClearCatalog(); _catalogLoaded = false; _catalogFailed = false; _catalogMessage = null; _operationError = _operationErrorScope = _observedTargetId = null;
        UiLocalization.RegisterItemNames(_catalog);
        _catalogTotal = 0; _selected = null;
        FilterItems(); RefreshStatus();
    }

    internal void SetCatalogPage(IReadOnlyList<RewardItemDefinition> items, int? total, int offset)
    {
        _pageReceived = true; _pageCount = items.Count;
        _catalogTotal = Math.Max(0, total ?? offset + items.Count);
        if (offset == 0)
        {
            ClearCatalog();
            if (total is > 0) _catalogById.EnsureCapacity(Math.Min(CatalogCap, total.Value));
        }
        AddCatalogItems(items);
        UiLocalization.RegisterItemNames(_catalog);
        if (!_loading) { _catalogLoaded = true; FilterItems(); RefreshStatus(); }
    }

    private void ClearCatalog()
    {
        ResetIcons(); _catalog.Clear(); _catalogById.Clear(); _rowCache.Clear();
    }

    private void AddCatalogItems(IReadOnlyList<RewardItemDefinition> items)
    {
        foreach (var item in items)
        {
            if (_catalog.Count >= CatalogCap) break;
            if (string.IsNullOrWhiteSpace(item.Id) || item.Id.Length > 256 || item.Kind is not ("weapon" or "passive")) continue;
            if (_catalogById.TryAdd(item.Id, item)) _catalog.Add(item);
        }
    }

    private RewardItemDefinition? FindCatalogItem(string? id) => id is not null && _catalogById.TryGetValue(id, out var item) ? item : null;

    internal void SetCatalogError(string? error)
    {
        _catalogFailed = true;
        _catalogMessage = string.IsNullOrWhiteSpace(error) ? "物品列表暂不可用，请稍后刷新。" : error;
        RefreshStatus();
    }

    internal void SetOperationError(string? error, string? scope = null)
    {
        _operationError = error ?? "插件未确认操作。";
        _operationErrorScope = scope;
        RefreshStatus();
    }

    internal void RenderPreview()
    {
        _previewMode = true;
        _connected = true; _playerReady = true; _blocked = true; _catalogLoaded = true;
        _loading = false; _pending = false; _catalogFailed = false; _catalogMessage = null;
        _search.Clear(); _kind.SelectedIndex = 0;
        _state = new ChestRewardState { Supported = true, UpgradeSupported = true, Message = "离线界面示意，未连接游戏" };
        ClearCatalog();
        AddCatalogItems(new[]
        {
            new RewardItemDefinition { Id = "TEST_WAND", Name = "魔杖（示例）", EnglishName = "Magic Wand (sample)", Kind = "weapon", MaxLevel = 8, UpgradeAllowed = true },
            new RewardItemDefinition { Id = "TEST_SPINACH", Name = "菠菜（示例）", EnglishName = "Spinach (sample)", Kind = "passive", MaxLevel = 5, UpgradeAllowed = true },
            new RewardItemDefinition { Id = "TEST_EVOLVED_WAND", Name = "进化武器（示例）", EnglishName = "Evolved Weapon (sample)", Kind = "weapon", IsEvolution = true, MaxLevel = 1, UpgradeAllowed = true }
        });
        _selected = _catalog[0];
        FilterItems(); RefreshStatus();
        UiLocalization.SetText(_summary, "示例数据 · 实际物品列表由连接后的游戏提供");
        UiLocalization.SetText(_status, "● 已关闭 · 离线界面示意，不执行游戏操作");
        UiLocalization.SetText(_lastReward, "尚未实测；此图不代表指定奖励已经在游戏内生效");
        UiLocalization.Refresh(this);
    }

    private void BuildLayout()
    {
        var root = _layoutRoot = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 7, Padding = new Padding(8, 6, 8, 4), Margin = Padding.Empty };
        root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 34));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 22));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 50));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 108));
        Controls.Add(root);
        var note = TextLabel("箱子与升级共用目标，可分别开启；每箱指定一件，升级保证一个可选项，其余照常（仍需手动选择）。\n应用目标会影响已开启的两项；关闭只停止后续指定，已获得物品保留本局。", Secondary);
        note.Font = new Font(Font.FontFamily, 8.5F); root.Controls.Add(note, 0, 0);
        var toolbar = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, RowCount = 1, Margin = Padding.Empty };
        toolbar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        toolbar.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 135));
        toolbar.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 120));
        _search.PlaceholderText = "搜索中文名、英文名或物品 ID";
        _search.Dock = DockStyle.Fill; _search.BackColor = Raised; _search.ForeColor = Primary; _search.BorderStyle = BorderStyle.FixedSingle;
        _search.Margin = new Padding(0, 5, 12, 5); _search.TextChanged += (_, _) => FilterItems(); toolbar.Controls.Add(_search, 0, 0);
        _kind.DropDownStyle = ComboBoxStyle.DropDownList; _kind.Items.AddRange(new object[] { "全部物品", "武器", "被动装备" });
        _kind.SelectedIndex = 0; _kind.Dock = DockStyle.Fill; _kind.Margin = new Padding(0, 5, 12, 5); _kind.BackColor = Raised; _kind.ForeColor = Primary;
        _kind.DrawMode = DrawMode.OwnerDrawFixed;
        _kind.DrawItem += (_, e) =>
        {
            using var fill = new SolidBrush(Raised); e.Graphics.FillRectangle(fill, e.Bounds);
            if (e.Index >= 0) TextRenderer.DrawText(e.Graphics, _kind.GetItemText(_kind.Items[e.Index]), e.Font, e.Bounds, Primary, TextFormatFlags.VerticalCenter | TextFormatFlags.Left);
            e.DrawFocusRectangle();
        };
        _kind.SelectedIndexChanged += (_, _) => FilterItems(); toolbar.Controls.Add(_kind, 1, 0);
        _refresh.Text = "刷新物品列表"; StyleButton(_refresh); _refresh.Dock = DockStyle.Fill; _refresh.Margin = new Padding(0, 2, 0, 3);
        _refresh.Click += async (_, _) => await RefreshCatalogAsync(); toolbar.Controls.Add(_refresh, 2, 0); root.Controls.Add(toolbar, 0, 1);
        _summary.Dock = DockStyle.Fill; _summary.ForeColor = Secondary; _summary.TextAlign = ContentAlignment.MiddleLeft; _summary.AutoEllipsis = true;
        root.Controls.Add(_summary, 0, 2);
        _items.Dock = DockStyle.Fill; _items.View = View.Details; _items.FullRowSelect = true; _items.MultiSelect = false; _items.HideSelection = false;
        _items.BorderStyle = BorderStyle.None; _items.BackColor = Surface; _items.ForeColor = Primary; _items.HeaderStyle = ColumnHeaderStyle.Nonclickable;
        _items.ShowItemToolTips = true;
        _items.SmallImageList = _icons;
        // Keep native rows out of the typing path. WinForms requests only the
        // visible rows, which are cached and reused across subsequent searches.
        _items.RetrieveVirtualItem += (_, e) => e.Item = GetRow(e.ItemIndex);
        _items.SearchForVirtualItem += (_, e) => FindRow(e);
        _items.VirtualMode = true;
        _items.Columns.Add("物品名称", 190); _items.Columns.Add("英文名称", 230); _items.Columns.Add("类型", 80);
        _items.Columns.Add("适用范围", 220); _items.Columns.Add("物品 ID", 230); _items.Columns.Add("最高等级", 75);
        _items.Resize += (_, _) => UpdateColumns();
        _items.SelectedIndexChanged += (_, _) => UpdateSelection();
        _items.VirtualItemsSelectionRangeChanged += (_, _) => UpdateSelection();
        root.Controls.Add(_items, 0, 3);
        var selection = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1, BackColor = Surface, Padding = new Padding(10, 5, 10, 5), Margin = new Padding(0, 4, 0, 0) };
        selection.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50)); selection.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        _selection.Dock = DockStyle.Fill; _selection.TextAlign = ContentAlignment.MiddleLeft; _selection.AutoEllipsis = true; selection.Controls.Add(_selection, 0, 0);
        _activeTarget.Dock = DockStyle.Fill; _activeTarget.TextAlign = ContentAlignment.MiddleLeft; _activeTarget.AutoEllipsis = true; selection.Controls.Add(_activeTarget, 1, 0);
        root.Controls.Add(selection, 0, 4);
        var actions = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 6, RowCount = 1, Padding = new Padding(2, 4, 2, 0), Margin = Padding.Empty };
        foreach (int width in new[] { 106, 119, 119, 234, 143 }) actions.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, width));
        actions.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        _applyTarget.Text = "应用目标"; StyleButton(_applyTarget); _applyTarget.Dock = DockStyle.Fill; _applyTarget.Margin = new Padding(8, 0, 6, 0);
        _applyTarget.Click += async (_, _) => await ApplyTargetAsync(); actions.Controls.Add(_applyTarget, 0, 0);
        _enable.Text = "开启箱子指定"; StyleButton(_enable); _enable.Dock = DockStyle.Fill; _enable.Margin = new Padding(0, 0, 6, 0);
        _enable.Click += async (_, _) => await SetRewardAsync(!_state.Enabled); actions.Controls.Add(_enable, 1, 0);
        _upgrade.Text = "开启升级指定"; StyleButton(_upgrade); _upgrade.Dock = DockStyle.Fill; _upgrade.Margin = new Padding(0, 0, 6, 0);
        _upgrade.Click += async (_, _) => await SetUpgradeAsync(!_state.UpgradeEnabled); actions.Controls.Add(_upgrade, 2, 0);
        _chestOnly.Text = "仅应用到箱子"; StyleButton(_chestOnly); _chestOnly.Dock = DockStyle.Fill; _chestOnly.Margin = new Padding(0, 0, 6, 0);
        _chestOnly.Click += async (_, _) => await ApplyChestOnlyAsync(); actions.Controls.Add(_chestOnly, 3, 0);
        _disable.Text = "全部恢复随机"; StyleButton(_disable); _disable.Dock = DockStyle.Fill; _disable.Margin = Padding.Empty;
        _disable.Click += async (_, _) => await DisableBothAsync(); actions.Controls.Add(_disable, 4, 0); root.Controls.Add(actions, 0, 5);
        var status = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 5, Margin = Padding.Empty };
        status.RowStyles.Add(new RowStyle(SizeType.Absolute, 36)); _operationHint.Dock = DockStyle.Fill;
        _operationHint.TextAlign = ContentAlignment.MiddleLeft; _operationHint.AutoEllipsis = true; status.Controls.Add(_operationHint, 0, 0);
        foreach (var label in new[] { _status, _lastReward, _upgradeStatus, _lastUpgrade })
        {
            status.RowStyles.Add(new RowStyle(SizeType.Percent, 25)); label.Dock = DockStyle.Fill;
            label.TextAlign = ContentAlignment.MiddleLeft; label.AutoEllipsis = true; status.Controls.Add(label, 0, status.Controls.Count);
        }
        root.Controls.Add(status, 0, 6);
    }

    private void UpdateColumns()
    {
        if (_items.Columns.Count < 6 || _updatingColumns) return;
        int clientWidth = _items.ClientSize.Width;
        bool english = UiLocalization.IsEnglish;
        if (_columnLayoutWidth == clientWidth && _columnLayoutEnglish == english) return;
        _columnLayoutWidth = clientWidth; _columnLayoutEnglish = english;
        Span<int> widths = stackalloc int[6];
        int available = english ? Math.Max(0, clientWidth - 395) : Math.Max(520, clientWidth - 395);
        widths[0] = Math.Max(english ? 210 : 150, available * (english ? 40 : 28) / 100);
        widths[1] = Math.Max(english ? 150 : 180, available * (english ? 27 : 35) / 100);
        widths[2] = english ? 145 : 80;
        widths[3] = english ? 155 : 220;
        widths[4] = Math.Max(190, available * (english ? 33 : 37) / 100);
        widths[5] = 75;
        bool changed = false;
        for (int index = 0; index < widths.Length; index++) changed |= _items.Columns[index].Width != widths[index];
        if (!changed) return;
        _updatingColumns = true;
        _items.BeginUpdate();
        try
        {
            for (int index = 0; index < widths.Length; index++)
                if (_items.Columns[index].Width != widths[index]) _items.Columns[index].Width = widths[index];
        }
        finally { _items.EndUpdate(); _updatingColumns = false; }
        // A scrollbar can alter client width while native columns are updated.
        // Converge once to that width; unchanged height-only resizes are free.
        if (_items.ClientSize.Width != clientWidth) UpdateColumns();
    }

    private void QueueInitialCatalog()
    {
        if (!Visible || !_connected || !CatalogSupported || _blocked || _loading || _catalogLoaded || _catalogFailed || !IsHandleCreated) return;
        BeginInvoke(async () => { if (!IsDisposed && Visible && _connected && !_catalogLoaded && !_catalogFailed) await RefreshCatalogAsync(); });
    }

    private async Task RefreshCatalogAsync()
    {
        if (!_connected || !CatalogSupported || _blocked || _loading || _pending) return;
        _loading = true; _catalogLoaded = false; _catalogFailed = false; _catalogMessage = null;
        ClearCatalog(); _catalogTotal = 0; _selected = null;
        FilterItems(); RefreshStatus();
        try
        {
            for (int offset = 0; offset < CatalogCap;)
            {
                _pageReceived = false; _pageCount = 0;
                await _send(new Request { Kind = "getItemCatalog", CatalogOffset = offset, CatalogLimit = PageSize }, null);
                if (!_connected || _catalogFailed) break;
                if (!_pageReceived) { SetCatalogError("插件没有返回物品列表，请更新配套插件或稍后刷新。"); break; }
                int next = offset + _pageCount;
                if (_pageCount == 0 || next >= _catalogTotal || next >= CatalogCap) { _catalogLoaded = true; break; }
                offset = next;
            }
            if (_catalogTotal > CatalogCap) _catalogMessage = $"物品列表超过 {CatalogCap} 项，已停止继续加载。";
        }
        finally
        {
            _loading = false;
            FilterItems(); RefreshStatus();
        }
    }

    private Task SetRewardAsync(bool enabled) => SetRewardEnabledAsync(enabled, false);
    private Task SetUpgradeAsync(bool enabled) => SetRewardEnabledAsync(enabled, true);

    private async Task SetRewardEnabledAsync(bool enabled, bool upgrade)
    {
        bool supported = upgrade ? _state.UpgradeSupported : _state.Supported;
        if (_pending || !_connected || _blocked || (enabled && (!_playerReady || !supported || _selected is null || _selected.UpgradeAllowed == false))) return;
        string? id = enabled ? _selected!.Id : null;
        string? name = enabled ? DisplayName(_selected!) : null;
        _operationError = _operationErrorScope = null; _pending = true; RefreshStatus();
        try
        {
            var request = new Request { Kind = "setChestReward", ItemId = id };
            if (upgrade) request.UpgradeEnabled = enabled; else request.Enabled = enabled;
            string message = upgrade
                ? enabled ? "已提交升级指定选项：" + name : "已关闭升级指定选项，后续升级恢复游戏原有规则。"
                : enabled ? "已提交箱子指定奖励：" + name : "已关闭箱子指定奖励，后续箱子恢复游戏原有规则。";
            await _send(request, message);
        }
        finally { _pending = false; RefreshStatus(); }
    }

    private async Task ApplyTargetAsync()
    {
        if (_pending || !_connected || _blocked || !_playerReady || _selected is null || !HasEnabledReward || (_state.UpgradeEnabled && _selected.UpgradeAllowed == false)) return;
        _operationError = _operationErrorScope = null; _pending = true; RefreshStatus();
        try { await _send(new Request { Kind = "setChestReward", ItemId = _selected.Id }, "已提交共享目标：" + DisplayName(_selected)); }
        finally { _pending = false; RefreshStatus(); }
    }

    private async Task ApplyChestOnlyAsync()
    {
        if (_pending || !_connected || _blocked || !_playerReady || !_state.Supported || _selected?.UpgradeAllowed != false) return;
        _operationError = _operationErrorScope = null; _pending = true; RefreshStatus();
        try
        {
            await _send(new Request { Kind = "setChestReward", Enabled = true, UpgradeEnabled = false, ItemId = _selected.Id },
                "已提交仅应用到箱子：" + DisplayName(_selected) + "；同时关闭升级指定，后续升级使用原有选项。");
        }
        finally { _pending = false; RefreshStatus(); }
    }

    private async Task DisableBothAsync()
    {
        if (_pending || !_connected || _blocked) return;
        _operationError = _operationErrorScope = null; _pending = true; RefreshStatus();
        try { await _send(new Request { Kind = "setChestReward", Enabled = false, UpgradeEnabled = false }, "箱子与升级指定均已关闭，后续恢复游戏原有随机规则。"); }
        finally { _pending = false; RefreshStatus(); }
    }

    private void FilterItems()
    {
        string? selectedId = _selected?.Id ?? _state.TargetItemId;
        string query = _search.Text.Trim();
        string? kind = _kind.SelectedIndex == 1 ? "weapon" : _kind.SelectedIndex == 2 ? "passive" : null;
        _filtering = true; _items.BeginUpdate();
        try
        {
            _items.SelectedIndices.Clear(); _selected = null;
            // Reset the count before replacing the backing array, including
            // when two queries have the same number of different matches.
            _items.VirtualListSize = 0;
            _filteredItems = _catalog.Where(item => (kind is null || item.Kind == kind) && (query.Length == 0 || (item.Name + " " + item.EnglishName + " " + item.Id).Contains(query, StringComparison.OrdinalIgnoreCase))).ToArray();
            _items.VirtualListSize = _filteredItems.Length;
            int selectedIndex = Array.FindIndex(_filteredItems, item => item.Id.Equals(selectedId, StringComparison.Ordinal));
            if (selectedIndex >= 0)
            {
                _items.SelectedIndices.Add(selectedIndex);
                _items.Items[selectedIndex].Focused = true;
                _selected = _filteredItems[selectedIndex];
            }
        }
        finally { _items.EndUpdate(); _filtering = false; }
        RefreshStatus();
    }

    private ListViewItem GetRow(int index)
    {
        if ((uint)index >= (uint)_filteredItems.Length) return new ListViewItem("");
        var item = _filteredItems[index];
        // Resetting VirtualListSize can request an old top row while the view
        // changes. Never reuse that row for a newer definition with the same ID.
        if (_rowCache.TryGetValue(item.Id, out var row) && ReferenceEquals(row.Tag, item)) return row;
        row = new ListViewItem(DisplayName(item)) { Tag = item, ImageIndex = _iconIndices.GetValueOrDefault(item.Id) };
        row.SubItems.Add(EnglishName(item)); row.SubItems.Add(UiLocalization.Translate(ItemKind(item)));
        row.SubItems.Add(UiLocalization.Translate(Applicability(item))); row.SubItems.Add(item.Id); row.SubItems.Add(item.MaxLevel > 0 ? item.MaxLevel.ToString() : "—");
        row.ToolTipText = DisplayName(item) + "\n" + EnglishName(item) + "\n" + item.Id + "\n" + UiLocalization.Translate(ApplicabilityHelp(item));
        _rowCache[item.Id] = row;
        return row;
    }

    private void UpdateSelection()
    {
        if (_filtering) return;
        int index = _items.SelectedIndices.Count == 1 ? _items.SelectedIndices[0] : -1;
        _selected = (uint)index < (uint)_filteredItems.Length ? _filteredItems[index] : null;
        RefreshStatus();
    }

    private void FindRow(SearchForVirtualItemEventArgs e)
    {
        if (!e.IsTextSearch || e.Text is null || _filteredItems.Length == 0) return;
        bool Matches(string text) => e.IsPrefixSearch
            ? text.StartsWith(e.Text, StringComparison.CurrentCultureIgnoreCase)
            : text.Equals(e.Text, StringComparison.CurrentCultureIgnoreCase);
        int start = e.StartIndex < 0 || e.StartIndex >= _filteredItems.Length ? 0 : e.StartIndex;
        for (int offset = 0; offset < _filteredItems.Length; offset++)
        {
            int index = (start + offset) % _filteredItems.Length;
            var item = _filteredItems[index];
            if (Matches(DisplayName(item)) || (e.IncludeSubItemsInSearch && (Matches(EnglishName(item)) || Matches(UiLocalization.Translate(ItemKind(item)))
                || Matches(UiLocalization.Translate(Applicability(item))) || Matches(item.Id) || Matches(item.MaxLevel > 0 ? item.MaxLevel.ToString() : "—"))))
            { e.Index = index; return; }
        }
    }

    private void RefreshStatus()
    {
        bool ready = _connected && !_blocked && !_loading && !_pending;
        bool targetChanged = _selected is not null && _selected.Id != _state.TargetItemId;
        bool upgradeUnadapted = _selected?.UpgradeAllowed == false;
        _refresh.Enabled = ready && CatalogSupported;
        _search.Enabled = _catalog.Count > 0 && !_loading; _kind.Enabled = _catalog.Count > 0 && !_loading; _items.Enabled = !_loading && !_pending;
        _applyTarget.Enabled = ready && _playerReady && HasEnabledReward && targetChanged && !(_state.UpgradeEnabled && upgradeUnadapted);
        _enable.Enabled = ready && (_state.Enabled || (_state.Supported && _playerReady && _selected is not null && !upgradeUnadapted));
        UiLocalization.SetText(_enable, _state.Enabled ? "关闭箱子指定" : "开启箱子指定");
        _enable.BackColor = _state.Enabled ? Color.FromArgb(34, 97, 79) : Raised; _enable.ForeColor = _state.Enabled ? Accent : Primary;
        _upgrade.Enabled = ready && (_state.UpgradeEnabled || (_state.UpgradeSupported && _playerReady && _selected is not null && !upgradeUnadapted));
        UiLocalization.SetText(_upgrade, _state.UpgradeEnabled ? "关闭升级指定" : "开启升级指定");
        _upgrade.BackColor = _state.UpgradeEnabled ? Color.FromArgb(34, 97, 79) : Raised; _upgrade.ForeColor = _state.UpgradeEnabled ? Accent : Primary;
        _chestOnly.Visible = upgradeUnadapted;
        if (_chestOnly.Parent is TableLayoutPanel actions) actions.ColumnStyles[3].Width = upgradeUnadapted ? 234 : 0;
        _chestOnly.Enabled = ready && _playerReady && _state.Supported && upgradeUnadapted;
        UiLocalization.SetText(_chestOnly, _state.UpgradeEnabled ? "仅应用到箱子（关闭升级）" : "仅应用到箱子");
        _disable.Enabled = ready && HasEnabledReward;
        string selectionSource = _selected is null ? "当前选择：请从上方列表选择物品" : "当前选择：" + DisplayName(_selected) + "\n" + EnglishName(_selected) + " · " + _selected.Id;
        UiLocalization.SetText(_selection, selectionSource);
        _selection.ForeColor = _selected is null ? Secondary : Primary;
        _tips.SetToolTip(_selection, _selected is null ? "选择不会自动开启，确认后分别开启箱子或升级指定。" : selectionSource + "\n" + ApplicabilityHelp(_selected) + "\n选择不会自动生效。两项共用目标，点击应用后才会提交。" + (string.IsNullOrWhiteSpace(_selected.Description) ? "" : "\n" + _selected.Description));
        var activeItem = FindCatalogItem(_state.TargetItemId);
        string? activeName = RewardName(_state.TargetItemId, _state.TargetItemName);
        string activeDetail = activeName + (activeItem is null || EnglishName(activeItem) == "—" || EnglishName(activeItem) == activeName ? "" : " / " + EnglishName(activeItem)) + "\n" + _state.TargetItemId;
        string activeTargetSource = !_connected ? "实际生效目标：未连接" : HasEnabledReward ? "实际生效目标：" + activeDetail : "实际生效目标：无（箱子和升级均关闭）" + (string.IsNullOrWhiteSpace(_state.TargetItemId) ? "" : "\n上次目标：" + activeName + " · " + _state.TargetItemId);
        UiLocalization.SetText(_activeTarget, activeTargetSource);
        _activeTarget.ForeColor = HasEnabledReward ? Accent : Secondary;
        _tips.SetToolTip(_activeTarget, activeTargetSource + "\n以插件返回状态为准；更改列表选择不会自动改变实际目标。");
        _tips.SetToolTip(_applyTarget, _state.UpgradeEnabled && upgradeUnadapted ? "当前连接的插件不支持所选目标的升级指定，不能同时应用到两项。可更新配套插件后刷新；若接受关闭升级指定，请点击“仅应用到箱子（关闭升级）”。" : "将当前选择应用到所有已开启的指定奖励；不自动改变开关。");
        _tips.SetToolTip(_enable, !_state.Enabled && upgradeUnadapted ? "当前插件未开放此目标的升级指定。可更新配套插件后刷新，或使用“仅应用到箱子”明确提交。" : "独立开启或关闭箱子指定；关闭不会修改升级开关。");
        _tips.SetToolTip(_upgrade, !_state.UpgradeEnabled && upgradeUnadapted ? ApplicabilityHelp(_selected!) : "独立开启或关闭升级指定；关闭始终可用，实际状态由插件确认。");
        _tips.SetToolTip(_chestOnly, "开启当前目标的箱子指定，并明确关闭升级指定。之后升级使用游戏原有选项，不会自动重开。");
        string operationHintSource = _operationError is not null && _connected
            ? (_operationErrorScope switch { "chest" => "箱子指定请求未执行：", "upgrade" => "升级指定请求未执行：", "rewardTarget" => "共享目标请求未执行：", _ => "指定奖励请求未执行：" }) + _operationError + (HasEnabledReward ? "；本次未更改，原目标仍生效。" : "；本次未更改，功能仍关闭。")
            : upgradeUnadapted ? ApplicabilityHelp(_selected!) + " 点击“仅应用到箱子”将开启箱子指定并关闭升级指定。"
            : _selected?.UpgradeAllowed is null && _selected is not null ? ApplicabilityHelp(_selected)
            : "选择与实际目标分别显示；只有插件确认成功后才会改变目标和开关。";
        _operationHint.ForeColor = _operationError is not null || upgradeUnadapted ? Warning : Secondary;
        string summarySource = !_connected ? "连接游戏后读取本机游戏的物品列表" : !CatalogSupported ? "指定奖励接口尚未就绪，请查看下方状态。" : _loading ? $"正在读取物品列表… 已读 {_catalog.Count} 项" : _catalogMessage ?? (_catalog.Count == 0 ? "尚无物品列表；进入关卡后可尝试刷新。" : $"显示 {_filteredItems.Length} / {_catalog.Count} 项 · 来源：当前游戏数据");
        string statusSource = !_connected ? "● 箱子指定关闭 · 未连接" : _state.Enabled ? "● 箱子指定已开启 · 目标：" + activeName : !_state.Supported ? "● 箱子指定不可用 · " + (_state.Message ?? "插件尚未提供箱子指定奖励，请更新配套插件。")
            : !_playerReady ? "● 箱子指定关闭 · 进入单人关卡后可开启" : "● 箱子指定关闭 · 使用游戏原有奖励规则";
        if (_connected && _state.Supported && !string.IsNullOrWhiteSpace(_state.Message)) statusSource += " · " + _state.Message;
        if (_pending) statusSource += " · 正在提交，实际状态尚未改变";
        _status.ForeColor = _state.Enabled ? Accent : _connected && !_state.Supported ? Warning : Secondary;
        string lastRewardSource = _connected && _state.ProcessedCount > 0 ? $"箱子指定奖励 {_state.ProcessedCount} 次 · 最近：{RewardName(_state.LastRewardItemId, _state.LastRewardName) ?? "等待记录"}" : "尚未收到实际箱子指定奖励记录";
        _lastReward.ForeColor = Secondary;
        string upgradeStatusSource = !_connected ? "● 升级指定关闭 · 未连接" : _state.UpgradeEnabled ? "● 升级指定已开启 · 选项目标：" + activeName : !_state.UpgradeSupported ? "● 升级指定不可用 · " + (_state.UpgradeMessage ?? "当前插件未提供此功能。")
            : !_playerReady ? "● 升级指定关闭 · 进入单人关卡后可开启" : "● 升级指定关闭 · 使用游戏原有选项";
        if (_connected && _state.UpgradeSupported && !string.IsNullOrWhiteSpace(_state.UpgradeMessage)) upgradeStatusSource += " · " + _state.UpgradeMessage;
        if (_pending) upgradeStatusSource += " · 正在提交，实际状态尚未改变";
        _upgradeStatus.ForeColor = _state.UpgradeEnabled ? Accent : _connected && !_state.UpgradeSupported ? Warning : Secondary;
        string lastUpgradeSource = _connected && _state.UpgradeProcessedCount > 0 ? $"已生成指定选项 {_state.UpgradeProcessedCount} 次 · 最近：{RewardName(_state.LastUpgradeItemId, _state.LastUpgradeName) ?? "等待记录"}（仍需手动选择）" : "尚未生成指定升级选项；出现选项不等于已经获得";
        _lastUpgrade.ForeColor = Secondary;
        if (_previewMode)
        {
            _refresh.Enabled = _enable.Enabled = _upgrade.Enabled = _applyTarget.Enabled = _chestOnly.Enabled = _disable.Enabled = false;
            summarySource = "示例数据 · 实际物品列表由连接后的游戏提供";
            operationHintSource = "离线界面示意，仅用于查看布局，不执行游戏操作。";
            statusSource = "● 箱子指定关闭 · 离线界面示意，不执行游戏操作";
            lastRewardSource = "尚未实测；此图不代表指定奖励已经在游戏内生效";
            upgradeStatusSource = "● 升级指定关闭 · 离线界面示意，不执行游戏操作";
            lastUpgradeSource = "示例列表不代表已生成或已获得真实升级选项";
        }
        // Build complete source strings first, then compare their final localized
        // text once. A stable heartbeat must not expose a Chinese intermediate.
        UiLocalization.SetText(_operationHint, operationHintSource);
        UiLocalization.SetText(_summary, summarySource);
        UiLocalization.SetText(_status, statusSource);
        UiLocalization.SetText(_lastReward, lastRewardSource);
        UiLocalization.SetText(_upgradeStatus, upgradeStatusSource);
        UiLocalization.SetText(_lastUpgrade, lastUpgradeSource);
        _tips.SetToolTip(_operationHint, operationHintSource);
        _tips.SetToolTip(_status, statusSource);
        _tips.SetToolTip(_lastReward, lastRewardSource);
        _tips.SetToolTip(_upgradeStatus, upgradeStatusSource);
        _tips.SetToolTip(_lastUpgrade, lastUpgradeSource);
    }

    private static string DisplayName(RewardItemDefinition item)
    {
        string? name = UiLocalization.IsEnglish ? item.EnglishName : item.Name;
        return string.IsNullOrWhiteSpace(name) ? item.Id : name;
    }
    private static string EnglishName(RewardItemDefinition item) => UiLocalization.IsEnglish
        ? string.IsNullOrWhiteSpace(item.Name) ? item.Id : item.Name
        : string.IsNullOrWhiteSpace(item.EnglishName) ? "—" : item.EnglishName;
    private string? RewardName(string? id, string? fallback)
    {
        var item = FindCatalogItem(id);
        if (item is not null) return DisplayName(item);
        if (UiLocalization.IsEnglish && !string.IsNullOrWhiteSpace(id)) return id;
        return !string.IsNullOrWhiteSpace(fallback) ? fallback : id;
    }
    private static string ItemKind(RewardItemDefinition item) => item.IsEvolution == true ? item.Kind == "passive" ? "进化被动" : "进化武器" : item.Kind == "passive" ? "被动装备" : "武器";
    private static string Applicability(RewardItemDefinition item) => item.UpgradeAllowed switch { true => "宝箱 / 升级", false => "仅宝箱（升级暂不可用）", null => "未提供" };
    private static string ApplicabilityHelp(RewardItemDefinition item) => item.UpgradeAllowed switch
    {
        true => "已适配宝箱和升级选项入口；满级等本局条件仍由插件校验，实际取得以结算记录为准。",
        false => "当前连接的插件版本不支持此物品的升级指定；请更新配套插件后刷新列表。" + (string.IsNullOrWhiteSpace(item.UpgradeUnsupportedReason) ? "" : " 插件说明：" + item.UpgradeUnsupportedReason),
        null => "适用范围未提供；英文名称和适用范围需新版配套插件支持，可刷新列表或更新插件。实际能否指定仍以游戏验证为准。"
    };
    private static Label TextLabel(string text, Color color) => new() { Text = text, ForeColor = color, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft };
    private static void StyleButton(Button button)
    {
        button.FlatStyle = FlatStyle.Flat; button.FlatAppearance.BorderColor = Color.FromArgb(63, 76, 94); button.FlatAppearance.BorderSize = 1;
        button.BackColor = Raised; button.ForeColor = Primary; button.Cursor = Cursors.Hand; button.UseVisualStyleBackColor = false;
    }
}
