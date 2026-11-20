using Trainer.Shared;

namespace Trainer.Desktop;

internal sealed partial class ChestRewardPanel
{
    private readonly Func<string, Task<RewardItemIcon?>>? _loadIcon;
    private readonly ImageList _icons = new() { ImageSize = new Size(32, 32), ColorDepth = ColorDepth.Depth32Bit, TransparentColor = Color.Transparent };
    private readonly List<Bitmap> _iconBitmaps = new();
    private readonly Dictionary<string, int> _iconIndices = new(StringComparer.Ordinal);
    private readonly HashSet<string> _missingIcons = new(StringComparer.Ordinal);
    private readonly System.Windows.Forms.Timer _iconTimer = new() { Interval = 40 };
    private bool _iconsSupported, _loadingIcons;
    private int _iconGeneration;

    private void UpdateIconTimer()
    {
        if (_loadIcon is not null && _iconsSupported && _connected && Visible && !IsDisposed) _iconTimer.Start();
        else _iconTimer.Stop();
    }

    private void ResetIcons()
    {
        _iconGeneration++;
        _iconIndices.Clear(); _missingIcons.Clear();
        _icons.Images.Clear();
        foreach (var bitmap in _iconBitmaps) bitmap.Dispose();
        _iconBitmaps.Clear();
        var placeholder = new Bitmap(32, 32);
        using (var graphics = Graphics.FromImage(placeholder))
        using (var pen = new Pen(Color.FromArgb(75, 88, 105)))
            graphics.DrawRectangle(pen, 7, 7, 17, 17);
        _iconBitmaps.Add(placeholder); _icons.Images.Add(placeholder);
    }

    private async Task LoadVisibleIconsAsync()
    {
        if (_loadingIcons || _loadIcon is null || !_iconsSupported || !_connected || _blocked || _loading || _pending
            || !Visible || !IsHandleCreated || IsDisposed || _filteredItems.Length == 0) return;
        _loadingIcons = true;
        int generation = _iconGeneration;
        try
        {
            // Typing only filters IDs; requests, decoding and resizing happen later.
            int top = _items.TopItem?.Index ?? 0;
            int count = Math.Max(1, _items.ClientSize.Height / 32 + 2);
            var visible = _filteredItems.Skip(top).Take(count)
                .Where(item => !_iconIndices.ContainsKey(item.Id) && !_missingIcons.Contains(item.Id)).Take(4).ToArray();
            foreach (var item in visible)
            {
                if (generation != _iconGeneration || IsDisposed || !Visible || _blocked || _loading || _pending) break;
                // A query may have changed while the previous icon was in flight.
                if (!IsIconVisible(item.Id)) continue;
                RewardItemIcon? icon;
                try { icon = await _loadIcon(item.Id); }
                catch (Exception) { break; }
                if (generation != _iconGeneration || IsDisposed) break;
                if (icon is null) break; // Busy connection: retry on a later tick.
                if (icon.Id != item.Id) continue;
                Bitmap? bitmap = await Task.Run(() => DecodeIcon(icon.Png));
                if (generation != _iconGeneration || IsDisposed) { bitmap?.Dispose(); break; }
                if (bitmap is null) { _missingIcons.Add(item.Id); continue; }
                int index = _icons.Images.Count;
                _iconBitmaps.Add(bitmap); _icons.Images.Add(bitmap); _iconIndices[item.Id] = index;
                if (_rowCache.TryGetValue(item.Id, out var row)) row.ImageIndex = index;
                _items.Invalidate();
            }
        }
        finally { _loadingIcons = false; }
    }

    private bool IsIconVisible(string id)
    {
        int top = _items.TopItem?.Index ?? 0;
        return _filteredItems.Skip(top).Take(Math.Max(1, _items.ClientSize.Height / 32 + 2)).Any(item => item.Id == id);
    }

    private static Bitmap? DecodeIcon(byte[]? png)
    {
        // Check dimensions before asking GDI+ to allocate decoded pixels.
        if (png is null || png.Length is < 33 or > 32768 || !png.AsSpan(0, 8).SequenceEqual(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 })
            || !png.AsSpan(12, 4).SequenceEqual("IHDR"u8)) return null;
        int width = System.Buffers.Binary.BinaryPrimitives.ReadInt32BigEndian(png.AsSpan(16, 4));
        int height = System.Buffers.Binary.BinaryPrimitives.ReadInt32BigEndian(png.AsSpan(20, 4));
        if (width is < 1 or > 64 || height is < 1 or > 64) return null;
        Bitmap? bitmap = null;
        try
        {
            using var stream = new MemoryStream(png, false);
            using var original = Image.FromStream(stream);
            bitmap = new Bitmap(32, 32);
            using var graphics = Graphics.FromImage(bitmap);
            graphics.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.NearestNeighbor;
            graphics.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.Half;
            double scale = Math.Min(32d / original.Width, 32d / original.Height);
            int drawWidth = Math.Max(1, (int)Math.Round(original.Width * scale));
            int drawHeight = Math.Max(1, (int)Math.Round(original.Height * scale));
            graphics.DrawImage(original, new Rectangle((32 - drawWidth) / 2, (32 - drawHeight) / 2, drawWidth, drawHeight), 0, 0, original.Width, original.Height, GraphicsUnit.Pixel);
            return bitmap;
        }
        catch (Exception exception) when (exception is ArgumentException or OutOfMemoryException or System.Runtime.InteropServices.ExternalException) { bitmap?.Dispose(); return null; }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            UiLocalization.LanguageChanged -= OnLanguageChanged;
            _iconGeneration++; _iconTimer.Stop(); _iconTimer.Dispose();
            _items.SmallImageList = null; _icons.Dispose();
            foreach (var bitmap in _iconBitmaps) bitmap.Dispose();
            _iconBitmaps.Clear(); _tips.Dispose();
        }
        base.Dispose(disposing);
    }
}
