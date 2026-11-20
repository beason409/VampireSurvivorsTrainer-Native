using BepInEx;
using BepInEx.Unity.IL2CPP;

namespace Trainer.Plugin;

[BepInPlugin(PluginGuid, "Vampire Survivors Trainer", Version)]
public sealed class TrainerPlugin : BasePlugin
{
    public const string PluginGuid = "local.vampiresurvivors.trainer";
    public const string Version = "1.0.1-preview.2";
    private TrainerRuntime? _runtime;
    private PipeServer? _server;

    public override void Load()
    {
        _runtime = new TrainerRuntime(Log);
#if GAME_INTEROP
        MainThreadPump.Runtime = _runtime;
        AddComponent<MainThreadPump>();
#else
        Log.LogWarning("Bootstrap build: Unity interop was absent at compile time; all trainer features remain unavailable.");
#endif
        _server = new PipeServer(_runtime, Log);
        _server.Start();
        Log.LogInfo($"Trainer {Version} loaded, all features OFF. Pipe: {Trainer.Shared.PipeProtocol.PipeName(Environment.ProcessId)}");
    }

    public override bool Unload()
    {
        _runtime?.DisableAll();
        _server?.Dispose();
        _runtime?.Dispose();
        return true;
    }
}

#if GAME_INTEROP
public sealed class MainThreadPump : UnityEngine.MonoBehaviour
{
    internal static TrainerRuntime? Runtime;
    public MainThreadPump(IntPtr pointer) : base(pointer) { }
    public void Update() => Runtime?.Tick();
    public void OnDestroy() => Runtime?.DisableAll();
}
#endif
