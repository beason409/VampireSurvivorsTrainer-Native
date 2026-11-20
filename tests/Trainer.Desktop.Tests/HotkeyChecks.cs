#if !BASELINE
using Trainer.Desktop;
using Trainer.Shared;

internal static partial class Program
{
    private static void HotkeyChecks()
    {
        var defaults = HotkeyService.CreateDefaults();
        Check(defaults.Count == 30 && defaults.Keys.Distinct().Count() == 30, "30 independently bindable toggle actions");
        Check(FeatureIds.All.Select((id, index) => defaults[id] == (int)Keys.F1 + index).All(value => value), "seven common defaults preserved");
        Check(StatCatalog.All.Where(stat => stat.Id != StatIds.Seal).All(stat => defaults[HotkeyService.StatAction(stat.Id)] == 0), "23 stat shortcuts start unbound");
        Check(!defaults.ContainsKey(HotkeyService.StatAction(StatIds.Seal)) && defaults.Keys.All(id => !id.Contains("goldSet") && !id.Contains("reward")), "Seal and non-toggle actions excluded");
        Check(HotkeyService.IsValid(Keys.Control | Keys.Alt | Keys.Shift | Keys.F24), "long modified F24 shortcut accepted");
        Check(HotkeyService.IsValid(Keys.Control | Keys.NumPad1) && HotkeyService.IsValid(Keys.Shift | Keys.D2), "modified number shortcuts accepted");
        Check(!HotkeyService.IsValid(Keys.A) && !HotkeyService.IsValid(Keys.None), "ordinary unmodified typing is not a shortcut");
        Check(!HotkeyService.IsValid(Keys.Alt | Keys.F4) && !HotkeyService.IsValid(Keys.Control | Keys.Alt | Keys.Delete), "reserved system chords rejected");
        Check(!HotkeyService.IsValid((Keys)0xFFFF) && !HotkeyService.IsValid((Keys)0x800000), "unknown keys and flags rejected");
        Check(HotkeyService.Format(Keys.Control | Keys.NumPad1) == "Ctrl+Num 1" && HotkeyService.Format(Keys.None) == "未绑定", "numeric and unbound captions");

        var bindings = new Dictionary<string, int> { [FeatureIds.Health] = (int)Keys.F1, ["stat:test"] = (int)(Keys.Control | Keys.NumPad1), ["unbound"] = 0 };
        var held = new HashSet<int>();
        var reads = new Dictionary<int, int>();
        var service = new HotkeyService();
        IReadOnlyList<string> Poll(bool allowed)
        {
            reads.Clear();
            return service.ReadPressed(bindings, allowed, key => { reads[key] = reads.GetValueOrDefault(key) + 1; return held.Contains(key); });
        }
        held.Add((int)Keys.F1);
        Check(Poll(true).Count == 0 && Poll(true).Count == 0, "held shortcut does not fire on focus entry");
        held.Clear(); Poll(true); held.Add((int)Keys.F1);
        Check(Poll(true).SequenceEqual(new[] { FeatureIds.Health }), "new physical press fires once");
        Check(Poll(true).Count == 0 && Poll(true).Count == 0, "holding does not repeat");
        Check(Poll(false).Count == 0 && reads.Count == 0, "inactive polling does not read keyboard state");
        Check(Poll(true).Count == 0, "focus return rearms held keys");
        held.Clear(); Poll(true); held.UnionWith(new[] { (int)Keys.ControlKey, (int)Keys.NumPad1 });
        Check(Poll(true).SequenceEqual(new[] { "stat:test" }), "modified stat shortcut dispatches");
        held.Add((int)Keys.ShiftKey);
        Check(Poll(true).Count == 0, "extra modifier prevents unintended action");
        bindings["same-physical-key"] = (int)(Keys.Alt | Keys.NumPad1);
        Poll(true);
        Check(reads[(int)Keys.NumPad1] == 1, "shared physical key sampled once per poll");

        held.Clear(); Poll(true); held.Add((int)Keys.F1);
        var firstPress = Poll(true);
        held.Clear(); Poll(true); held.Add((int)Keys.F1);
        var secondPress = Poll(true);
        Check(firstPress.SequenceEqual(new[] { FeatureIds.Health }) && secondPress.SequenceEqual(firstPress), "returned shortcut events survive subsequent polls");
        held.Clear();
        Check(Poll(true).Count == 0, "physical key samples are refreshed on each poll");

        var idleService = new HotkeyService();
        Func<int, bool> keyUp = static _ => false;
        for (int index = 0; index < 100; index++) idleService.ReadPressed(defaults, true, keyUp);
        long allocated = GC.GetAllocatedBytesForCurrentThread();
        for (int index = 0; index < 1000; index++) idleService.ReadPressed(defaults, true, keyUp);
        allocated = GC.GetAllocatedBytesForCurrentThread() - allocated;
        Check(allocated < 128_000, "idle shortcut polling stays below 128 bytes per tick after warmup");
        allocated = GC.GetAllocatedBytesForCurrentThread();
        for (int index = 0; index < 1000; index++) idleService.ReadPressed(defaults, false, keyUp);
        Check(GC.GetAllocatedBytesForCurrentThread() == allocated, "inactive shortcut polling allocates no transient collections");

        string directory = Path.Combine(Path.GetTempPath(), "trainer-hotkey-check-" + Guid.NewGuid().ToString("N"));
        string path = Path.Combine(directory, "settings.json");
        Directory.CreateDirectory(directory);
        try
        {
            var legacy = new DesktopSettings { GameDirectory = "D:\\Games\\Vampire Survivors", Language = "en-US" };
            legacy.Hotkeys = FeatureIds.All.Select((id, index) => new { id, key = (int)Keys.F8 + index }).ToDictionary(item => item.id, item => item.key);
            legacy.Hotkeys[FeatureIds.Health] = 0;
            legacy.Hotkeys[HotkeyService.StatAction(StatIds.Seal)] = (int)Keys.F24;
            legacy.Hotkeys["reward:power"] = (int)Keys.F23;
            legacy.Save(path);
            var loaded = DesktopSettings.Load(path);
            Check(loaded.Hotkeys.Count == 30 && loaded.Hotkeys[FeatureIds.Health] == 0 && loaded.Hotkeys[FeatureIds.Damage] == (int)Keys.F11, "legacy bindings and explicitly unbound key retained");
            Check(loaded.Hotkeys.Where(pair => pair.Key.StartsWith("stat:")).All(pair => pair.Value == 0), "missing stat shortcuts added unbound");
            Check(loaded.GameDirectory == legacy.GameDirectory && loaded.Language == "en", "migration preserves directory and language");
            loaded.Hotkeys[HotkeyService.StatAction(StatIds.Might)] = (int)(Keys.Control | Keys.NumPad1);
            loaded.Save(path);
            Check(DesktopSettings.Load(path).Hotkeys[HotkeyService.StatAction(StatIds.Might)] == (int)(Keys.Control | Keys.NumPad1), "custom stat binding persists after reload");
            loaded.Hotkeys[HotkeyService.StatAction(StatIds.Might)] = loaded.Hotkeys[FeatureIds.Damage];
            loaded.Save(path);
            loaded = DesktopSettings.Load(path);
            Check(loaded.Hotkeys[HotkeyService.StatAction(StatIds.Might)] == 0 && loaded.Hotkeys.Values.Where(key => key != 0).Distinct().Count() == loaded.Hotkeys.Values.Count(key => key != 0), "duplicate stored binding normalized without shared dispatch");
        }
        finally { File.Delete(path); Directory.Delete(directory); }
    }
}
#endif
