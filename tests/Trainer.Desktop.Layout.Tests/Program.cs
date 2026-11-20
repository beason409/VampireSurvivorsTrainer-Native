using System.Collections;
using System.Diagnostics;
using System.Reflection;
using System.Text.Json;
using Trainer.Desktop;
using Trainer.Shared;

internal static class Program
{
    private static int _passed;
    private const BindingFlags Fields = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;

    [STAThread]
    private static int Main(string[] args)
    {
        Application.SetUnhandledExceptionMode(UnhandledExceptionMode.ThrowException);
        ApplicationConfiguration.Initialize();
        var requests = new List<Request>();
        var captured = new List<string>();
        var bindings = new Dictionary<string, string>();
        var state = new TrainerState { PlayerReady = true, Stats = StatCatalog.All.Select(definition => new StatState { Id = definition.Id, Supported = true, Value = definition.DefaultValue, OriginalValue = definition.DefaultValue, CurrentValue = definition.DefaultValue }).ToList() };
        TaskCompletionSource? pending = null;
        StatEditorPanel? panel = null;
        string resultPath = Path.GetFullPath(args.Length > 0 ? args[0] : "artifacts/stat-layout/result.json");
        string output = Path.GetDirectoryName(resultPath)!;
        Directory.CreateDirectory(output);
        try
        {
            // Form.SetBoundsCore always clamps to the virtual desktop's maximum
            // tracking size. Keep a small native window for handles/focus, and
            // resize an undocked real Panel to test wide layouts independently.
            using var window = new Form { ClientSize = new Size(980, 530), ShowInTaskbar = false, StartPosition = FormStartPosition.Manual, Location = new Point(-32000, -32000), Font = new Font("Microsoft YaHei UI", 9F) };
            using var host = new Panel { ClientSize = new Size(1110, 530), Location = Point.Empty, Margin = Padding.Empty };
            window.Controls.Add(host);
            panel = new StatEditorPanel(async (request, _) =>
            {
                requests.Add(request);
                if (pending is not null) await pending.Task;
                if (request.Kind == "setStat")
                {
                    var item = state.Stats.Single(stat => stat.Id == request.Stat);
                    item.Enabled = request.Enabled == true;
                    if (request.Value is double value) item.Value = value;
                    item.CurrentValue = item.Enabled ? item.Value : item.OriginalValue;
                }
                else if (request.Kind == "disableStats") foreach (var item in state.Stats) item.Enabled = false;
                panel!.ApplyState(state, false);
            }, id => captured.Add(id), id => bindings.GetValueOrDefault("stat:" + id, "未绑定"));
            host.Controls.Add(panel); window.Show(); _ = panel.Handle; Pump();
            var rows = Field<IDictionary>(panel, "_rows");
            Check(rows.Count == 23 && !rows.Contains(StatIds.Seal), "UI builds exactly 23 rows and no Seal controls");
            Check(!Field<TableLayoutPanel>(panel, "_table").AutoSize && Field<TableLayoutPanel>(panel, "_table").Height == 23 * 69, "stat list uses an explicit 23-row height without preferred-size auto measurement");
            Check(!Descendants(panel).Any(control => control.Text.Contains("封印") || control.Text.Contains("Seal")), "Seal does not appear in any UI text");
            object might = rows[StatIds.Might]!;
            var bodyPanel = Property<TableLayoutPanel>(might, "Panel");
            var heading = Field<TableLayoutPanel>(panel, "_headings");
            Check(bodyPanel.DisplayRectangle.Left == bodyPanel.Padding.Left && bodyPanel.DisplayRectangle.Top == bodyPanel.Padding.Top
                && Property<Button>(might,"HotkeyButton").Bounds.Left == bodyPanel.Padding.Left + Property<Button>(might,"HotkeyButton").Margin.Left, "fast layout uses the already padded DisplayRectangle exactly once");
            Check(CenterX(Property<Label>(might,"Original")) == CenterX(heading.Controls[2]), "numeric header and row centers match with the vertical scrollbar visible");
            var hotkey = Property<Button>(might, "HotkeyButton");
            Check(rows.Values.Cast<object>().All(row => Property<Button>(row, "HotkeyButton").Text == "未绑定"), "every displayed stat starts with an unbound hotkey button");
            hotkey.PerformClick();
            Check(captured.SequenceEqual(new[] { StatIds.Might }) && requests.Count == 0, "disconnected hotkey binding button reports only its stat ID without a game command");
            bindings["stat:" + StatIds.Might] = "Ctrl+F8"; panel.RefreshHotkeyLabels();
            Check(hotkey.Text == "Ctrl+F8", "refresh displays the current parent-provided binding");
            Check(Field<ToolTip>(panel, "_tips").GetToolTip(hotkey)?.StartsWith("Ctrl+F8", StringComparison.Ordinal) == true, "hotkey tooltip includes the full current binding");
            Await(panel.ToggleHotkeyAsync(StatIds.Might));
            Check(requests.Count == 0, "disconnected stat hotkey cannot send a command");
            state.Stats.Single(item => item.Id == StatIds.Seal).Enabled = true;
            panel.ApplyState(state, false);
            Check(!panel.HasEnabledStats && !Field<Button>(panel, "_disableStats").Enabled, "a Seal flag from a compatible old protocol does not count as a displayed override");
            Await(panel.ToggleHotkeyAsync(StatIds.Seal)); Await(panel.ToggleHotkeyAsync("unknown"));
            Check(requests.Count == 0, "Seal and unknown stat IDs cannot be invoked");
            var target = Property<NumericUpDown>(might, "Target"); target.Value = 123.4m;
            Await(panel.ToggleHotkeyAsync(StatIds.Might));
            Check(requests.Count == 1 && requests[0].Kind == "setStat" && requests[0].Stat == StatIds.Might && requests[0].Enabled == true && requests[0].Value == 123.4, "hotkey enables the stat using its currently edited target");
            Check(state.Stats.Single(item => item.Id == StatIds.Might).Enabled && !Property<bool>(might, "Dirty"), "confirmed enable state clears the submitted dirty value");
            Await(panel.ToggleHotkeyAsync(StatIds.Might));
            Check(requests[^1].Enabled == false && requests[^1].Value is null, "the next hotkey turns the actual active stat off without a target value");
            int count = requests.Count;
            panel.SetBlocked(true); Await(panel.ToggleHotkeyAsync(StatIds.Might));
            Check(requests.Count == count, "blocked UI prevents a stat hotkey command");
            panel.SetBlocked(false); state.PlayerReady = false; panel.ApplyState(state, false); Await(panel.ToggleHotkeyAsync(StatIds.Might));
            Check(requests.Count == count, "menu state prevents a gameplay stat hotkey");
            state.PlayerReady = true; state.Stats.Single(item => item.Id == StatIds.Might).Supported = false; panel.ApplyState(state, false); Await(panel.ToggleHotkeyAsync(StatIds.Might));
            Check(requests.Count == count, "unsupported stat cannot send a command");
            state.Stats.Single(item => item.Id == StatIds.Might).Supported = true; panel.ApplyState(state, false);
            var search = Field<TextBox>(panel, "_search"); search.Text = "armor";
            Check(!Property<TableLayoutPanel>(might, "Panel").Visible, "query hides an unrelated stat row");
            Check(Field<TableLayoutPanel>(panel, "_table").Height == 69, "filter height equals the single matching row");
            Pump(); object armorRow = rows[StatIds.Armor]!;
            Check(CenterX(Property<Label>(armorRow,"Original")) == CenterX(heading.Controls[2]), "numeric header and row centers still match after filtering removes the scrollbar");
            Await(panel.ToggleHotkeyAsync(StatIds.Might));
            Check(requests.Count == count + 1 && requests[^1].Enabled == true && search.Text == "armor" && !Property<TableLayoutPanel>(might, "Panel").Visible, "a bound stat remains usable while filtered out and does not alter the query");
            panel.Visible = false; Await(panel.ToggleHotkeyAsync(StatIds.Might)); panel.Visible = true;
            Check(requests[^1].Enabled == false, "a bound stat remains usable when its tab is hidden");
            pending = new(TaskCreationOptions.RunContinuationsAsynchronously);
            Task first = panel.ToggleHotkeyAsync(StatIds.Might);
            count = requests.Count;
            Await(panel.ToggleHotkeyAsync(StatIds.Might));
            Check(!first.IsCompleted && requests.Count == count && !Property<Button>(might, "Toggle").Enabled, "a pending stat request rejects repeated hotkey activation");
            pending.SetResult(); Await(first); pending = null;
            search.Clear(); target.Value = 456.7m;
            Check(Field<TableLayoutPanel>(panel, "_table").Height == 23 * 69, "clearing the filter restores all rows and scroll extent");
            UiLocalization.SetLanguage("en"); Pump();
            Check(target.Value == 456.7m && Property<bool>(might, "Dirty") && hotkey.Text == "Ctrl+F8", "English switching preserves the pending value, dirty flag and binding text");
            Check(Property<Button>(rows[StatIds.Armor]!, "HotkeyButton").Text == "Unbound", "unbound caption is localized to English");
            int textChanges = 0;
            foreach (var control in Descendants(panel).Where(control => control is not TextBoxBase && control is not NumericUpDown)) control.TextChanged += (_, _) => textChanges++;
            for (int index = 0; index < 20; index++) { panel.ApplyState(state, false); Pump(); }
            Check(textChanges == 0, "identical state refreshes do not rewrite English status or hotkey text");
            search.Text = "might";
            var rail = target.Parent!;
            Check(bodyPanel.Controls.Count == 4 && rail != bodyPanel && new Control[] { Property<Label>(might,"Original"), Property<Label>(might,"Current"), Property<Button>(might,"Apply"), Property<Button>(might,"Toggle") }.All(control => control.Parent == rail),
                "the five fixed right-side controls share one rail while their public row references remain intact");
            Rectangle localTargetBounds = target.Bounds;
            int childMoves = 0, railMoves = 0;
            target.LocationChanged += (_, _) => childMoves++;
            rail.LocationChanged += (_, _) => railMoves++;
            bindings["stat:" + StatIds.Might] = "Ctrl+Alt+Shift+F24"; panel.RefreshHotkeyLabels(); Pump();
            Check(TextRenderer.MeasureText(hotkey.Text, hotkey.Font).Width <= hotkey.ClientSize.Width - 8, "the longest modifier chord fits without truncation");
            Check(rows.Values.Cast<object>().All(row => Property<TableLayoutPanel>(row,"Panel").ColumnStyles[0].Width == heading.ColumnStyles[0].Width), "the longest caption updates all rows and the header to the same hotkey width");
            int previousTargetLeft = int.MinValue;
            foreach (int width in new[] { 980, 1110, 1280 })
            {
                host.ClientSize = new Size(width, 530); Pump();
                int targetLeft = host.PointToClient(target.PointToScreen(Point.Empty)).X;
                Check(host.ClientSize.Width == width && panel.Width == width && CenterX(Property<Label>(might,"Original")) == CenterX(heading.Controls[2]) && targetLeft > previousTargetLeft
                    && target.PointToScreen(new Point(target.Width,0)).X <= bodyPanel.PointToScreen(new Point(bodyPanel.DisplayRectangle.Right,0)).X,
                    $"visible numeric controls and headers follow each resize width without overlap (requested {width}, host {host.Width}, window {window.Width}, panel {panel.Width}, target {targetLeft}, previous {previousTargetLeft}, DPI {host.DeviceDpi})");
                previousTargetLeft = targetLeft;
                using var widthBitmap = new Bitmap(host.ClientSize.Width, host.ClientSize.Height);
                panel.DrawToBitmap(widthBitmap, new Rectangle(Point.Empty, widthBitmap.Size)); widthBitmap.Save(Path.Combine(output, $"panel-en-{width}.png"));
            }
            Check(childMoves == 0 && railMoves > 0 && target.Bounds == localTargetBounds, "resizing moves the fixed rail instead of moving each native editor individually");
            using (var bitmap = new Bitmap(host.ClientSize.Width, host.ClientSize.Height)) { panel.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size)); bitmap.Save(Path.Combine(output, "panel-en.png")); }
            UiLocalization.SetLanguage("zh-CN"); Pump();
            Check(target.Value == 456.7m && Property<bool>(might, "Dirty") && search.Text == "might", "switching back preserves the edited value, dirty flag and query");
            using (var bitmap = new Bitmap(host.ClientSize.Width, host.ClientSize.Height)) { panel.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size)); bitmap.Save(Path.Combine(output, "panel-zh.png")); }
            search.Clear(); host.ClientSize = new Size(1110, 530); Pump();
            var scroll = Field<Panel>(panel, "_scroll");
            object lastRow = rows[StatIds.Omni]!; var lastPanel = (FastStatColumnsPanel)Property<TableLayoutPanel>(lastRow,"Panel");
            Check(lastPanel.Visible && lastPanel.NeedsViewportLayout, "offscreen rows stay logically visible and retain pending child geometry");
            scroll.AutoScrollPosition = new Point(0, lastPanel.Bottom); Pump();
            Check(!lastPanel.NeedsViewportLayout && CenterX(Property<Label>(lastRow,"Original")) == CenterX(heading.Controls[2]), "scrolling to the bottom lays out newly visible children before painting");
            host.ClientSize = new Size(1280, 530); Pump();
            Check(CenterX(Property<Label>(lastRow,"Original")) == CenterX(heading.Controls[2]) && ((FastStatColumnsPanel)bodyPanel).NeedsViewportLayout,
                "bottom visible controls follow resizing while offscreen top children are deferred");
            scroll.AutoScrollPosition = Point.Empty; Pump();
            Check(!((FastStatColumnsPanel)bodyPanel).NeedsViewportLayout && CenterX(Property<Label>(might,"Original")) == CenterX(heading.Controls[2]), "scrolling back to the top flushes deferred children at the current width");
            search.Text = "omni"; Pump();
            Check(!lastPanel.NeedsViewportLayout && CenterX(Property<Label>(lastRow,"Original")) == CenterX(heading.Controls[2]), "filtering a former offscreen row into the viewport updates its child layout");
            count = requests.Count; Await(panel.ToggleHotkeyAsync(StatIds.Might));
            Check(requests.Count == count + 1 && search.Text == "omni", "offscreen and filtered hotkeys still use their retained row model");
            search.Clear(); scroll.AutoScrollPosition = Point.Empty; host.ClientSize = new Size(1110,530); Pump();
            int oldOffscreenWidth = lastPanel.Width, offscreenSizeChanges = 0;
            lastPanel.SizeChanged += (_, _) => offscreenSizeChanges++;
            host.ClientSize = new Size(1280,530); Pump();
            Check(lastPanel.Width == oldOffscreenWidth && offscreenSizeChanges == 0 && lastPanel.Top == 22 * 69,
                "offscreen row windows retain native width without losing their logical vertical position");
            var lastTarget = Property<NumericUpDown>(lastRow,"Target");
            Check(lastTarget.Focus(), "a logically visible offscreen editor remains focusable"); Pump();
            Check(lastTarget.Focused && !lastPanel.NeedsViewportLayout && CenterX(Property<Label>(lastRow,"Original")) == CenterX(heading.Controls[2]),
                "focusing an offscreen editor scrolls in and places the row before use");
            var table = Field<TableLayoutPanel>(panel,"_table");
            foreach (RowStyle style in table.RowStyles) if (style.Height > 0) style.Height *= 1.5f;
            float scaledRowHeight = table.RowStyles[0].Height;
            decimal savedTarget = target.Value;
            search.Text = "no-matching-stat"; Pump();
            Check(table.Height == table.Padding.Vertical, "filtering every scaled row out keeps a zero content extent");
            search.Clear(); Pump();
            Check(table.RowStyles.Cast<RowStyle>().All(style => style.Height == scaledRowHeight)
                && table.Height == 23 * (int)Math.Round(scaledRowHeight) + table.Padding.Vertical && target.Value == savedTarget,
                "filter restore preserves actual scaled row heights and the pending numeric value");
            panel.SetDisconnected(); count = requests.Count; Await(panel.ToggleHotkeyAsync(StatIds.Might));
            Check(requests.Count == count && !panel.HasEnabledStats, "disconnect clears active UI and blocks hotkey commands");
            var result = new { passed = _passed, failed = 0, rows = rows.Count, requests = requests.Count, gameConnected = false };
            File.WriteAllText(resultPath, JsonSerializer.Serialize(result, new JsonSerializerOptions { WriteIndented = true }));
            Console.WriteLine(JsonSerializer.Serialize(result));
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            File.WriteAllText(resultPath, JsonSerializer.Serialize(new { passed = _passed, failed = 1, error = error.ToString(), gameConnected = false }, new JsonSerializerOptions { WriteIndented = true }));
            return 1;
        }
    }

    private static T Field<T>(object instance, string name) => (T)instance.GetType().GetField(name, Fields)!.GetValue(instance)!;
    private static int CenterX(Control control) => control.PointToScreen(new Point(control.Width / 2, 0)).X;
    private static T Property<T>(object instance, string name) => (T)instance.GetType().GetProperty(name, Fields)!.GetValue(instance)!;
    private static void Check(bool condition, string description) { if (!condition) throw new InvalidOperationException(description); _passed++; Console.WriteLine("PASS " + description); }
    private static IEnumerable<Control> Descendants(Control control) { foreach (Control child in control.Controls) { yield return child; foreach (var inner in Descendants(child)) yield return inner; } }
    private static void Pump() { Application.DoEvents(); Thread.Sleep(1); }
    private static void Await(Task task)
    {
        var watch = Stopwatch.StartNew();
        while (!task.IsCompleted) { Pump(); if (watch.ElapsedMilliseconds > 5000) throw new TimeoutException("Synthetic request timed out."); }
        task.GetAwaiter().GetResult();
    }
}
