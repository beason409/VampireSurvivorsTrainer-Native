# Native resource and ordinary-wallet routes

Target: Steam Build `25984009`, game `1.16.111`, Unity `6000.0.62f1`, Windows x64. File identity, runtime metadata checks, native executable-entry verification and session protection retain [armor-route.md](armor-route.md) and [ordinary-stats-route.md](ordinary-stats-route.md).

## Verified declarations

Read-only Mono.Cecil inspection of the cached generated runtime assembly verified the declarations and the game tokens passed by wrapper static initializers to `GetIl2CppMethodByToken`. These are metadata identities, not addresses or wrapper-method tokens. `PlayerModifierStats`, `ModifierStats`, `MagnetZone` and `PlayerOptions` are in `VampireSurvivors.Objects`; `CharacterController` is in `.Objects.Characters`; `EggFloat` and `EggDouble` are classes in `.Framework.NumberTypes`; `PlayerOptionsData` is in `.Data`.

| Type | Instance method unless marked static | Return / explicit parameters | Game token |
| --- | --- | --- | ---: |
| CharacterController | get_PlayerStats | `PlayerModifierStats ()` | 100689485 |
| CharacterController | get_Magnet | `MagnetZone ()` | 100689563 |
| CharacterController | PlayerStatsUpgrade | `void (ModifierStats, bool)` | 100689813 |
| CharacterController | ResetStats | `void ()` | 100689812 |
| CharacterController | PRevivals, virtual | `EggDouble ()` | 100689706 |
| PlayerModifierStats | get_ReRolls | `EggFloat ()` | 100672283 |
| PlayerModifierStats | get_Skips | `EggFloat ()` | 100672285 |
| PlayerModifierStats | get_Banish | `EggFloat ()` | 100672293 |
| PlayerModifierStats | get_Charm | `int ()` | 100672303 |
| PlayerModifierStats | get_Defang | `float ()` | 100672305 |
| PlayerModifierStats | get_Recycle | `float ()` | 100672313 |
| PlayerModifierStats | ResetStats | `void ()` | 100672315 |
| PlayerModifierStats | Set | `void (ModifierStats)` | 100672316 |
| PlayerModifierStats | Upgrade | `void (ModifierStats, bool)` | 100672317 |
| MagnetZone | RefreshSize | `void ()` | 100672180 |
| EggFloat | .ctor | `void (float, float)` | 100730442 |
| EggFloat | static op_Implicit | `float (EggFloat)` | 100730447 |
| EggDouble | .ctor | `void (double, double)` | 100730423 |
| EggDouble | static op_Implicit | `double (EggDouble)` | 100730428 |
| PlayerOptions | get_MainGameConfig | `PlayerOptionsData ()` | 100672332 |
| PlayerOptions | get_Config | `PlayerOptionsData ()` | 100672335 |
| PlayerOptions | Save | `void (bool commitImmediately, bool createBackup)` | 100672387 |
| PlayerOptions | AddCoinsFlat | `void (float)` | 100672430 |
| PlayerOptions | RemoveCoinsFlat | `float (float)` | 100672432 |
| PlayerOptions | RemoveCoins | `void (int, bool, PlayerOptionsData)` | 100672434 |
| PlayerOptions | RemoveCoins | `void (int, bool)` | 100672435 |
| PlayerOptions | RemoveCoins | `void (float, bool)` | 100672436 |
| PlayerOptionsData | get_Coins | `float ()` | 100731404 |
| PlayerOptionsData | set_Coins, recovery only | `void (float)` | 100731405 |
| MainMenuPage | get_PlayerOptions | `PlayerOptions ()` | 100669706 |
| MainMenuPage | Update, virtual | `void ()` | 100669710 |
| MultiplayerManager | static get_Instance | `MultiplayerManager ()` | 100695696 |
| MultiplayerManager | get_IsMultiplayer | `bool ()` | 100695697 |
| MultiplayerManager | get_IsOnlineMultiplayer | `bool ()` | 100695699 |
| MultiplayerManager | Tick, virtual final | `void ()` | 100695709 |
| UnityEngine.Object | static FindObjectOfType | `Object (System.Type, bool includeInactive)` | 100668602 |
| UnityEngine.Object | static op_Implicit | `bool (Object)` | 100668535 |

Verified instance fields: `PlayerModifierStats.<ReRolls>k__BackingField`, `<Skips>k__BackingField` and `<Banish>k__BackingField` are `EggFloat`; `<Revivals>k__BackingField` is `EggDouble`; `<UsedRevivals>k__BackingField` and `<Charm>k__BackingField` are `int`; `<Recycle>k__BackingField` and `<Defang>k__BackingField` are `float`. `MagnetZone.Radius` is an `EggFloat` field. The running metadata supplies field offsets and exact types; no offset is embedded.

## Temporary resources and normal upgrades

The implementation follows the managed `StatHooks.cs` bindings. Rerolls, skips, banishes and revival totals use independent number objects with a zero egg contribution. Preservation and defang bind a chance in `[0,1]`, converted from the displayed percent. Charm binds the actual local PlayerModifierStats integer field so native inlined consumers see it too. Its getter retains the native bound return and records natural reads only; it does not apply another target filter. Separate normal and bound samples remain available even when that getter is inlined. Charm shares the original stage `RecalculateCurseAndCharm` refresh with curse on enable, value change and disable; normal mutation completion also schedules a safe main-thread refresh after its bound value is restored.

`TemporaryResource` retains the normal binding and a separate temporary budget. Repeating the same target does not allocate a new budget or refill a consumed count. Native consumption can mutate or replace the temporary number object. The outermost native `Upgrade`, `Set` and `ResetStats` scopes capture the remaining temporary object, bind the normal object for the original call, retain its upgraded normal binding, and restore the remaining temporary binding. A scope destructor performs the resume on C++ unwinding as well as normal return. Revival-used resets to zero once on enable or revival-target change, then follows normal native consumption. Disabling restores the independent normal total and normal used count.

The complete CharacterController `PlayerStatsUpgrade` and `ResetStats` calls also suspend resources, so a rebuilt PlayerModifierStats can clone normal data rather than a temporary budget. If the same verified character replaces its stats owner, the backend captures the actual current remaining values, reads the replacement owner's own normal baseline, restores the old owner, and rebases those remaining values onto the new owner through the shared `RebaseTemporaryResource` policy. It does not call Configure/Create or refill to the requested target. Revival total and used travel together. Failure restores through the owner to which each prepared baseline belongs. A different character, scene or destruction generation does not inherit the budget.

Pickup radius uses the same independent EggFloat policy. The full `CharacterController.PlayerStatsUpgrade` scope is required because it updates Radius after `PlayerModifierStats.Upgrade` returns. The native `RefreshSize` applies enable/value/off transitions and completed normal upgrades. This changes the actual Radius field used by native consumers; a getter-only override would be insufficient. Preservation and defang similarly change the native chance fields, including consumers that inline their reads. Defang affects subsequent native enemy generation, rather than retrospectively editing existing enemies.

All detached normal/temporary reference values and owners have strong IL2CPP GC handles. The matching Il2CppInterop.Runtime assembly declares direct `GameAssembly` P/Invokes as `IntPtr il2cpp_gchandle_new(IntPtr, bool)`, `IntPtr il2cpp_gchandle_get_target(IntPtr)` and `void il2cpp_gchandle_free(IntPtr)`. The native export declarations and stored handles therefore use `uintptr_t` throughout, with pointer-width and exact export-type assertions. An older 32-bit handle declaration truncates this supported Windows x64 ABI. The production ownership policy is the shared `StrongRootT<Api>` in `gc_root.hpp`, allowing offline adapters to exercise high-bit handles and copy/move/free behavior directly. New objects are rooted before their constructor is invoked.

The generated `MagnetZone.Radius` and PlayerModifierStats reference-field setters use `il2cpp_gc_wbarrier_set_field(owner, owner + runtimeFieldOffset, rawManagedObject)`. The backend follows that exact route through the shared `WriteReferenceField` helper, with offset/alignment bounds and immediate field-value readback before a native RefreshSize or any later consumer. It never passes the address of a local object-pointer variable as a reference value. Float/int fields retain `il2cpp_field_set_value` with the scalar address. Read-only PE export-table inspection of the fingerprinted game image confirmed `il2cpp_gc_wbarrier_set_field`, `il2cpp_field_set_value_object` and `il2cpp_field_set_value` are present non-forwarded exports; runtime resolution still fails the resource group closed if its required barrier is missing. Missing exports, exact tokens, types, executable routes, mutation hooks or current-character revival virtual route keep the corresponding capability unavailable. Ambiguous folded native hook entries are rejected.

Observation samples are cached at most every 250 ms. The shared `SampleBound` policy reads the independent normal and actual current field values directly. Controlled native revival checks established that the revival field and PRevivals return already reflect the remaining amount: a remaining value of 1 alongside a used count of 1 must display 1, not 0. The used counter remains isolated for native consumption and restoration, but is never subtracted again for display. Bound values are reported separately from natural getter receipts: active binding, sampling and RefreshSize never count as a gameplay read. Reroll/skip/banish, revival, preservation and defang hooks record only actual native getter calls. Inlined consumers can produce a real effect without such a getter receipt; a zero read count is not invented into a success claim.

Resources restore on explicit disable, session/watchdog clearing and loss of the current safe actor on the next main-thread update. There is one narrow natural-death continuation: only an already active revival total/used budget may remain while the same manager freshly returns the same previously live character, scene and destruction generation, in verified solo mode, with a real death signal or a continuing revival interval. This is reported as `game.revivalPending`; paused state alone never grants it. PlayerReady stays false, all other feature/stat switches close, and pending commands are cancelled. New revival enables or target changes are rejected during that interval, while explicit disable remains immediate. The same consumed budget continues when the character revives. Scene/actor changes, teardown, unsafe mode, stale observations, heartbeat expiry and session replacement still close it.

Rooted plain managed bindings can be restored without dereferencing an old character. During a verified revival interval only revival data stays bound; no Unity collider, health or stage refresh is called on the dead actor. Unity refreshes require a freshly verified current live character and matching MagnetZone; a destroyed component is never called. Teardown restores before the original destruction. After Stop, resident lifecycle and mutation callbacks allow main-thread restoration; worker threads do not access Unity objects.

## Ordinary wallet and permanent gold

MainMenu has no running GameManager Update. The verified MainMenuPage.Update and MultiplayerManager.Tick callbacks therefore share a main-thread, 250 ms menu observation/Pump path. The original callbacks always run first. The service tick must belong to the current MultiplayerManager singleton and its verified final virtual route. It continues observation while the main page is inactive on power-up or selection subpages. MainMenuPage is found with `includeInactive:true`, checked for Unity liveness, and its verified `_multiplayerManager` field must equal that singleton. Both multiplayer and online flags must be false; PlayerOptions must be initialized/config-ready and use MainGameConfig. A retained gameplay manager must additionally have no character and independently match the same options and solo mode. Outside the exact MainMenu scene, menu callbacks leave gameplay observation to GameManager and never clear an already observed live gameplay actor. Menu deductions recheck the live singleton mode between observation samples.

All four native deduction overloads require the observed local PlayerOptions, safe/fresh/session/heartbeat state, a separately authenticated backup proof, and `Config == MainGameConfig`. The explicit-config overload additionally requires that exact MainGameConfig. Only a positive finite deduction is replaced with zero; original methods and other parameters retain the native flow. Integer deductions preserve the exact original integer when not overridden. Overload resolution uses the verified game token plus complete signature, including the two same-arity RemoveCoins overloads.

Permanent gold requires the ordinary single-player `MainMenu`, no current character, matching MainGameConfig, an exactly representable integer target, and the runtime's fresh observation, session and deadline checks. The main-thread writer revalidates the active scene, live menu, singleton mode, wallet and teardown generation for each mutation/save; a gameplay manager is optional, and any retained one must pass the no-character checks. The shared pure `PlanGoldChange` policy uses one original AddCoinsFlat or RemoveCoins call when the difference is exactly representable and projects to the target. Otherwise it plans at most two native calls: RemoveCoins of the current balance to zero, verify zero, then AddCoinsFlat of the target and verify it. For example, a large representable balance and a small representable target can require this route because their difference is not a representable float. No save occurs between the two calls. Only after the target is confirmed does the writer call `Save(true, true)` and verify it again. A private writing scope bypasses no-gold-spend filtering throughout. An adjusted or rounded result is never intentionally saved as a successful request. Failure plans a native restoration to the previous balance; if that cannot reproduce the prior float, the recovery path alone uses the verified Coins setter on the same current config. The restored balance is verified, saved and checked again; failure to confirm rollback is returned explicitly. A changed/destroyed scene or wallet prevents any attempt to write through stale ownership. Backend code never grants backup authorization.

## Active adventure wallet

For the supported build, `PlayerOptions.Config` selects the current adventure's `PlayerOptionsData` when its adventure slot is set. `PlayerOptions.Save(bool, bool)` takes the slot from that active data, stores it in the main configuration's `AdventuresSaveData` dictionary, and commits via the normal game save path. The verified `PlayerOptionsData.Coins` setter writes only the selected object's spendable coins.

Adventure gold requires an active solo adventure in the menu. The active config, PlayerOptions adventure config and AdventureManager adventure config must agree, differ from MainGameConfig, and carry the same slot as AdventureManager.CurrentAdventure. The queued write retains both the slot and config identity and rechecks them before writing, saving or rolling back. The ordinary write and no-spend paths still use their existing normal-mode wallet rules.

## Validation boundary

Metadata inspection and offline policy fixtures establish declared signatures and budget/control behavior. They do not establish actual pickup collider size, chance consumers, native exception unwinding, in-game resource consumption/upgrades, wallet persistence or save recovery. Those still require controlled live validation with the protected backup path. This evidence preparation did not open the game, install a loader, or read/change a real save or configuration.
