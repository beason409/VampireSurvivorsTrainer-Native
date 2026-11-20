using System.Runtime.InteropServices;
using Trainer.Shared;

namespace Trainer.Desktop;

internal sealed class HotkeyService
{
    private readonly Dictionary<string, bool> _held = new();
    private readonly Dictionary<int, bool> _keyStates = new();
    private bool _wasAllowed;
    private int? _lastGamePid;

    internal static string StatAction(string id) => "stat:" + id;
    internal static IReadOnlyList<string> Actions { get; } = FeatureIds.All.Concat(StatCatalog.All
        .Where(item => item.Id != StatIds.Seal).Select(item => StatAction(item.Id))).ToArray();

    internal static Dictionary<string, int> CreateDefaults()
    {
        var result = Actions.ToDictionary(id => id, _ => 0);
        for (int index = 0; index < FeatureIds.All.Length; index++) result[FeatureIds.All[index]] = (int)Keys.F1 + index;
        return result;
    }

    public static bool IsValid(Keys keys)
    {
        Keys code = keys & Keys.KeyCode;
        if (!Enum.IsDefined(code)) return false;
        if ((code == Keys.F4 && keys.HasFlag(Keys.Alt))
            || (code == Keys.Delete && keys.HasFlag(Keys.Control) && keys.HasFlag(Keys.Alt))) return false;
        if ((keys & ~(Keys.KeyCode | Keys.Control | Keys.Alt | Keys.Shift)) != 0) return false;
        if (code is >= Keys.F1 and <= Keys.F24 or >= Keys.NumPad0 and <= Keys.Divide) return true;
        if ((keys & Keys.Modifiers) == Keys.None) return false;
        return code is >= Keys.A and <= Keys.Z or >= Keys.D0 and <= Keys.D9
            or >= Keys.Space and <= Keys.Help or >= Keys.OemSemicolon and <= Keys.OemBackslash
            or Keys.Back or Keys.Return or Keys.Delete or Keys.Insert;
    }

    public static string Format(Keys keys)
    {
        if (keys == Keys.None) return "未绑定";
        var parts = new List<string>();
        if (keys.HasFlag(Keys.Control)) parts.Add("Ctrl");
        if (keys.HasFlag(Keys.Alt)) parts.Add("Alt");
        if (keys.HasFlag(Keys.Shift)) parts.Add("Shift");
        Keys code = keys & Keys.KeyCode;
        parts.Add(code is >= Keys.D0 and <= Keys.D9 ? ((int)code - (int)Keys.D0).ToString()
            : code is >= Keys.NumPad0 and <= Keys.NumPad9 ? "Num " + ((int)code - (int)Keys.NumPad0)
            : code switch { Keys.Multiply => "Num *", Keys.Add => "Num +", Keys.Subtract => "Num -", Keys.Divide => "Num /", Keys.Decimal => "Num .", _ => code.ToString() });
        return string.Join("+", parts);
    }

    public IReadOnlyList<string> Poll(IReadOnlyDictionary<string, int> bindings, int? gamePid, bool enabled)
    {
        if (_lastGamePid != gamePid) { _lastGamePid = gamePid; _wasAllowed = false; _held.Clear(); }
        bool allowed = false;
        if (enabled && gamePid.HasValue)
        {
            GetWindowThreadProcessId(GetForegroundWindow(), out uint foregroundPid);
            allowed = foregroundPid == gamePid.Value;
        }
        return ReadPressed(bindings, allowed, key => IsDown(key));
    }

    internal IReadOnlyList<string> ReadPressed(IReadOnlyDictionary<string, int> bindings, bool allowed, Func<int, bool> isDown)
    {
        if (!allowed) { _wasAllowed = false; _held.Clear(); return Array.Empty<string>(); }
        List<string>? pressed = null;
        bool arm = !_wasAllowed;
        _wasAllowed = true;
        bool control = isDown(0x11), alt = isDown(0x12), shift = isDown(0x10);
        _keyStates.Clear();
        foreach (var binding in bindings)
        {
            Keys keys = (Keys)binding.Value;
            if (keys == Keys.None || !IsValid(keys)) { _held[binding.Key] = false; continue; }
            int code = (int)(keys & Keys.KeyCode);
            if (!_keyStates.TryGetValue(code, out bool keyDown)) _keyStates[code] = keyDown = isDown(code);
            bool down = keyDown && control == keys.HasFlag(Keys.Control) && alt == keys.HasFlag(Keys.Alt) && shift == keys.HasFlag(Keys.Shift);
            if (down && !_held.GetValueOrDefault(binding.Key) && !arm) (pressed ??= new List<string>()).Add(binding.Key);
            _held[binding.Key] = down;
        }
        return pressed ?? (IReadOnlyList<string>)Array.Empty<string>();
    }

    private static bool IsDown(int key) => (GetAsyncKeyState(key) & 0x8000) != 0;
    [DllImport("user32.dll")] private static extern short GetAsyncKeyState(int vKey);
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
}
