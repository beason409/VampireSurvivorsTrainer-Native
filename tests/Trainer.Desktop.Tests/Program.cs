using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;
using Trainer.Desktop;
using Trainer.Shared;

internal static partial class Program
{
    private static int _passed;
    private static readonly List<object> Measurements = new();

    [STAThread]
    private static int Main(string[] args)
    {
        Application.SetUnhandledExceptionMode(UnhandledExceptionMode.ThrowException);
        ApplicationConfiguration.Initialize();
#if BASELINE
        const string variant = "baseline";
#else
        const string variant = "fixed";
#endif
        string? output = OptionValues(args, "--output").SingleOrDefault();
        var files = OptionValues(args, "--catalog").ToArray();
        var catalog = files.Length == 0 ? SyntheticCatalog(344) : ReadCatalog(files);
        try
        {
            // Warm the CLR/WinForms paths independently of the full-list first-key measurement.
            using (var warmup = new Fixture(catalog.Take(3).ToArray())) warmup.Search.Text = "p";
            if (args.Contains("--benchmark"))
            {
                MeasureTyping(catalog, files.Length == 0 ? "344-synthetic" : "actual-game-catalog");
                MeasureTyping(SyntheticCatalog(4096), "catalog-cap-synthetic");
            }
            FunctionalChecks(catalog);
            NativeSelectionAndCacheChecks();
#if !BASELINE
            OptimizationChecks();
            if (args.Contains("--benchmark")) MeasureCatalogLoading();
            IconChecks();
            LocalizationChecks(args.Contains("--benchmark"));
            FlickerChecks();
            HotkeyChecks();
#endif
            Console.WriteLine($"{variant}: {_passed} desktop search checks passed.");
            if (output is not null)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(output))!);
                File.WriteAllText(output, JsonSerializer.Serialize(new { variant, passed = _passed, failed = 0, actualCatalogCount = catalog.Length, gameConnected = false, window = "offscreen Form with live native handles", measurements = Measurements }, new JsonSerializerOptions { WriteIndented = true }));
                Console.WriteLine(output);
            }
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            if (output is not null)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(output))!);
                File.WriteAllText(output, JsonSerializer.Serialize(new { variant, passed = _passed, failed = 1, error = exception.ToString(), measurements = Measurements }, new JsonSerializerOptions { WriteIndented = true }));
            }
            return 1;
        }
    }

    private static IEnumerable<string> OptionValues(string[] args, string option)
    {
        for (int index = 0; index < args.Length; index++)
            if (args[index] == option && index + 1 < args.Length) yield return args[++index];
    }

    private static RewardItemDefinition[] ReadCatalog(IEnumerable<string> files)
    {
        var result = new List<RewardItemDefinition>();
        foreach (string path in files)
        {
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            result.AddRange(document.RootElement.GetProperty("response").GetProperty("itemCatalog").Deserialize<List<RewardItemDefinition>>(PipeProtocol.JsonOptions)!);
        }
        return result.ToArray();
    }

    private static RewardItemDefinition[] SyntheticCatalog(int count) => Enumerable.Range(0, count).Select(index => new RewardItemDefinition
    {
        Id = index == 0 ? "POWER" : index % 3 == 0 ? $"POWER_{index:D4}" : $"ITEM_{index:D4}",
        Name = index == 0 ? "菠菜" : index % 2 == 0 ? "示例武器" + index : "示例装备" + index,
        EnglishName = index == 0 ? "Spinach" : index % 3 == 0 ? "Power Sample " + index : "Example Item " + index,
        Kind = index == 0 || index % 2 != 0 ? "passive" : "weapon", MaxLevel = 8, UpgradeAllowed = index == 0 || index % 7 != 0,
        Description = "Offline synthetic catalog row for search performance regression testing.", IsEvolution = index % 11 == 0
    }).ToArray();

    private static void MeasureTyping(RewardItemDefinition[] catalog, string dataset)
    {
        using var fixture = new Fixture(catalog);
        Check(fixture.List.IsHandleCreated && fixture.Search.IsHandleCreated, dataset + " has native list and textbox handles");
        Check(fixture.List.Items.Count == catalog.Length, dataset + " displays all loaded rows");
        int requestsBefore = fixture.Requests.Count;
        foreach (int repetition in Enumerable.Range(0, 3))
        {
            foreach (string query in new[] { "p", "po", "pow", "powe", "power", "" })
            {
                var timer = Stopwatch.StartNew();
                fixture.Search.Text = query;
                timer.Stop();
                double handlerMilliseconds = timer.Elapsed.TotalMilliseconds;
                timer.Restart();
                fixture.List.Refresh();
                Application.DoEvents();
                timer.Stop();
                int expected = catalog.Count(item => Matches(item, query, 0));
                Check(fixture.List.Items.Count == expected, dataset + " query " + (query.Length == 0 ? "<clear>" : query) + " matches expected count");
                Measurements.Add(new { dataset, catalogCount = catalog.Length, repetition, query, matchingRows = expected, handlerMilliseconds, repaintMilliseconds = timer.Elapsed.TotalMilliseconds });
                Console.WriteLine($"MEASURE {dataset} run={repetition} query='{query}' count={expected} handler={handlerMilliseconds:F3}ms repaint={timer.Elapsed.TotalMilliseconds:F3}ms");
            }
        }
        Check(fixture.Requests.Count == requestsBefore, dataset + " typing never sends plugin requests");
    }

    private static void FunctionalChecks(RewardItemDefinition[] catalog)
    {
        using var fixture = new Fixture(catalog);
        foreach (int type in new[] { 0, 1, 2 })
        {
            fixture.Kind.SelectedIndex = type;
            foreach (string query in new[] { "power", "POWER", "Power", "菠菜", "POWER", " a ", "不可能匹配的名称", "" })
            {
                fixture.Search.Text = query;
                var expected = catalog.Where(item => Matches(item, query, type)).Select(item => item.Id).ToArray();
                var actual = Enumerable.Range(0, fixture.List.Items.Count).Select(index => ((RewardItemDefinition)fixture.List.Items[index].Tag!).Id).ToArray();
                Check(actual.SequenceEqual(expected), $"query '{query}' and kind {type} preserve case-insensitive Chinese/English/ID matching and catalog order");
            }
        }
        fixture.Kind.SelectedIndex = 0;
        fixture.Search.Clear();
        fixture.List.Items[catalog.Length - 1].Selected = true;
        fixture.List.Items[catalog.Length - 1].Focused = true;
        fixture.Search.Focus();
        Check(fixture.Search.Focused, "search textbox accepts input focus");
        fixture.Search.Text = "不可能匹配的名称";
        fixture.Search.Clear();
        Check(fixture.Selected is null && fixture.List.SelectedIndices.Count == 0 && fixture.Search.Focused, "high-index selection followed by zero matches and clear leaves no stale selection and preserves input focus");
        fixture.Search.Text = "POWER";
        int selectedIndex = Enumerable.Range(0, fixture.List.Items.Count).First(index => ((RewardItemDefinition)fixture.List.Items[index].Tag!).Id == "POWER");
        fixture.List.Items[selectedIndex].Selected = true;
        Application.DoEvents();
        Check(fixture.Selected?.Id == "POWER" && Field<Button>(fixture.Panel, "_enable").Enabled, "real row selection enables an explicit reward action");
        fixture.Search.Text = "power";
        Check(fixture.Selected?.Id == "POWER" && fixture.List.SelectedIndices.Count == 1, "selection survives narrowing when the selected ID remains visible");
        Check(fixture.Search.Focused, "restoring a selected row does not steal textbox focus");
        fixture.Search.Text = "不可能匹配的名称";
        Check(fixture.Selected is null && fixture.List.SelectedIndices.Count == 0 && !Field<Button>(fixture.Panel, "_enable").Enabled, "filtering selected row away clears selection and action");
        fixture.Search.Clear();
        Check(fixture.Selected is null && fixture.List.SelectedIndices.Count == 0, "clearing search does not select or enable an arbitrary row");
        fixture.State.ChestReward.TargetItemId = "POWER";
        fixture.Panel.ApplyState(fixture.State, false);
        fixture.Search.Text = "power";
        Check(fixture.Selected?.Id == "POWER", "active target fallback remains selected when included in results");
        fixture.Search.Text = "不可能匹配的名称";
        Check(fixture.Selected is null, "active target hidden by query is not a stale action selection");
        Check(fixture.Requests.Count == 0, "selection and filtering never mutate the plugin");
        fixture.Search.Clear();
        fixture.Panel.SetDisconnected();
        Check(fixture.List.Items.Count == 0 && fixture.Selected is null && !fixture.Search.Enabled && !Field<Button>(fixture.Panel, "_enable").Enabled, "disconnect removes rows and all stale selection/action state");
        fixture.Panel.SetDisconnected();
        fixture.Panel.Dispose();
        Check(fixture.Panel.IsDisposed && fixture.List.IsDisposed, "panel disposal releases its native list");
    }

    private static void NativeSelectionAndCacheChecks()
    {
        var definitions = new[]
        {
            new RewardItemDefinition { Id = "MATCH_A_001", Name = "甲", EnglishName = "Alpha", Kind = "weapon", UpgradeAllowed = true },
            new RewardItemDefinition { Id = "MATCH_B_002", Name = "乙", EnglishName = "Beta", Kind = "passive", UpgradeAllowed = true },
            new RewardItemDefinition { Id = "MATCH_C_003", Name = "丙", EnglishName = "Gamma", Kind = "weapon", UpgradeAllowed = true }
        };
        using var fixture = new Fixture(definitions);
        Check(fixture.List.FindItemWithText("Beta", true, 0, false)?.SubItems[4].Text == "MATCH_B_002", "native text search finds an exact English subitem");
        Check(fixture.List.FindItemWithText("Bet", true, 0, true)?.SubItems[4].Text == "MATCH_B_002", "native text search supports prefixes");
        Check(fixture.List.FindItemWithText("Beta", false, 0, false) is null && fixture.List.FindItemWithText("does-not-exist", true, 0, true) is null, "native text search honors subitem scope and reports no match");
        fixture.Search.Text = "MATCH_A";
        Check(fixture.List.Items.Count == 1 && fixture.List.Items[0].Text == "甲", "first single-match query renders correct row");
        fixture.List.Items[0].Selected = true;
        fixture.Search.Text = "MATCH_B";
        fixture.List.Refresh();
        Check(fixture.List.Items.Count == 1 && fixture.List.Items[0].Text == "乙" && fixture.List.Items[0].SubItems[4].Text == "MATCH_B_002" && fixture.Selected is null, "same match count with different IDs invalidates visible content and selection");
        fixture.Search.Clear();
        var rectangle = fixture.List.Items[0].Bounds;
        nint coordinates = (nint)(((rectangle.Top + rectangle.Height / 2) << 16) | (rectangle.Left + 10));
        // A synchronous WM_LBUTTONDOWN enters native drag tracking; queue both
        // messages so the up event can complete that tracking loop.
        PostMessage(fixture.List.Handle, 0x0201, 1, coordinates); // WM_LBUTTONDOWN
        PostMessage(fixture.List.Handle, 0x0202, 0, coordinates); // WM_LBUTTONUP
        Application.DoEvents();
        Check(fixture.Selected?.Id == "MATCH_A_001", "native mouse selection maps the clicked visible row to its ID");
        InvokeTask(fixture.Panel, "SetRewardAsync", true);
        Check(fixture.Requests.Last().Kind == "setChestReward" && fixture.Requests.Last().ItemId == "MATCH_A_001" && fixture.Requests.Last().Enabled == true, "mouse-selected target submits the correct chest request");
        SendMessage(fixture.List.Handle, 0x0100, 0x28, 0); // WM_KEYDOWN, VK_DOWN
        SendMessage(fixture.List.Handle, 0x0101, 0x28, 0); // WM_KEYUP, VK_DOWN
        Application.DoEvents();
        Check(fixture.Selected?.Id == "MATCH_B_002", "native keyboard selection updates the current target");
        InvokeTask(fixture.Panel, "SetUpgradeAsync", true);
        Check(fixture.Requests.Last().ItemId == "MATCH_B_002" && fixture.Requests.Last().UpgradeEnabled == true, "keyboard-selected target submits the correct upgrade request");
        fixture.Search.Text = "MATCH_B";
        fixture.List.Items[0].Selected = true;
        definitions[1] = new RewardItemDefinition { Id = "MATCH_B_002", Name = "乙已更新", EnglishName = "Updated Beta", Kind = "passive", UpgradeAllowed = false };
        fixture.Panel.SetCatalogPage(definitions, definitions.Length, 0);
        Check(fixture.List.Items[0].Text == "乙已更新" && fixture.List.Items[0].SubItems[1].Text == "Updated Beta" && fixture.List.Items[0].ToolTipText.Contains("Updated Beta") && fixture.List.Items[0].ToolTipText.Contains("不支持此物品") && fixture.Selected?.UpgradeAllowed == false, "catalog replacement refreshes a cached ID's names, metadata and tooltip");
        fixture.Panel.SetDisconnected();
        definitions[1] = new RewardItemDefinition { Id = "MATCH_B_002", Name = "重连乙", EnglishName = "Reconnected Beta", Kind = "passive", UpgradeAllowed = true };
        fixture.Panel.SetCatalogPage(definitions, definitions.Length, 0);
        fixture.Panel.ApplyState(fixture.State, false);
        Check(fixture.List.Items.Count == 1 && fixture.List.Items[0].Text == "重连乙" && fixture.List.Items[0].SubItems[1].Text == "Reconnected Beta" && fixture.Selected is null, "reconnect keeps the query and replaces old cached row data without a stale selection");
    }

    private static void InvokeTask(object instance, string name, params object[] args)
    {
        var task = (Task)instance.GetType().GetMethod(name, BindingFlags.NonPublic | BindingFlags.Instance)!.Invoke(instance, args)!;
        while (!task.IsCompleted) Application.DoEvents();
        task.GetAwaiter().GetResult();
    }

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern nint SendMessage(nint window, uint message, nint wParam, nint lParam);
    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PostMessage(nint window, uint message, nint wParam, nint lParam);

    private static bool Matches(RewardItemDefinition item, string query, int kind)
    {
        query = query.Trim();
        return (kind == 0 || item.Kind == (kind == 1 ? "weapon" : "passive")) && (query.Length == 0 || (item.Name + " " + item.EnglishName + " " + item.Id).Contains(query, StringComparison.OrdinalIgnoreCase));
    }

    private static T Field<T>(object instance, string name) => (T)instance.GetType().GetField(name, BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(instance)!;
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
        _passed++;
    }

    private sealed class Fixture : IDisposable
    {
        public readonly Form Host;
        public readonly ChestRewardPanel Panel;
        public readonly List<Request> Requests = new();
        public Func<Request, Task>? OnSend;
        public readonly TrainerState State = new() { PlayerReady = true, ChestReward = new() { Supported = true, UpgradeSupported = true } };
        public readonly TextBox Search;
        public readonly ComboBox Kind;
        public readonly ListView List;
        public RewardItemDefinition? Selected => Field<RewardItemDefinition?>(Panel, "_selected");

        public Fixture(RewardItemDefinition[] catalog)
        {
            Host = new Form { ClientSize = new Size(1110, 530), ShowInTaskbar = false, StartPosition = FormStartPosition.Manual, Location = new Point(-32000, -32000), Font = new Font("Microsoft YaHei UI", 9F) };
            Panel = new ChestRewardPanel((request, _) => { Requests.Add(request); return OnSend?.Invoke(request) ?? Task.CompletedTask; });
            Host.Controls.Add(Panel);
            Host.Show();
            CreateHandles(Host);
            Application.DoEvents();
            Search = Field<TextBox>(Panel, "_search"); Kind = Field<ComboBox>(Panel, "_kind"); List = Field<ListView>(Panel, "_items");
            Panel.SetCatalogPage(catalog, catalog.Length, 0);
            Panel.ApplyState(State, false);
            List.Refresh();
            Application.DoEvents();
        }

        public void Dispose() { Host.Dispose(); Application.DoEvents(); }
        private static void CreateHandles(Control control)
        {
            _ = control.Handle;
            foreach (Control child in control.Controls) CreateHandles(child);
        }
    }
}
