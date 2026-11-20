# Native armor pilot: supported-build evidence

Scope: Steam Build `25984009`, game `1.16.111`, Unity `6000.0.62f1`, Windows x64. This is the first native backend slice: state observation and a temporary armor return-value override. The existing managed trainer remains the full implementation.

## File and metadata identity

The locally installed files were read without loading or running the game. Their SHA-256 values match `config/supported-game.json`:

| File | SHA-256 |
| --- | --- |
| GameAssembly.dll | `BD5465840091C480AA97073092B4A2BB93608D0BF218125D535BE0D7252D48EE` |
| global-metadata.dat | `EBC951FB2ADE1EE2822AB0565D07108667D1FD608F47512A05E94D7D9043CC2A` |

Fresh decompilation of the generated interoperability assemblies confirmed these declarations. Tokens identify metadata methods within this exact build; they are not process addresses.

| Image / type | Method | Signature | Token |
| --- | --- | --- | ---: |
| VampireSurvivors.Runtime.dll / VampireSurvivors.Objects.Characters.CharacterController | PArmor | public virtual instance `float ()` | 100689690 |
| same CharacterController | get_IsDead | instance `bool ()` | 100689456 |
| VampireSurvivors.Runtime.dll / VampireSurvivors.Framework.GameManager | Update | private instance `void ()` | 100695246 |
| same GameManager | OnDestroy | protected virtual instance `void ()` | 100695105 |
| same GameManager | get_Player | instance `CharacterController ()` | 100695075 |
| same GameManager | get_IsMultiplayer | instance `bool ()` | 100695070 |
| same GameManager | get_IsPaused | instance `bool ()` | 100694986 |
| VampireSurvivors.Runtime.dll / VampireSurvivors.Objects.PlayerOptions | get_IsInitialized | instance `bool ()` | 100672343 |
| same PlayerOptions | get_IsConfigReady | instance `bool ()` | 100672333 |
| UnityEngine.CoreModule.dll / UnityEngine.SceneManagement.SceneManager | GetActiveScene | static `Scene ()` | 100669541 |
| UnityEngine.CoreModule.dll / UnityEngine.SceneManagement.Scene | GetNameInternal | static `string (int)` | 100669502 |

State fields are looked up by name and checked for their exact types and instance flags. No object-relative field offset is embedded: GameManager `_playerOptions`, `_inGameOverState`, `<StartedAsOnlineMultiplayerRun>k__BackingField`; Scene `m_Handle`.

## MethodInfo layout and exported APIs

The fixed GameAssembly exports 371 names, including 241 IL2CPP APIs. The pilot resolves its required APIs by name. `il2cpp_runtime_invoke`, method signature/token APIs, metadata image/class lookup, field reads, thread attachment, string conversion, and virtual-method resolution are present. There is no `il2cpp_method_get_pointer` export in this image.

The installed `Il2CppInterop.Runtime.dll` has product version `1.5.3+dbda1cb353b0f4253345dc45136d170b9e50a5a0`, SHA-256 `65AB051A681C2C1E52DF7596FF738BE401AAE72CFF882FF18241CF54A3EA38A4`. Its metadata and decompiled `NativeMethodInfoStructHandler_29_2` agree with the [fixed upstream source](https://github.com/BepInEx/Il2CppInterop/blob/dbda1cb353b0f4253345dc45136d170b9e50a5a0/Il2CppInterop.Runtime/Runtime/VersionSpecific/MethodInfo/MethodInfo_29_2.cs). That handler applies from Unity 2023.2 onward; the version selector chooses the newest applicable handler.

For this x64 layout: method entry is at `+0`, virtual entry at `+0x08`, invoker at `+0x10`, name at `+0x18`, class at `+0x20`, return type at `+0x28`, token at `+0x48`, flags at `+0x4c`, parameter count at `+0x52`. These are version-specific metadata layout facts, not reusable object field offsets.

Independent static disassembly of the fixed GameAssembly corroborates the prefix: `il2cpp_method_get_name` reads `[rcx+0x18]`; `il2cpp_method_get_flags` reads `[rcx+0x4c]`. The exported runtime-invoke wrapper reaches RVA `0x6d0300`, then `0x6d0370`. At `0x6d038c` the MethodInfo is retained in RDI; the dispatch paths load `[MethodInfo+0]` and call the invoker at `[MethodInfo+0x10]` (for example RVAs `0x6d03a7` / `0x6d03aa`, and `0x6d03fe` / `0x6d0417`).

Before hooking, the implementation cross-checks this prefix against the exported name/return type/flags/parameter count/token APIs, rejects generic or inflated methods, and requires the entry to be readable, executable, and in a GameAssembly executable section. The first 32 entry bytes must match the verified file image, so an existing Harmony/detour modification is rejected. The normal launcher also rejects coexistence with a managed backend.

## Main-thread and lifecycle contract

Initialization may attach a worker to resolve immutable metadata. It does not invoke Unity instance methods. All game instance observations and queued changes run after the original GameManager.Update on its Unity thread. Scene strings are sampled at most four times per second. Other observations retain their live source values.

The readiness predicate uses the local GameManager.Player identity, `!IsMultiplayer`, `!StartedAsOnlineMultiplayerRun`, initialized/config-ready options, a non-dead local player and no game-over state. A bounded registry of updating GameManager instances rejects multiple observed managers. The hook filters only the exact local player's identity; it never dereferences the cached identity outside the game callback. A different virtual armor entry disables this pilot route rather than applying a base-class guess.

GameManager.OnDestroy is hooked in addition to Update and PArmor. It clears effects and the observed player before the original teardown. A destruction generation is checked after the original Update and again after observation, so a nested teardown cannot publish the old player again. All three hooks are prepared before activation. Stopped callbacks and trampolines remain allocated until process exit; there is no hot unload.

The added observation/teardown business has its own C++ exception fence. Failure sets the atomic stop gate first, then attempts to clear runtime flags and report unsupported status; the armor callback consequently returns its original result even if reporting fails. The game's original Update, armor getter and teardown calls remain outside that fence. Sampling uses a scope guard, and a missing virtual armor route reports an unknown sample rather than a fabricated zero.

## Armor semantics and remaining live checks

The managed implementation's `StatCatalog` defines armor as an absolute finite value from 0 to 10000, default 20. `StatHooks.CharacterStatPostfix` applies it only to the local ready player. The native pilot follows the same range and returns a filtered result after calling the original getter. It writes no armor backing field, permanent upgrade, wallet or save. Original-value sampling bypasses the filter, so closing the override exposes the game's current normal result, including intervening normal upgrades.

The direct native hook callbacks use the Windows x64 IL2CPP instance-method shape `(this, MethodInfo*)`, with float results in the platform's float return convention. Source/signature/layout checks and compilation do not prove that this game actually reaches the hook in every damage path. The first live validation must establish: correct main-thread callback/return behavior; native armor calls outside sampling; actual damage behavior; disable/normal-upgrade preservation; player replacement, run end and reconnect clearing; pause behavior; and heartbeat/request expiry. Reading back the sampled getter alone is insufficient evidence of gameplay armor effect. No game was started, injected into or changed while preparing this evidence.
