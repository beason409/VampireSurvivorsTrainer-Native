#if !BASELINE
using System.Diagnostics;
using System.Drawing.Imaging;
using System.Reflection;
using Trainer.Desktop;
using Trainer.Shared;

internal static partial class Program
{
    private static void IconChecks()
    {
        byte[] red = IconPng(Color.Red), blue = IconPng(Color.Blue);
        var definitions = SyntheticCatalog(344);
        var requested = new List<string>();
        using (var fixture = new IconFixture(definitions, id => { requested.Add(id); return Task.FromResult<RewardItemIcon?>(new() { Id = id, Png = red }); }, supported: false))
        {
            AwaitIconTask(fixture.Load());
            Check(requested.Count == 0 && fixture.List.Items[0].ImageIndex == 0, "old-plugin capability uses a placeholder without sending unsupported icon requests");
            fixture.EnableIcons();
            var visibleIds = fixture.VisibleIds();
            AwaitIconTask(fixture.Load());
            Check(requested.Count is > 0 and <= 4 && requested.All(visibleIds.Contains), "one icon load pass requests at most four items from the visible viewport");
            string loadedId = requested[0];
            requested.Clear();
            fixture.List.EnsureVisible(250);
            visibleIds = fixture.VisibleIds();
            AwaitIconTask(fixture.Load());
            Check(requested.Count is > 0 and <= 4 && requested.All(visibleIds.Contains), "scrolling requests only the new visible rows instead of walking the full catalog");
            fixture.Search.Text = loadedId;
            int rowIndex = Enumerable.Range(0, fixture.List.Items.Count).First(index => ((RewardItemDefinition)fixture.List.Items[index].Tag!).Id == loadedId);
            int imageIndex = fixture.List.Items[rowIndex].ImageIndex;
            Check(imageIndex > 0 && HasColor(fixture.Icons, imageIndex, Color.Red), "completed PNG attaches to its item ID after filtering changes row positions");
            Check(HasTransparentCorner(fixture.Icons, imageIndex), "transparent PNG retains its transparent canvas in the native image list");
        }

        requested.Clear();
        using (var fixture = new IconFixture(definitions.Take(1).ToArray(), id => { requested.Add(id); return Task.FromResult<RewardItemIcon?>(new() { Id = id, Png = red }); }))
        {
            AwaitIconTask(fixture.Load());
            int imageCount = fixture.Icons.Images.Count;
            fixture.Search.Text = "power";
            _ = fixture.List.Items[0];
            fixture.Search.Clear();
            AwaitIconTask(fixture.Load());
            Check(requested.Count == 1 && fixture.Icons.Images.Count == imageCount && fixture.List.Items[0].ImageIndex > 0, "filtering and revisiting a cached icon neither requests nor decodes it again");
        }

        int calls = 0;
        using (var fixture = new IconFixture(definitions.Take(1).ToArray(), id => Task.FromResult<RewardItemIcon?>(++calls == 1 ? null : new() { Id = id, Png = red })))
        {
            AwaitIconTask(fixture.Load());
            Check(fixture.List.Items[0].ImageIndex == 0, "temporarily unavailable icon leaves readable text with a placeholder");
            AwaitIconTask(fixture.Load());
            Check(calls == 2 && fixture.List.Items[0].ImageIndex > 0, "temporary null response remains retryable");
        }

        foreach (byte[]? invalidPng in new byte[]?[] { null, Array.Empty<byte>(), new byte[] { 1, 2, 3, 4 } })
        {
            calls = 0;
            using var fixture = new IconFixture(definitions.Take(1).ToArray(), id => { calls++; return Task.FromResult<RewardItemIcon?>(new() { Id = id, Png = invalidPng! }); });
            AwaitIconTask(fixture.Load()); AwaitIconTask(fixture.Load());
            Check(calls == 1 && fixture.List.Items[0].ImageIndex == 0 && fixture.Icons.Images.Count == 1 && fixture.Search.Enabled, "explicit missing or corrupt PNG is cached as a text-safe fallback without repeated decoding");
        }

        var pending = NewIconCompletion();
        requested.Clear();
        using (var fixture = new IconFixture(definitions, id => { requested.Add(id); return requested.Count == 1 ? pending.Task : Task.FromResult<RewardItemIcon?>(new() { Id = id, Png = blue }); }))
        {
            Task loading = fixture.Load();
            Check(!loading.IsCompleted && requested.Count == 1 && fixture.Search.Enabled && fixture.List.Enabled, "awaiting a slow icon keeps search and list interactive");
            fixture.Search.Focus();
            string newId = definitions[300].Id;
            var timer = Stopwatch.StartNew();
            fixture.Search.Text = newId;
            timer.Stop();
            Check(fixture.Search.Text == newId && fixture.Search.Focused && !loading.IsCompleted, "typing is handled immediately while an icon request is unresolved");
            Measurements.Add(new { dataset = "slow-icon-pending", query = newId, handlerMilliseconds = timer.Elapsed.TotalMilliseconds });
            var visibleIds = fixture.VisibleIds();
            pending.SetResult(new() { Id = requested[0], Png = red });
            AwaitIconTask(loading);
            Check(requested.Skip(1).All(visibleIds.Contains), "after an awaited request the remaining icon requests follow the current filtered viewport");
            Check(fixture.List.Items.Count > 0 && (fixture.List.Items[0].ImageIndex == 0 || !HasColor(fixture.Icons, fixture.List.Items[0].ImageIndex, Color.Red)), "a delayed icon never attaches to an unrelated row that reused the same index");
        }

        pending = NewIconCompletion(); calls = 0;
        using (var fixture = new IconFixture(definitions.Take(1).ToArray(), id => ++calls == 1 ? pending.Task : Task.FromResult<RewardItemIcon?>(new() { Id = id, Png = blue })))
        {
            Task loading = fixture.Load();
            fixture.Panel.SetCatalogPage(new[] { new RewardItemDefinition { Id = "POWER", Name = "Fresh", EnglishName = "Fresh Spinach", Kind = "passive", UpgradeAllowed = true } }, 1, 0);
            pending.SetResult(new() { Id = "POWER", Png = red });
            AwaitIconTask(loading);
            Check(fixture.Icons.Images.Count == 1 && fixture.List.Items[0].ImageIndex == 0, "catalog replacement rejects an old generation response even when the ID is unchanged");
            AwaitIconTask(fixture.Load());
            Check(HasColor(fixture.Icons, fixture.List.Items[0].ImageIndex, Color.Blue), "new catalog generation can load fresh image data for that same ID");
        }

        pending = NewIconCompletion(); calls = 0;
        using (var fixture = new IconFixture(definitions.Take(1).ToArray(), id => ++calls == 1 ? pending.Task : Task.FromResult<RewardItemIcon?>(new() { Id = id, Png = blue })))
        {
            Task loading = fixture.Load();
            fixture.Panel.SetDisconnected();
            pending.SetResult(new() { Id = "POWER", Png = red });
            AwaitIconTask(loading);
            Check(fixture.List.Items.Count == 0 && fixture.Icons.Images.Count == 1, "disconnect rejects in-flight icon results and clears previous images");
            fixture.Panel.SetCatalogPage(definitions.Take(1).ToArray(), 1, 0);
            fixture.Panel.ApplyState(fixture.State, false); fixture.PauseTimer();
            AwaitIconTask(fixture.Load());
            Check(HasColor(fixture.Icons, fixture.List.Items[0].ImageIndex, Color.Blue), "reconnect loads the new image without reusing the old connection's PNG");
        }

        pending = NewIconCompletion();
        using (var fixture = new IconFixture(definitions.Take(1).ToArray(), _ => pending.Task))
        {
            Task loading = fixture.Load();
            fixture.Panel.Dispose();
            pending.SetResult(new() { Id = "POWER", Png = red });
            AwaitIconTask(loading);
            Check(fixture.Panel.IsDisposed, "disposing the panel during an awaited icon request safely ignores the late result");
        }
    }

    private static TaskCompletionSource<RewardItemIcon?> NewIconCompletion() => new(TaskCreationOptions.RunContinuationsAsynchronously);

    private static void AwaitIconTask(Task task)
    {
        var timer = Stopwatch.StartNew();
        while (!task.IsCompleted)
        {
            Application.DoEvents();
            if (timer.Elapsed > TimeSpan.FromSeconds(5)) throw new TimeoutException("Synthetic icon task did not settle in five seconds.");
            Thread.Sleep(1);
        }
        task.GetAwaiter().GetResult();
    }

    private static byte[] IconPng(Color color)
    {
        using var bitmap = new Bitmap(32, 32, PixelFormat.Format32bppArgb);
        for (int x = 13; x < 19; x++) for (int y = 13; y < 19; y++) bitmap.SetPixel(x, y, color);
        using var stream = new MemoryStream();
        bitmap.Save(stream, ImageFormat.Png);
        return stream.ToArray();
    }

    private static bool HasColor(ImageList icons, int index, Color expected)
    {
        if (index < 1 || index >= icons.Images.Count) return false;
        using var bitmap = new Bitmap(icons.Images[index]);
        Color pixel = bitmap.GetPixel(16, 16);
        return pixel.A == 255 && pixel.R == expected.R && pixel.G == expected.G && pixel.B == expected.B;
    }

    private static bool HasTransparentCorner(ImageList icons, int index)
    {
        using var bitmap = new Bitmap(icons.Images[index]);
        return bitmap.GetPixel(0, 0).A == 0;
    }

    private sealed class IconFixture : IDisposable
    {
        public readonly Form Host;
        public readonly ChestRewardPanel Panel;
        public readonly TextBox Search;
        public readonly ListView List;
        public readonly TrainerState State = new() { PlayerReady = true, ChestReward = new() { Supported = true, UpgradeSupported = true } };
        public ImageList Icons => Field<ImageList>(Panel, "_icons");

        public IconFixture(RewardItemDefinition[] catalog, Func<string, Task<RewardItemIcon?>> loader, bool supported = true)
        {
            Host = new Form { ClientSize = new Size(1110, 530), ShowInTaskbar = false, StartPosition = FormStartPosition.Manual, Location = new Point(-32000, -32000), Font = new Font("Microsoft YaHei UI", 9F) };
            Panel = new ChestRewardPanel((_, _) => Task.CompletedTask, loader);
            Host.Controls.Add(Panel); Host.Show();
            _ = Panel.Handle;
            Search = Field<TextBox>(Panel, "_search"); List = Field<ListView>(Panel, "_items"); _ = Search.Handle; _ = List.Handle;
            Panel.SetCatalogPage(catalog, catalog.Length, 0);
            if (supported) State.Diagnostics["chest.catalog.icons"] = "supported";
            Panel.ApplyState(State, false); PauseTimer();
            Application.DoEvents();
        }

        public void PauseTimer()
        {
            foreach (var field in Panel.GetType().GetFields(BindingFlags.NonPublic | BindingFlags.Instance))
                if (field.GetValue(Panel) is System.Windows.Forms.Timer timer) timer.Stop();
        }

        public void EnableIcons() { State.Diagnostics["chest.catalog.icons"] = "supported"; Panel.ApplyState(State, false); PauseTimer(); }
        public Task Load() => (Task)Panel.GetType().GetMethod("LoadVisibleIconsAsync", BindingFlags.NonPublic | BindingFlags.Instance)!.Invoke(Panel, null)!;

        public HashSet<string> VisibleIds()
        {
            var bounds = List.ClientRectangle;
            return Enumerable.Range(0, List.Items.Count).Where(index => List.Items[index].Bounds.IntersectsWith(bounds)).Select(index => ((RewardItemDefinition)List.Items[index].Tag!).Id).ToHashSet(StringComparer.Ordinal);
        }

        public void Dispose() { Host.Dispose(); Application.DoEvents(); }
    }
}
#endif
