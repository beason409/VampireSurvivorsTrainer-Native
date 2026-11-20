using System.Diagnostics;
using Trainer.Plugin;
using Trainer.Shared;

var tests = new (string Name, Action Test)[]
{
    ("runtime snapshots read current diagnostics without waiting for another game frame and own their copies", () =>
    {
        using var runtime = Ready();
        var adapter = GameAdapter.LastCreated!;
        adapter.Diagnostics.SetFloat("combat.enemy.lastInput", 0.1f);
        var first = runtime.Snapshot();
        Assert(first.Diagnostics["combat.enemy.lastInput"] == "0.1", "Snapshot waited for a polling frame");
        first.Diagnostics["combat.enemy.lastInput"] = "external mutation";
        adapter.Diagnostics.SetFloat("combat.enemy.lastInput", 0.2f);
        Assert(runtime.Snapshot().Diagnostics["combat.enemy.lastInput"] == "0.2", "Snapshot leaked its mutable dictionary");
        runtime.Tick();
        Assert(runtime.Snapshot().Diagnostics["combat.enemy.lastInput"] == "0.2", "Idle frame discarded current diagnostics");
    }),
    ("missing and invalid icon reads preserve active rewards features and temporary stats", () =>
    {
        using var runtime=Ready(); int session=runtime.BeginSession();
        Assert(Exchange(runtime,new Request{Kind="setChestReward",Enabled=true,UpgradeEnabled=true,ItemId="TEST_WAND"},session).Ok,"Fixture rewards failed");
        Assert(Exchange(runtime,new Request{Kind="setFeature",Feature=FeatureIds.Health,Enabled=true},session).Ok,"Fixture feature failed");
        Assert(Exchange(runtime,new Request{Kind="setStat",Stat=StatIds.Armor,Enabled=true,Value=20},session).Ok,"Fixture stat failed");
        runtime.RecordChestReward("TEST_WAND","Wand"); runtime.RecordUpgradeReward("TEST_WAND","Wand");
        foreach(string? id in new string?[]{"TEST_WAND","UNKNOWN",null,"../invalid",new string('A',300)})
        {
            var response=Exchange(runtime,new Request{Kind="getItemIcon",ItemId=id},session);
            Assert(response.Ok && response.ItemIcon is not null && response.ItemIcon.Png.Length==0,"Unavailable icon was treated as a rejected mutation");
            var reward=response.State!.ChestReward;
            Assert(reward.Enabled && reward.UpgradeEnabled && reward.TargetItemId=="TEST_WAND" && reward.ProcessedCount==1 && reward.UpgradeProcessedCount==1,"Icon read changed reward switches target or receipts");
            Assert(runtime.Enabled(FeatureIds.Health) && runtime.StatEnabled(StatIds.Armor) && runtime.StatValue(StatIds.Armor)==20,"Icon read changed another active effect");
        }
        Assert(GameAdapter.LastCreated!.RewardIconRequests.Count==5 && GameAdapter.LastCreated.GoldWrites==0,"Icon requests did not use read-only adapter routing");
    }),
    ("menu catalog advertises icon support immediately and icons do not require a player", () =>
    {
        using var runtime=Ready(); GameAdapter.LastCreated!.PlayerReady=false; runtime.Tick(); int session=runtime.BeginSession();
        var catalog=Exchange(runtime,new Request{Kind="getItemCatalog",CatalogLimit=1},session);
        Assert(catalog.Ok && catalog.State is not null && !catalog.State.PlayerReady && catalog.State.Diagnostics.TryGetValue("chest.catalog.icons",out string? capability) && capability=="supported","First menu catalog response did not advertise icon support");
        var icon=Exchange(runtime,new Request{Kind="getItemIcon",ItemId="TEST_WAND"},session);
        Assert(icon.Ok && icon.ItemIcon?.Id=="TEST_WAND" && !icon.State!.PlayerReady && !icon.State.ChestReward.Enabled && !icon.State.ChestReward.UpgradeEnabled,"Safe main menu could not read an icon independently of gameplay state");
        Assert(GameAdapter.LastCreated.RewardIconRequests.Single()=="TEST_WAND" && GameAdapter.LastCreated.GoldWrites==0,"Menu icon did not use a read-only adapter call");
    }),
    ("evolved weapons can replace the shared target without changing either enabled route", () =>
    {
        using var runtime=Ready(); int session=runtime.BeginSession();
        Assert(Exchange(runtime,new Request{Kind="setChestReward",Enabled=true,UpgradeEnabled=true,ItemId="TEST_WAND"},session).Ok,"Fixture enable failed");
        var response=Exchange(runtime,new Request{Kind="setChestReward",ItemId="TEST_EVOLUTION"},session);
        Assert(response.Ok && response.ErrorScope is null,"Evolved weapon was blanket rejected");
        Assert(runtime.ChestTarget()=="TEST_EVOLUTION" && runtime.UpgradeTarget()=="TEST_EVOLUTION","Shared evolved target changed route flags");
        Assert(response.State!.ChestReward.ProcessedCount==0 && response.State.ChestReward.UpgradeProcessedCount==0,"Selecting a target falsely recorded an obtained/offered item");
    }),
    ("unsupported upgrade rejection names its scope and preserves the shared active target", () =>
    {
        using var runtime=Ready(); int session=runtime.BeginSession();
        Assert(Exchange(runtime,new Request{Kind="setChestReward",Enabled=true,UpgradeEnabled=true,ItemId="TEST_WAND"},session).Ok,"Fixture enable failed");
        var rejected=Exchange(runtime,new Request{Kind="setChestReward",ItemId="TEST_UNSUPPORTED"},session);
        Assert(!rejected.Ok && rejected.ErrorScope=="upgrade","Upgrade rejection incorrectly blamed both routes");
        var state=rejected.State!.ChestReward;
        Assert(state.Enabled && state.UpgradeEnabled && state.TargetItemId=="TEST_WAND","Failed shared switch changed active state");
        Assert(Exchange(runtime,new Request{Kind="setChestReward",Enabled=true,UpgradeEnabled=false,ItemId="TEST_UNSUPPORTED"},session).Ok,"Explicit chest-only switch rejected");
        Assert(runtime.ChestTarget()=="TEST_UNSUPPORTED" && runtime.UpgradeTarget() is null,"Chest-only shortcut did not explicitly turn upgrade off");
        GameAdapter.LastCreated!.RejectChestTarget=true;
        var chestError=Exchange(runtime,new Request{Kind="setChestReward",Enabled=true,ItemId="TEST_WAND"},session);
        Assert(!chestError.Ok && chestError.ErrorScope=="chest" && runtime.ChestTarget()=="TEST_UNSUPPORTED","Chest error scope/state incorrect");
        var unknown=Exchange(runtime,new Request{Kind="setChestReward",ItemId="UNKNOWN"},session);
        Assert(!unknown.Ok && unknown.ErrorScope=="rewardTarget","Shared target error has incorrect scope");
    }),
    ("reward routes share a validated target with independent toggles", () =>
    {
        using var runtime=Ready(); int session=runtime.BeginSession();
        Assert(Exchange(runtime,new Request{Kind="setChestReward",UpgradeEnabled=true,ItemId="TEST_WAND"},session).Ok,"Upgrade rejected");
        Assert(runtime.UpgradeTarget()=="TEST_WAND" && runtime.ChestTarget() is null,"Upgrade enabled chest");
        Assert(Exchange(runtime,new Request{Kind="setChestReward",Enabled=true},session).Ok,"Shared target not reused");
        Assert(Exchange(runtime,new Request{Kind="setChestReward",ItemId="TEST_SPINACH"},session).Ok,"Shared target edit rejected");
        Assert(runtime.ChestTarget()=="TEST_SPINACH" && runtime.UpgradeTarget()=="TEST_SPINACH","Target routes diverged");
        GameAdapter.LastCreated!.RejectChestTarget=true;
        Assert(Exchange(runtime,new Request{Kind="setChestReward",Enabled=false},session).Ok,"Disable incorrectly revalidated maxed target");
        Assert(runtime.ChestTarget() is null && runtime.UpgradeTarget()=="TEST_SPINACH","Independent disable changed other toggle");
        runtime.RecordUpgradeReward("TEST_SPINACH","菠菜（测试）");
        Assert(runtime.Snapshot().ChestReward.UpgradeProcessedCount==1 && runtime.Snapshot().ChestReward.ProcessedCount==0,"Option receipt confused with obtained chest receipt");
        runtime.EndSession(session);
        Assert(runtime.UpgradeTarget() is null,"Disconnect retained upgrade");
        runtime.BeginSession(); Assert(runtime.Snapshot().ChestReward.UpgradeProcessedCount==0,"Session retained option receipts");
    }),
    ("upgrade capability rejection is atomic and clearing works in menu", () =>
    {
        using var runtime=Ready(); int session=runtime.BeginSession();
        Assert(Exchange(runtime,new Request{Kind="setChestReward",Enabled=true,ItemId="TEST_WAND"},session).Ok,"Fixture chest rejected");
        runtime.SetUpgradeCapability(false,"fixture missing hook");
        Assert(!Exchange(runtime,new Request{Kind="setChestReward",UpgradeEnabled=true,ItemId="TEST_SPINACH"},session).Ok,"Unsupported upgrade enabled");
        Assert(runtime.ChestTarget()=="TEST_WAND" && runtime.UpgradeTarget() is null,"Rejected upgrade changed target");
        GameAdapter.LastCreated!.PlayerReady=false; runtime.Tick();
        Assert(Exchange(runtime,new Request{Kind="setChestReward",Enabled=false,UpgradeEnabled=false},session).Ok,"Menu blocked clear");
    }),
    ("upgrade native validation cannot revive an expired session", () =>
    {
        using var runtime=Ready(); int session=runtime.BeginSession();
        GameAdapter.LastCreated!.DuringChestValidation=()=>runtime.EndSession(session);
        Assert(!Exchange(runtime,new Request{Kind="setChestReward",UpgradeEnabled=true,ItemId="TEST_WAND"},session).Ok,"Expired upgrade succeeded");
        Assert(runtime.UpgradeTarget() is null,"Expired upgrade revived");
    }),
    ("chest catalog is read-only paginated and rejects invalid page bounds", () =>
    {
        using var runtime=Ready(); int session=runtime.BeginSession();
        var page=Exchange(runtime,new Request{Kind="getItemCatalog",CatalogLimit=1},session);
        Assert(page.Ok && page.ItemCatalogTotal==4 && page.ItemCatalog?.Count==1,"Catalog page wrong");
        Assert(page.State!.Diagnostics.TryGetValue("chest.catalog.icons",out string? iconSupport) && iconSupport=="supported","Catalog response deferred icon capability until a later heartbeat");
        Assert(page.ItemCatalog![0].Id=="TEST_WAND" && !page.State!.ChestReward.Enabled,"Catalog request enabled reward");
        Assert(page.ItemCatalog[0].EnglishName=="Test Wand" && page.ItemCatalog[0].UpgradeAllowed==true,"Catalog clone lost English/compatibility metadata");
        var next=Exchange(runtime,new Request{Kind="getItemCatalog",CatalogOffset=1,CatalogLimit=1},session);
        Assert(next.ItemCatalog?.Single().Kind=="passive","Catalog offset ignored");
        var evolution=Exchange(runtime,new Request{Kind="getItemCatalog",CatalogOffset=2,CatalogLimit=1},session).ItemCatalog!.Single();
        Assert(evolution.IsEvolution==true && evolution.UpgradeAllowed==true && evolution.UpgradeUnsupportedReason is null,"Evolution capability metadata lost");
        var unsupported=Exchange(runtime,new Request{Kind="getItemCatalog",CatalogOffset=3,CatalogLimit=1},session).ItemCatalog!.Single();
        Assert(unsupported.IsEvolution==true && unsupported.UpgradeAllowed==false && unsupported.UpgradeUnsupportedReason=="进化被动升级尚未适配","Unsupported upgrade metadata lost");
        foreach(var invalid in new[]{new Request{Kind="getItemCatalog",CatalogOffset=-1},new Request{Kind="getItemCatalog",CatalogLimit=201}})
            Assert(!Exchange(runtime,invalid,session).Ok,"Invalid pagination accepted");
        Assert(GameAdapter.LastCreated!.GoldWrites==0 && !runtime.Snapshot().BackupAuthorized,"Catalog touched wallet");
    }),
    ("chest target validates against the current catalog before enabling", () =>
    {
        using var runtime=Ready(); int session=runtime.BeginSession();
        Assert(!Exchange(runtime,new Request{Kind="setChestReward",Enabled=true,ItemId="UNKNOWN"},session).Ok,"Unknown target accepted");
        Assert(Exchange(runtime,new Request{Kind="setChestReward",Enabled=true,ItemId="TEST_WAND"},session).Ok,"Valid target rejected");
        Assert(runtime.ChestTarget()=="TEST_WAND","Target not active");
        GameAdapter.LastCreated!.RejectChestTarget=true;
        Assert(!Exchange(runtime,new Request{Kind="setChestReward",Enabled=true,ItemId="TEST_SPINACH"},session).Ok,"Unobtainable target accepted");
        Assert(runtime.ChestTarget()=="TEST_WAND","Rejected edit corrupted active target");
        runtime.RecordChestReward("TEST_WAND","魔杖（测试）");
        Assert(runtime.Snapshot().ChestReward.ProcessedCount==1,"Confirmed reward missing from state");
        Assert(Exchange(runtime,new Request{Kind="setChestReward",Enabled=false},session).Ok && runtime.ChestTarget() is null,"Disable without target rejected");
    }),
    ("chest reward resets on disconnect disable-all run end and unsupported hooks", () =>
    {
        using var runtime=Ready(); int session=runtime.BeginSession();
        void Enable()=>Assert(Exchange(runtime,new Request{Kind="setChestReward",Enabled=true,ItemId="TEST_WAND"},session).Ok,"Enable failed");
        Enable(); runtime.EndSession(session); Assert(runtime.ChestTarget() is null,"Disconnected reward remained enabled");
        session=runtime.BeginSession(); Assert(!runtime.Snapshot().ChestReward.Enabled,"Reconnect enabled old reward");
        Enable(); Exchange(runtime,new Request{Kind="disableAll"},session); Assert(runtime.ChestTarget() is null,"Disable-all omitted chest reward");
        Enable(); GameAdapter.LastCreated!.PlayerReady=false; runtime.Tick();
        Assert(!runtime.Snapshot().ChestReward.Enabled,"Ended run retained reward toggle");
        Assert(!Exchange(runtime,new Request{Kind="setChestReward",Enabled=true,ItemId="TEST_WAND"},session).Ok,"Menu allowed chest toggle");
        GameAdapter.LastCreated!.PlayerReady=true; runtime.Tick(); runtime.SetChestCapability(false,"fixture missing native method");
        Assert(!Exchange(runtime,new Request{Kind="setChestReward",Enabled=true,ItemId="TEST_WAND"},session).Ok,"Unsupported hook enabled");
    }),
    ("disconnect during native chest target validation cannot re-enable the effect", () =>
    {
        using var runtime=Ready(); int session=runtime.BeginSession();
        GameAdapter.LastCreated!.DuringChestValidation=()=>runtime.EndSession(session);
        var response=Exchange(runtime,new Request{Kind="setChestReward",Enabled=true,ItemId="TEST_WAND"},session);
        Assert(!response.Ok && !runtime.Snapshot().ChestReward.Enabled,"Native validation re-enabled an ended session");
    }),
    ("temporary stat edits validate values without wallet authorization or writes", () =>
    {
        using var runtime=Ready(); int session=runtime.BeginSession();
        Assert(Exchange(runtime,new Request{Kind="setStat",Stat=StatIds.Might,Enabled=true,Value=100},session).Ok,"Stat edit rejected");
        Assert(runtime.StatEnabled(StatIds.Might) && runtime.StatValue(StatIds.Might)==100,"Stat target not applied");
        Assert(!runtime.Snapshot().BackupAuthorized && GameAdapter.LastCreated!.GoldWrites==0,"Stat edit touched wallet authorization");
        foreach(double invalid in new[]{double.NaN,double.PositiveInfinity,-101d,901d})
            Assert(!Exchange(runtime,new Request{Kind="setStat",Stat=StatIds.Might,Enabled=true,Value=invalid},session).Ok,"Invalid target accepted");
        Assert(runtime.StatValue(StatIds.Might)==100,"Rejected target corrupted prior value");
        Assert(!Exchange(runtime,new Request{Kind="setStat",Stat=StatIds.Rerolls,Enabled=true,Value=2.5},session).Ok,"Fractional count accepted");
        Assert(!Exchange(runtime,new Request{Kind="setStat",Stat=StatIds.Seal,Enabled=true,Value=1},session).Ok,"Unsupported permanent option accepted");
    }),
    ("stat-only clear preserves feature switches and disconnect clears both", () =>
    {
        using var runtime=Ready(); int session=runtime.BeginSession();
        Exchange(runtime,new Request{Kind="setFeature",Feature=FeatureIds.Health,Enabled=true},session);
        Exchange(runtime,new Request{Kind="setStat",Stat=StatIds.Armor,Enabled=true,Value=20},session);
        Assert(Exchange(runtime,new Request{Kind="disableStats"},session).Ok,"Clear stats rejected");
        Assert(!runtime.StatEnabled(StatIds.Armor) && runtime.Enabled(FeatureIds.Health),"Stat clear changed feature switches");
        Exchange(runtime,new Request{Kind="setStat",Stat=StatIds.Armor,Enabled=true,Value=30},session);
        runtime.EndSession(session);
        Assert(runtime.Snapshot().Stats.All(s=>!s.Enabled) && runtime.Snapshot().Features.All(f=>!f.Enabled),"Disconnect left an override enabled");
        int next=runtime.BeginSession();
        Assert(runtime.Snapshot().Stats.All(s=>!s.Enabled),"Reconnect enabled old stats");
    }),
    ("stat overrides stop when run ends and menu refuses enabling", () =>
    {
        using var runtime=Ready();int session=runtime.BeginSession();
        Exchange(runtime,new Request{Kind="setStat",Stat=StatIds.Recovery,Enabled=true,Value=5},session);
        GameAdapter.LastCreated!.PlayerReady=false;runtime.Tick();
        Assert(runtime.Snapshot().Stats.All(s=>!s.Enabled),"Ended run retained stat flags");
        Assert(!Exchange(runtime,new Request{Kind="setStat",Stat=StatIds.Recovery,Enabled=true,Value=5},session).Ok,"Menu enabled gameplay stat");
    }),
    ("stat units match panel percentages and absolute values", () =>
    {
        Assert(StatCatalog.Find(StatIds.Might)!.ToRaw(100)==2,"Might percent conversion wrong");
        Assert(Math.Abs(StatCatalog.Find(StatIds.Cooldown)!.ToRaw(90)-.1)<1e-9,"Cooldown reduction conversion wrong");
        Assert(StatCatalog.Find(StatIds.Recovery)!.ToRaw(5)==5,"Recovery converted as percent");
        Assert(StatCatalog.Find(StatIds.MoveSpeed)!.FromRaw(3)==200,"Movement panel conversion wrong");
    }),
    ("menu wallet protection works independently from player-ready", () =>
    {
        using var runtime = Ready();
        GameAdapter.LastCreated!.PlayerReady = false; runtime.Tick();
        int session = runtime.BeginSession();
        Assert(Exchange(runtime, new Request { Kind = "hello" }, session, true).Ok, "Hello failed");
        Assert(Exchange(runtime, new Request { Kind = "setFeature", Feature = FeatureIds.NoGoldSpend, Enabled = true }, session).Ok, "Wallet toggle failed");
        Assert(runtime.Enabled(FeatureIds.NoGoldSpend), "Menu wallet incorrectly requires player");
        Exchange(runtime, new Request { Kind = "setFeature", Feature = FeatureIds.Health, Enabled = true }, session);
        Assert(!runtime.Enabled(FeatureIds.Health), "Player effects active in menu");
    }),
    ("reconnect clears every effect and wallet authorization", () =>
    {
        using var runtime = Ready(); int session = runtime.BeginSession();
        Exchange(runtime, new Request { Kind = "hello" }, session, true);
        Exchange(runtime, new Request { Kind = "setFeature", Feature = FeatureIds.Health, Enabled = true }, session);
        Assert(runtime.Enabled(FeatureIds.Health), "Fixture failed to enable effect");
        int next = runtime.BeginSession();
        Assert(runtime.Snapshot().Features.All(f => !f.Enabled) && !runtime.Snapshot().BackupAuthorized, "Reconnect retained flags/auth");
        Assert(!Exchange(runtime, new Request { Kind = "setGold", Value = 1000 }, next).Ok, "Reconnect retained gold permission");
    }),
    ("ended session cannot execute already-queued hello or gold mutation", () =>
    {
        using var runtime = Ready(); int session = runtime.BeginSession();
        var hello = runtime.Submit(new Request { Kind = "hello" }, session, true);
        var mutation = runtime.Submit(new Request { Kind = "setGold", Value = 999 }, session);
        runtime.EndSession(session); runtime.Tick();
        Assert(!hello.Result.Ok && !mutation.Result.Ok, "Stale queued command executed");
        Assert(GameAdapter.LastCreated!.GoldWrites == 0 && !runtime.Snapshot().BackupAuthorized, "Stale session reauthorized or wrote gold");
    }),
    ("watchdog expiry invalidates commands from old pipe session", () =>
    {
        using var runtime = Ready(); int session = runtime.BeginSession();
        Exchange(runtime, new Request { Kind = "hello" }, session, true);
        Exchange(runtime, new Request { Kind = "setFeature", Feature = FeatureIds.Health, Enabled = true }, session);
        Assert(Exchange(runtime,new Request{Kind="setChestReward",Enabled=true,ItemId="TEST_WAND"},session).Ok,"Fixture chest enable failed");
        Thread.Sleep(3150); runtime.CheckHeartbeat();
        Assert(runtime.Snapshot().Features.All(f => !f.Enabled) && !runtime.Snapshot().BackupAuthorized && !runtime.Snapshot().ChestReward.Enabled, "Watchdog retained flags/auth");
        runtime.Heartbeat(session);
        Assert(!Exchange(runtime, new Request { Kind = "hello" }, session, true).Ok, "Old session reauthorized after watchdog");
    }),
    ("queued mutation expires before desktop's two-second request timeout", () =>
    {
        using var runtime = Ready(); int session = runtime.BeginSession();
        Exchange(runtime, new Request { Kind = "hello" }, session, true);
        var command = runtime.Submit(new Request { Kind = "setGold", Value = 555 }, session);
        Thread.Sleep(2100); // Simulate Unity main thread paused beyond desktop request timeout.
        runtime.Tick();
        Assert(!command.Result.Ok && GameAdapter.LastCreated!.GoldWrites == 0, "Gold mutated after desktop had timed out");
    })
}.Concat(DiagnosticChecks.All).ToArray();
int failed = 0;
foreach (var test in tests)
{
    try { test.Test(); Console.WriteLine("PASS " + test.Name); }
    catch (Exception error) { failed++; Console.Error.WriteLine("FAIL " + test.Name + "\n" + error.Message); }
}
Console.WriteLine($"{tests.Length - failed}/{tests.Length} actual-source runtime fixtures passed.");
if (failed == 0 && args.Length == 2 && args[0] == "--measure-diagnostics") DiagnosticChecks.Measure(args[1]);
return failed == 0 ? 0 : 1;

static TrainerRuntime Ready()
{
    GameAdapter.LastCreated = null;
    var runtime = new TrainerRuntime(new BepInEx.Logging.ManualLogSource());
    var timeout = Stopwatch.StartNew();
    do
    {
        runtime.Tick();
        if (GameAdapter.LastCreated != null) { runtime.Tick(); return runtime; }
        Thread.Sleep(1);
    } while (timeout.ElapsedMilliseconds < 2000);
    runtime.Dispose();
    throw new Exception("Runtime fixture initialization failed");
}
static Response Exchange(TrainerRuntime runtime, Request request, int session, bool? backupValid = null)
{
    var task = runtime.Submit(request, session, backupValid);
    runtime.Tick();
    return task.GetAwaiter().GetResult();
}
static void Assert(bool condition, string message) { if (!condition) throw new Exception(message); }
