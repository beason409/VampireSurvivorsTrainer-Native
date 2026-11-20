# Native health, experience and damage routes

Target: Steam Build `25984009`, game `1.16.111`, Unity `6000.0.62f1`, Windows x64. File identity, MethodInfo layout and entry-byte checks remain those in [armor-route.md](armor-route.md). The game-specific fields are resolved by metadata; no object field offset is embedded.

## Exact native entries

Fresh read-only decompilation of the locally generated interoperability image identified the following declarations in `VampireSurvivors.Runtime.dll`, namespace `VampireSurvivors.Objects.Characters`:

| Type | Method | Managed signature | Metadata token |
| --- | --- | --- | ---: |
| CharacterController | TakeDamage | protected virtual instance `void (float)` | 100689749 |
| CharacterController | AddXp | public instance `void (float, XPMultiplierMode)` | 100689674 |
| EnemyController | GetDamaged | public virtual instance `void (float, HitVfxType, float, WeaponType, bool)` | 100692720 |
| EnemyController | IsBullet | public instance `bool ()` | 100692699 |

`XPMultiplierMode` is in the character namespace; `HitVfxType` and `WeaponType` are in `VampireSurvivors.Data`. Their enum base types must resolve to `System.Int32`. The XP modes are Normal=0, IgnoreGameKiller=1, IgnoreAll=2; every call retains the incoming mode. WeaponType.VOID=0 is excluded from the owned-source cache.

The Windows x64 callback shapes are the generated instance method arguments followed by the hidden MethodInfo pointer. The original functions are called once, outside the trainer's C++ business exception fence. Optional combat routes are advertised only when their full signatures, tokens, native entries and required field metadata pass validation; a combat hook that cannot be prepared remains unsupported.

## Local health and experience

Player `_currentHp` and `_xp` are `System.Single`; `_level` is `System.Int32`. Their values are copied with `il2cpp_field_get_value` on the established Unity callback thread. Separate frame-sample flags distinguish a current observation from a real intercepted game event.

For enabled health protection, only a finite positive incoming damage to the current ready local player is filtered to zero. The original TakeDamage still executes so the game retains its hit feedback, invulnerability and collision flow. There is no HP snapshot restoration, forced healing, permanent invulnerability flag, or save write.

Static disassembly of the fixed game image had already identified TakeDamage at RVA `0x06f34980`: it subtracts incoming damage from current HP, then enters its ordinary positive-HP feedback/callback path even for zero damage. Skipping the entire method previously caused excessive repeated collision callbacks. The native route preserves that corrected behavior.

Experience multiplies the explicit positive incoming float before the original AddXp. The native mode, character growth, level-up and UI rules continue inside the original call. Diagnostics retain raw XP and level before/after, so an ordinary level-up reset is distinguishable from a missing XP gain; an input multiplier is not presented as an unconditional final-XP ratio.

## Owned weapon damage scope

The original managed implementation restricts the multiplier to the current local player's active or hidden equipment types. The native implementation reads the same sources:

| Type / member | Signature or metadata | Token |
| --- | --- | ---: |
| CharacterController.get_WeaponsManager | `CharacterWeaponsManager ()` | 100689533 |
| EquipmentManager.get_ActiveEquipment | `List<Equipment> ()` | 100672071 |
| EquipmentManager.get_HiddenEquipment | `List<Equipment> ()` | 100672072 |
| Equipment.get_Owner | `CharacterController ()` | 100672035 |
| Equipment.get_Type | `WeaponType ()` | 100672033 |

These equipment types are in namespace `VampireSurvivors.Objects`. The list metadata must identify `System.Collections.Generic.List` and an `_items` array whose element class is exactly Equipment. Inflated list accessors are used only through `runtime_invoke`, never interpreted as an unverified native hook ABI. Collection counts are bounded, item types are verified, and only equipment whose owner is the current local player contributes a type. The bounded type cache refreshes at most four times per second and is cleared on role/lifecycle boundaries.

Enemy `_hp` is `System.Single`; `<IsDead>k__BackingField` is `System.Boolean`. Only positive finite hits to non-dead, non-bullet enemies from a verified owned weapon source reach the multiplier. Unknown/environment sources remain unchanged and do not count as local weapon hits. Knockback, visuals, invulnerability, death and reward arguments retain their original values. The original GetDamaged continues to perform the actual damage and death processing.

## Runtime and validation

Each real callback can record input/output and natural HP/XP before/after without a per-hit formatted string. Original-value sampling is isolated from these real-callback counters. A teardown generation and local identity check prevent a returning old-player callback from publishing into a later run. Feature switches share the existing session, fresh-observation, single-player, request-expiry and disconnect rules.

Live validation must compare off/on/off natural contact for health and its callback spacing, normal gem pickups across XP modes/level boundaries, and nonlethal hits whose enemy HP exceeds the incoming damage for the damage multiplier. Low-HP overkill caps actual loss and cannot establish the multiplier by itself. These declarations and a successful native build establish the route implementation; live results are recorded separately after controlled testing.
