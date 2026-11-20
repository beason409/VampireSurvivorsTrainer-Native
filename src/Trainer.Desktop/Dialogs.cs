namespace Trainer.Desktop;

internal static class LocalizedMessageBox
{
    internal static DialogResult Show(string text, string caption, MessageBoxButtons buttons, MessageBoxIcon icon) =>
        UiLocalization.IsEnglish ? ShowEnglish(null, text, caption, buttons) : MessageBox.Show(text, caption, buttons, icon);
    internal static DialogResult Show(IWin32Window owner, string text, string caption, MessageBoxButtons buttons, MessageBoxIcon icon) =>
        UiLocalization.IsEnglish ? ShowEnglish(owner, text, caption, buttons) : MessageBox.Show(owner, text, caption, buttons, icon);

    private static DialogResult ShowEnglish(IWin32Window? owner, string text, string caption, MessageBoxButtons buttons)
    {
        string translated = UiLocalization.Translate(text);
        using var dialog = new Form { Text = UiLocalization.Translate(caption), StartPosition = FormStartPosition.CenterParent,
            BackColor = Color.FromArgb(18, 22, 28), ForeColor = Color.FromArgb(233, 238, 246), Font = new Font("Segoe UI", 9F),
            FormBorderStyle = FormBorderStyle.FixedDialog, MinimizeBox = false, MaximizeBox = false, ShowInTaskbar = false };
        int textHeight = Math.Clamp(TextRenderer.MeasureText(translated, dialog.Font, new Size(560, 1000), TextFormatFlags.WordBreak).Height + 24, 80, 430);
        dialog.ClientSize = new Size(600, textHeight + 72);
        var content = new TextBox { Text = translated, ReadOnly = true, Multiline = true, BorderStyle = BorderStyle.None,
            BackColor = dialog.BackColor, ForeColor = dialog.ForeColor, Location = new Point(20, 20), Size = new Size(560, textHeight), ScrollBars = ScrollBars.Vertical };
        dialog.Controls.Add(content);
        var bar = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 48, Padding = new Padding(12, 4, 12, 8), FlowDirection = FlowDirection.RightToLeft };
        dialog.Controls.Add(bar);
        DialogResult[] results = buttons switch
        {
            MessageBoxButtons.OKCancel => [DialogResult.OK, DialogResult.Cancel],
            MessageBoxButtons.YesNo => [DialogResult.Yes, DialogResult.No],
            MessageBoxButtons.YesNoCancel => [DialogResult.Yes, DialogResult.No, DialogResult.Cancel],
            MessageBoxButtons.RetryCancel => [DialogResult.Retry, DialogResult.Cancel],
            _ => [DialogResult.OK]
        };
        foreach (var result in results.Reverse())
        {
            var button = new DarkButton { Text = result.ToString(), DialogResult = result, Width = 96, Height = 30,
                BackColor = Color.FromArgb(39, 47, 58), ForeColor = dialog.ForeColor, FlatStyle = FlatStyle.Flat };
            bar.Controls.Add(button);
            if (result == results[0]) dialog.AcceptButton = button;
            if (result == results[^1]) dialog.CancelButton = button;
        }
        return owner is null ? dialog.ShowDialog() : dialog.ShowDialog(owner);
    }
}
