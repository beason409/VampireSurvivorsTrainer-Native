#if !BASELINE
using System.Diagnostics;
using System.Reflection;
using Trainer.Desktop;
using Trainer.Shared;

internal static partial class Program
{
    private static RewardItemDefinition CatalogItem(string id, string name) => new()
    {
        Id = id, Name = name, EnglishName = name, Kind = "weapon", UpgradeAllowed = true
    };

    private static void OptimizationChecks()
    {
        UiLocalization.SetLanguage("zh-CN");
        CatalogPagingChecks();
        RewardSubmissionChecks();
    }

    private static void CatalogPagingChecks()
    {
        var first = CatalogItem("DUPLICATE", "first");
        using var fixture = new Fixture(new[] { first, CatalogItem("duplicate", "lowercase") });
        fixture.Panel.SetCatalogPage(new[]
        {
            CatalogItem("DUPLICATE", "later duplicate"), CatalogItem("NEXT", "next"),
            CatalogItem(null!, "invalid"), CatalogItem("", "invalid"), CatalogItem(" ", "invalid"),
            CatalogItem(new string('x', 257), "invalid"),
            new RewardItemDefinition { Id = "OTHER", Kind = "other" },
            CatalogItem(new string('x', 256), "boundary")
        }, 10, 2);
        var rows = Enumerable.Range(0, fixture.List.Items.Count).Select(index => (RewardItemDefinition)fixture.List.Items[index].Tag!).ToArray();
        Check(rows.Select(item => item.Id).SequenceEqual(new[] { "DUPLICATE", "duplicate", "NEXT", new string('x', 256) }), "paged catalog keeps ordinal IDs, first occurrence, valid ID bounds and server order");
        Check(ReferenceEquals(rows[0], first), "a duplicate ID in a later page retains the original item and metadata");
        foreach (string? id in new string?[] { null, "", "missing" })
        {
            fixture.State.ChestReward.TargetItemId = id;
            fixture.State.ChestReward.TargetItemName = "server fallback";
            fixture.State.ChestReward.Enabled = true;
            fixture.Panel.ApplyState(fixture.State, false);
            Check(Field<Label>(fixture.Panel, "_activeTarget").Text.Contains("server fallback"), "unknown or empty target ID preserves the server name fallback: " + (id ?? "<null>"));
        }
        fixture.Panel.SetCatalogPage(new[] { CatalogItem("DUPLICATE", "reloaded") }, 1, 0);
        Check(fixture.List.Items.Count == 1 && fixture.List.Items[0].Text == "reloaded", "offset-zero reload permits the same ID with fresh metadata");
        fixture.Panel.SetDisconnected();
        fixture.Panel.SetCatalogPage(new[] { CatalogItem("DUPLICATE", "reconnected") }, 1, 0);
        fixture.Panel.ApplyState(fixture.State, false);
        Check(fixture.List.Items.Count == 1 && fixture.List.Items[0].Text == "reconnected", "disconnect clears catalog IDs before the same ID is loaded again");
        fixture.OnSend = request =>
        {
            if (request.Kind == "getItemCatalog") fixture.Panel.SetCatalogPage(new[] { CatalogItem("DUPLICATE", "refreshed") }, 1, request.CatalogOffset);
            return Task.CompletedTask;
        };
        InvokeTask(fixture.Panel, "RefreshCatalogAsync");
        Check(fixture.List.Items.Count == 1 && fixture.List.Items[0].Text == "refreshed", "explicit catalog refresh clears prior IDs and cached rows");
        fixture.Panel.RenderPreview();
        fixture.Panel.SetCatalogPage(new[] { CatalogItem("TEST_WAND", "wrong preview replacement"), CatalogItem("EXTRA", "extra") }, 4, 3);
        Check(fixture.List.Items.Count == 4 && fixture.List.Items[0].Text != "wrong preview replacement", "preview items participate in duplicate-ID handling before another page arrives");
    }

    private static void RewardSubmissionChecks()
    {
        using var fixture = new Fixture(new[] { CatalogItem("SELECTED", "selected") });
        fixture.List.Items[0].Selected = true;
        fixture.State.ChestReward.TargetItemId = "ACTIVE";
        fixture.State.ChestReward.TargetItemName = "active target";
        fixture.State.ChestReward.Enabled = fixture.State.ChestReward.UpgradeEnabled = true;
        fixture.Panel.ApplyState(fixture.State, false);
        foreach (string method in new[] { "SetRewardAsync", "SetUpgradeAsync" })
        {
            bool upgrade = method == "SetUpgradeAsync";
            var completion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            fixture.OnSend = _ => completion.Task;
            var submission = (Task)typeof(ChestRewardPanel).GetMethod(method, BindingFlags.NonPublic | BindingFlags.Instance)!.Invoke(fixture.Panel, new object[] { true })!;
            Check(Field<bool>(fixture.Panel, "_pending"), method + " remains pending while the sender is awaiting confirmation");
            int sent = fixture.Requests.Count;
            InvokeTask(fixture.Panel, upgrade ? "SetRewardAsync" : "SetUpgradeAsync", false);
            Check(fixture.Requests.Count == sent, method + " blocks the other route while a request is pending");
            var expected = new InvalidOperationException("synthetic sender failure");
            completion.SetException(expected);
            while (!submission.IsCompleted) Application.DoEvents();
            Exception? actual = null;
            try { submission.GetAwaiter().GetResult(); } catch (Exception exception) { actual = exception; }
            var request = fixture.Requests[^1];
            Check(ReferenceEquals(actual, expected) && !Field<bool>(fixture.Panel, "_pending"), method + " propagates sender failure and always releases its pending state");
            Check(request.ItemId == "SELECTED" && request.Enabled == (upgrade ? null : true) && request.UpgradeEnabled == (upgrade ? true : null), method + " sets only its own route flag and selected target");
            Check(fixture.State.ChestReward.TargetItemId == "ACTIVE" && fixture.State.ChestReward.Enabled && fixture.State.ChestReward.UpgradeEnabled, method + " leaves the confirmed target and both route states unchanged after failure");
        }
        fixture.OnSend = null;
        fixture.State.PlayerReady = false;
        fixture.State.ChestReward.Supported = fixture.State.ChestReward.UpgradeSupported = false;
        fixture.Panel.ApplyState(fixture.State, false);
        fixture.Search.Text = "no matching selection";
        InvokeTask(fixture.Panel, "SetRewardAsync", false);
        var chest = fixture.Requests[^1];
        InvokeTask(fixture.Panel, "SetUpgradeAsync", false);
        var upgradeRequest = fixture.Requests[^1];
        Check(chest.Enabled == false && chest.UpgradeEnabled is null && chest.ItemId is null && upgradeRequest.Enabled is null && upgradeRequest.UpgradeEnabled == false && upgradeRequest.ItemId is null, "either route can be disabled without a ready player, supported catalog or selection");
    }

    private static void MeasureCatalogLoading()
    {
        var catalog = SyntheticCatalog(4096);
        foreach (int repetition in Enumerable.Range(0, 7))
        {
            using var fixture = new Fixture(Array.Empty<RewardItemDefinition>());
            typeof(ChestRewardPanel).GetField("_loading", BindingFlags.NonPublic | BindingFlags.Instance)!.SetValue(fixture.Panel, true);
            long allocated = GC.GetAllocatedBytesForCurrentThread();
            var watch = Stopwatch.StartNew();
            for (int offset = 0; offset < catalog.Length; offset += 200)
                fixture.Panel.SetCatalogPage(catalog.Skip(offset).Take(200).ToArray(), catalog.Length, offset);
            watch.Stop();
            allocated = GC.GetAllocatedBytesForCurrentThread() - allocated;
            Measurements.Add(new { dataset = "catalog-load-4096", repetition, milliseconds = watch.Elapsed.TotalMilliseconds, allocatedBytes = allocated });
            Console.WriteLine($"CATALOG-LOAD run={repetition} elapsed={watch.Elapsed.TotalMilliseconds:F3}ms allocated={allocated}");
        }
    }
}
#endif
