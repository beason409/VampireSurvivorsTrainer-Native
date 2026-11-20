#if !BASELINE
using System.Text.Json;
using Trainer.Desktop;
using Trainer.Shared;

internal static partial class Program
{
    private static void LocalizationChecks(bool benchmark)
    {
        int passedBefore = _passed;
        UiLocalization.SetLanguage("zh-CN");
        try
        {
            LocalizationSettingsChecks();
            LocalizationTextChecks();
            LocalizationControlChecks();
            LocalizationDeferredHandleChecks();
            LocalizationRewardChecks();
            if (benchmark)
            {
                UiLocalization.SetLanguage("en");
                MeasureTyping(SyntheticCatalog(344), "344-synthetic-english");
                MeasureTyping(SyntheticCatalog(4096), "catalog-cap-synthetic-english");
            }
            Console.WriteLine($"Localization: {_passed - passedBefore} checks passed.");
        }
        finally
        {
            UiLocalization.SetLanguage("zh-CN");
            PumpLocalizationEvents();
        }
    }

    private static void LocalizationSettingsChecks()
    {
        string directory = Path.Combine(Path.GetTempPath(), "Trainer.Localization.Tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        string legacy = Path.Combine(directory, "legacy.json"), saved = Path.Combine(directory, "saved.json"), unknown = Path.Combine(directory, "unknown.json");
        try
        {
            Check(new DesktopSettings().Language == "zh-CN" && DesktopSettings.Load(Path.Combine(directory, "missing.json")).Language == "zh-CN", "new or missing settings default to Chinese");
            var original = new DesktopSettings { GameDirectory = @"X:\Fixture Games\吸血鬼幸存者" };
            original.Hotkeys[FeatureIds.Health] = (int)(Keys.Control | Keys.H);
            original.Values[FeatureIds.Experience] = 27;
            original.Values[FeatureIds.Damage] = 31;
            original.Values[FeatureIds.Movement] = 2.5m;
            var legacyData = JsonSerializer.SerializeToElement(original, PipeProtocol.JsonOptions).EnumerateObject()
                .Where(property => !property.Name.Equals("language", StringComparison.OrdinalIgnoreCase))
                .ToDictionary(property => property.Name, property => property.Value.Clone());
            File.WriteAllText(legacy, JsonSerializer.Serialize(legacyData));
            var migrated = DesktopSettings.Load(legacy);
            Check(migrated.Language == "zh-CN", "legacy settings without a language migrate to Chinese");
            Check(SameSettingsData(migrated, original), "language migration preserves game path, custom hotkeys, and multiplier values");
            migrated.Language = "en";
            migrated.Save(saved);
            var english = DesktopSettings.Load(saved);
            Check(english.Language == "en" && SameSettingsData(english, original), "English language survives an actual settings save/load without changing user configuration");
            english.Language = "unsupported-language";
            english.Save(unknown);
            var fallback = DesktopSettings.Load(unknown);
            Check(fallback.Language == "zh-CN" && SameSettingsData(fallback, original), "unknown stored language falls back to Chinese and preserves other settings");
            UiLocalization.SetLanguage("en");
            Check(UiLocalization.IsEnglish && UiLocalization.Language == "en", "English selection exposes a consistent current language");
            UiLocalization.SetLanguage("unsupported-language");
            Check(!UiLocalization.IsEnglish && UiLocalization.Language == "zh-CN", "unknown runtime language falls back to Chinese");
        }
        finally
        {
            foreach (string path in new[] { legacy, saved, unknown, legacy + ".tmp", saved + ".tmp", unknown + ".tmp" })
                if (File.Exists(path)) File.Delete(path);
            Directory.Delete(directory);
        }
    }

    private static bool SameSettingsData(DesktopSettings left, DesktopSettings right) => left.GameDirectory == right.GameDirectory
        && left.Hotkeys.OrderBy(pair => pair.Key).SequenceEqual(right.Hotkeys.OrderBy(pair => pair.Key))
        && left.Values.OrderBy(pair => pair.Key).SequenceEqual(right.Values.OrderBy(pair => pair.Key));

    private static void LocalizationTextChecks()
    {
        UiLocalization.SetLanguage("en");
        Check(UiLocalization.Translate("游戏版本已验证") == "Game version verified", "embedded Core English resources are available to the actual localization engine");
        const string unchanged = @"ITEM_ID_42 X:\Games\Vampire Survivors\SaveData";
        Check(UiLocalization.Translate(unchanged) == unchanged, "untranslated IDs and file paths remain unchanged");
        string summary = UiLocalization.Translate("显示 2 / 344 项 · 来源：当前游戏数据");
        Check(!ContainsChinese(summary) && summary.Contains('2') && summary.Contains("344"), "formatted catalog summary translates while retaining both dynamic counts");
        string outcome = UiLocalization.Translate("已生成指定选项 2 次 · 最近：Sample Sword（仍需手动选择）");
        Check(!ContainsChinese(outcome) && outcome.Contains('2') && outcome.Contains("Sample Sword"), "formatted upgrade status translates and retains item names and counts");
        const string path = @"X:\Games\Vampire Survivors\SaveData";
        Check(UiLocalization.Translate("文件内容无效：" + path) == "Invalid file contents: " + path, "Core error prefixes translate without changing appended file paths");
        string nested = UiLocalization.Translate("无法验证游戏版本：" + "目录中缺少此游戏的 EXE、GameAssembly 或 IL2CPP 元数据。");
        Check(!ContainsChinese(nested) && nested.Contains("GameAssembly") && nested.Contains("IL2CPP"), "nested Core error messages translate without changing technical identifiers");
        UiLocalization.SetLanguage("zh-CN");
        Check(UiLocalization.Translate("文件内容无效：" + path) == "文件内容无效：" + path, "Chinese mode preserves original dynamic messages exactly");
    }

    private static void LocalizationControlChecks()
    {
        UiLocalization.SetLanguage("zh-CN");
        using var host = new Form { Text = "吸血鬼幸存者修改器", ClientSize = new Size(500, 400), ShowInTaskbar = false, StartPosition = FormStartPosition.Manual, Location = new Point(-32000, -32000) };
        var label = new Label { Text = "游戏版本已验证", Location = new Point(10, 10), Size = new Size(450, 25) };
        var button = new Button { Text = "应用目标", Location = new Point(10, 40) };
        using var tips = new LocalizedToolTip();
        tips.SetToolTip(button, "游戏版本已验证");
        var input = new TextBox { Text = "power 中文 42", PlaceholderText = "搜索中文名、英文名或物品 ID", Location = new Point(10, 75), Width = 300 };
        var number = new NumericUpDown { Minimum = 0, Maximum = 1000, DecimalPlaces = 1, Value = 128.5m, Location = new Point(10, 110) };
        var kind = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Location = new Point(10, 145) };
        kind.Items.AddRange(new object[] { "全部物品", "武器", "被动装备" }); kind.SelectedIndex = 1;
        var list = new ListView { View = View.Details, Location = new Point(10, 180), Size = new Size(450, 120) };
        list.Columns.Add("物品名称", 180); list.Columns.Add("英文名称", 180);
        host.Controls.AddRange(new Control[] { label, button, input, number, kind, list });
        host.Show();
        input.Select(2, 3);
        UiLocalization.Attach(host);
        UiLocalization.SetLanguage("en");
        PumpLocalizationEvents();
        Check(host.Text == "Vampire Survivors Trainer" && label.Text == "Game version verified" && !ContainsChinese(button.Text), "an attached visible control tree translates form, label, and button captions");
        Check(!ContainsChinese(input.PlaceholderText) && input.PlaceholderText.Length > 0, "search placeholders translate independently of user input");
        Check(input.Text == "power 中文 42" && input.SelectionStart == 2 && input.SelectionLength == 3 && number.Value == 128.5m, "language switching preserves typed text, text selection, and numeric values");
        Check(kind.SelectedIndex == 1 && kind.Items.Cast<object>().All(item => !ContainsChinese(kind.GetItemText(item)!)), "translated dropdown display options retain their selected semantic index");
        Check(list.Columns.Cast<ColumnHeader>().All(column => !ContainsChinese(column.Text)), "native list headers translate with their parent control tree");
        Check(tips.GetToolTip(button) == "Game version verified", "existing tooltips switch to English with their controls");
        const string path = @"X:\Games\Vampire Survivors\SaveData";
        label.Text = "文件内容无效：";
        label.Text += path;
        tips.SetToolTip(button, "文件内容无效：" + path);
        PumpLocalizationEvents();
        Check(label.Text == "Invalid file contents: " + path, "consecutive dynamic Text assignments are translated after the complete message is assembled");
        Check(tips.GetToolTip(button) == "Invalid file contents: " + path, "tooltips assigned while English is active translate immediately and retain file paths");
        UiLocalization.SetLanguage("zh-CN");
        PumpLocalizationEvents();
        Check(host.Text == "吸血鬼幸存者修改器" && button.Text == "应用目标" && input.PlaceholderText == "搜索中文名、英文名或物品 ID", "switching back restores the original Chinese captions and placeholder");
        Check(label.Text == "文件内容无效：" + path && kind.Items[1]?.ToString() == "武器" && kind.SelectedIndex == 1 && list.Columns[0].Text == "物品名称", "switching back restores current dynamic text, dropdown values, and headers");
        Check(input.Text == "power 中文 42" && number.Value == 128.5m, "round-trip switching leaves editable user values unchanged");
        Check(tips.GetToolTip(button) == "文件内容无效：" + path, "switching back restores the latest tooltip's Chinese source");
    }

    private static void LocalizationDeferredHandleChecks()
    {
        UiLocalization.SetLanguage("en");
        using var host = new Form { ShowInTaskbar = false, StartPosition = FormStartPosition.Manual, Location = new Point(-32000, -32000) };
        var status = new Label { Text = "游戏版本已验证" };
        host.Controls.Add(status);
        UiLocalization.Attach(host);
        Check(!host.IsHandleCreated && status.Text == "Game version verified", "English controls can be initialized before a native window exists");
        const string path = @"X:\Games\Vampire Survivors\SaveData";
        status.Text = "文件内容无效：" + path;
        _ = host.Handle;
        _ = status.Handle;
        PumpLocalizationEvents();
        Check(status.Text == "Invalid file contents: " + path, "a status changed before handle creation translates when the native window becomes available");
        var added = new Label { Text = "备份证明路径无效。" };
        host.Controls.Add(added);
        PumpLocalizationEvents();
        Check(added.Text == "The backup verification record path is invalid.", "controls added to an already-attached tree inherit the active language");
        UiLocalization.SetLanguage("zh-CN");
        PumpLocalizationEvents();
        Check(status.Text == "文件内容无效：" + path && added.Text == "备份证明路径无效。", "startup-deferred and dynamically added controls both restore their Chinese sources");
    }

    private static void LocalizationRewardChecks()
    {
        UiLocalization.SetLanguage("zh-CN");
        int requests = 0, iconRequests = 0;
        byte[] png = IconPng(Color.Red);
        using var host = new Form { ClientSize = new Size(1110, 530), ShowInTaskbar = false, StartPosition = FormStartPosition.Manual, Location = new Point(-32000, -32000), Font = new Font("Microsoft YaHei UI", 9F) };
        using var panel = new ChestRewardPanel((_, _) => { requests++; return Task.CompletedTask; }, id => { iconRequests++; return Task.FromResult<RewardItemIcon?>(new() { Id = id, Png = png }); });
        host.Controls.Add(panel);
        host.Show();
        var definitions = new[]
        {
            new RewardItemDefinition { Id = "ICON_SWORD", Name = "示例武器", EnglishName = "Sample Sword", Kind = "weapon", UpgradeAllowed = true },
            new RewardItemDefinition { Id = "ICON_PASSIVE", Name = "示例被动", EnglishName = "Sample Passive", Kind = "passive", UpgradeAllowed = true }
        };
        panel.SetCatalogPage(definitions, definitions.Length, 0);
        var state = new TrainerState { PlayerReady = true, ChestReward = new() { Supported = true, UpgradeSupported = true } };
        state.Diagnostics["chest.catalog.icons"] = "supported";
        panel.ApplyState(state, false);
        var timer = Field<System.Windows.Forms.Timer>(panel, "_iconTimer"); timer.Stop();
        var search = Field<TextBox>(panel, "_search");
        var kind = Field<ComboBox>(panel, "_kind");
        var list = Field<ListView>(panel, "_items");
        search.Text = "示例"; kind.SelectedIndex = 1;
        list.Items[0].Selected = true; list.Items[0].Focused = true;
        InvokeTask(panel, "LoadVisibleIconsAsync");
        UiLocalization.Attach(host);
        PumpLocalizationEvents(); timer.Stop();
        int imageIndex = list.Items[0].ImageIndex;
        int images = Field<ImageList>(panel, "_icons").Images.Count;
        int generation = Field<int>(panel, "_iconGeneration");
        var cache = Field<Dictionary<string, int>>(panel, "_iconIndices").ToDictionary(pair => pair.Key, pair => pair.Value);
        int requestsBefore = requests, iconsBefore = iconRequests;
        Check(imageIndex > 0 && images == 2 && iconRequests == 1, "language regression fixture has one real decoded synthetic icon and an explicit selected target");
        UiLocalization.SetLanguage("en");
        PumpLocalizationEvents(); timer.Stop();
        Check(search.Text == "示例" && kind.SelectedIndex == 1 && list.Items.Count == 1 && Field<RewardItemDefinition?>(panel, "_selected")?.Id == "ICON_SWORD", "English switching preserves a Chinese query, kind filter, and selected item ID");
        Check(list.Items[0].ImageIndex == imageIndex && Field<ImageList>(panel, "_icons").Images.Count == images && Field<int>(panel, "_iconGeneration") == generation
            && Field<Dictionary<string, int>>(panel, "_iconIndices").OrderBy(pair => pair.Key).SequenceEqual(cache.OrderBy(pair => pair.Key)), "English switching preserves decoded images and the item-ID icon cache");
        Check(!ContainsChinese(Field<Label>(panel, "_summary").Text) && !ContainsChinese(Field<Label>(panel, "_status").Text) && !ContainsChinese(list.Columns[0].Text), "visible reward summary, state, and list header show English");
        state.ChestReward.Message = "游戏版本已验证";
        panel.ApplyState(state, false); timer.Stop();
        panel.SetOperationError("安装包缺少 BepInEx 加载文件。");
        PumpLocalizationEvents();
        Check(!ContainsChinese(Field<Label>(panel, "_status").Text) && !ContainsChinese(Field<Label>(panel, "_operationHint").Text), "newly received reward state and nested errors remain fully translated in English");
        UiLocalization.SetLanguage("zh-CN");
        PumpLocalizationEvents(); timer.Stop();
        Check(search.Text == "示例" && kind.SelectedIndex == 1 && list.Items.Count == 1 && Field<RewardItemDefinition?>(panel, "_selected")?.Id == "ICON_SWORD" && list.Items[0].ImageIndex == imageIndex, "switching back to Chinese retains query, filter, selection, and loaded icon");
        Check(ContainsChinese(Field<Label>(panel, "_summary").Text) && Field<Label>(panel, "_status").Text.Contains("游戏版本已验证"), "switching back renders the latest state in Chinese");
        Check(requests == requestsBefore && iconRequests == iconsBefore, "changing language never sends mutation/catalog requests or reloads a cached icon");
    }

    private static bool ContainsChinese(string text) => text.Any(character => character is >= '\u4e00' and <= '\u9fff');

    private static void PumpLocalizationEvents()
    {
        // Settle deferred TextChanged updates without advancing long-running timers.
        for (int index = 0; index < 4; index++) Application.DoEvents();
    }
}
#endif
