using System.Diagnostics;
using System.Globalization;
using Trainer.Core;
using Trainer.Shared;

namespace Trainer.Desktop;

internal sealed class MainForm : Form
{
    private static readonly Color Background = Color.FromArgb(18, 22, 28);
    private static readonly Color Surface = Color.FromArgb(28, 34, 43);
    private static readonly Color SurfaceRaised = Color.FromArgb(39, 47, 58);
    private static readonly Color TextPrimary = Color.FromArgb(233, 238, 246);
    private static readonly Color TextSecondary = Color.FromArgb(155, 169, 189);
    private static readonly Color Accent = Color.FromArgb(91, 211, 170);
    private static readonly Color Warning = Color.FromArgb(240, 190, 101);

    private readonly DesktopSettings _settings;
    private readonly GamePaths _paths;
    private readonly BackupService _backups;
    private readonly InstallationService _installation;
    private readonly TrainerClient _client = new();
    private readonly HotkeyService _hotkeys = new();
    private readonly CancellationTokenSource _lifetime = new();
    private readonly ComboBox _languagePicker = new() { Name = "LanguageSelector" };
    private readonly List<string> _logSources = new();
    private bool _changingLanguage;
    private readonly System.Windows.Forms.Timer _pollTimer = new() { Interval = 500 };
    private readonly System.Windows.Forms.Timer _keyTimer = new() { Interval = 30 };
    private readonly Dictionary<string, FeatureRow> _rows = new();
    private readonly Dictionary<string, FeatureState> _states = new();
    private readonly HashSet<string> _pending = new();
    private readonly List<Control> _maintenance = new();
    private readonly TextBox _gamePath = new();
    private readonly Label _connection = new();
    private readonly Label _details = new();
    private readonly Label _backupStatus = new();
    private readonly Label _goldLabel = new();
    private readonly NumericUpDown _goldAmount = new();
    private readonly Button _readGold = new DarkButton();
    private readonly Button _setGold = new DarkButton();
    private readonly Button _disableAll = new DarkButton();
    private readonly TextBox _log = new();
    private readonly Label _footer = new();
    private readonly Label _setupStatus = new();
    private Button _primaryAction = null!;
    private Button _maintenanceToggle = null!;
    private TableLayoutPanel _rootLayout = null!;
    private TableLayoutPanel _maintenancePanel = null!;
    private TableLayoutPanel _goldArea = null!;
    private ColumnStyle _featureHotkeyHeader = null!;
    private bool _interactiveResize;
    private TrainerState? _resizeState;
    private readonly LocalizedToolTip _tips = new() { AutoPopDelay = 15000, InitialDelay = 400, ReshowDelay = 100 };
    private readonly TabControl _tabs = new ResponsiveTabControl();
    private StatEditorPanel _statEditor = null!;
    private ChestRewardPanel _chestRewards = null!;
    private TrainerState? _lastState;
    private readonly bool _diagnosticsMode;
    private bool _polling, _busy, _closing, _updating, _requestInFlight;
    private int _stateGeneration;
    private bool _connected, _backupAuthorized, _playerReady;
    private bool _closeAfterMaintenance;
    private int? _gamePid;
    private string? _backupToken, _capturing;
    private string _lastPollMessage = "";
    private string _lastValidatedPath = "";
    private string? _versionError;
    private GameDiscoveryResult? _discovery;
    private DateTime _lastDiscoveryUtc, _lastConnectionAttemptUtc, _launchRequestedUntil;
    private string _lastFingerprintStamp = "";
    private int? _validatedPid;
    private bool _maintenanceExpanded, _pluginPresent, _launchRequested, _launchObservedProcess;
    private bool _primaryActionInFlight, _lastDiscoverySucceeded;
    private Task<bool>? _discoveryTask;
    private Task _pollIdle = Task.CompletedTask;
    private string _lastDiscoveryPath = "";
    protected override bool ShowWithoutActivation => _diagnosticsMode || base.ShowWithoutActivation;

    public MainForm(bool diagnosticsMode = false)
    {
        _diagnosticsMode = diagnosticsMode;
        _paths = diagnosticsMode
            ? new GamePaths(Path.GetFullPath(Path.Combine("preview-fixtures", "profile")), Array.Empty<string>())
            : new GamePaths();
        _settings = diagnosticsMode ? new DesktopSettings { Language = UiLocalization.Language } : DesktopSettings.Load(RuntimePaths.SettingsPath);
        UiLocalization.SetLanguage(_settings.Language);
        _backups = new BackupService(_paths, RuntimePaths.DataDirectory);
        _installation = new InstallationService(_paths, RuntimePaths.DataDirectory);
        Text = "吸血鬼幸存者 · 实时修改器";
        Icon = AppBranding.CreateIcon();
        Font = new Font("Microsoft YaHei UI", 9F);
        AutoScaleMode = AutoScaleMode.Dpi;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(1180, 940);
        MinimumSize = new Size(1060, 800);
        BackColor = Background; ForeColor = TextPrimary;
        DoubleBuffered = true;
        KeyPreview = true;
        SuspendLayout();
        BuildLayout();
        RefreshHotkeyLabels();
        ResponsiveLayout.Attach(this);
        UiLocalization.Attach(this);
        ResumeLayout(true);
        UiLocalization.LanguageChanged += OnLanguageChanged;
        Disposed += (_, _) => { UiLocalization.LanguageChanged -= OnLanguageChanged; _tips.Dispose(); };
        _gamePath.Text = _settings.GameDirectory;
        _pollTimer.Tick += async (_, _) => await PollAsync();
        _keyTimer.Tick += async (_, _) =>
        {
            foreach (string id in _hotkeys.Poll(_settings.Hotkeys, _gamePid, _connected && !_busy && !_requestInFlight && _capturing is null))
                await ActivateHotkeyAsync(id);
        };
        KeyDown += CaptureKey;
        FormClosing += OnClosing;
        Shown += async (_, _) =>
        {
            if (_diagnosticsMode)
            {
                Log("界面检查模式：不连接游戏，不执行存档或安装操作。");
                UiLocalization.SetText(_backupStatus, "界面检查模式 · 不连接游戏、不执行存档操作");
                foreach (var control in _maintenance) control.Enabled = false;
                return;
            }
            Log("界面已就绪。功能默认为关闭；只有插件确认支持后才允许开启。");
            await RefreshDiscoveryAsync(true);
            await RefreshBackupAsync();
            _pollTimer.Start(); _keyTimer.Start();
            await PollAsync();
        };
    }

    private void BuildLayout()
    {
        var root = _rootLayout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(24, 18, 24, 14), ColumnCount = 1, RowCount = 8, BackColor = Background };
        root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        foreach (float height in new float[] { 70, 82, -1, 66, 0, 86, 52, 30 })
            root.RowStyles.Add(height == 0 ? new RowStyle(SizeType.Percent, 100) : new RowStyle(SizeType.Absolute, Math.Max(0, height)));
        Controls.Add(root);

        var heading = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, RowCount = 1, Margin = Padding.Empty };
        heading.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        heading.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 64));
        heading.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        heading.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 280));
        var brand = new PictureBox { Image = AppBranding.CreateMark(56), SizeMode = PictureBoxSizeMode.CenterImage, Dock = DockStyle.Fill, Margin = new Padding(0, 0, 8, 8) };
        brand.Disposed += (_, _) => brand.Image?.Dispose();
        heading.Controls.Add(brand, 0, 0);
        var headingText = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, WrapContents = false, Dock = DockStyle.Fill, Margin = Padding.Empty };
        headingText.Controls.Add(Label("吸血鬼幸存者", new Font(Font.FontFamily, 21, FontStyle.Bold), TextPrimary));
        headingText.Controls.Add(Label("VAMPIRE SURVIVORS  /  实时修改器", new Font(Font.FontFamily, 8), TextSecondary));
        heading.Controls.Add(headingText, 1, 0);
        var version = Label($"{ReleaseInfo.Version}  ·  {ReleaseInfo.Author}\n单人模式 · Windows x64", Font, TextSecondary);
        version.AutoSize = false; version.AutoEllipsis = true; version.TextAlign = ContentAlignment.MiddleRight; version.Dock = DockStyle.Fill;
        var languageArea = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2, Margin = Padding.Empty };
        languageArea.RowStyles.Add(new RowStyle(SizeType.Absolute, 36)); languageArea.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        languageArea.Controls.Add(version, 0, 0);
        var languageLine = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.RightToLeft, WrapContents = false, Margin = Padding.Empty };
        _languagePicker.DropDownStyle = ComboBoxStyle.DropDownList; _languagePicker.Width = 116;
        _languagePicker.Items.AddRange(new object[] { "简体中文", "English" }); _languagePicker.SelectedIndex = UiLocalization.IsEnglish ? 1 : 0;
        _languagePicker.BackColor = SurfaceRaised; _languagePicker.ForeColor = TextPrimary; _languagePicker.Margin = new Padding(4, 2, 0, 0);
        _languagePicker.SelectedIndexChanged += (_, _) =>
        {
            if (_changingLanguage) return;
            _settings.Language = _languagePicker.SelectedIndex == 1 ? "en" : "zh-CN";
            UiLocalization.SetLanguage(_settings.Language);
            if (!_diagnosticsMode) SaveSettings();
        };
        languageLine.Controls.Add(_languagePicker);
        languageLine.Controls.Add(new Label { Text = "Language", AutoSize = true, ForeColor = TextSecondary, Margin = new Padding(0, 5, 0, 0) });
        languageArea.Controls.Add(languageLine, 0, 1);
        heading.Controls.Add(languageArea, 2, 0); root.Controls.Add(heading, 0, 0);

        _maintenancePanel = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2, Margin = Padding.Empty, BackColor = Background };
        _maintenancePanel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        _maintenancePanel.RowStyles.Add(new RowStyle(SizeType.Absolute, 44));
        _maintenancePanel.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
        _maintenancePanel.Visible = false;

        var pathLine = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 4, RowCount = 1, Margin = new Padding(0, 4, 0, 4) };
        pathLine.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        pathLine.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 76));
        pathLine.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        pathLine.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 72));
        pathLine.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 92));
        pathLine.Controls.Add(Label("游戏位置", Font, TextSecondary, true), 0, 0);
        StyleInput(_gamePath); _gamePath.Name = "GameDirectory"; _gamePath.ReadOnly = true; _gamePath.PlaceholderText = "自动识别运行中的游戏或 Steam 安装位置；仅未找到时需要手选";
        _gamePath.Dock = DockStyle.Fill; _gamePath.Margin = new Padding(0, 4, 8, 0);
        pathLine.Controls.Add(_gamePath, 1, 0);
        pathLine.Controls.Add(ActionButton("更改…", BrowseGameAsync), 2, 0);
        pathLine.Controls.Add(ActionButton("重新检测", DetectGameAsync), 3, 0);
        _maintenancePanel.Controls.Add(pathLine, 0, 0);

        var preparation = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 4, RowCount = 1, Margin = new Padding(0, 3, 0, 5), BackColor = Surface, Padding = new Padding(10, 10, 10, 10) };
        preparation.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 156));
        preparation.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        preparation.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 124));
        preparation.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 116));
        _primaryAction = ActionButton("正在识别…", PrimaryActionAsync, 148, true, maintenance: false);
        _primaryAction.Dock = DockStyle.Fill;
        preparation.Controls.Add(_primaryAction, 0, 0);
        UiLocalization.SetText(_setupStatus, "自动识别运行中的游戏与 Steam 安装位置，无需先选目录。\n首次使用只需点一次，自动备份进度、准备插件并启动游戏。");
        _setupStatus.Dock = DockStyle.Fill; _setupStatus.TextAlign = ContentAlignment.MiddleLeft;
        _setupStatus.ForeColor = TextSecondary; _setupStatus.AutoEllipsis = true; _setupStatus.Margin = new Padding(8, 0, 6, 0);
        preparation.Controls.Add(_setupStatus, 1, 0);
        _maintenanceToggle = ActionButton("维护与备份 ▾", ToggleMaintenanceAsync);
        preparation.Controls.Add(_maintenanceToggle, 2, 0);
        preparation.Controls.Add(ActionButton("说明与来源", ShowReleaseInfoAsync), 3, 0);
        root.Controls.Add(preparation, 0, 1);

        var tools = new FlowLayoutPanel { Dock = DockStyle.Fill, Margin = Padding.Empty, WrapContents = false };
        tools.Controls.Add(ActionButton("备份存档", CreateBackupAsync, 104));
        tools.Controls.Add(ActionButton("安装插件", InstallAsync, 104));
        tools.Controls.Add(ActionButton("卸载插件", UninstallAsync, 104));
        tools.Controls.Add(ActionButton("恢复备份…", RestoreAsync, 110));
        tools.Controls.Add(ActionButton("打开备份目录", OpenBackupDirectoryAsync, 130));
        UiLocalization.SetText(_disableAll, "关闭全部"); StyleButton(_disableAll); _disableAll.Width = 104; _disableAll.Enabled = false;
        _disableAll.Click += async (_, _) => await ExecuteRequestAsync(new Request { Kind = "disableAll" }, "已关闭全部持续效果、属性覆盖和指定奖励。");
        _maintenancePanel.Controls.Add(tools, 0, 1); root.Controls.Add(_maintenancePanel, 0, 2);

        var statusPanel = new TableLayoutPanel { Dock = DockStyle.Fill, BackColor = Surface, Padding = new Padding(12, 8, 12, 6), ColumnCount = 3, RowCount = 2, Margin = new Padding(0, 2, 0, 4) };
        statusPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 175)); statusPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100)); statusPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 112));
        statusPanel.RowStyles.Add(new RowStyle(SizeType.Percent, 50)); statusPanel.RowStyles.Add(new RowStyle(SizeType.Percent, 50));
        UiLocalization.SetText(_connection, "●  未连接游戏"); _connection.AutoEllipsis = true; _connection.ForeColor = Warning; _connection.Dock = DockStyle.Fill; _connection.TextAlign = ContentAlignment.MiddleLeft;
        UiLocalization.SetText(_details, "识别位置和运行状态后，会自动连接已准备的游戏"); _details.ForeColor = TextSecondary; _details.Dock = DockStyle.Fill; _details.TextAlign = ContentAlignment.MiddleLeft; _details.AutoEllipsis = true;
        UiLocalization.SetText(_backupStatus, "备份：正在检查"); _backupStatus.ForeColor = TextSecondary; _backupStatus.Dock = DockStyle.Fill; _backupStatus.AutoEllipsis = true; _backupStatus.TextAlign = ContentAlignment.MiddleLeft;
        statusPanel.Controls.Add(_connection, 0, 0); statusPanel.Controls.Add(_details, 1, 0); statusPanel.Controls.Add(_backupStatus, 0, 1); statusPanel.SetColumnSpan(_backupStatus, 2);
        _disableAll.Dock = DockStyle.Fill; _disableAll.Margin = new Padding(8, 2, 0, 2);
        statusPanel.Controls.Add(_disableAll, 2, 0); statusPanel.SetRowSpan(_disableAll, 2);
        root.Controls.Add(statusPanel, 0, 3);

        _tabs.Dock = DockStyle.Fill; _tabs.Margin = Padding.Empty; _tabs.Padding = new Point(16, 6);
        var featureTab = new TabPage("常用功能") { BackColor = Background, Padding = new Padding(8, 4, 8, 4) };
        var statTab = new TabPage("属性编辑") { BackColor = Background, Padding = Padding.Empty };
        var chestTab = new TabPage("指定奖励") { BackColor = Background, Padding = Padding.Empty };
        _tabs.TabPages.Add(featureTab); _tabs.TabPages.Add(statTab); _tabs.TabPages.Add(chestTab); root.Controls.Add(_tabs, 0, 4);
        _statEditor = new StatEditorPanel(ExecuteRequestAsync, id => BeginCapture(HotkeyService.StatAction(id)), id => HotkeyLabel(HotkeyService.StatAction(id))); statTab.Controls.Add(_statEditor);
        _chestRewards = new ChestRewardPanel(ExecuteRequestAsync, FetchRewardIconAsync); chestTab.Controls.Add(_chestRewards);
        var featureArea = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2, Margin = Padding.Empty };
        featureArea.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        featureArea.RowStyles.Add(new RowStyle(SizeType.Absolute, 28)); featureArea.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        featureTab.Controls.Add(featureArea);
        var columnHeading = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 4, RowCount = 1, Margin = Padding.Empty };
        columnHeading.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        _featureHotkeyHeader = new ColumnStyle(SizeType.Absolute, 88);
        columnHeading.ColumnStyles.Add(_featureHotkeyHeader); columnHeading.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        columnHeading.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 270)); columnHeading.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 100));
        columnHeading.Controls.Add(Label("快捷键", new Font(Font.FontFamily, 8), TextSecondary, true), 0, 0);
        columnHeading.Controls.Add(Label("功能与实际状态", new Font(Font.FontFamily, 8), TextSecondary, true), 1, 0);
        columnHeading.Controls.Add(Label("倍率设置", new Font(Font.FontFamily, 8), TextSecondary, true), 2, 0);
        columnHeading.Controls.Add(Label("实时开关", new Font(Font.FontFamily, 8), TextSecondary, true), 3, 0);
        featureArea.Controls.Add(columnHeading, 0, 0);

        var features = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 8, Margin = Padding.Empty, AutoScroll = true };
        features.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        foreach (var definition in new[]
        {
            (FeatureIds.Health, "生命不减", "保护玩家免受普通伤害", false),
            (FeatureIds.InstantKill, "一击必杀", "敌人与首领统一规则", false),
            (FeatureIds.Experience, "经验倍率", "实际获得的经验 × 倍率", true),
            (FeatureIds.Damage, "伤害倍率", "玩家造成的伤害 × 倍率", true),
            (FeatureIds.Movement, "移动倍率", "正常移动速度 × 倍率", true),
            (FeatureIds.Cooldown, "快速冷却", "普通武器攻击冷却至多 0.1 秒", false),
            (FeatureIds.NoGoldSpend, "金币花费不减", "永久钱包消费不扣款；收入正常", false)
        })
        {
            features.RowStyles.Add(new RowStyle(SizeType.Absolute, 52));
            var row = new FeatureRow(definition.Item1, definition.Item2, definition.Item3);
            _rows.Add(row.Id, row);
            row.Panel = new FeatureRowPanel { Dock = DockStyle.Fill, ColumnCount = 4, RowCount = 1, BackColor = Surface, Padding = new Padding(6, 3, 6, 3), Margin = new Padding(0, 0, 0, 3), MinimumSize = new Size(0, 48) };
            row.Panel.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            row.Panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 80)); row.Panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            row.Panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 270)); row.Panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 94));
            UiLocalization.SetText(row.Hotkey, HotkeyLabel(row.Id)); StyleButton(row.Hotkey); row.Hotkey.Dock = DockStyle.Fill; row.Hotkey.Margin = new Padding(0, 4, 8, 4);
            row.Hotkey.Click += (_, _) => BeginCapture(row.Id); row.Panel.Controls.Add(row.Hotkey, 0, 0);
            row.Reason = new FeatureCaption(row.Name, new Font(Font.FontFamily, 10, FontStyle.Bold), TextPrimary)
                { Dock = DockStyle.Fill, Margin = Padding.Empty, Font = new Font(Font.FontFamily, 8), ForeColor = TextSecondary };
            UiLocalization.SetText(row.Reason, "未连接 · " + row.Description);
            row.Panel.Controls.Add(row.Reason, 1, 0);
            if (definition.Item4) row.Panel.AddRightControl(BuildMultiplier(row), 2);
            else row.Panel.AddRightControl(Label("—", Font, TextSecondary, true), 2);
            StyleButton(row.Toggle); UiLocalization.SetText(row.Toggle, "未连接"); row.Toggle.Dock = DockStyle.Fill; row.Toggle.Margin = new Padding(0, 4, 0, 4); row.Toggle.Enabled = false;
            row.Toggle.Click += async (_, _) => await ToggleFeatureAsync(row.Id); row.Panel.AddRightControl(row.Toggle, 3);
            features.Controls.Add(row.Panel, 0, features.Controls.Count);
        }
        // TableLayoutPanel gives the last row remaining space, including an
        // absolute row. A separate empty row keeps all seven feature rows equal.
        features.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        featureArea.Controls.Add(features, 0, 1);

        var gold = _goldArea = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 5, RowCount = 2, BackColor = Surface, Padding = new Padding(12, 8, 12, 8), Margin = new Padding(0, 4, 0, 4) };
        gold.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, UiLocalization.IsEnglish ? 150 : 115)); gold.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        gold.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 180)); gold.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 110)); gold.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 110));
        gold.RowStyles.Add(new RowStyle(SizeType.Absolute, 32)); gold.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        gold.Controls.Add(Label("永久金币", new Font(Font.FontFamily, 10, FontStyle.Bold), TextPrimary, true), 0, 0);
        UiLocalization.SetText(_goldLabel, "尚未读取"); _goldLabel.ForeColor = Accent; _goldLabel.Dock = DockStyle.Fill; _goldLabel.TextAlign = ContentAlignment.MiddleLeft;
        gold.Controls.Add(_goldLabel, 1, 0);
        var goldActions = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, RowCount = 1, Margin = Padding.Empty };
        goldActions.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 180)); goldActions.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 110)); goldActions.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 110));
        goldActions.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        gold.Controls.Add(goldActions, 2, 0); gold.SetColumnSpan(goldActions, 3);
        _goldAmount.Maximum = 999_999_999; _goldAmount.Minimum = 0; _goldAmount.Value = 999999; _goldAmount.ThousandsSeparator = true; _goldAmount.Dock = DockStyle.Fill; _goldAmount.Margin = new Padding(4, 2, 8, 2); StyleNumeric(_goldAmount);
        goldActions.Controls.Add(_goldAmount, 0, 0);
        UiLocalization.SetText(_readGold, "读取金币"); StyleButton(_readGold); _readGold.Dock = DockStyle.Fill; _readGold.Enabled = false;
        _readGold.Click += async (_, _) => await ExecuteRequestAsync(new Request { Kind = "getGold" }, "已读取游戏钱包。"); goldActions.Controls.Add(_readGold, 1, 0);
        UiLocalization.SetText(_setGold, "设置金币"); StyleButton(_setGold); _setGold.Dock = DockStyle.Fill; _setGold.Enabled = false;
        _setGold.Click += async (_, _) =>
        {
            if (LocalizedMessageBox.Show(this, $"将永久钱包设置为 {_goldAmount.Value:N0} 金币？\n此操作会保存到游戏进度中。", "设置永久金币", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) == DialogResult.OK)
                await ExecuteRequestAsync(new Request { Kind = "setGold", Value = (double)_goldAmount.Value, BackupToken = _backupToken }, "金币设置请求已由插件处理。");
        };
        goldActions.Controls.Add(_setGold, 2, 0);
        var goldNote = Label("金币操作需要本次启动前的有效备份。关闭金币不减不会回写旧余额。", new Font(Font.FontFamily, 8), TextSecondary, true);
        gold.Controls.Add(goldNote, 0, 1); gold.SetColumnSpan(goldNote, 5); root.Controls.Add(gold, 0, 5);

        _log.Name = "ActivityLog"; _log.Multiline = true; _log.ReadOnly = true; _log.ScrollBars = ScrollBars.Vertical; _log.BorderStyle = BorderStyle.None;
        _log.BackColor = Background; _log.ForeColor = TextSecondary; _log.Dock = DockStyle.Fill; _log.Font = new Font("Microsoft YaHei UI", 8);
        _log.Margin = new Padding(0, 6, 0, 0); root.Controls.Add(_log, 0, 6);
        UiLocalization.SetText(_footer, "快捷键仅在游戏前台生效 · 点击快捷键可改绑 · 断开连接后自动关闭持续效果");
        _footer.Font = new Font(Font.FontFamily, 8); _footer.ForeColor = TextSecondary; _footer.Dock = DockStyle.Fill; _footer.TextAlign = ContentAlignment.MiddleLeft;
        root.Controls.Add(_footer, 0, 7);
    }

    private Control BuildMultiplier(FeatureRow row)
    {
        var box = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false, Margin = Padding.Empty, Padding = new Padding(0, 7, 0, 0) };
        row.MultiplierPanel = box;
        box.Enabled = false;
        decimal[] presets = row.Id == FeatureIds.Movement ? [1, 2, 3] : [1, 10, 100];
        row.Number = new NumericUpDown { DecimalPlaces = row.Id == FeatureIds.Movement ? 2 : 1, Minimum = row.Id == FeatureIds.Movement ? .25m : 1m, Maximum = row.Id == FeatureIds.Movement ? 10 : 1000, Width = 83, Value = _settings.Values[row.Id], Margin = new Padding(0, 2, 3, 0) };
        StyleNumeric(row.Number); box.Controls.Add(row.Number);
        box.Controls.Add(Label("×", Font, TextSecondary));
        foreach (decimal value in presets)
        {
            var button = new DarkButton { Text = value.ToString(CultureInfo.InvariantCulture) + "×", Width = value >= 100 ? 49 : 43, Height = 27, Margin = new Padding(3, 0, 0, 0), TabStop = false };
            StyleButton(button); button.Click += (_, _) => row.Number.Value = value; box.Controls.Add(button);
        }
        row.Number.ValueChanged += async (_, _) =>
        {
            if (_updating) return;
            _settings.Values[row.Id] = row.Number.Value; SaveSettings();
            if (_states.TryGetValue(row.Id, out var state) && state.Enabled && !_pending.Contains(row.Id))
                await SetFeatureAsync(row, true);
        };
        return box;
    }

    private async Task PollAsync(bool fromPrimaryAction = false)
    {
        if (_diagnosticsMode || _polling || _busy || _closing || _requestInFlight || (_primaryActionInFlight && !fromPrimaryAction)) return;
        _polling = true;
        var completed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        _pollIdle = completed.Task;
        int generation = _stateGeneration;
        try
        {
            if (!await RefreshDiscoveryAsync()) return;
            if (_closing || _busy || _requestInFlight || (_primaryActionInFlight && !fromPrimaryAction)) return;
            generation = _stateGeneration;
            if (_discovery?.RunningProcesses.Count > 1)
            {
                if (_client.IsConnected) await _client.DisposeAsync();
                SetDisconnected("检测到多个游戏实例", "请正常退出多余的游戏实例，只保留一个；窗口会自动重新识别并连接。");
                return;
            }
            _gamePid = FindGameProcessId();
            if (!_gamePid.HasValue)
            {
                if (_client.IsConnected) await _client.DisposeAsync();
                string title = _discovery?.ProcessScanFailed == true ? "运行状态暂不确定" : _discovery?.IsGameRunning == true ? "需要选择游戏实例" : "游戏尚未启动";
                string detail = _discovery?.ProcessScanFailed == true ? "暂时无法读取游戏进程，请稍后重试；尚未执行准备操作。" : _discovery?.IsGameRunning == true
                    ? "检测到多个游戏进程，请在“维护与备份”中确认要连接的位置。"
                    : _discovery?.Selected is null ? "正在自动识别游戏；也可先从 Steam 启动游戏。" : "已找到游戏，点击“准备并启动”即可。";
                SetDisconnected(title, detail);
                return;
            }

            // A PID permits a read-only hello; changes still require the
            // verified directory of the selected running process.
            bool processPathKnown = _discovery?.Selected?.ProcessIds.Contains(_gamePid.Value) == true;
            if (!processPathKnown)
            {
                if (DateTime.UtcNow - _lastConnectionAttemptUtc < TimeSpan.FromSeconds(3)) return;
                _lastConnectionAttemptUtc = DateTime.UtcNow;
                if (_client.IsConnected) await _client.DisposeAsync();
                try
                {
                    var observed = await _client.ConnectAsync(_gamePid.Value, null, _lifetime.Token);
                    SetDisconnected("已识别游戏进程", observed.Ok
                        ? "插件已响应，但无法核对进程的安装位置；修改保持关闭。请正常退出游戏后再由修改器启动。"
                        : "已发现游戏；暂时无法读取运行位置，请确认游戏目录。所有修改保持关闭。");
                }
                finally { await _client.DisposeAsync(); }
                return;
            }

            string stamp = FingerprintStamp(_settings.GameDirectory);
            if (_lastValidatedPath != _settings.GameDirectory || _validatedPid != _gamePid || _lastFingerprintStamp != stamp)
            {
                string directory = _settings.GameDirectory;
                var check = await Task.Run(() => VerifyTrustedGame(directory));
                if (generation != _stateGeneration || _closing) return;
                _versionError = check.IsSupported ? null : check.Message;
                _lastValidatedPath = directory; _lastFingerprintStamp = stamp; _validatedPid = _gamePid;
            }
            if (generation != _stateGeneration) return;
            if (_versionError is not null)
            {
                if (_client.IsConnected) await _client.DisposeAsync();
                SetDisconnected("版本待适配", _versionError); return;
            }
            Response response;
            if (!_client.IsConnected || _client.ProcessId != _gamePid)
            {
                if (DateTime.UtcNow - _lastConnectionAttemptUtc < TimeSpan.FromSeconds(3)) return;
                _lastConnectionAttemptUtc = DateTime.UtcNow;
                // Verify on connection, not on every heartbeat. All disk hashing
                // stays off the UI thread and never grants a client connection
                // based on a mutable installation journal alone.
                string game = _settings.GameDirectory;
                await Task.Run(() => TrustedPayloadCatalog.LoadEmbedded().VerifyInstalledPayload(game));
                if (generation != _stateGeneration || _closing || _busy) return;
                _backupToken = await Task.Run(() => _backups.LatestSessionProofPath(_settings.GameDirectory));
                if (generation != _stateGeneration || _closing || _busy) return;
                response = await _client.ConnectAsync(_gamePid.Value, _backupToken, _lifetime.Token);
                if (!response.Ok) throw new IOException(response.Error ?? "插件拒绝连接。");
                Log("已自动连接游戏插件，持续功能保持关闭。");
            }
            else response = await _client.SendAsync(new Request { Kind = "heartbeat" }, _lifetime.Token);
            if (generation != _stateGeneration || _closing) return;
            if (!response.Ok)
            {
                await _client.DisposeAsync();
                throw new IOException(response.Error ?? "插件状态读取失败。");
            }
            if (response.State is not null) ApplyState(response.State);
            else throw new IOException("插件没有返回有效状态。");
        }
        catch (OperationCanceledException) when (_closing) { }
        catch (Exception ex)
        {
            if (generation == _stateGeneration && !_closing)
            {
                string detail = !_pluginPresent
                    ? "游戏正在运行，但没有检测到配套插件。请先正常退出游戏，再点“准备并启动”。"
                    : ex is OperationCanceledException
                        ? "正在等待插件响应。首次初始化可能需要稍等，请等待游戏主菜单。"
                        : "尚未连接插件：" + ex.Message;
                SetDisconnected(_pluginPresent ? "正在等待游戏连接" : "需要准备配套文件", detail);
            }
        }
        finally { _polling = false; completed.TrySetResult(); }
    }

    private int? FindGameProcessId()
    {
        var selected = _discovery?.Selected;
        if (selected?.ProcessIds.Count == 1) return selected.ProcessIds[0];
        if (selected?.ProcessIds.Count > 1) return null;
        return _discovery?.RunningProcesses.Count == 1 ? _discovery.RunningProcesses[0].ProcessId : null;
    }

    private static VersionCheck VerifyTrustedGame(string directory) => FingerprintService.Verify(directory,
        TrustedPayloadCatalog.LoadEmbedded().ReadSupportedBuild(RuntimePaths.SupportedBuildPath));

    private static TrustedPayloadCatalog VerifyTrustedInstallationFiles()
    {
        var catalog = TrustedPayloadCatalog.LoadEmbedded();
        catalog.ReadSupportedBuild(RuntimePaths.SupportedBuildPath);
        catalog.VerifyPayload(RuntimePaths.PayloadDirectory);
        return catalog;
    }

    private static string FingerprintStamp(string directory)
    {
        var files = new[]
        {
            Path.Combine(directory, "GameAssembly.dll"),
            Path.Combine(directory, "VampireSurvivors_Data", "il2cpp_data", "Metadata", "global-metadata.dat"),
            GamePaths.FindAppManifest(directory) ?? Path.Combine(directory, "missing-appmanifest"),
            RuntimePaths.SupportedBuildPath
        };
        return string.Join("|", files.Select(path =>
        {
            try { var file = new FileInfo(path); return file.Exists ? $"{file.Length}:{file.LastWriteTimeUtc.Ticks}" : "missing"; }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException) { return "unreadable"; }
        }));
    }

    private void ApplyState(TrainerState state)
    {
        if (_interactiveResize) { _resizeState = state; return; }
        _lastState = state;
        _connected = true; _backupAuthorized = state.BackupAuthorized; _playerReady = state.PlayerReady;
        UiLocalization.SetText(_connection, state.PlayerReady ? "●  已连接 · 关卡中" : "●  已连接 · 等待关卡");
        _connection.ForeColor = Accent;
        _launchRequested = false;
        UiLocalization.SetText(_details, $"Build {state.GameBuild}  ·  {state.Scene}  ·  插件 {state.PluginVersion}");
        _tips.SetToolTip(_details, UiLocalization.GetSource(_details));
        UiLocalization.SetText(_backupStatus, state.BackupAuthorized ? "备份：本次启动已授权金币修改" : "备份：本次启动未获金币授权；退出游戏后备份，再启动即可");
        _backupStatus.ForeColor = state.BackupAuthorized ? TextSecondary : Warning;
        UiLocalization.SetText(_goldLabel, state.Gold.HasValue ? state.Gold.Value.ToString("N0", CultureInfo.CurrentCulture) : "当前场景不可读取");
        _states.Clear(); foreach (var feature in state.Features) _states[feature.Id] = feature;
        foreach (var row in _rows.Values)
        {
            _states.TryGetValue(row.Id, out var feature);
            bool ready = feature?.Supported == true && (row.Id == FeatureIds.NoGoldSpend ? state.BackupAuthorized : state.PlayerReady);
            row.Toggle.Enabled = ready && !_pending.Contains(row.Id) && !_busy && !_requestInFlight;
            UiLocalization.SetText(row.Toggle, feature?.Supported != true ? "不可用" : feature.Enabled ? "已开启" : "开启");
            row.Toggle.BackColor = feature?.Enabled == true ? Color.FromArgb(34, 97, 79) : SurfaceRaised;
            row.Toggle.ForeColor = feature?.Enabled == true ? Accent : TextPrimary;
            UiLocalization.SetText(row.Reason, !string.IsNullOrWhiteSpace(feature?.Reason) ? feature.Reason : feature?.Supported != true ? "本版暂未识别此功能" : !ready ? row.Id == FeatureIds.NoGoldSpend ? "需要本次启动前的有效备份" : "进入关卡后可用" : feature.Enabled ? "已开启 · " + row.Description : row.Description);
            row.Reason.ForeColor = feature?.Enabled == true ? Accent : feature?.Supported != true ? Warning : TextSecondary;
            string reasonTip = UiLocalization.GetSource(row.Reason);
            if (row.Id == FeatureIds.Damage)
            {
                if (state.Diagnostics.TryGetValue("combat.enemy.lastInput", out string? input) && state.Diagnostics.TryGetValue("combat.enemy.lastHookInput", out string? hookInput))
                    reasonTip += $"\n最近一次真实命中：输入伤害 {input} → 送入游戏 {hookInput}";
                else reasonTip += "\n尚未收到真实命中记录。";
            }
            _tips.SetToolTip(row.Reason, reasonTip);
            if (row.MultiplierPanel is not null) row.MultiplierPanel.Enabled = ready && !_pending.Contains(row.Id) && !_busy && !_requestInFlight;
            if (feature?.Enabled == true && row.Number is not null && !row.Number.ContainsFocus && !_pending.Contains(row.Id) && double.IsFinite(feature.Value))
            {
                _updating = true;
                try { row.Number.Value = (decimal)Math.Clamp(feature.Value, (double)row.Number.Minimum, (double)row.Number.Maximum); }
                finally { _updating = false; }
            }
        }
        _statEditor.ApplyState(state, _busy || _requestInFlight || _closing);
        _chestRewards.ApplyState(state, _busy || _requestInFlight || _closing);
        foreach (var control in _maintenance) control.Enabled = !_busy && !_requestInFlight && !_closing;
        _readGold.Enabled = state.Gold.HasValue && !_busy && !_requestInFlight;
        _setGold.Enabled = state.Gold.HasValue && state.BackupAuthorized && !_busy && !_requestInFlight;
        _disableAll.Enabled = !_busy && !_requestInFlight && (_states.Values.Any(f => f.Enabled) || _statEditor.HasEnabledStats || _chestRewards.HasEnabledReward);
        _lastPollMessage = "";
        UpdatePrimaryAction();
    }

    private void SetDisconnected(string title, string detail)
    {
        _resizeState = null;
        _stateGeneration++;
        _connected = false; _backupAuthorized = false; _playerReady = false; _states.Clear(); _lastState = null;
        _statEditor.SetDisconnected();
        _chestRewards.SetDisconnected();
        UiLocalization.SetText(_connection, "●  " + title); _connection.ForeColor = Warning; UiLocalization.SetText(_details, detail);
        _tips.SetToolTip(_details, detail);
        UiLocalization.SetText(_backupStatus, _backupToken is null
            ? "备份：首次准备时会自动备份；永久金币需本次启动前的有效备份"
            : "备份：已找到本地备份；当前未连接，金币修改不可用");
        _backupStatus.ForeColor = TextSecondary;
        UiLocalization.SetText(_goldLabel, "尚未连接"); _readGold.Enabled = _setGold.Enabled = _disableAll.Enabled = false;
        foreach (var row in _rows.Values)
        {
            row.Toggle.Enabled = false; UiLocalization.SetText(row.Toggle, "未连接"); row.Toggle.BackColor = SurfaceRaised; row.Toggle.ForeColor = TextPrimary;
            if (row.MultiplierPanel is not null) row.MultiplierPanel.Enabled = false;
            UiLocalization.SetText(row.Reason, "未连接 · " + row.Description); row.Reason.ForeColor = TextSecondary;
            _tips.SetToolTip(row.Reason, UiLocalization.GetSource(row.Reason));
        }
        if (detail != _lastPollMessage) { Log(detail); _lastPollMessage = detail; }
        UpdatePrimaryAction();
    }

    private async Task ToggleFeatureAsync(string id)
    {
        if (_connected && !_busy && !_requestInFlight && _capturing is null && _rows.TryGetValue(id, out var row) && row.Toggle.Enabled && !_pending.Contains(id))
            await SetFeatureAsync(row, !_states[id].Enabled);
    }

    private async Task ActivateHotkeyAsync(string action)
    {
        if (!_connected || _closing || _busy || _requestInFlight || _capturing is not null) return;
        if (action.StartsWith("stat:", StringComparison.Ordinal)) await _statEditor.ToggleHotkeyAsync(action[5..]);
        else if (FeatureIds.All.Contains(action)) await ToggleFeatureAsync(action);
    }

    private async Task SetFeatureAsync(FeatureRow row, bool enabled)
    {
        if (!_pending.Add(row.Id)) return;
        row.Toggle.Enabled = false;
        if (row.MultiplierPanel is not null) row.MultiplierPanel.Enabled = false;
        try
        {
            await ExecuteRequestAsync(new Request { Kind = "setFeature", Feature = row.Id, Enabled = enabled, Value = row.Number is null ? 1 : (double)row.Number.Value, BackupToken = _backupToken }, enabled ? $"已开启：{row.Name}" : $"已关闭：{row.Name}");
        }
        finally
        {
            _pending.Remove(row.Id);
            if (_connected && _lastState is not null && !_closing) ApplyState(_lastState);
        }
    }

    private async Task ExecuteRequestAsync(Request request, string? success = null)
    {
        if (!_connected || _closing || _busy || _requestInFlight) return;
        _requestInFlight = true; _stateGeneration++;
        if (_lastState is not null) ApplyState(_lastState);
        try
        {
            var response = await _client.SendAsync(request, _lifetime.Token);
            if (response.Ok && response.State is null)
            {
                await _client.DisposeAsync();
                throw new IOException("插件未返回实际状态，操作结果尚未确认。");
            }
            if (response.State is not null) ApplyState(response.State);
            if (request.Kind == "getItemCatalog")
            {
                if (response.Ok && response.ItemCatalog is not null) _chestRewards.SetCatalogPage(response.ItemCatalog, response.ItemCatalogTotal, request.CatalogOffset);
                else _chestRewards.SetCatalogError(response.Error ?? "插件未提供物品列表，请更新配套插件或稍后刷新。");
            }
            if (request.Kind == "setChestReward" && !response.Ok)
                _chestRewards.SetOperationError(response.Error, response.ErrorScope);
            if (!response.Ok) { Log("未执行：" + (response.Error ?? "插件未确认操作。")); return; }
            if (success is not null) Log(success);
        }
        catch (Exception ex) when (!_closing) { SetDisconnected("连接已中断", ex.Message); }
        finally
        {
            _requestInFlight = false;
            if (_connected && _lastState is not null && !_closing) ApplyState(_lastState);
        }
    }

    private async Task<RewardItemIcon?> FetchRewardIconAsync(string id)
    {
        if (!_connected || _closing || _busy || _requestInFlight || _interactiveResize || !_client.IsConnected) return null;
        int? pid = _client.ProcessId;
        try
        {
            // Icon reads share only the exchange lock; input and actions stay enabled.
            var response = await _client.SendAsync(new Request { Kind = "getItemIcon", ItemId = id }, _lifetime.Token);
            if (_closing || !_connected || _client.ProcessId != pid) return null;
            return response.Ok ? response.ItemIcon : new RewardItemIcon { Id = id };
        }
        catch (Exception) { return null; } // Regular polling handles connection changes.
    }

    private void BeginCapture(string id)
    {
        if (!HotkeyService.Actions.Contains(id)) return;
        CancelCapture(); _capturing = id;
        RefreshHotkeyLabels();
        UiLocalization.SetText(_footer, "按 F1～F24、数字小键盘或 Ctrl / Alt / Shift 组合；Delete 清除，Esc 取消。"); _footer.ForeColor = Accent;
        Focus();
    }

    private void CaptureKey(object? sender, KeyEventArgs e)
    {
        if (_capturing is null) return;
        e.SuppressKeyPress = true; e.Handled = true;
        if (e.KeyCode == Keys.Escape) { CancelCapture(); return; }
        bool clear = e.KeyData is Keys.Delete or Keys.Back;
        if (!clear && !HotkeyService.IsValid(e.KeyData)) return;
        string id = _capturing;
        if (!clear && _settings.Hotkeys.Any(p => p.Key != id && p.Value == (int)e.KeyData)) { UiLocalization.SetText(_footer, "此快捷键已被使用，请选择另一个。Esc 取消。"); return; }
        _settings.Hotkeys[id] = clear ? 0 : (int)e.KeyData;
        SaveSettings(); CancelCapture(); Log($"快捷键已更新：{HotkeyActionName(id)} → {HotkeyService.Format((Keys)_settings.Hotkeys[id])}");
    }

    private void CancelCapture()
    {
        _capturing = null; UiLocalization.SetText(_footer, "快捷键仅在游戏前台生效 · 点击快捷键可改绑 · 断开连接后自动关闭持续效果"); _footer.ForeColor = TextSecondary;
        RefreshHotkeyLabels();
    }

    private string HotkeyLabel(string action) => _capturing == action ? "按新键…" : HotkeyService.Format((Keys)_settings.Hotkeys.GetValueOrDefault(action));
    private string HotkeyActionName(string action) => action.StartsWith("stat:", StringComparison.Ordinal)
        ? StatCatalog.Find(action[5..])?.Name ?? action : _rows.TryGetValue(action, out var row) ? row.Name : action;

    private void RefreshHotkeyLabels()
    {
        int width = 80;
        foreach (var row in _rows.Values)
        {
            string source = HotkeyLabel(row.Id);
            UiLocalization.SetText(row.Hotkey, source);
            _tips.SetToolTip(row.Hotkey, source);
            width = Math.Max(width, Math.Min(190, TextRenderer.MeasureText(UiLocalization.Translate(source), Font).Width + 20));
        }
        foreach (var row in _rows.Values) if (row.Panel.ColumnStyles[0].Width != width) row.Panel.ColumnStyles[0].Width = width;
        if (_featureHotkeyHeader is not null && _featureHotkeyHeader.Width != width + 8) _featureHotkeyHeader.Width = width + 8;
        _statEditor?.RefreshHotkeyLabels();
    }

    protected override void OnResizeBegin(EventArgs e)
    {
        _interactiveResize = true;
        base.OnResizeBegin(e);
    }

    protected override void OnResizeEnd(EventArgs e)
    {
        _interactiveResize = false;
        var latest = _resizeState; _resizeState = null;
        if (latest is not null && !_closing && !IsDisposed) ApplyState(latest);
        base.OnResizeEnd(e);
    }


    private async Task BrowseGameAsync()
    {
        using var dialog = new FolderBrowserDialog { Description = UiLocalization.Translate("选择包含 VampireSurvivors.exe 的游戏目录"), UseDescriptionForTitle = true, InitialDirectory = _gamePath.Text };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        if (!GamePaths.IsGameDirectory(dialog.SelectedPath))
        {
            LocalizedMessageBox.Show(this, "这个文件夹不是完整的游戏目录。请选择同时包含 VampireSurvivors.exe、GameAssembly.dll 和 VampireSurvivors_Data 的文件夹。", "未识别到游戏", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        await ChangeGameDirectoryAsync(dialog.SelectedPath);
        await RefreshDiscoveryAsync(true);
        await RefreshBackupAsync();
    }

    private Task DetectGameAsync() => DetectGameAsync(false);

    private async Task DetectGameAsync(bool fromPrimaryAction)
    {
        _lastConnectionAttemptUtc = DateTime.MinValue;
        _lastValidatedPath = "";
        if (!await RefreshDiscoveryAsync(true)) return;
        await PollAsync(fromPrimaryAction);
    }

    private async Task<bool> RefreshDiscoveryAsync(bool force = false)
    {
        if (_diagnosticsMode || _closing) return false;
        // Explicit actions share and await the entire pending scan, including
        // result application. A background refresh never changes button state.
        for (int attempt = 0; attempt < 2; attempt++)
        {
            Task<bool> scan;
            if (_discoveryTask is { IsCompleted: false } pending) scan = pending;
            else
            {
                if (!force && DateTime.UtcNow - _lastDiscoveryUtc < TimeSpan.FromSeconds(3)) return _lastDiscoverySucceeded;
                scan = _discoveryTask = RefreshDiscoveryCoreAsync();
            }
            bool succeeded = await scan;
            if (_closing) return false;
            if (!ReferenceEquals(scan, _discoveryTask))
            {
                if (!force) return false;
                continue;
            }
            if (string.Equals(_lastDiscoveryPath, _settings.GameDirectory, StringComparison.OrdinalIgnoreCase)) return succeeded;
            // A folder change invalidates the old result. Never prepare a game
            // using that snapshot; one fresh scan is allowed for this action.
            if (!force) return false;
        }
        return false;
    }

    private async Task<bool> RefreshDiscoveryCoreAsync()
    {
        UpdatePrimaryAction();
        string saved = _settings.GameDirectory;
        _lastDiscoveryPath = saved;
        _lastDiscoverySucceeded = false;
        try
        {
            var result = await Task.Run(() => _paths.DiscoverGame(saved));
            if (_closing) return false;
            if (!string.Equals(saved, _settings.GameDirectory, StringComparison.OrdinalIgnoreCase))
            {
                _lastDiscoveryUtc = DateTime.MinValue;
                return false;
            }
            _discovery = result;
            _lastDiscoveryUtc = DateTime.UtcNow;
            if (_launchRequested && result.IsGameRunning) _launchObservedProcess = true;
            if (_launchRequested && _launchObservedProcess && !result.IsGameRunning && !result.ProcessScanFailed) _launchRequested = false;
            if (result.Selected is not null && !string.Equals(result.Selected.Directory, _settings.GameDirectory, StringComparison.OrdinalIgnoreCase))
                await ChangeGameDirectoryAsync(result.Selected.Directory);
            _gamePath.Text = result.Selected?.Directory ?? "";
            _pluginPresent = result.Selected is not null && File.Exists(Path.Combine(result.Selected.Directory, "BepInEx", "plugins", "SurvivorsTrainer", "Trainer.Plugin.dll"));
            string source = result.Selected is null ? "尚未确定游戏位置" : DiscoverySource(result.Selected);
            _tips.SetToolTip(_gamePath, source + (result.Warnings.Count > 0 ? "\n" + string.Join("\n", result.Warnings) : ""));
            _lastDiscoveryPath = result.Selected?.Directory ?? saved;
            _lastDiscoverySucceeded = true;
            return true;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException or System.ComponentModel.Win32Exception)
        {
            SetDisconnected("暂未识别游戏", "自动识别未完成；可从 Steam 启动游戏后重试，或在“维护与备份”中更改位置。");
            Log("自动识别：" + ex.Message);
            _lastDiscoveryUtc = DateTime.UtcNow;
            return false;
        }
        finally { UpdatePrimaryAction(); }
    }

    private static string DiscoverySource(GameInstallationCandidate selected)
    {
        if (selected.Sources.Contains(GameDiscoverySource.RunningProcess)) return "已从正在运行的游戏自动识别";
        if (selected.Sources.Contains(GameDiscoverySource.SteamManifest) || selected.Sources.Contains(GameDiscoverySource.SteamLibrary)) return "已从 Steam 游戏库自动识别";
        if (selected.Sources.Contains(GameDiscoverySource.SavedDirectory)) return "已识别保存的游戏位置";
        return "已自动识别游戏位置";
    }

    private async Task ChangeGameDirectoryAsync(string directory)
    {
        if (string.IsNullOrWhiteSpace(directory)) return;
        string full = Path.GetFullPath(directory.Trim());
        if (string.Equals(full, _settings.GameDirectory, StringComparison.OrdinalIgnoreCase)) return;
        if (_client.IsConnected)
        {
            try
            {
                using var timeout = new CancellationTokenSource(1300);
                await _client.SendAsync(new Request { Kind = "disableAll" }, timeout.Token);
            }
            catch { }
            await _client.DisposeAsync();
        }
        _settings.GameDirectory = full; _gamePath.Text = full; _lastValidatedPath = ""; _backupToken = null; _versionError = null;
        _lastConnectionAttemptUtc = DateTime.MinValue;
        SetDisconnected("正在识别游戏状态", "游戏位置已识别，正在检查是否已启动。");
        SaveSettings();
    }

    private Task ToggleMaintenanceAsync()
    {
        _maintenanceExpanded = !_maintenanceExpanded;
        _maintenancePanel.Visible = _maintenanceExpanded;
        _rootLayout.RowStyles[2].Height = _maintenanceExpanded ? 94 : 0;
        UiLocalization.SetText(_maintenanceToggle, _maintenanceExpanded ? "维护与备份 ▴" : "维护与备份 ▾");
        return Task.CompletedTask;
    }

    private async Task PrimaryActionAsync()
    {
        if (_diagnosticsMode || _closing || _busy || _requestInFlight || _primaryActionInFlight) return;
        if (_connected) { _tabs.SelectedIndex = 0; return; }
        _primaryActionInFlight = true;
        UpdatePrimaryAction();
        try
        {
            if (!await RefreshDiscoveryAsync(true) || _closing || _busy || _requestInFlight) return;
            if (_discovery?.ProcessScanFailed == true) { await DetectGameAsync(true); return; }
            if (_discovery?.RunningProcesses.Count > 1)
            {
                SetDisconnected("检测到多个游戏实例", "请正常退出多余的游戏实例，只保留一个；窗口会自动重新识别并连接。");
                return;
            }
            if (_discovery?.IsGameRunning == true)
            {
                _lastConnectionAttemptUtc = DateTime.MinValue;
                _lastValidatedPath = "";
                await PollAsync(true);
                return;
            }
            if (_discovery?.Selected is null)
            {
                if (_discovery?.RequiresSelection == true) await ChooseDiscoveredGameAsync();
                else await BrowseGameAsync();
                return;
            }
            await PrepareAndLaunchAsync();
        }
        finally
        {
            _primaryActionInFlight = false;
            UpdatePrimaryAction();
        }
    }

    private async Task ChooseDiscoveredGameAsync()
    {
        if (_discovery is null || _discovery.Candidates.Count == 0) { await BrowseGameAsync(); return; }
        var candidates = _discovery.Candidates.ToArray();
        using var dialog = new Form { Text = "选择要使用的游戏", ClientSize = new Size(740, 270), StartPosition = FormStartPosition.CenterParent, BackColor = Background, ForeColor = TextPrimary, Font = Font, MinimizeBox = false, MaximizeBox = false, FormBorderStyle = FormBorderStyle.FixedDialog };
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(18), RowCount = 3, ColumnCount = 1 };
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 38)); layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 44)); dialog.Controls.Add(layout);
        layout.Controls.Add(Label("发现多个安装位置，请确认本次要使用哪一个。", Font, TextPrimary, true), 0, 0);
        var list = new ListBox { Dock = DockStyle.Fill, BackColor = Surface, ForeColor = TextPrimary, IntegralHeight = false, BorderStyle = BorderStyle.None, HorizontalScrollbar = true };
        foreach (var candidate in candidates) list.Items.Add(candidate.Directory);
        list.SelectedIndex = 0; layout.Controls.Add(list, 0, 1);
        var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.RightToLeft, Padding = new Padding(0, 6, 0, 0) };
        var select = new DarkButton { Text = "使用所选位置", Width = 130, DialogResult = DialogResult.OK }; StyleButton(select);
        var browse = new DarkButton { Text = "选择其他文件夹…", Width = 160, DialogResult = DialogResult.Retry }; StyleButton(browse);
        actions.Controls.Add(select); actions.Controls.Add(browse); layout.Controls.Add(actions, 0, 2); dialog.AcceptButton = select;
        UiLocalization.Attach(dialog);
        var choice = dialog.ShowDialog(this);
        if (choice == DialogResult.Retry) { await BrowseGameAsync(); return; }
        if (choice != DialogResult.OK || list.SelectedIndex < 0) return;
        await ChangeGameDirectoryAsync(candidates[list.SelectedIndex].Directory);
        await RefreshDiscoveryAsync(true);
        await RefreshBackupAsync();
    }

    private void UpdatePrimaryAction()
    {
        if (_primaryAction is null || _diagnosticsMode) return;
        if (_closing) { _primaryAction.Enabled = false; return; }
        bool enabled = !_busy && !_requestInFlight && !_primaryActionInFlight;
        if (_busy)
        {
            UiLocalization.SetText(_primaryAction, "正在准备…");
            _primaryAction.Enabled = enabled;
            return;
        }
        if (_connected)
        {
            UiLocalization.SetText(_primaryAction, "已连接游戏"); enabled = false;
            SetPreparationStatus(_playerReady ? "已就绪，选择下方功能即可实时生效。\n关闭修改器后会撤销持续效果和本局属性覆盖。" : "已连接游戏，进入关卡后即可使用战斗与属性功能。\n所有开关初始关闭；无需再选择目录或重复准备。");
        }
        else if (_discovery?.ProcessScanFailed == true)
        {
            UiLocalization.SetText(_primaryAction, "重新检测");
            SetPreparationStatus("暂时无法确认游戏是否正在运行，尚未执行任何准备。\n请稍后重试；也可以先从 Steam 正常启动游戏。");
        }
        else if (_discovery?.RunningProcesses.Count > 1)
        {
            UiLocalization.SetText(_primaryAction, "等待保留一个实例"); enabled = false;
            SetPreparationStatus("检测到多个游戏实例，暂不自动选择或修改。\n请正常退出多余的游戏，只保留一个，窗口会自动继续。");
        }
        else if (_discovery?.IsGameRunning == true)
        {
            bool knownPath = _discovery.Selected?.ProcessIds.Contains(_discovery.RunningProcesses[0].ProcessId) == true;
            UiLocalization.SetText(_primaryAction, knownPath ? "重新连接" : "重新识别");
            if (!knownPath) SetPreparationStatus("已发现游戏，但暂时无法核对它的安装位置，修改保持关闭。\n请正常退出游戏后再由修改器准备并启动；无需结束进程。", true);
            else if (_versionError is not null) SetPreparationStatus("游戏版本待适配，修改功能保持关闭。\n" + _versionError, true);
            else if (!_pluginPresent && !_launchRequested) SetPreparationStatus("已自动发现正在运行的游戏，但尚未准备配套文件。\n请正常退出游戏，再点“准备并启动”；不会自动结束游戏。", true);
            else SetPreparationStatus("已发现游戏，正在自动连接；首次准备可能需要稍等。\n若主菜单出现后仍未连接，可正常退出游戏再重新准备。");
        }
        else if (_launchRequested && DateTime.UtcNow < _launchRequestedUntil)
        {
            UiLocalization.SetText(_primaryAction, "等待游戏启动"); enabled = false;
            SetPreparationStatus("已向 Steam 发送启动请求，正在等待游戏出现。\n如果 Steam 有登录或同步提示，请先在 Steam 中处理，无需重复点击启动。");
        }
        else if (_discovery?.Selected is not null)
        {
            _launchRequested = false;
            UiLocalization.SetText(_primaryAction, "准备并启动");
            SetPreparationStatus(DiscoverySource(_discovery.Selected) + "，无需手选目录。\n点击后自动备份进度、准备配套文件并通过 Steam 启动。");
        }
        else if (_discovery is null)
        {
            UiLocalization.SetText(_primaryAction, "正在识别…"); enabled = false;
            SetPreparationStatus("正在自动查找游戏进程和 Steam 游戏库…\n这里只读取运行与安装信息，不会更改游戏或存档。");
        }
        else
        {
            UiLocalization.SetText(_primaryAction, _discovery.RequiresSelection ? "选择游戏位置" : "查找游戏位置");
            SetPreparationStatus(_discovery.RequiresSelection ? "发现多个安装位置，请选择本次使用的游戏。\n确认后会记住选择，下次自动识别。" : "暂未找到游戏，可先从 Steam 启动一次，窗口会自动识别。\n也可点击左侧按钮选择安装文件夹。");
        }
        // Apply the final state once. Never briefly enable a connected/waiting
        // button or disable a usable action merely because a background scan ran.
        _primaryAction.Enabled = enabled;
    }

    private void SetPreparationStatus(string text, bool warning = false)
    {
        UiLocalization.SetText(_setupStatus, text);
        _setupStatus.ForeColor = warning ? Warning : TextSecondary;
        _tips.SetToolTip(_setupStatus, text);
    }

    private Task ShowReleaseInfoAsync()
    {
        using var dialog = new ReleaseInfoDialog(RuntimePaths.DataDirectory);
        dialog.ShowDialog(this);
        return Task.CompletedTask;
    }

    private async Task PrepareAndLaunchAsync() => await MaintenanceAsync("正在准备游戏与修改器…", async () =>
    {
        try
        {
            if (_launchRequested && DateTime.UtcNow < _launchRequestedUntil)
            {
                Log("已经向 Steam 发送启动请求，继续等待当前启动结果。");
                return;
            }
            SetPreparationStatus("1 / 4  查找游戏并核对版本…\n准备操作不会结束正在运行的游戏。");
            if (!await RefreshDiscoveryAsync(true) || _closing) return;
            if (_discovery?.Selected is null || !GamePaths.IsGameDirectory(_settings.GameDirectory))
                throw new DirectoryNotFoundException("尚未确定游戏位置。可先从 Steam 启动一次，或在“维护与备份”中选择游戏位置。");
            if (new WindowsProcessState().IsGameRunning)
            {
                SetPreparationStatus("游戏正在运行，已保留当前游戏。\n安装或重新备份前，请先正常退出游戏，再点“准备并启动”。", true);
                Log("检测到游戏正在运行；未安装插件、未备份或结束进程。已安装的插件会自动连接。");
                return;
            }
            var catalog = await Task.Run(VerifyTrustedInstallationFiles);
            var check = await Task.Run(() => VerifyTrustedGame(_settings.GameDirectory));
            if (!check.IsSupported) throw new InvalidOperationException(check.Message);

            string payload = RuntimePaths.PayloadDirectory;
            if (!File.Exists(Path.Combine(payload, "winhttp.dll"))
                || !File.Exists(Path.Combine(payload, "BepInEx", "plugins", "SurvivorsTrainer", "Trainer.Plugin.dll")))
                throw new InvalidDataException("配套安装文件不完整。请完整解压当前发行包，并保留程序旁的 payload 文件夹。");
            bool recorded = File.Exists(_installation.GetManifestPath(_settings.GameDirectory));
            bool installed = await Task.Run(() => _installation.IsInstalled(_settings.GameDirectory));
            if (recorded && !installed)
                throw new InvalidOperationException("现有插件文件与安装记录不一致。请先用下方“卸载插件”检查并清理原安装，再重新准备；被其他工具改动的文件会保留。");
            if (installed && !await Task.Run(() => ReleaseInfo.PayloadMatchesGame(payload, _settings.GameDirectory)))
                throw new InvalidOperationException("已安装的插件与当前发行包版本不同。请先使用原数据目录的“卸载插件”卸载旧版，再点击“准备并启动”。存档备份不会因此删除。");
            if (installed) await Task.Run(() => catalog.VerifyInstalledPayload(_settings.GameDirectory));
            if (!recorded && File.Exists(Path.Combine(_settings.GameDirectory, "winhttp.dll")))
                throw new InvalidOperationException("游戏目录已有加载器，但当前修改器没有它的安装记录。请先从原修改器卸载或处理已有模组，再准备本工具，避免接管其他工具的文件。");

            SetPreparationStatus("2 / 4  备份当前进度…\n会保留存档原路径、时间与校验值。");
            var backup = await Task.Run(() => _backups.CreateBackup(_settings.GameDirectory));
            _backupToken = backup.ProofPath;
            Log($"启动前备份完成：{backup.FileCount} 个文件 · {backup.Id}");

            SetPreparationStatus(installed ? "3 / 4  已安装相同配套文件，跳过重复安装。" : "3 / 4  安装配套插件…\n安装清单用于日后卸载和恢复原文件。");
            if (!installed)
            {
                var result = await Task.Run(() => _installation.Install(_settings.GameDirectory, payload, backup.ProofPath, RuntimePaths.SupportedBuildPath));
                Log($"配套插件安装完成，已记录 {result.FileCount} 个文件。");
            }
            else Log("已核对相同版本的配套文件，保留现有安装。");

            // Core checks guard each file operation. Recheck before launching too,
            // since the user can start the game while preparation is awaiting I/O.
            if (new WindowsProcessState().IsGameRunning)
                throw new InvalidOperationException("准备期间检测到游戏已启动。请正常退出后重试，确保本次备份发生在启动前。");
            await Task.Run(() => catalog.VerifyInstalledPayload(_settings.GameDirectory));
            SetPreparationStatus("4 / 4  已向 Steam 发送启动请求，等待游戏连接。\n首次初始化可能需要下载加载器所需文件并生成游戏接口，请等待主菜单。");
            Process.Start(new ProcessStartInfo("steam://rungameid/1794680") { UseShellExecute = true });
            _launchRequested = true; _launchObservedProcess = false; _launchRequestedUntil = DateTime.UtcNow.AddSeconds(90);
            Log("准备完成，已向 Steam 发送启动请求。进入游戏后会自动连接，所有功能初始关闭。");
            await RefreshBackupAsync();
        }
        catch (Exception ex)
        {
            SetPreparationStatus("准备未完成：" + ex.Message, true);
            throw;
        }
    });

    private async Task CreateBackupAsync() => await MaintenanceAsync("正在备份存档…", async () =>
    {
        var result = await Task.Run(() => _backups.CreateBackup(_settings.GameDirectory));
        _backupToken = result.ProofPath;
        Log($"备份完成：{result.FileCount} 个文件 · {result.Id}");
        await RefreshBackupAsync();
    });

    private async Task InstallAsync() => await MaintenanceAsync("正在安装插件…", async () =>
    {
        await Task.Run(VerifyTrustedInstallationFiles);
        var result = await Task.Run(() => _backups.CreateBackup(_settings.GameDirectory));
        _backupToken = result.ProofPath;
        Log($"安装前备份完成：{result.Id}");
        await Task.Run(() => _installation.Install(_settings.GameDirectory, RuntimePaths.PayloadDirectory, result.ProofPath, RuntimePaths.SupportedBuildPath));
        Log("插件已安装。启动游戏后将自动连接，首次初始化可能需要稍等。");
        await RefreshBackupAsync();
    });

    private async Task UninstallAsync() => await MaintenanceAsync("正在卸载插件…", async () =>
    {
        var result = await Task.Run(() => _installation.Uninstall(_settings.GameDirectory));
        Log($"卸载操作完成：移除或恢复 {result.RemovedOrRestoredCount} 个文件。");
        if (result.PreservedModifiedFiles.Count > 0)
            Log("以下文件在安装后发生变化，已保留，请按需检查：" + string.Join("、", result.PreservedModifiedFiles));
    });

    private async Task RestoreAsync()
    {
        if (_busy) return;
        var backups = await Task.Run(() => _backups.ListBackups().ToList());
        if (backups.Count == 0) { Log("尚无可恢复的备份。"); return; }
        using var dialog = new Form { Text = "恢复本地存档", StartPosition = FormStartPosition.CenterParent, ClientSize = new Size(650, 310), BackColor = Background, ForeColor = TextPrimary, Font = Font, MinimizeBox = false, MaximizeBox = false, FormBorderStyle = FormBorderStyle.FixedDialog };
        var panel = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(18), ColumnCount = 1, RowCount = 3 };
        panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 74)); panel.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 42)); dialog.Controls.Add(panel);
        panel.Controls.Add(Label("恢复会回滚本地游戏进度。请先退出游戏和 Steam。\n恢复前会再保存一份当前存档；下次云同步若出现冲突，需选择希望保留的进度。\n此操作不撤销 Steam 成就。", Font, Warning, true), 0, 0);
        var list = new ListBox { Dock = DockStyle.Fill, BackColor = Surface, ForeColor = TextPrimary, BorderStyle = BorderStyle.None, IntegralHeight = false };
        foreach (var backup in backups) list.Items.Add($"{backup.CreatedUtc.ToLocalTime():yyyy-MM-dd HH:mm:ss}  ·  {backup.FileCount} 个文件  ·  {backup.Id}");
        list.SelectedIndex = 0; panel.Controls.Add(list, 0, 1);
        var restore = new DarkButton { Text = "恢复所选备份", DialogResult = DialogResult.OK, Width = 150 }; StyleButton(restore); var bottom = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.RightToLeft, Padding = new Padding(0, 5, 0, 0) }; bottom.Controls.Add(restore); panel.Controls.Add(bottom, 0, 2); dialog.AcceptButton = restore;
        UiLocalization.Attach(dialog);
        if (dialog.ShowDialog(this) != DialogResult.OK || list.SelectedIndex < 0) return;
        var selected = backups[list.SelectedIndex];
        await MaintenanceAsync("正在校验并恢复备份…", async () =>
        {
            await Task.Run(() => _backups.Restore(_settings.GameDirectory, selected.ManifestPath));
            _backupToken = null; Log("本地存档恢复完成。下次启动前请重新备份。"); await RefreshBackupAsync();
        });
    }

    private Task OpenBackupDirectoryAsync()
    {
        string directory = Path.Combine(RuntimePaths.DataDirectory, "backups"); Directory.CreateDirectory(directory);
        Process.Start(new ProcessStartInfo(directory) { UseShellExecute = true }); return Task.CompletedTask;
    }

    private async Task RefreshBackupAsync()
    {
        if (_diagnosticsMode || !GamePaths.IsGameDirectory(_settings.GameDirectory)) return;
        try
        {
            _backupToken = await Task.Run(() => _backups.LatestSessionProofPath(_settings.GameDirectory));
            if (!_connected) { UiLocalization.SetText(_backupStatus, _backupToken is null ? "备份：准备并启动时会自动备份当前进度" : "备份：已找到可用备份，金币授权由插件核验"); _backupStatus.ForeColor = TextSecondary; }
        }
        catch (Exception ex) { Log("备份状态检查：" + ex.Message); }
    }

    private async Task MaintenanceAsync(string message, Func<Task> action)
    {
        if (_busy || _closing || _requestInFlight) return;
        _busy = true; _stateGeneration++; CancelCapture();
        UpdatePrimaryAction();
        _statEditor.SetBlocked(true);
        _chestRewards.SetBlocked(true);
        foreach (var control in _maintenance) control.Enabled = false;
        _gamePath.Enabled = false; _readGold.Enabled = _setGold.Enabled = _disableAll.Enabled = false;
        foreach (var row in _rows.Values)
        {
            row.Toggle.Enabled = false;
            if (row.MultiplierPanel is not null) row.MultiplierPanel.Enabled = false;
        }
        Log(message);
        try
        {
            // A previous poll may still hold read handles while verifying the
            // installed payload. Invalidate it, then wait without blocking the
            // UI before installation/uninstallation can replace those files.
            await _pollIdle;
            await action();
        }
        catch (Exception ex) { Log("操作未完成：" + ex.Message); LocalizedMessageBox.Show(this, ex.Message, "操作未完成", MessageBoxButtons.OK, MessageBoxIcon.Warning); }
        finally
        {
            _busy = false; foreach (var control in _maintenance) control.Enabled = true; _gamePath.Enabled = true;
            UpdatePrimaryAction();
            _statEditor.SetBlocked(false);
            _chestRewards.SetBlocked(false);
            if (_connected && _lastState is not null && !_closing) ApplyState(_lastState);
            _lastValidatedPath = "";
            if (_closeAfterMaintenance) BeginInvoke(Close);
            else if (!_closing) await PollAsync();
        }
    }

    private async void OnClosing(object? sender, FormClosingEventArgs e)
    {
        if (_closing) return;
        if (_busy)
        {
            e.Cancel = true;
            _closeAfterMaintenance = true;
            Log("正在完成文件操作，完成后会自动关闭窗口。");
            return;
        }
        e.Cancel = true; _closing = true; UpdatePrimaryAction(); _pollTimer.Stop(); _keyTimer.Stop(); CancelCapture();
        try
        {
            if (_client.IsConnected)
            {
                using var timeout = new CancellationTokenSource(1300);
                await _client.SendAsync(new Request { Kind = "disableAll" }, timeout.Token);
            }
        }
        catch { }
        _lifetime.Cancel();
        await _client.DisposeAsync();
        SaveSettings();
        BeginInvoke(Close);
    }

    private void SaveSettings()
    {
        if (_diagnosticsMode) return;
        try { _settings.Save(RuntimePaths.SettingsPath); }
        catch (Exception ex) { Log("设置未保存：" + ex.Message); }
    }

    private void Log(string message)
    {
        if (IsDisposed) return;
        _logSources.Add($"{DateTime.Now:HH:mm:ss}  {message}");
        if (_logSources.Count > 150) _logSources.RemoveRange(0, 50);
        _log.AppendText($"{DateTime.Now:HH:mm:ss}  {UiLocalization.Translate(message)}{Environment.NewLine}");
    }

    private void OnLanguageChanged(object? sender, EventArgs e)
    {
        if (IsDisposed) return;
        _changingLanguage = true;
        try { _languagePicker.SelectedIndex = UiLocalization.IsEnglish ? 1 : 0; }
        finally { _changingLanguage = false; }
        _goldArea.ColumnStyles[0].Width = UiLocalization.IsEnglish ? 150 : 115;
        RefreshHotkeyLabels();
        _log.Text = string.Join(Environment.NewLine, _logSources.Select(line => line[..10] + UiLocalization.Translate(line[10..])))
            + (_logSources.Count > 0 ? Environment.NewLine : "");
        UiLocalization.Refresh(this);
    }

    internal void RenderPreview(string? path, bool stats, string? startupState = null)
    {
        if (!_diagnosticsMode) throw new InvalidOperationException("仅在界面检查模式中生成预览。");
        // All scenarios are presentation fixtures. Do not discover actual
        // processes, registry entries, settings, directories or saves here.
        switch (startupState ?? "found")
        {
            case "restart":
                UiLocalization.SetText(_primaryAction, "重新连接");
                UiLocalization.SetText(_connection, "●  已发现游戏");
                UiLocalization.SetText(_details, "游戏正在运行，尚未连接配套插件");
                SetPreparationStatus("已自动发现正在运行的游戏，但尚未准备配套文件。\n请正常退出游戏，再点“准备并启动”；不会自动结束游戏。", true);
                break;
            case "multiple":
                UiLocalization.SetText(_primaryAction, "选择游戏位置");
                UiLocalization.SetText(_connection, "●  等待选择");
                UiLocalization.SetText(_details, "发现多个安装位置，确认一次后会记住选择");
                SetPreparationStatus("发现多个安装位置，请选择本次使用的游戏。\n确认后会记住选择，下次自动识别。");
                break;
            case "missing":
                UiLocalization.SetText(_primaryAction, "查找游戏位置");
                UiLocalization.SetText(_connection, "●  尚未发现游戏");
                UiLocalization.SetText(_details, "可先从 Steam 启动游戏，窗口会自动识别");
                SetPreparationStatus("暂未找到游戏，可先从 Steam 启动一次，窗口会自动识别。\n也可点击左侧按钮选择安装文件夹。");
                break;
            case "launching":
                UiLocalization.SetText(_primaryAction, "等待游戏启动");
                UiLocalization.SetText(_connection, "●  等待游戏启动");
                UiLocalization.SetText(_details, "已向 Steam 发送启动请求");
                SetPreparationStatus("已向 Steam 发送启动请求，正在等待游戏出现。\n如果 Steam 有登录或同步提示，请先在 Steam 中处理，无需重复点击启动。");
                break;
            default:
                UiLocalization.SetText(_primaryAction, "准备并启动");
                UiLocalization.SetText(_connection, "●  游戏尚未启动");
                UiLocalization.SetText(_details, "已自动找到游戏，无需先选择安装目录");
                SetPreparationStatus("已从 Steam 游戏库自动识别，无需手选目录。\n点击后自动备份进度、准备配套文件并通过 Steam 启动。");
                break;
        }
        UiLocalization.SetText(_backupStatus, "备份：准备并启动时会自动备份当前进度");
        _gamePath.Text = "（自动识别的游戏位置 · 界面示意）";
        foreach (var control in _maintenance) control.Enabled = false;
        _primaryAction.Enabled = startupState != "launching";
        _gamePath.Enabled = false;
        if (stats) _tabs.SelectedIndex = 1;
        if (startupState == "chest") { _tabs.SelectedIndex = 2; _chestRewards.RenderPreview(); }
        bool renderNativeList = true; // The language dropdown also requires native painting.
        if (renderNativeList)
        {
            // Native ListView rows and combo rendering need a visible window
            // handle. Keep the diagnostic window off-screen and non-activating;
            // the diagnostic Shown branch never starts polling or reads saves.
            ShowInTaskbar = false; StartPosition = FormStartPosition.Manual;
            Location = new Point(-32000, -32000); Show(); Application.DoEvents();
            _log.Clear();
            _logSources.Clear();
        }
        _ = Handle;
        PerformLayout();
        CreatePreviewHandles(this);
        UiLocalization.Refresh(this);
        UiLocalization.Refresh(_statEditor);
        UiLocalization.Refresh(_chestRewards);
        using var bitmap = new Bitmap(Width, Height);
        DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size));
        if (path is not null)
        {
            string fullPath = Path.GetFullPath(path);
            Directory.CreateDirectory(Path.GetDirectoryName(fullPath)!);
            bitmap.Save(fullPath, System.Drawing.Imaging.ImageFormat.Png);
        }
        if (renderNativeList) Hide();
    }

    private static void CreatePreviewHandles(Control parent)
    {
        foreach (Control child in parent.Controls)
        {
            _ = child.Handle;
            child.PerformLayout();
            CreatePreviewHandles(child);
        }
    }

    private Button ActionButton(string title, Func<Task> action, int width = 0, bool accent = false, bool maintenance = true)
    {
        var button = new DarkButton { Text = title, Height = 34, Margin = new Padding(0, 3, 8, 3) };
        if (width > 0) button.Width = width; else button.Dock = DockStyle.Fill;
        StyleButton(button); if (accent) { button.BackColor = Color.FromArgb(34, 97, 79); button.ForeColor = Accent; }
        button.Click += async (_, _) => { try { await action(); } catch (Exception ex) { Log("操作未完成：" + ex.Message); } };
        if (maintenance) _maintenance.Add(button);
        return button;
    }

    private static Label Label(string text, Font font, Color color, bool fill = false) => new() { Text = text, Font = font, ForeColor = color, AutoSize = !fill, Dock = fill ? DockStyle.Fill : DockStyle.None, TextAlign = ContentAlignment.MiddleLeft, Margin = Padding.Empty };
    private static void StyleButton(Button button)
    {
        button.FlatStyle = FlatStyle.Flat; button.FlatAppearance.BorderSize = 1; button.FlatAppearance.BorderColor = Color.FromArgb(60, 72, 88);
        button.FlatAppearance.MouseOverBackColor = Color.FromArgb(52, 65, 80); button.FlatAppearance.MouseDownBackColor = Color.FromArgb(35, 84, 70);
        button.BackColor = SurfaceRaised; button.ForeColor = TextPrimary; button.Cursor = Cursors.Hand; button.UseVisualStyleBackColor = false;
    }
    private static void StyleInput(TextBox box) { box.BackColor = SurfaceRaised; box.ForeColor = TextPrimary; box.BorderStyle = BorderStyle.FixedSingle; }
    private static void StyleNumeric(NumericUpDown numeric) { numeric.BackColor = SurfaceRaised; numeric.ForeColor = TextPrimary; numeric.BorderStyle = BorderStyle.FixedSingle; }

    private sealed class FeatureRow(string id, string name, string description)
    {
        public string Id { get; } = id;
        public string Name { get; } = name;
        public string Description { get; } = description;
        public FeatureRowPanel Panel { get; set; } = null!;
        public Button Hotkey { get; } = new DarkButton();
        public Button Toggle { get; } = new DarkButton();
        public Control Reason { get; set; } = null!;
        public NumericUpDown? Number { get; set; }
        public Control? MultiplierPanel { get; set; }
    }
}
