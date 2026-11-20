using System.Diagnostics;
using Trainer.Core;

namespace Trainer.Desktop;

internal sealed class ReleaseInfoDialog : Form
{
    private readonly TextBox _verification = new();
    private readonly Action<string> _launchLocation;

    internal ReleaseInfoDialog(string dataDirectory) : this(dataDirectory,
        target => Process.Start(new ProcessStartInfo(target) { UseShellExecute = true })) { }

    internal ReleaseInfoDialog(string dataDirectory, Action<string> launchLocation)
    {
        _launchLocation = launchLocation ?? throw new ArgumentNullException(nameof(launchLocation));
        Text = "说明与来源 · Vampire Survivors Trainer";
        Icon = AppBranding.CreateIcon();
        StartPosition = FormStartPosition.CenterParent;
        ClientSize = new Size(820, 780);
        MinimumSize = new Size(760, 720);
        BackColor = Color.FromArgb(18, 22, 28);
        ForeColor = Color.FromArgb(233, 238, 246);
        Font = new Font("Microsoft YaHei UI", 9F);
        AutoScaleMode = AutoScaleMode.Dpi;
        MinimizeBox = false;
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 6, Padding = new Padding(22) };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 66));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 90));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 106));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 66));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 44));
        Controls.Add(layout);
        var brand = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1, Margin = Padding.Empty };
        brand.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 68));
        brand.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        brand.Controls.Add(new PictureBox { Image = AppBranding.CreateMark(56), SizeMode = PictureBoxSizeMode.CenterImage,
            Dock = DockStyle.Fill, Margin = new Padding(0, 0, 10, 10) }, 0, 0);
        brand.Controls.Add(new Label { Text = $"Vampire Survivors Trainer  {ReleaseInfo.Version}\n作者署名：{ReleaseInfo.Author}", Dock = DockStyle.Fill,
            ForeColor = ForeColor, Font = new Font(Font.FontFamily, 13, FontStyle.Bold), AutoEllipsis = true }, 1, 0);
        layout.Controls.Add(brand, 0, 0);
        var repository = ReleaseInfo.RepositoryUri;
        layout.Controls.Add(ReadOnlyText(
            "产品与用途\r\nVampire Survivors 的本机单人游戏辅助工具，可调整关卡属性和指定奖励。\r\n\r\n"
            + "版本兼容\r\n支持 Windows x64 / Steam 普通单人模式，仅适用于发行包列明的游戏版本。配套插件随包提供；自包含版本无需另装 .NET。\r\n\r\n"
            + "本机修改\r\n准备时校验游戏版本并备份存档，再向游戏目录安装加载器和 BepInEx 插件。多数功能影响当前关卡；永久金币设值会保存。卸载依据安装记录处理。\r\n\r\n"
            + "隐私\r\n通过当前用户的本机管道连接游戏，不上传游戏或存档数据，不收集使用遥测。\r\n\r\n"
            + $"来源与许可\r\n作者署名：{ReleaseInfo.Author}。源码与发行来源见项目主页；使用、修改和再分发以随包许可文件为准。\r\n"
            + (repository is null ? "此构建未配置项目主页。" : "项目主页：" + repository.AbsoluteUri)), 0, 1);
        var dataArea = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2, BackColor = Color.FromArgb(28, 34, 43), Padding = new Padding(10, 7, 10, 7), Margin = new Padding(0, 0, 0, 10) };
        dataArea.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        dataArea.RowStyles.Add(new RowStyle(SizeType.Absolute, 23));
        dataArea.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        dataArea.Controls.Add(new Label { Text = "本机数据保存", Dock = DockStyle.Fill, ForeColor = ForeColor, Font = new Font(Font, FontStyle.Bold) }, 0, 0);
        dataArea.Controls.Add(new Label { Text = "窗口设置、存档备份和安装记录仅保存在本机。可用下方按钮打开数据目录。", Dock = DockStyle.Fill, ForeColor = Color.FromArgb(155, 169, 189) }, 0, 1);
        layout.Controls.Add(dataArea, 0, 2);
        layout.Controls.Add(ReadOnlyText("Windows 代码签名\r\n" + ReleaseInfo.SignatureDescription()), 0, 3);
        _verification.ReadOnly = true; _verification.Multiline = true; _verification.Dock = DockStyle.Fill; _verification.TabStop = false;
        _verification.BorderStyle = BorderStyle.None; _verification.BackColor = BackColor; _verification.ForeColor = Color.FromArgb(155, 169, 189);
        _verification.Text = "安装和启动前自动核对内置配套文件指纹。此处还可校验整个发行目录。文件自检不等于 Windows 代码签名，也不保证安全软件零误报。";
        _verification.ScrollBars = ScrollBars.Vertical; layout.Controls.Add(_verification, 0, 4);
        var buttons = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.LeftToRight, WrapContents = false, Margin = Padding.Empty };
        var source = Button("查看源码", 110);
        source.Enabled = repository is not null;
        source.Click += (_, _) => OpenLocation(repository!.AbsoluteUri);
        buttons.Controls.Add(source);
        var verify = Button("校验发行文件", 130);
        verify.Click += async (_, _) =>
        {
            verify.Enabled = false;
            UiLocalization.SetText(_verification, "正在核对本地发行文件，请稍等…");
            try
            {
                string result = await Task.Run(() =>
                {
                    var catalog = TrustedPayloadCatalog.LoadEmbedded();
                    catalog.VerifyPayload(RuntimePaths.PayloadDirectory);
                    catalog.ReadSupportedBuild(RuntimePaths.SupportedBuildPath);
                    return ReleaseInfo.VerifyPackage(AppContext.BaseDirectory);
                });
                if (!IsDisposed) UiLocalization.SetText(_verification, result);
            }
            catch (Exception) { if (!IsDisposed) UiLocalization.SetText(_verification, "校验未完成。请确认发行包已完整解压，并从项目发行页重新下载后重试。"); }
            finally { if (!IsDisposed) verify.Enabled = true; }
        };
        buttons.Controls.Add(verify);
        var data = Button("打开数据目录", 130);
        data.Enabled = Directory.Exists(dataDirectory);
        data.Click += (_, _) => OpenLocation(dataDirectory);
        buttons.Controls.Add(data);
        var close = Button("关闭窗口", 82);
        close.DialogResult = DialogResult.OK; buttons.Controls.Add(close);
        AcceptButton = close; CancelButton = close; layout.Controls.Add(buttons, 0, 5);
        UiLocalization.Attach(this);
        Shown += (_, _) => close.Focus();
    }

    private TextBox ReadOnlyText(string text) => new()
    {
        Text = text, ReadOnly = true, Multiline = true, TabStop = false, Dock = DockStyle.Fill, BorderStyle = BorderStyle.None,
        BackColor = Color.FromArgb(28, 34, 43), ForeColor = ForeColor, ScrollBars = ScrollBars.Vertical, Margin = new Padding(0, 0, 0, 10)
    };

    private static Button Button(string text, int width) => new DarkButton
    {
        Text = text, Width = width, Height = 32, FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(39, 47, 58),
        ForeColor = Color.FromArgb(233, 238, 246), Margin = new Padding(0, 4, 10, 0), Cursor = Cursors.Hand, UseVisualStyleBackColor = false
    };

    private void OpenLocation(string target)
    {
        try { _launchLocation(target); }
        catch (Exception) { UiLocalization.SetText(_verification, "未能打开。请检查本机访问权限后重试。"); }
    }

    internal void RenderPreview(string? path)
    {
        _ = Handle;
        void CreateChildren(Control parent)
        {
            parent.PerformLayout();
            foreach (Control child in parent.Controls) { _ = child.Handle; CreateChildren(child); }
        }
        CreateChildren(this);
        using var bitmap = new Bitmap(Width, Height);
        DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size));
        if (path is null) return;
        string full = Path.GetFullPath(path);
        Directory.CreateDirectory(Path.GetDirectoryName(full)!);
        bitmap.Save(full, System.Drawing.Imaging.ImageFormat.Png);
    }
}
