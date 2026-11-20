namespace Trainer.Desktop;

internal static class AppBranding
{
    private const string IconResource = "Trainer.Desktop.Assets.App.ico";

    internal static Icon CreateIcon()
    {
        using Stream stream = typeof(AppBranding).Assembly.GetManifestResourceStream(IconResource)
            ?? throw new InvalidOperationException("应用图标资源缺失。");
        using var embedded = new Icon(stream, 256, 256);
        return (Icon)embedded.Clone();
    }

    internal static Bitmap CreateMark(int size = 64)
    {
        using Stream stream = typeof(AppBranding).Assembly.GetManifestResourceStream(IconResource)
            ?? throw new InvalidOperationException("应用图标资源缺失。");
        using var icon = new Icon(stream, size, size);
        using var original = icon.ToBitmap();
        var result = new Bitmap(size, size, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
        using var graphics = Graphics.FromImage(result);
        graphics.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
        graphics.DrawImage(original, new Rectangle(0, 0, size, size));
        return result;
    }
}
