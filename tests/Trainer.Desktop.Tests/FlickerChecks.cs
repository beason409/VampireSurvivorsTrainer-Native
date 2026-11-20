#if !BASELINE
using Trainer.Desktop;
using Trainer.Shared;

internal static partial class Program
{
    private static void FlickerChecks()
    {
        int before = _passed;
        try
        {
            SetTextFlickerChecks();
            foreach (string language in new[] { "en", "zh-CN" }) RewardStateFlickerChecks(language);
            Console.WriteLine($"Flicker: {_passed - before} checks passed.");
        }
        finally
        {
            UiLocalization.SetLanguage("zh-CN");
            PumpLocalizationEvents();
        }
    }

    private static void SetTextFlickerChecks()
    {
        UiLocalization.SetLanguage("en");
        using var host = new Form { ClientSize = new Size(500, 180), ShowInTaskbar = false, StartPosition = FormStartPosition.Manual, Location = new Point(-32000, -32000) };
        var status = new Label { Dock = DockStyle.Top, Height = 30 };
        host.Controls.Add(status);
        // Production constructors can set a status before Attach is called.
        UiLocalization.SetText(status, "游戏版本已验证");
        UiLocalization.Attach(host);
        host.Show();
        PumpLocalizationEvents();
        using var events = new CaptionChanges(host);
        Check(status.Text == "Game version verified", "SetText translates an English-startup status before attaching native controls");
        for (int index = 0; index < 20; index++)
        {
            UiLocalization.SetText(status, "游戏版本已验证");
            PumpLocalizationEvents();
        }
        Check(events.Count == 0, "repeating an unchanged source through SetText raises zero TextChanged events");
        UiLocalization.SetText(status, "显示 2 / 344 项 · 来源：当前游戏数据");
        Check(events.Count == 1 && ReferenceEquals(events.Changes[0].Control, status) && !ContainsChinese(events.Changes[0].Text), "SetText writes a new formatted caption once without exposing an intermediate Chinese string");
        Check(status.Text.Contains('2') && status.Text.Contains("344"), "direct translated status updates retain dynamic numeric values");
        events.Reset();
        UiLocalization.SetText(status, "显示 3 / 344 项 · 来源：当前游戏数据");
        PumpLocalizationEvents();
        Check(events.Count == 1 && status.Text.Contains('3') && !events.Changes.Any(change => ContainsChinese(change.Text)), "changing a formatted number generates one final-language update");
        UiLocalization.SetLanguage("zh-CN");
        PumpLocalizationEvents();
        Check(status.Text == "显示 3 / 344 项 · 来源：当前游戏数据", "SetText preserves the latest source for a later Chinese language switch");
        UiLocalization.SetLanguage("en");
        PumpLocalizationEvents();
        Check(!ContainsChinese(status.Text) && status.Text.Contains('3'), "a SetText caption can switch back to English without stale text");
        UiLocalization.SetText(status, "");
        UiLocalization.SetLanguage("zh-CN");
        PumpLocalizationEvents();
        Check(status.Text.Length == 0, "clearing a status through SetText does not resurrect its previous source on language change");
        UiLocalization.SetLanguage("en");
        PumpLocalizationEvents();
        events.Reset();
        UiLocalization.SetText(status, "游戏版本已验证");
        UiLocalization.SetLanguage("zh-CN");
        PumpLocalizationEvents();
        Check(status.Text == "游戏版本已验证", "an English-startup SetText binding remembers its Chinese source after a later nonempty update");
    }

    private static void RewardStateFlickerChecks(string language)
    {
        UiLocalization.SetLanguage(language);
        using var fixture = new Fixture(SyntheticCatalog(8));
        fixture.Panel.ApplyState(fixture.State, false);
        PumpLocalizationEvents();
        using var captions = new CaptionChanges(fixture.Panel);
        using var paints = new ComboPaints(fixture.Kind);
        int requestsBefore = fixture.Requests.Count;
        for (int index = 0; index < 12; index++)
        {
            fixture.Panel.ApplyState(fixture.State, false);
            PumpLocalizationEvents();
        }
        Measurements.Add(new { dataset = "stable-reward-state-" + language, repetitions = 12, textChanged = captions.Count, comboPaints = paints.Count });
        Check(captions.Count == 0, language + " repeated identical reward state updates produce no label/button TextChanged events");
        Check(paints.Count == 0, language + " repeated identical reward state does not repaint the kind ComboBox");
        captions.Reset(); paints.Reset();
        for (int index = 0; index < 40; index++) Application.DoEvents();
        Measurements.Add(new { dataset = "idle-reward-state-" + language, pumpCycles = 40, textChanged = captions.Count, comboPaints = paints.Count });
        Check(captions.Count == 0 && paints.Count == 0, language + " idle native event pumping does not trigger a self-sustaining caption/combobox refresh loop");
        fixture.State.ChestReward.ProcessedCount = 1;
        fixture.State.ChestReward.LastRewardName = "Fixture reward";
        fixture.Panel.ApplyState(fixture.State, false);
        PumpLocalizationEvents();
        var rewardLabel = Field<Label>(fixture.Panel, "_lastReward");
        Check(captions.Count == 1 && captions.Changes.All(change => ReferenceEquals(change.Control, rewardLabel)), language + " a new reward result changes only its result caption once");
        Check(rewardLabel.Text.Contains('1') && rewardLabel.Text.Contains("Fixture reward"), language + " result updates preserve the new count and item name");
        Check(language != "en" || !captions.Changes.Any(change => ContainsChinese(change.Text)), "English reward updates never expose transient Chinese text");
        captions.Reset(); paints.Reset();
        fixture.State.ChestReward.UpgradeProcessedCount = 2;
        fixture.State.ChestReward.LastUpgradeName = "Fixture upgrade";
        fixture.Panel.ApplyState(fixture.State, false);
        PumpLocalizationEvents();
        var upgradeLabel = Field<Label>(fixture.Panel, "_lastUpgrade");
        Check(captions.Count == 1 && captions.Changes.All(change => ReferenceEquals(change.Control, upgradeLabel)), language + " an upgrade count changes only its upgrade caption once");
        Check(upgradeLabel.Text.Contains('2') && upgradeLabel.Text.Contains("Fixture upgrade"), language + " upgrade updates remain visible and preserve dynamic values");
        Check(language != "en" || !captions.Changes.Any(change => ContainsChinese(change.Text)), "English upgrade updates never expose transient Chinese text");
        captions.Reset(); paints.Reset();
        for (int index = 0; index < 12; index++) { fixture.Panel.ApplyState(fixture.State, false); PumpLocalizationEvents(); }
        Check(captions.Count == 0 && paints.Count == 0, language + " the new state settles with no repeated text or combobox redraws");
        Check(fixture.Requests.Count == requestsBefore, language + " repeated status rendering never sends game requests");
    }

    private sealed class CaptionChanges : IDisposable
    {
        private readonly List<Control> _controls = new();
        internal readonly List<(Control Control, string Text)> Changes = new();
        internal int Count => Changes.Count;
        internal CaptionChanges(Control root) => Bind(root);
        private void Bind(Control control)
        {
            if (control is Label or Button)
            {
                _controls.Add(control);
                control.TextChanged += Changed;
            }
            foreach (Control child in control.Controls) Bind(child);
        }
        private void Changed(object? sender, EventArgs e)
        {
            var control = (Control)sender!;
            Changes.Add((control, control.Text));
        }
        internal void Reset() => Changes.Clear();
        public void Dispose() { foreach (var control in _controls) control.TextChanged -= Changed; }
    }

    private sealed class ComboPaints : NativeWindow, IDisposable
    {
        internal int Count { get; private set; }
        internal ComboPaints(ComboBox combo) => AssignHandle(combo.Handle);
        protected override void WndProc(ref Message message)
        {
            if (message.Msg == 0x000F) Count++; // WM_PAINT
            base.WndProc(ref message);
        }
        internal void Reset() => Count = 0;
        public void Dispose() { if (Handle != IntPtr.Zero) ReleaseHandle(); }
    }
}
#endif
